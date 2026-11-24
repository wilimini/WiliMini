/* Bounded presentation queue of decoded YUV420P frames.
 *
 * The decoder thread owns every FFmpeg object: it takes a reference to each
 * queued AVFrame and drops it when the slot is reused or the queue finishes.
 * The "pts_present" thread only reads the POD PspVideoYuvFrame descriptors,
 * waits for the frame's pixels to be complete (pacer_frame_ready) and hands
 * them to the presenter, which converts them with the GE at the audio clock.
 * No FFmpeg calls or reference counts cross the thread boundary. */
#ifndef PSP_FRAME_PACER_H
#define PSP_FRAME_PACER_H
#include <wili_log.h>
#define PACER_MAX_SLOTS 8
static unsigned pacer_slots = PACER_MAX_SLOTS;
static int64_t audio_clock_us(void);
static int audio_playback_failed(void);
/* Returns nonzero once the planes of a frame published with this sequence
 * number may be read. Sequence 0 is always ready. Called from pts_present. */
static int pacer_frame_ready(uint32_t seq);
static AVFrame *pacer_frames[PACER_MAX_SLOTS];
static PspVideoYuvFrame pacer_descs[PACER_MAX_SLOTS];
static uint32_t pacer_seq[PACER_MAX_SLOTS];
static uint64_t pacer_wait_us, pacer_present_us;
static int64_t pacer_pts[PACER_MAX_SLOTS], presentation_pts_us;
static SceUID pacer_space = -1, pacer_ready = -1, pacer_thread = -1;
static volatile unsigned pacer_acquired, pacer_produced, pacer_consumed;
static volatile int pacer_stop, pacer_error;
static unsigned pacer_presented, pacer_dropped;
static int64_t pacer_max_late_us, pacer_total_late_us;
static uint64_t pacer_warmup_us, pacer_last_us;
static volatile int pacer_startup_ready;
static volatile int pacer_seek_preview;
static unsigned pacer_generation;
typedef struct { int64_t pts, audio, target; uint64_t wall; int dropped; } PacerRecord;
#define PACER_RECORD_LIMIT 6000
static PacerRecord *pacer_records;
static unsigned pacer_record_count;

/* Waits until the slot's pixels are complete. Returns nonzero to drop it. */
static int pacer_wait_frame(unsigned slot)
{
    const uint32_t seq = pacer_seq[slot];
    if (pacer_frame_ready(seq)) return 0;
    const uint64_t begin = sceKernelGetSystemTimeWide();
    int drop = 0;
    while (!pacer_frame_ready(seq)) {
        if (quit_requested || playback_seeking || pacer_error || audio_playback_failed()) {
            drop = 1;
            break;
        }
        if (sceKernelGetSystemTimeWide() - begin > 2000000u) {
            pacer_error = -211;
            drop = 1;
            break;
        }
        sceKernelDelayThread(500);
    }
    pacer_wait_us += sceKernelGetSystemTimeWide() - begin;
    return drop;
}

static int pacer_worker(SceSize size, void *arg)
{
    (void)size; (void)arg;
    int64_t previous_pts = AV_NOPTS_VALUE;
    unsigned generation = pacer_generation;
    for (;;) {
        /* Without a new frame, refresh the overlays at about 30 Hz. */
        SceUInt refresh_timeout = 33000;
        int ready = sceKernelWaitSema(pacer_ready, 1, &refresh_timeout);
        if (ready < 0) {
            if (pacer_stop) return 0;
            if (!quit_requested && present_video(NULL, PSP_VIDEO_PIXEL_FORMAT_YUV420P) < 0) pacer_error = -113;
            continue;
        }
        if (pacer_stop && pacer_consumed == pacer_produced) return 0;
        if (generation != pacer_generation) { generation = pacer_generation; previous_pts = AV_NOPTS_VALUE; }
        unsigned slot = pacer_consumed % pacer_slots;
        int64_t target = pacer_pts[slot];
        /* Present halfway between neighbouring timestamps: the nearest picture. */
        if (previous_pts != AV_NOPTS_VALUE && target > previous_pts)
            target -= (target - previous_pts) / 2;
        int drop = quit_requested || playback_seeking || pacer_error || audio_playback_failed();
        while (!drop && !pacer_startup_ready && !quit_requested && !playback_seeking)
            sceKernelDelayThread(1000);
        while (!drop && !quit_requested) {
            if (playback_seeking) { drop = 1; break; }
            int64_t delta = target - audio_clock_us();
            int paused = playback_is_paused();
            if ((!paused && delta <= 0) || (paused && (!pacer_presented || pacer_seek_preview))) break;
            if (present_video(NULL, PSP_VIDEO_PIXEL_FORMAT_YUV420P) < 0) { pacer_error = -113; drop = 1; break; }
            sceKernelDelayThread(delta > 2000 || paused ? 2000 : (unsigned)delta);
            if (audio_playback_failed()) drop = 1;
        }
        if (playback_seeking) drop = 1;
        if (!drop && !quit_requested) drop = pacer_wait_frame(slot);
        int64_t late = drop ? 0 : audio_clock_us() - target;
        /* Never spend GE time displaying obsolete queued pictures. */
        if (!drop && !playback_is_paused() && pacer_produced - pacer_consumed > 1) {
            int64_t next_pts = pacer_pts[(pacer_consumed + 1) % pacer_slots];
            if (audio_clock_us() >= (pacer_pts[slot] + next_pts) / 2) drop = 1;
        }
        if (!drop && !quit_requested) {
            const uint64_t begin = sceKernelGetSystemTimeWide();
            int ret = present_video(&pacer_descs[slot], PSP_VIDEO_PIXEL_FORMAT_YUV420P);
            pacer_present_us += sceKernelGetSystemTimeWide() - begin;
            if (ret < 0) pacer_error = ret;
            else {
                late = audio_clock_us() - target;
                if (late > pacer_max_late_us) pacer_max_late_us = late;
                if (late > 0) pacer_total_late_us += late;
                ++pacer_presented;
                pacer_seek_preview = 0;
            }
        } else ++pacer_dropped;
        pacer_last_us = sceKernelGetSystemTimeWide();
        if (pacer_records && pacer_record_count < PACER_RECORD_LIMIT)
            pacer_records[pacer_record_count++] = (PacerRecord){pacer_pts[slot],
                playback_seeking ? 0 : audio_clock_us(), target, pacer_last_us,
                drop || quit_requested || pacer_error};
        previous_pts = pacer_pts[slot];
        ++pacer_consumed;
        if (pacer_consumed == WARMUP_FRAMES) pacer_warmup_us = pacer_last_us;
        sceKernelSignalSema(pacer_space, 1);
    }
}
static void pacer_free_frames(void)
{
    for (unsigned i = 0; i < PACER_MAX_SLOTS; ++i) av_frame_free(&pacer_frames[i]);
}
static int pacer_start(void)
{
    // A failed shutdown may still own the previous frames.
    if (pacer_frames[0]) return AVERROR(EBUSY);
    memset(pacer_descs, 0, sizeof(pacer_descs));
    memset(pacer_seq, 0, sizeof(pacer_seq));
    pacer_wait_us = pacer_present_us = 0;
    pacer_acquired = pacer_produced = pacer_consumed = pacer_presented = pacer_dropped = 0;
    pacer_stop = pacer_error = pacer_startup_ready = pacer_seek_preview = 0;
    pacer_generation = 0;
    pacer_max_late_us = pacer_total_late_us = 0;
    pacer_warmup_us = pacer_last_us = 0;
    for (unsigned i = 0; i < pacer_slots; ++i) {
        pacer_frames[i] = av_frame_alloc();
        if (!pacer_frames[i]) {
            pacer_free_frames();
            return AVERROR(ENOMEM);
        }
    }
    WLOG_I(WLC_MEM, "pacer_start frames=%u", pacer_slots);
    pacer_space = sceKernelCreateSema("pacer_space", 0, pacer_slots, pacer_slots, NULL);
    pacer_ready = sceKernelCreateSema("pacer_ready", 0, 0, pacer_slots + 1, NULL);
    int ret = -110;
    if (pacer_space < 0 || pacer_ready < 0) goto failed;
    pacer_thread = sceKernelCreateThread("pts_present", pacer_worker, 0x14, 16 * 1024,
                                         PSP_THREAD_ATTR_USER, NULL);
    if (pacer_thread < 0) { ret = pacer_thread; goto failed; }
    ret = sceKernelStartThread(pacer_thread, 0, NULL);
    if (ret >= 0) return ret;
    sceKernelDeleteThread(pacer_thread);
failed:
    pacer_thread = -1;
    if (pacer_space >= 0) sceKernelDeleteSema(pacer_space);
    if (pacer_ready >= 0) sceKernelDeleteSema(pacer_ready);
    pacer_space = pacer_ready = -1;
    pacer_free_frames();
    return ret;
}
static int pacer_acquire(void)
{
    for (;;) {
        if (pacer_error || quit_requested) return -111;
        if (playback_seeking) return -114;
        SceUInt timeout = 100000;
        int ret = sceKernelWaitSema(pacer_space, 1, &timeout);
        if (ret >= 0) return (int)(pacer_acquired++ % pacer_slots);
        if ((unsigned)ret != 0x800201a8u) return ret;
    }
}
/* Queues a decoded frame. ready_seq is passed to pacer_frame_ready(). */
static int pacer_submit(const AVFrame *frame, uint32_t ready_seq, int64_t pts)
{
    int slot = pacer_acquire();
    if (slot < 0) return slot;
    av_frame_unref(pacer_frames[slot]);
    if (av_frame_ref(pacer_frames[slot], frame) < 0) {
        --pacer_acquired;
        sceKernelSignalSema(pacer_space, 1);
        return AVERROR(ENOMEM);
    }
    PspVideoYuvFrame *desc = &pacer_descs[slot];
    for (int p = 0; p < 3; ++p) {
        desc->planes[p] = frame->data[p];
        desc->strides[p] = frame->linesize[p];
    }
    desc->width = frame->width;
    desc->height = frame->height;
    pacer_seq[slot] = ready_seq;
    pacer_pts[slot] = pts;
    __asm__ volatile("" ::: "memory");
    ++pacer_produced;
    sceKernelSignalSema(pacer_ready, 1);
    return 0;
}
/* Waits until every queued frame was presented or dropped, then releases the
 * decoder's frames so a seek or EOF does not pin them. */
static int pacer_finish(void)
{
    while (pacer_consumed != pacer_produced) sceKernelDelayThreadCB(1000);
    for (unsigned i = 0; i < PACER_MAX_SLOTS; ++i)
        if (pacer_frames[i]) av_frame_unref(pacer_frames[i]);
    return pacer_error;
}
static void pacer_close(void)
{
    pacer_finish();
    pacer_stop = 1;
    if (pacer_thread >= 0) {
        sceKernelSignalSema(pacer_ready, 1);
        sceKernelWaitThreadEndCB(pacer_thread, NULL);
        sceKernelDeleteThread(pacer_thread);
        pacer_thread = -1;
    }
    if (pacer_space >= 0) sceKernelDeleteSema(pacer_space);
    if (pacer_ready >= 0) sceKernelDeleteSema(pacer_ready);
    pacer_space = pacer_ready = -1;
}
/* Frees the slots. Only call once ME can no longer write the frames. */
static void pacer_release(void)
{
    pacer_free_frames();
}
#endif
