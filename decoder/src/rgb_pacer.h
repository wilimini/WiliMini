/* Bounded RGB queue: CPU owns decoding/ME; this thread owns timed DMA only.
 * No FFmpeg objects or reference counts cross thread boundaries. */
#ifndef PSP_RGB_PACER_H
#define PSP_RGB_PACER_H
#include <wili_log.h>
#if PSP_AUDIO && PSP_RGB_PACE
#include <pspdmac.h>
#include <malloc.h>
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
#include "me_frame_pipeline.h"
#endif
#define RGB_SLOTS 8
static unsigned rgb_slots = RGB_SLOTS;
static int64_t audio_clock_us(void);
static int audio_playback_failed(void);
// Playback owns these buffers until the ME and presentation worker have stopped.
static void *rgb_pixels;
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
/* SC thread owns AVFrame refs in rgb_yuv_frames[]; pts_present only reads POD
 * rgb_yuv_descs[] and uncached ME sequence completion, never calling FFmpeg. */
static AVFrame *rgb_yuv_frames[RGB_SLOTS];
static PspVideoYuvFrame rgb_yuv_descs[RGB_SLOTS];
static uint32_t rgb_yuv_seq[RGB_SLOTS];
static uint64_t rgb_ge_wait_us, rgb_ge_present_us;
static inline int rgb_ge_active(void) { return presenter != NULL; }
#endif
static int64_t rgb_pts[RGB_SLOTS], presentation_pts_us;
static SceUID rgb_space = -1, rgb_ready = -1, rgb_thread = -1;
static volatile unsigned rgb_acquired, rgb_produced, rgb_consumed;
static volatile int rgb_stop, rgb_error;
static unsigned rgb_presented, rgb_dropped;
static int64_t rgb_max_late_us, rgb_total_late_us;
static uint64_t rgb_warmup_us, rgb_last_us;
static volatile int rgb_startup_ready;
static volatile int rgb_seek_preview;
static unsigned rgb_generation;
typedef struct { int64_t pts, audio, target; uint64_t wall; int dropped; } RgbRecord;
#define RGB_RECORD_LIMIT 6000
static RgbRecord *rgb_records;
static unsigned rgb_record_count;

static void *rgb_slot_pixels(unsigned slot)
{
    return (uint8_t *)rgb_pixels + slot * 512 * 272 * rgb_pixel_bytes;
}

static int rgb_worker(SceSize size, void *arg)
{
    (void)size; (void)arg;
    int64_t previous_pts = AV_NOPTS_VALUE;
    unsigned generation = rgb_generation;
    for (;;) {
        SceUInt refresh_timeout = 33000;
        int ready = sceKernelWaitSema(rgb_ready, 1, presenter ? &refresh_timeout : NULL);
        if (ready < 0) {
            if (rgb_stop) return 0;
            if (presenter && !quit_requested && present_video(NULL, output_pixel_format) < 0) rgb_error = -113;
            continue;
        }
        if (rgb_stop && rgb_consumed == rgb_produced) return 0;
        if (generation != rgb_generation) { generation = rgb_generation; previous_pts = AV_NOPTS_VALUE; }
        unsigned slot = rgb_consumed % rgb_slots;
        int64_t target = rgb_pts[slot];
#if PSP_RGB_NEAREST_PTS
        if (previous_pts != AV_NOPTS_VALUE && target > previous_pts)
            target -= (target - previous_pts) / 2;
#endif
        int drop = quit_requested || playback_seeking || rgb_error || audio_playback_failed();
        while (!drop && !rgb_startup_ready && !quit_requested && !playback_seeking)
            sceKernelDelayThread(1000);
        while (!drop && !quit_requested) {
            if (playback_seeking) { drop = 1; break; }
            int64_t delta = target - audio_clock_us();
            int paused = playback_is_paused();
            if ((!paused && delta <= 0) || (presenter && paused && (!rgb_presented || rgb_seek_preview))) break;
            if (presenter && present_video(NULL, output_pixel_format) < 0) { rgb_error = -113; drop = 1; break; }
            sceKernelDelayThread(delta > 2000 || paused ? 2000 : (unsigned)delta);
            if (audio_playback_failed()) drop = 1;
        }
        if (playback_seeking) drop = 1;
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
        if (rgb_ge_active() && !drop && !quit_requested) {
            uint32_t need_seq = rgb_yuv_seq[slot];
            if (need_seq && !mefp_seq_is_done(need_seq)) {
                uint64_t w0 = sceKernelGetSystemTimeWide();
                while (!mefp_seq_is_done(need_seq)) {
                    if (quit_requested || playback_seeking || rgb_error || audio_playback_failed()) {
                        drop = 1;
                        break;
                    }
                    if (sceKernelGetSystemTimeWide() - w0 > 2000000u) {
                        rgb_error = -211;
                        drop = 1;
                        break;
                    }
                    sceKernelDelayThread(500);
                }
                rgb_ge_wait_us += sceKernelGetSystemTimeWide() - w0;
            }
        }
#endif
        int64_t late = drop ? 0 : audio_clock_us() - target;
        /* Never spend DMA bandwidth displaying obsolete queued pictures. */
        if (!drop && !playback_is_paused() && rgb_produced - rgb_consumed > 1) {
            int64_t next_pts = rgb_pts[(rgb_consumed + 1) % rgb_slots];
            if (audio_clock_us() >= (rgb_pts[slot] + next_pts) / 2) drop = 1;
        }
        if (!drop && !quit_requested) {
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
            int ret;
            if (rgb_ge_active()) {
                uint64_t p0 = sceKernelGetSystemTimeWide();
                ret = present_video(&rgb_yuv_descs[slot], PSP_VIDEO_PIXEL_FORMAT_YUV420P);
                rgb_ge_present_us += sceKernelGetSystemTimeWide() - p0;
            } else {
                void *pixels = rgb_slot_pixels(slot);
                ret = presenter ? present_video(pixels, output_pixel_format) :
                    sceDmacMemcpy((void *)(0x44000000 + 512 * rgb_pixel_bytes),
                                  (uint8_t *)pixels + 512 * rgb_pixel_bytes,
                                  512 * 270 * rgb_pixel_bytes);
            }
#else
            void *pixels = rgb_slot_pixels(slot);
            int ret = presenter ? present_video(pixels, output_pixel_format) :
                sceDmacMemcpy((void *)(0x44000000 + 512 * rgb_pixel_bytes),
                              (uint8_t *)pixels + 512 * rgb_pixel_bytes,
                              512 * 270 * rgb_pixel_bytes);
#endif
            if (ret < 0) rgb_error = ret;
            else {
                late = audio_clock_us() - target;
                if (late > rgb_max_late_us) rgb_max_late_us = late;
                if (late > 0) rgb_total_late_us += late;
                ++rgb_presented;
                rgb_seek_preview = 0;
            }
        } else ++rgb_dropped;
        rgb_last_us = sceKernelGetSystemTimeWide();
        if (rgb_records && rgb_record_count < RGB_RECORD_LIMIT)
            rgb_records[rgb_record_count++] = (RgbRecord){rgb_pts[slot], playback_seeking ? 0 : audio_clock_us(), target, rgb_last_us,
                                                         drop || quit_requested || rgb_error};
        previous_pts = rgb_pts[slot];
        ++rgb_consumed;
        if (rgb_consumed == WARMUP_FRAMES) rgb_warmup_us = rgb_last_us;
        sceKernelSignalSema(rgb_space, 1);
    }
}
static int rgb_start(void)
{
    // A failed ME shutdown may still own the previous output allocation.
    if (rgb_pixels) return AVERROR(EBUSY);
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
    if (rgb_yuv_frames[0]) return AVERROR(EBUSY);
    memset(rgb_yuv_descs, 0, sizeof(rgb_yuv_descs));
    memset(rgb_yuv_seq, 0, sizeof(rgb_yuv_seq));
    rgb_ge_wait_us = rgb_ge_present_us = 0;
#endif
    rgb_acquired = rgb_produced = rgb_consumed = rgb_presented = rgb_dropped = 0;
    rgb_stop = rgb_error = rgb_startup_ready = rgb_seek_preview = 0;
    rgb_generation = 0;
    rgb_max_late_us = rgb_total_late_us = 0;
    rgb_warmup_us = rgb_last_us = 0;
    const uint64_t setup_start = sceKernelGetSystemTimeWide();
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
    if (rgb_ge_active()) {
        for (unsigned i = 0; i < rgb_slots; ++i) {
            rgb_yuv_frames[i] = av_frame_alloc();
            if (!rgb_yuv_frames[i]) {
                for (unsigned j = 0; j < i; ++j) av_frame_free(&rgb_yuv_frames[j]);
                return AVERROR(ENOMEM);
            }
        }
        WLOG_I(WLC_MEM, "rgb_start mode=ge_yuv frames=%u bytes=0 setup_us=%u", rgb_slots,
               (unsigned)(sceKernelGetSystemTimeWide() - setup_start));
    } else
#endif
    {
        rgb_pixels = memalign(64, rgb_slots * 512 * 272 * rgb_pixel_bytes);
        if (!rgb_pixels) return AVERROR(ENOMEM);
        memset(rgb_pixels, 0, rgb_slots * 512 * 272 * rgb_pixel_bytes);
        sceKernelDcacheWritebackInvalidateRange(rgb_pixels, rgb_slots * 512 * 272 * rgb_pixel_bytes);
        WLOG_I(WLC_MEM, "rgb_start frames=%u bytes=%u setup_us=%u", rgb_slots,
               (unsigned)(rgb_slots * 512 * 272 * rgb_pixel_bytes),
               (unsigned)(sceKernelGetSystemTimeWide() - setup_start));
    }
    rgb_space = sceKernelCreateSema("rgb_space", 0, rgb_slots, rgb_slots, NULL);
    rgb_ready = sceKernelCreateSema("rgb_ready", 0, 0, rgb_slots + 1, NULL);
    int ret = -110;
    if (rgb_space < 0 || rgb_ready < 0) goto failed;
    rgb_thread = sceKernelCreateThread("pts_present", rgb_worker, 0x14, 16 * 1024,
                                      PSP_THREAD_ATTR_USER, NULL);
    if (rgb_thread < 0) { ret = rgb_thread; goto failed; }
    ret = sceKernelStartThread(rgb_thread, 0, NULL);
    if (ret >= 0) return ret;
    sceKernelDeleteThread(rgb_thread);
failed:
    rgb_thread = -1;
    if (rgb_space >= 0) sceKernelDeleteSema(rgb_space);
    if (rgb_ready >= 0) sceKernelDeleteSema(rgb_ready);
    rgb_space = rgb_ready = -1;
    free(rgb_pixels);
    rgb_pixels = NULL;
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
    for (unsigned i = 0; i < RGB_SLOTS; ++i) av_frame_free(&rgb_yuv_frames[i]);
#endif
    return ret;
}
static int rgb_acquire(void)
{
    for (;;) {
        if (rgb_error || quit_requested) return -111;
        if (playback_seeking) return -114;
        SceUInt timeout = 100000;
        int ret = sceKernelWaitSema(rgb_space, 1, &timeout);
        if (ret >= 0) return (int)(rgb_acquired++ % rgb_slots);
        if ((unsigned)ret != 0x800201a8u) return ret;
    }
}
static void rgb_publish(int64_t pts)
{
    rgb_pts[rgb_produced % rgb_slots] = pts;
    ++rgb_produced;
    sceKernelSignalSema(rgb_ready, 1);
}
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
static int rgb_submit_yuv(const AVFrame *frame, uint32_t ready_seq, int64_t pts)
{
    int slot = rgb_acquire();
    if (slot < 0) return slot;
    av_frame_unref(rgb_yuv_frames[slot]);
    if (av_frame_ref(rgb_yuv_frames[slot], frame) < 0) {
        --rgb_acquired;
        sceKernelSignalSema(rgb_space, 1);
        return AVERROR(ENOMEM);
    }
    rgb_yuv_descs[slot].planes[0]  = frame->data[0];
    rgb_yuv_descs[slot].planes[1]  = frame->data[1];
    rgb_yuv_descs[slot].planes[2]  = frame->data[2];
    rgb_yuv_descs[slot].strides[0] = frame->linesize[0];
    rgb_yuv_descs[slot].strides[1] = frame->linesize[1];
    rgb_yuv_descs[slot].strides[2] = frame->linesize[2];
    rgb_yuv_descs[slot].width      = frame->width;
    rgb_yuv_descs[slot].height     = frame->height;
    rgb_yuv_seq[slot]              = ready_seq;
    __asm__ volatile("" ::: "memory");
    rgb_publish(pts);
    return 0;
}
#endif
static int rgb_finish(void)
{
    while (rgb_consumed != rgb_produced) sceKernelDelayThreadCB(1000);
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
    for (unsigned i = 0; i < RGB_SLOTS; ++i)
        if (rgb_yuv_frames[i]) av_frame_unref(rgb_yuv_frames[i]);
#endif
    return rgb_error;
}
static void rgb_close(void)
{
    rgb_finish();
    rgb_stop = 1;
    if (rgb_thread >= 0) {
        sceKernelSignalSema(rgb_ready, 1);
        sceKernelWaitThreadEndCB(rgb_thread, NULL);
        sceKernelDeleteThread(rgb_thread);
        rgb_thread = -1;
    }
    if (rgb_space >= 0) sceKernelDeleteSema(rgb_space);
    if (rgb_ready >= 0) sceKernelDeleteSema(rgb_ready);
    rgb_space = rgb_ready = -1;
}
static void rgb_release(void)
{
    free(rgb_pixels);
    rgb_pixels = NULL;
#if PSP_GE_RENDER && PSP_ME_FRAME_PIPELINE
    for (unsigned i = 0; i < RGB_SLOTS; ++i) av_frame_free(&rgb_yuv_frames[i]);
#endif
}
#endif
#endif
