#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <psppower.h>
#include <pspiofilemgr.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>

#include "psp_profile.h"
#include "frame_hash.h"
#include "scale_map.h"
#include "yuv_rgb.h"
#include "vfpu_render.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <wili_log.h>
#include "playback_diagnostics.h"
#define WARMUP_FRAMES 30
#if PSP_PTS_PACE
static uint64_t presentation_deadline, presentation_origin_us;
static int64_t presentation_origin_pts;
#if !PSP_RGB_PACE
static void wait_presentation(uint64_t deadline);
#endif
#endif
static volatile int quit_requested;
#include "psp_video.h"
#include "playback_controls.h"
static PlaybackControls controls;
static SceUID controls_lock = -1;
static const PspVideoPresenter *presenter;
static int seek_requested;
static volatile int playback_seeking;
static unsigned playback_seeks;
static int native_seek_enabled;
static int quality_requested;
static void begin_playback_seek(void);
static int64_t seek_target_ms;
static int seek_paused;
static int64_t playback_now_us(void) {
    int64_t now = sceKernelGetSystemTimeWide();
    if (controls_lock < 0) return now;
    sceKernelWaitSema(controls_lock, 1, NULL);
    int64_t value = pc_clock(&controls, now);
    sceKernelSignalSema(controls_lock, 1);
    return value;
}
static int playback_is_paused(void) {
    if (controls_lock < 0) return 0;
    sceKernelWaitSema(controls_lock, 1, NULL);
    int value = controls.paused;
    sceKernelSignalSema(controls_lock, 1);
    return value;
}
static int present_video(const uint32_t *pixels);
static void playback_metadata(int64_t duration_ms, int seekable) {
    if (controls_lock < 0) return;
    sceKernelWaitSema(controls_lock, 1, NULL);
    controls.duration_ms = duration_ms > 0 ? duration_ms : 0;
    controls.seekable = seekable && duration_ms > 0;
    sceKernelSignalSema(controls_lock, 1);
}
#include "rgb_pacer.h"
#include "me_render.h"
#include "me_decode.h"

#include "psp_video.h"
#include "source_io.h"
static const char *video_source = "file:./cache/video.mp4";
static const char *video_log_root = ".";
static const PspVideoSource *video_io_source;
static const PspVideoSource *audio_io_source;
static int client_playback;
extern const unsigned avpriv_psp_approx_reconstruction_v2;
static unsigned cancel_button = PSP_CTRL_CIRCLE;
static int64_t playback_start_ms, playback_position_us, video_origin_us;
static int64_t first_video_us;
extern int avpriv_psp_cabac_mode(void) __attribute__((weak));
static PspVideoStage video_stage;
const char *psp_video_stage_name(PspVideoStage stage) {
    switch (stage) {
    case PSP_VIDEO_STAGE_SETUP: return "setup";
    case PSP_VIDEO_STAGE_LOG: return "log";
    case PSP_VIDEO_STAGE_ME_INIT: return "me_init";
    case PSP_VIDEO_STAGE_ME_RENDER_INIT: return "me_render_init";
    case PSP_VIDEO_STAGE_SOURCE: return "source";
    case PSP_VIDEO_STAGE_VIDEO_INIT: return "video_init";
    case PSP_VIDEO_STAGE_AUDIO_INIT: return "audio_init";
    case PSP_VIDEO_STAGE_PLAY: return "play";
    case PSP_VIDEO_STAGE_CLEANUP: return "cleanup";
    default: return "none";
    }
}
static int poll_controller;
static int video_cancel_thread(SceSize size, void *arg) {
    (void)size; (void)arg;
    while (poll_controller && !quit_requested) {
        SceCtrlData pad = {0};
        sceCtrlPeekBufferPositive(&pad, 1);
        if (controls_lock >= 0) {
            unsigned keys = 0;
            if (pad.Buttons & cancel_button) keys |= PC_BACK;
            if (pad.Buttons & (cancel_button == PSP_CTRL_CIRCLE ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE)) keys |= PC_CONFIRM;
            if (pad.Buttons & PSP_CTRL_START) keys |= PC_PAUSE;
            if (pad.Buttons & PSP_CTRL_TRIANGLE) keys |= PC_PANEL;
            if (pad.Buttons & PSP_CTRL_LEFT) keys |= PC_LEFT;
            if (pad.Buttons & PSP_CTRL_RIGHT) keys |= PC_RIGHT;
            if (pad.Buttons & PSP_CTRL_UP) keys |= PC_UP;
            if (pad.Buttons & PSP_CTRL_DOWN) keys |= PC_DOWN;
            if (pad.Buttons & PSP_CTRL_SQUARE) keys |= PC_DANMAKU;
            if (pad.Buttons & PSP_CTRL_LTRIGGER) keys |= PC_REWIND;
            if (pad.Buttons & PSP_CTRL_RTRIGGER) keys |= PC_FORWARD;
            if (playback_seeking || video_stage != PSP_VIDEO_STAGE_PLAY)
                keys &= ~(PC_LEFT | PC_RIGHT | PC_REWIND | PC_FORWARD);
            sceKernelWaitSema(controls_lock, 1, NULL);
            if (presenter && presenter->update_settings) presenter->update_settings(presenter->opaque, &controls.settings);
            int action = pc_input(&controls, keys, sceKernelGetSystemTimeWide());
            if (action == PC_QUALITY) quality_requested = controls.settings.qualities[controls.quality_index];
            if (action == PC_SEEK) {
                seek_target_ms = controls.preview_ms;
                seek_paused = controls.paused;
                seek_requested = 1;
            }
            sceKernelSignalSema(controls_lock, 1);
            if (action == PC_SEEK) {
                if (native_seek_enabled) begin_playback_seek(); else psp_video_stop();
            }
            else if (action == PC_EXIT || action == PC_QUALITY) psp_video_stop();
        } else if (pad.Buttons & cancel_button) psp_video_stop();
        sceKernelDelayThreadCB(10000);
    }
    return 0;
}

#define ROOT video_log_root
#define SOURCE video_source
#define SAMPLE_FRAMES 300
#define MAX_RECORDS 6000
#define DISPLAY_STRIDE 512
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 272
#define VIDEO_HEIGHT 270
#define HALF_WIDTH 320
#define HALF_HEIGHT 180
#define HALF_X ((DISPLAY_WIDTH - HALF_WIDTH) / 2)
#define HALF_Y ((DISPLAY_HEIGHT - HALF_HEIGHT) / 2)

typedef struct {
    int64_t pts;
    unsigned receive_us;
    unsigned render_us;
} FrameRecord;

typedef struct {
    uint64_t read_us;
    uint64_t decode_us;
    uint64_t render_us;
    uint64_t measured_us;
    int64_t media_boundary_pts;
    int64_t media_us;
    AVRational media_time_base;
    unsigned packets;
    unsigned frames;
    unsigned total_frames;
    unsigned catchup_events, catchup_packets;
    unsigned errors;
    unsigned eof;
    unsigned log_truncated;
    uint32_t frame_hash;
    uint32_t render_hash;
} RunStats;

int psp_profile_enabled;
PspProfileStage psp_profile_stages[PSP_PROFILE_STAGES];
static int profile_requested;
static unsigned timer_pair_us;
typedef struct { int64_t pts, dts; unsigned us; int result; } PacketRecord;
static PacketRecord packet_records[MAX_RECORDS];
static unsigned packet_record_count;
static unsigned packet_records_truncated;
static FrameRecord records[MAX_RECORDS];
#if PSP_AUDIO_VERIFY
static struct { int64_t pts; uint8_t luma[144]; } visual_records[512];
static unsigned visual_count;
#endif
#include "audio_playback.h"
static void interrupt_source_reads(int interrupted) {
    if (video_io_source && video_io_source->interrupt_reads)
        video_io_source->interrupt_reads(video_io_source->opaque, interrupted);
    if (audio_io_source && audio_io_source != video_io_source && audio_io_source->interrupt_reads)
        audio_io_source->interrupt_reads(audio_io_source->opaque, interrupted);
}
static void begin_playback_seek(void) {
    playback_seeking = 1;
#if PSP_AUDIO
    audio.stop = 1;
#endif
    interrupt_source_reads(1);
    sceKernelWaitSema(controls_lock, 1, NULL);
    controls.seeking = 0;
    controls.position_ms = seek_target_ms;
    controls.visible = 1;
    sceKernelSignalSema(controls_lock, 1);
}
static int present_video(const uint32_t *pixels) {
    if (!presenter || !presenter->present) return 0;
    int64_t position = playback_start_ms;
#if PSP_AUDIO
    if (!playback_seeking && audio.rate > 0 && audio.epoch_us && audio.first_pts_us != AV_NOPTS_VALUE) {
        position = (audio_clock_us() - video_origin_us) / 1000;
        if (position < playback_start_ms) position = playback_start_ms;
    }
#endif
    sceKernelWaitSema(controls_lock, 1, NULL);
    if (!controls.paused && !playback_seeking) controls.position_ms = position;
    PspVideoStatus status = {controls.position_ms, controls.duration_ms, controls.preview_ms,
        controls.paused, controls.visible, controls.seeking, controls.seekable, playback_seeking,
        controls.menu_open, controls.menu_row,
        controls.settings.quality_count > 0 ? controls.settings.qualities[controls.quality_index] : 0,
        controls.settings, video_stage, controls.menu_value};
    sceKernelSignalSema(controls_lock, 1);
    return presenter->present(presenter->opaque, pixels, &status);
}
static int seek_in_session(AVFormatContext *format, AVCodecContext *codec,
                           AVPacket *packet, AVFrame *frame, const char *run_path) {
#if PSP_AUDIO && PSP_RGB_PACE && PSP_ME_RENDER
    /* Stop producers before retiring ME and consuming every old RGB slot.
     * The presentation owner keeps the last clean image on screen. */
    int ret = me_render_finish();
    if (ret < 0) return ret;
    ret = rgb_finish();
    if (ret < 0) return ret;
    audio_close(0);
    if (audio.error) return audio.error;
    if (quit_requested) return AVERROR_EXIT;
    rgb_startup_ready = 0;
    interrupt_source_reads(0);
    playback_start_ms = seek_target_ms;
    playback_position_us = playback_start_ms * 1000;
    av_packet_unref(packet);
    av_frame_unref(frame);
    if (format->pb) { format->pb->error = 0; format->pb->eof_reached = 0; }
    ret = av_seek_frame(format, -1, playback_position_us + source_origin_us(format), AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return ret;
    avcodec_flush_buffers(codec);
    ret = audio_open(video_source, audio_io_source ? audio_io_source : video_io_source, run_path);
    if (ret < 0) return ret;
    sceKernelWaitSema(controls_lock, 1, NULL);
    controls.position_ms = playback_start_ms;
    controls.visible = 1;
    controls.hide_at = sceKernelGetSystemTimeWide() + 3000000;
    sceKernelSignalSema(controls_lock, 1);
    ++rgb_generation;
    rgb_seek_preview = 1;
    seek_requested = 0;
    ++playback_seeks;
    playback_seeking = 0;
    WLOG_I(WLC_PLAYER, "seek=%u target_ms=%lld session=retained", playback_seeks, (long long)playback_start_ms);
    return 0;
#else
    (void)format; (void)codec; (void)packet; (void)frame; (void)run_path;
    return AVERROR(ENOSYS);
#endif
}
#if PSP_PTS_PACE && !PSP_RGB_PACE
static void wait_presentation(uint64_t deadline)
{
    while (!quit_requested) {
        uint64_t now = sceKernelGetSystemTimeWide();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        sceKernelDelayThreadCB(remaining > 10000 ? 10000 : (unsigned)remaining);
    }
}
#endif
#if PSP_YUV_LUT
static YuvRgbTables yuv_rgb_tables;
#endif
#if PSP_SPATIAL_LUT
static ScaleMap scale_map;
#endif

static unsigned tick_us(void)
{
    return (unsigned)sceKernelGetSystemTimeWide();
}

unsigned psp_profile_tick(void)
{
    return tick_us();
}

#ifndef PSP_VIDEO_LIBRARY
static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    quit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("psp_ffmpeg_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void install_exit_callback(void)
{
    int tid = sceKernelCreateThread("psp_ffmpeg_callbacks", callback_thread,
                                    0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (tid >= 0)
        sceKernelStartThread(tid, 0, NULL);
}

#endif

static int draw_frame(const AVFrame *frame)
{
#if PSP_ME_RENDER
    int ret = me_render_submit(frame);
#if PSP_AUDIO && PSP_AUDIO_PRESENT_IMMEDIATE && !PSP_RGB_PACE
    /* Audio remains independent while this waits. Present the current picture
     * without adding the next picture's variable decode time to its latency. */
    if (ret >= 0) ret = me_render_finish();
#endif
    return ret;
#elif PSP_ME_DECODE_RENDER
    return me_decode_render(frame);
#endif
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width != 640 || frame->height != 360)
        return -1;

    uint32_t *dst = (uint32_t *)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u);
#if PSP_VFPU_RENDER
    vfpu_draw_full(frame->data, frame->linesize, dst);
#elif PSP_HALF_RENDER
    for (int y = 0; y < HALF_HEIGHT; ++y) {
        uint32_t *line = dst + (y + HALF_Y) * DISPLAY_STRIDE + HALF_X;
        const uint8_t *yl = frame->data[0] + (y * 2) * frame->linesize[0];
        const uint8_t *ul = frame->data[1] + y * frame->linesize[1];
        const uint8_t *vl = frame->data[2] + y * frame->linesize[2];
        for (int x = 0; x < HALF_WIDTH; ++x)
            line[x] = yuv_rgb_pixel(&yuv_rgb_tables, yl[x * 2], ul[x], vl[x]);
    }
#elif PSP_PAIRED_ROWS && PSP_GROUPED_RENDER && PSP_YUV_LUT && PSP_SPATIAL_LUT
    for (int y = 0; y < VIDEO_HEIGHT; y += 3) {
        int sy = y * 4 / 3;
        uint32_t *line0 = dst + (y + 1) * DISPLAY_STRIDE;
        uint32_t *line1 = line0 + DISPLAY_STRIDE;
        uint32_t *line2 = line1 + DISPLAY_STRIDE;
        const uint8_t *yl0 = frame->data[0] + sy * frame->linesize[0];
        const uint8_t *yl1 = yl0 + frame->linesize[0];
        const uint8_t *yl2 = yl1 + frame->linesize[0];
        const uint8_t *ul = frame->data[1] + (sy / 2) * frame->linesize[1];
        const uint8_t *vl = frame->data[2] + (sy / 2) * frame->linesize[2];

        for (int group = 0; group < DISPLAY_WIDTH / 3; ++group) {
            int sx = group * 4, dx = group * 3, cx = group * 2;
            int u0 = ul[cx], v0 = vl[cx];
            int red0 = yuv_rgb_tables.v_red[v0];
            int green0 = yuv_rgb_tables.u_green[u0] + yuv_rgb_tables.v_green[v0];
            int blue0 = yuv_rgb_tables.u_blue[u0];
            int u1 = ul[cx + 1], v1 = vl[cx + 1];
            int red1 = yuv_rgb_tables.v_red[v1];
            int green1 = yuv_rgb_tables.u_green[u1] + yuv_rgb_tables.v_green[v1];
            int blue1 = yuv_rgb_tables.u_blue[u1];
            line0[dx] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl0[sx]], red0, green0, blue0);
            line0[dx + 1] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl0[sx + 1]], red0, green0, blue0);
            line0[dx + 2] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl0[sx + 2]], red1, green1, blue1);
            line1[dx] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl1[sx]], red0, green0, blue0);
            line1[dx + 1] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl1[sx + 1]], red0, green0, blue0);
            line1[dx + 2] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl1[sx + 2]], red1, green1, blue1);
        }

        ul += frame->linesize[1];
        vl += frame->linesize[2];
        for (int group = 0; group < DISPLAY_WIDTH / 3; ++group) {
            int sx = group * 4, dx = group * 3, cx = group * 2;
            int u0 = ul[cx], v0 = vl[cx];
            int red0 = yuv_rgb_tables.v_red[v0];
            int green0 = yuv_rgb_tables.u_green[u0] + yuv_rgb_tables.v_green[v0];
            int blue0 = yuv_rgb_tables.u_blue[u0];
            line2[dx] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl2[sx]], red0, green0, blue0);
            line2[dx + 1] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl2[sx + 1]], red0, green0, blue0);
            int u1 = ul[cx + 1], v1 = vl[cx + 1];
            line2[dx + 2] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl2[sx + 2]],
                                                    yuv_rgb_tables.v_red[v1],
                                                    yuv_rgb_tables.u_green[u1] + yuv_rgb_tables.v_green[v1],
                                                    yuv_rgb_tables.u_blue[u1]);
        }
    }
#else
    for (int y = 0; y < VIDEO_HEIGHT; ++y) {
#if PSP_SPATIAL_LUT
        int sy = scale_map.source_y[y];
#else
        int sy = y * 360 / VIDEO_HEIGHT;
#endif
        uint32_t *line = dst + (y + 1) * DISPLAY_STRIDE;
        const uint8_t *yl = frame->data[0] + sy * frame->linesize[0];
        const uint8_t *ul = frame->data[1] + (sy / 2) * frame->linesize[1];
        const uint8_t *vl = frame->data[2] + (sy / 2) * frame->linesize[2];
#if PSP_GROUPED_RENDER && PSP_YUV_LUT && PSP_SPATIAL_LUT
        for (int group = 0; group < DISPLAY_WIDTH / 3; ++group) {
            int sx = group * 4;
            int dx = group * 3;
            int cx = group * 2;
            int u0 = ul[cx], v0 = vl[cx];
            int red0 = yuv_rgb_tables.v_red[v0];
            int green0 = yuv_rgb_tables.u_green[u0] + yuv_rgb_tables.v_green[v0];
            int blue0 = yuv_rgb_tables.u_blue[u0];
            line[dx] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl[sx]],
                                              red0, green0, blue0);
            line[dx + 1] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl[sx + 1]],
                                                  red0, green0, blue0);
            int u1 = ul[cx + 1], v1 = vl[cx + 1];
            line[dx + 2] = yuv_rgb_pixel_offsets(&yuv_rgb_tables, yuv_rgb_tables.y[yl[sx + 2]],
                                                  yuv_rgb_tables.v_red[v1],
                                                  yuv_rgb_tables.u_green[u1] + yuv_rgb_tables.v_green[v1],
                                                  yuv_rgb_tables.u_blue[u1]);
        }
#else
        for (int x = 0; x < DISPLAY_WIDTH; ++x) {
#if PSP_SPATIAL_LUT
            int sx = scale_map.luma_x[x];
            int cx = scale_map.chroma_x[x];
#else
            int sx = x * 640 / DISPLAY_WIDTH;
            int cx = sx / 2;
#endif
#if PSP_YUV_LUT
            line[x] = yuv_rgb_pixel(&yuv_rgb_tables, yl[sx],
                                    ul[cx], vl[cx]);
#else
            int c = (int)yl[sx] - 16;
            int d = (int)ul[cx] - 128;
            int e = (int)vl[cx] - 128;
            int r = yuv_rgb_clamp((298 * c + 459 * e + 128) >> 8);
            int g = yuv_rgb_clamp((298 * c - 55 * d - 136 * e + 128) >> 8);
            int b = yuv_rgb_clamp((298 * c + 541 * d + 128) >> 8);
            line[x] = 0xff000000u | ((uint32_t)b << 16) |
                      ((uint32_t)g << 8) | (uint32_t)r;
#endif
        }
#endif
    }
#endif
    sceDisplaySetFrameBuf((void *)0x44000000, DISPLAY_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    return 0;
}

static int select_run_dir(char *path, size_t length)
{
    snprintf(path, length, "%s/results", ROOT);
    sceIoMkdir(path, 0777);
    for (int i = 1; i < 1000; ++i) {
        SceIoStat stat;
        snprintf(path, length, "%s/results/run-%03d", ROOT, i);
        if (sceIoGetstat(path, &stat) < 0 && sceIoMkdir(path, 0777) >= 0)
            return 0;
    }
    return -1;
}

#if BILI_PPSSPP_SCE_MPEG
#include "ppsspp_player.h"
#endif

static void save_results(const char *path, const RunStats *s, int render,
                         int full, int error_code)
{
    char file[160];
    if (profile_requested) {
        static const char *names[] = {"cabac_mb", "cavlc_mb", "reconstruct_mb_inclusive", "loop_filter_row",
            "motion_compensation", "intra_luma_predict_residual", "intra_chroma_predict",
            "luma_residual", "chroma_residual", "intra_border_exchange",
            "edge_luma", "edge_chroma", "weight_luma", "weight_chroma",
            "biweight_luma", "biweight_chroma", "prefetch", "await_references",
            "qpel_unknown", "chroma_unknown"};
        snprintf(file, sizeof(file), "%s/profile.csv", path);
        FILE *profile = fopen(file, "w");
        if (profile) {
            fputs("stage,calls,samples,sampled_us,interval,timer_pair_us\n", profile);
            for (int i = 0; i < PSP_PROFILE_STAGES; ++i) {
                char label[80];
                if (i < PSP_PROFILE_QPEL_BASE) {
                    snprintf(label, sizeof(label), "%s", names[i]);
                } else if (i < PSP_PROFILE_CHROMA_BASE) {
                    int k = i - PSP_PROFILE_QPEL_BASE;
                    snprintf(label, sizeof(label), "qpel_%d_%s_x%d_y%d", 16 >> (k / 32),
                             (k % 32) / 16 ? "avg" : "put", k % 4, (k % 16) / 4);
                } else {
                    static const char *phase[] = {"copy", "horizontal", "vertical", "bilinear"};
                    int k = i - PSP_PROFILE_CHROMA_BASE;
                    snprintf(label, sizeof(label), "chroma_%d_%s_%s", 8 >> (k / 8),
                             (k % 8) / 4 ? "avg" : "put", phase[k % 4]);
                }
                fprintf(profile, "%s,%u,%u,%llu,%d,%u\n", label,
                        psp_profile_stages[i].calls, psp_profile_stages[i].samples,
                        (unsigned long long)psp_profile_stages[i].us,
                        PSP_PROFILE_INTERVAL, timer_pair_us);
            }
            fclose(profile);
        }
        snprintf(file, sizeof(file), "%s/packets.csv", path);
        profile = fopen(file, "w");
        if (profile) {
            fputs("packet_index,pts,dts,send_us,result\n", profile);
            for (unsigned i = 0; i < packet_record_count; ++i)
                fprintf(profile, "%u,%lld,%lld,%u,%d\n", i,
                        (long long)packet_records[i].pts, (long long)packet_records[i].dts,
                        packet_records[i].us, packet_records[i].result);
            fclose(profile);
        }
    }
    snprintf(file, sizeof(file), "%s/frames.csv", path);
#if PSP_AUDIO_VERIFY
    snprintf(file, sizeof(file), "%s/visual.csv", path);
    FILE *visual = fopen(file, "w");
    if (visual) {
        fputs("pts,luma\n", visual);
        for (unsigned i = 0; i < visual_count; ++i) {
            fprintf(visual, "%lld,", (long long)visual_records[i].pts);
            for (int j = 0; j < 144; ++j) fprintf(visual, "%02x", visual_records[i].luma[j]);
            fputc('\n', visual);
        }
        fclose(visual);
    }
    snprintf(file, sizeof(file), "%s/frames.csv", path);
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    snprintf(file, sizeof(file), "%s/presentation.csv", path);
    FILE *present = fopen(file, "w");
    if (present) {
        fputs("output_index,pts_us,audio_us,wall_us,dropped,target_us\n", present);
        for (unsigned i = 0; diagnostic_audio_valid && i < rgb_consumed && i < 6000; ++i)
            fprintf(present, "%u,%lld,%lld,%llu,%d,%lld\n", i,
                    (long long)rgb_records[i].pts, (long long)rgb_records[i].audio,
                    (unsigned long long)rgb_records[i].wall, rgb_records[i].dropped,
                    (long long)rgb_records[i].target);
        fclose(present);
    }
    snprintf(file, sizeof(file), "%s/frames.csv", path);
#endif
    FILE *out = fopen(file, "w");
    if (out) {
        fputs("output_index,pts,receive_us,render_us\n", out);
        for (unsigned i = 0; i < s->frames && i < MAX_RECORDS; ++i)
            fprintf(out, "%u,%lld,%u,%u\n", i, (long long)records[i].pts,
                    records[i].receive_us, records[i].render_us);
        fclose(out);
    }

    snprintf(file, sizeof(file), "%s/summary.json", path);
    out = fopen(file, "w");
    if (out) {
        fputs("{\n", out);
        fputs("  \"backend\": \"psp-native\",\n", out);
        fprintf(out, "  \"me_backend\": \"%s\",\n", playback_me_backend_name());
        fprintf(out, "  \"diagnostics_schema\": 1,\n  \"compiled_utc\": \"%s\",\n"
                "  \"build_type\": \"%s\",\n  \"model_id\": %d,\n"
                "  \"devkit_version\": \"%08x\",\n  \"attempt_in_process\": %u,\n",
                WILI_DIAGNOSTIC_COMPILED_UTC, WILI_DIAGNOSTIC_BUILD_TYPE,
                diagnostic_model, diagnostic_devkit, diagnostic_attempt);
        fprintf(out, "  \"start_ms\": %lld,\n  \"position_ms\": %lld,\n  \"first_video_us\": %lld,\n",
                (long long)playback_start_ms, (long long)(playback_position_us / 1000), (long long)first_video_us);
        fprintf(out, "  \"client_catchup_events\": %u,\n  \"client_catchup_packets\": %u,\n", s->catchup_events, s->catchup_packets);
#if PSP_AUDIO
        fprintf(out, "  \"audio_stats_valid\": %s,\n", diagnostic_audio_valid ? "true" : "false");
        if (diagnostic_audio_valid) audio_report(out);
#endif
        fprintf(out, "  \"skip_frame\": %d,\n  \"media_us\": %lld,\n",
                PSP_SKIP_FRAME, (long long)s->media_us);
        fprintf(out, "  \"skip_loop_filter\": %d,\n", PSP_SKIP_LOOP_FILTER);
        fprintf(out, "  \"approx_qpel\": %d,\n", PSP_APPROX_QPEL);
        fprintf(out, "  \"packed_chroma\": %d,\n", PSP_PACKED_CHROMA);
        fprintf(out, "  \"pts_pace\": %d,\n", PSP_PTS_PACE);
        fprintf(out, "  \"me_render_queue\": %d,\n", PSP_ME_RENDER_QUEUE);
        fprintf(out, "  \"me_cache\": %d,\n", PSP_ME_CACHE);
        fprintf(out, "  \"me_full_cache\": %d,\n", PSP_ME_FULL_CACHE);
        fprintf(out, "  \"integer_unaligned_copy\": %d,\n", PSP_INTEGER_UNALIGNED_COPY);
        fprintf(out, "  \"unrolled_avg\": %d,\n", PSP_UNROLLED_AVG);
        fprintf(out, "  \"cabac_movn\": %d,\n",
                avpriv_psp_cabac_mode ? avpriv_psp_cabac_mode() : 0);
#if PSP_APPROX_QPEL
        fprintf(out, "  \"approx_reconstruction\": %u,\n", avpriv_psp_approx_reconstruction_v2);
#endif
#if PSP_ME_RENDER
        fprintf(out, "  \"me_submitted\": %u,\n  \"me_completed\": %u,\n", me_submitted, me_completed);
#if PSP_PTS_PACE
        fprintf(out, "  \"late_over_40ms_including_warmup\": %u,\n"
                "  \"max_lateness_us\": %u,\n  \"total_lateness_us\": %llu,\n",
                me_late_frames, me_max_lateness_us, (unsigned long long)me_total_lateness_us);
#endif
#endif
#if PSP_ME_DECODE
        fprintf(out, "  \"me_decode\": %d,\n  \"me_decode_jobs\": %u,\n  \"me_decode_blocks\": %u,\n"
                "  \"me_decode_checked\": %u,\n  \"me_decode_mismatches\": %u,\n  \"me_decode_error\": %d,\n",
                PSP_ME_DECODE, me_decode_jobs, me_decode_blocks, me_decode_checked,
                me_decode_mismatches, me_decode_error);
#if PSP_ME_DECODE == 4
        fprintf(out, "  \"pipeline_batches\": %u,\n  \"pipeline_handoffs\": %u,\n"
                "  \"pipeline_wait_us\": %u,\n  \"pipeline_max_batch\": %u,\n  \"pipeline_fallback\": %u,\n",
                me_pipeline_batches, me_pipeline_handoffs, me_pipeline_wait_us,
                me_pipeline_max_batch, me_pipeline_fallback);
        fprintf(out, "  \"pipeline_failure_mb\": %u,\n  \"pipeline_failure_pixel\": %u,\n  \"pipeline_failure_values\": %u,\n",
                me_pipeline_failure_mb, me_pipeline_failure_pixel, me_pipeline_failure_values);
        fprintf(out, "  \"pipeline_filters\": %u,\n  \"pipeline_filter_rows\": %u,\n",
                me_pipeline_filters, me_pipeline_filter_rows);
        fprintf(out, "  \"pipeline_trace\": [%u, %u, %u, %u],\n",
                me_pipeline_trace[0], me_pipeline_trace[1], me_pipeline_trace[2], me_pipeline_trace[3]);
#endif
#if PSP_ME_DECODE_RENDER
        fprintf(out, "  \"me_decode_render_submitted\": %u,\n  \"me_decode_render_completed\": %u,\n",
                me_decode_render_submitted, me_decode_render_completed);
#endif
#endif
        fprintf(out, "  \"me_render\": %d,\n  \"profiling\": %s,\n  \"packet_records_truncated\": %s,\n",
                PSP_ME_RENDER,
                profile_requested ? "true" : "false",
                packet_records_truncated ? "true" : "false");
        fprintf(out, "  \"error_stage\": \"%s\",\n", psp_video_stage_name(video_stage));
        fprintf(out,
                "  \"build\": \"%s\",\n"
                "  \"expected_input_sha256\": \"%s\",\n"
                "  \"mode\": \"%s\",\n  \"full\": %s,\n"
                "  \"cpu_mhz\": %d,\n  \"bus_mhz\": %d,\n"
                "  \"warmup_frames\": %d,\n  \"frames\": %u,\n"
                "  \"packets\": %u,\n  \"read_us\": %llu,\n"
                "  \"decode_us\": %llu,\n  \"render_us\": %llu,\n"
                "  \"measured_us\": %llu,\n  \"eof\": %s,\n"
                "  \"errors\": %u,\n  \"error_code\": %d,\n"
                "  \"log_truncated\": %s,\n  \"frame_hash\": \"%08x\",\n"
                "  \"render_hash\": \"%08x\"\n}\n",
                PSP_BUILD_ID, client_playback ? "" : "24e7523c7184bb7af58ba19c6a1d139151e506263f8e93d09c4a670abbdf2cf4", render ? "render" : "decode_only", full ? "true" : "false",
                scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(),
                WARMUP_FRAMES, s->frames, s->packets,
                (unsigned long long)s->read_us, (unsigned long long)s->decode_us,
                (unsigned long long)s->render_us, (unsigned long long)s->measured_us,
                s->eof ? "true" : "false", s->errors, error_code,
                s->log_truncated ? "true" : "false", (unsigned)s->frame_hash,
                (unsigned)s->render_hash);
        fclose(out);
    }
}

static int receive_frames(AVCodecContext *codec, AVFrame *frame,
                          RunStats *s, int render, int full,
                          unsigned *output_count, unsigned *measure_start)
{
    video_stage = PSP_VIDEO_STAGE_PLAY;
    while (!quit_requested) {
        if (playback_seeking) return AVERROR_EXIT;
        unsigned begin = tick_us();
        int ret = avcodec_receive_frame(codec, frame);
        unsigned elapsed = tick_us() - begin;
        if (*output_count >= WARMUP_FRAMES)
            s->decode_us += elapsed;
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return ret;
        if (ret < 0) {
            ++s->errors;
            return ret;
        }

        const int64_t frame_us = frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1 :
            av_rescale_q(frame->best_effort_timestamp, codec->pkt_timebase, AV_TIME_BASE_Q) - video_origin_us;
        if (frame_us >= 0 && frame_us < playback_start_ms * 1000) {
            av_frame_unref(frame);
            continue;
        }
        if (frame_us >= 0) {
            if (first_video_us < 0) first_video_us = frame_us;
            playback_position_us = frame_us;
        }

        unsigned render_us = 0;
#if PSP_AUDIO_VERIFY
        if (visual_count < 512) {
            visual_records[visual_count].pts = frame->best_effort_timestamp;
            for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 16; ++x)
                    visual_records[visual_count].luma[y * 16 + x] =
                        frame->data[0][(20 + y * 40) * frame->linesize[0] + 20 + x * 40];
            ++visual_count;
        }
#endif
        if (render) {
#if PSP_PTS_PACE
            int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) {
#if PSP_AUDIO
                presentation_deadline = audio_video_deadline(pts, codec->pkt_timebase);
#if PSP_RGB_PACE
                presentation_pts_us = av_rescale_q(pts, codec->pkt_timebase, AV_TIME_BASE_Q);
#endif
#else
                if (presentation_origin_pts == AV_NOPTS_VALUE) {
                    presentation_origin_pts = pts;
                    /* A short initial buffer lets decoding overlap the first presentation. */
                    presentation_origin_us = sceKernelGetSystemTimeWide() +
                        (PSP_ME_RENDER_QUEUE ? 1000000 : 250000);
                }
                int64_t offset = av_rescale_q(pts - presentation_origin_pts,
                                             codec->pkt_timebase, AV_TIME_BASE_Q);
                presentation_deadline = presentation_origin_us + (offset > 0 ? offset : 0);
#endif
            } else presentation_deadline = sceKernelGetSystemTimeWide();
#endif
#if PSP_PTS_PACE && !PSP_ME_RENDER && !PSP_ME_DECODE_RENDER && !PSP_RGB_PACE
            wait_presentation(presentation_deadline);
            if (quit_requested) { av_frame_unref(frame); return AVERROR_EXIT; }
#endif
            begin = tick_us();
            ret = draw_frame(frame);
            if (!*output_count && video_io_source && video_io_source->first_frame)
                video_io_source->first_frame(video_io_source->opaque);
            render_us = tick_us() - begin;
            if (ret < 0) {
                if (!playback_seeking) ++s->errors;
                av_frame_unref(frame);
                return ret;
            }
#if PSP_HEADLESS_VERIFY
#if PSP_ME_RENDER
            if (me_render_finish() < 0) { ++s->errors; return -1; }
#elif PSP_ME_DECODE_RENDER
            if (me_decode_render_finish() < 0) { ++s->errors; return -1; }
#endif
            if (*output_count >= WARMUP_FRAMES) {
                const uint8_t *pixels = (const uint8_t *)((uintptr_t)sceGeEdramGetAddr() |
                                                           0x40000000u);
#if PSP_HALF_RENDER
                s->render_hash = hash_visible_plane(s->render_hash,
                                                     pixels + (HALF_Y * DISPLAY_STRIDE + HALF_X) * 4,
                                                     HALF_WIDTH * 4, HALF_HEIGHT,
                                                     DISPLAY_STRIDE * 4);
#else
                s->render_hash = hash_visible_plane(s->render_hash,
                                                     pixels + DISPLAY_STRIDE * 4,
                                                     DISPLAY_WIDTH * 4, VIDEO_HEIGHT,
                                                     DISPLAY_STRIDE * 4);
#endif
            }
#endif
        }

#if PSP_HEADLESS_VERIFY
        if (*output_count >= WARMUP_FRAMES) {
            s->frame_hash = hash_visible_plane(s->frame_hash, frame->data[0],
                                               frame->width, frame->height, frame->linesize[0]);
            for (int plane = 1; plane <= 2; ++plane)
                s->frame_hash = hash_visible_plane(s->frame_hash, frame->data[plane],
                                                   frame->width / 2, frame->height / 2,
                                                   frame->linesize[plane]);
        }
#endif

        if (*output_count >= WARMUP_FRAMES) {
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
                s->media_boundary_pts != AV_NOPTS_VALUE)
                s->media_us = av_rescale_q(frame->best_effort_timestamp -
                                           s->media_boundary_pts,
                                           s->media_time_base, AV_TIME_BASE_Q);
            if (s->frames < MAX_RECORDS) {
                records[s->frames].pts = frame->best_effort_timestamp;
                records[s->frames].receive_us = elapsed;
                records[s->frames].render_us = render_us;
            } else {
                s->log_truncated = 1;
            }
            ++s->frames;
            s->render_us += render_us;
        } else {
            s->media_boundary_pts = frame->best_effort_timestamp;
        }
        ++*output_count;
        if (*output_count == WARMUP_FRAMES) {
            const struct mallinfo heap = mallinfo();
            WLOG_I(WLC_MEM, "decode heap_used=%u system_free=%u system_max=%u",
                   (unsigned)heap.uordblks, sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
        }
        av_frame_unref(frame);
        if (*output_count == WARMUP_FRAMES) {
#if PSP_ME_RENDER && !PSP_ME_RENDER_QUEUE
            if (me_render_finish() < 0) return -1;
#elif PSP_ME_DECODE_RENDER
            if (me_decode_render_finish() < 0) return -1;
#endif
            memset(psp_profile_stages, 0, sizeof(psp_profile_stages));
            psp_profile_enabled = profile_requested;
            *measure_start = tick_us();
        }

        if (!full && s->frames >= SAMPLE_FRAMES)
            return 1;
    }
    return -1;
}

static int run_decode(int render, int full, char *run_path, size_t path_size,
                      RunStats *stats)
{
    AVFormatContext *format = NULL;
    VideoSourceIO input_io = {0};
    AVCodecContext *codec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    unsigned output_count = 0;
    unsigned measure_start = 0;
    int ret = -1;
    int video_index = -1;
    int catchup_to_key = 0;
#if PSP_AUDIO
    int audio_started = 0;
#endif

    psp_profile_enabled = 0;
    stats->media_boundary_pts = AV_NOPTS_VALUE;
#if PSP_PTS_PACE
    presentation_origin_pts = AV_NOPTS_VALUE;
    presentation_origin_us = presentation_deadline = 0;
#endif
    memset(psp_profile_stages, 0, sizeof(psp_profile_stages));
#if PSP_HEADLESS_VERIFY
    stats->frame_hash = 2166136261u;
    stats->render_hash = 2166136261u;
#endif
    packet_record_count = packet_records_truncated = 0;
    diagnostic_audio_valid = 0;
    diagnostic_path[0] = 0;
#if PSP_AUDIO_VERIFY
    visual_count = 0;
#endif
    timer_pair_us = ~0u;
    for (int i = 0; i < 256; ++i) {
        unsigned start = tick_us();
        unsigned pair = tick_us() - start;
        if (pair < timer_pair_us) timer_pair_us = pair;
    }
    video_stage = PSP_VIDEO_STAGE_LOG;
    if (select_run_dir(run_path, path_size) < 0)
        return -2;
    playback_diagnostic_begin(run_path);
#if PSP_ME_DECODE
    video_stage = PSP_VIDEO_STAGE_ME_INIT;
    ret = me_decode_start();
    if (ret < 0) goto done;
#endif
#if PSP_ME_RENDER
    if (render) {
        video_stage = PSP_VIDEO_STAGE_ME_RENDER_INIT;
        playback_diagnostic("me_render_init_begin", 0);
        ret = me_render_start();
        playback_diagnostic("me_render_init_end", ret);
        if (ret < 0) goto done;
    }
#endif

    video_stage = PSP_VIDEO_STAGE_SOURCE;
    ret = source_open(&format, SOURCE, video_io_source, &input_io);
    if (ret < 0) goto done;
    source_memory("video_source");
    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) goto done;
    source_memory("video_probe");
    video_stage = PSP_VIDEO_STAGE_VIDEO_INIT;
    video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index < 0) { ret = video_index; goto done; }
    video_origin_us = source_origin_us(format);
    playback_metadata(format->duration == AV_NOPTS_VALUE ? 0 : format->duration / 1000,
                      !format->pb || (format->pb->seekable & AVIO_SEEKABLE_NORMAL));
    ret = source_start_at(format, video_index, playback_start_ms);
    if (ret < 0) goto done;
    if (format->streams[video_index]->codecpar->codec_id != AV_CODEC_ID_H264) {
        ret = AVERROR_DECODER_NOT_FOUND; goto done;
    }
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!decoder) { ret = -3; goto done; }
    codec = avcodec_alloc_context3(decoder);
    if (!codec) { ret = AVERROR(ENOMEM); goto done; }
    ret = avcodec_parameters_to_context(codec, format->streams[video_index]->codecpar);
    if (ret < 0) goto done;
    codec->thread_count = 1;
    if (client_playback) codec->max_pixels = 1280 * 720;
    codec->pkt_timebase = format->streams[video_index]->time_base;
    stats->media_time_base = codec->pkt_timebase;
    codec->skip_frame = (enum AVDiscard)PSP_SKIP_FRAME;
    codec->skip_loop_filter = (enum AVDiscard)PSP_SKIP_LOOP_FILTER;
    ret = avcodec_open2(codec, decoder, NULL);
    if (ret < 0) goto done;
    WLOG_I(WLC_PLAYER, "VIDEO width=%d height=%d refs=%d", codec->width, codec->height, codec->refs);
    playback_diagnostic("video_codec_ready", 0);
    source_memory("video_codec");
#if PSP_APPROX_QPEL
    if (avpriv_psp_approx_reconstruction_v2 != 2) { ret = -91; goto done; }
#endif
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) { ret = AVERROR(ENOMEM); goto done; }
#if PSP_AUDIO
    audio_started = 1;
    diagnostic_audio_valid = 1;
    video_stage = PSP_VIDEO_STAGE_AUDIO_INIT;
    ret = audio_open(SOURCE, audio_io_source ? audio_io_source : video_io_source, run_path);
    playback_diagnostic("audio_init_end", ret);
    if (ret < 0) goto done;
#endif

    if (render && !presenter) {
        pspDebugScreenInitEx(NULL, PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
#if PSP_HALF_RENDER
        memset((void *)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u),
               0, DISPLAY_STRIDE * DISPLAY_HEIGHT * sizeof(uint32_t));
#endif
        sceDisplaySetFrameBuf((void *)0x44000000, DISPLAY_STRIDE,
                              PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    }

    video_stage = PSP_VIDEO_STAGE_PLAY;
decode_again:
    while (!quit_requested) {
        if (playback_seeking) {
            ret = seek_in_session(format, codec, packet, frame, run_path);
            if (ret < 0) goto done;
            catchup_to_key = 0;
            stats->eof = 0;
        }
        unsigned begin = tick_us();
        ret = av_read_frame(format, packet);
        if (playback_seeking) { av_packet_unref(packet); continue; }
        unsigned elapsed = tick_us() - begin;
        if (output_count >= WARMUP_FRAMES)
            stats->read_us += elapsed;
        if (ret == AVERROR_EOF) {
            stats->eof = 1;
            break;
        }
        if (ret < 0) { ++stats->errors; goto done; }
        if (packet->stream_index != video_index) {
            av_packet_unref(packet);
            continue;
        }
        if (output_count >= WARMUP_FRAMES)
            ++stats->packets;
        begin = tick_us();
#if PSP_AUDIO && PSP_AUDIO_ADAPTIVE
        /* Keep I/P reconstruction, but shed B reconstruction when the packet
         * no longer has enough time before its audio-clock presentation. */
        codec->skip_frame = (enum AVDiscard)PSP_SKIP_FRAME;
        if (!client_playback && packet->pts != AV_NOPTS_VALUE &&
            (uint64_t)sceKernelGetSystemTimeWide() + PSP_AUDIO_VIDEO_LEAD_US >
                audio_video_deadline(packet->pts, codec->pkt_timebase)) {
            codec->skip_frame = AVDISCARD_BIDIR;
            ++audio.adaptive_packets;
        }
#endif
#if PSP_AUDIO
        /* A different stream can contain reference B pictures. Discarding all
         * B pictures to catch up corrupts their descendants. For clients, skip
         * compressed packets until a future keyframe, then reset references.
         * Audio keeps its original clock; no corrupt references are submitted. */
        if (client_playback && packet->pts != AV_NOPTS_VALUE && output_count) {
            int64_t packet_us = av_rescale_q(packet->pts, codec->pkt_timebase, AV_TIME_BASE_Q);
            int64_t clock_us = audio_clock_us();
            if (!catchup_to_key && clock_us - packet_us > 250000) {
                catchup_to_key = 1;
                ++stats->catchup_events;
            }
            if (catchup_to_key) {
                if ((packet->flags & AV_PKT_FLAG_KEY) && packet_us >= clock_us + 250000) {
                    avcodec_flush_buffers(codec);
                    catchup_to_key = 0;
                } else {
                    ++stats->catchup_packets;
                    av_packet_unref(packet);
                    continue;
                }
            }
        }
#endif
        ret = avcodec_send_packet(codec, packet);
#if PSP_ME_DECODE
        if (me_decode_error) ret = me_decode_error;
#endif
        elapsed = tick_us() - begin;
        if (output_count >= WARMUP_FRAMES)
            stats->decode_us += elapsed;
        if (profile_requested && output_count >= WARMUP_FRAMES) {
            if (packet_record_count < MAX_RECORDS) {
                PacketRecord *record = &packet_records[packet_record_count++];
                *record = (PacketRecord){packet->pts, packet->dts, elapsed, ret};
            } else packet_records_truncated = 1;
        }
        av_packet_unref(packet);
        if (ret < 0) { ++stats->errors; goto done; }
        ret = receive_frames(codec, frame, stats, render, full,
                             &output_count, &measure_start);
        if (playback_seeking) continue;
        if (ret == 1) { ret = 0; goto done; }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) goto done;
    }

    if (!quit_requested && stats->eof) {
        unsigned begin = tick_us();
        ret = avcodec_send_packet(codec, NULL);
        if (output_count >= WARMUP_FRAMES)
            stats->decode_us += tick_us() - begin;
        if (ret >= 0) {
            ret = receive_frames(codec, frame, stats, render, 1,
                                 &output_count, &measure_start);
            if (ret == AVERROR_EOF) ret = 0;
        }
    }

done:
    if (playback_seeking && !quit_requested && ret >= 0) goto decode_again;
    playback_diagnostic("cleanup_begin", ret);
#if PSP_ME_DECODE
    {
        int finish = me_decode_stop();
        if (finish < 0) {
            if (ret >= 0) { ret = finish; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
#endif
#if PSP_ME_RENDER
    if (render) {
        unsigned begin = tick_us();
        int finish = me_render_finish();
        stats->render_us += tick_us() - begin;
        if (finish < 0) {
            if (ret >= 0) { ret = finish; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
#endif
    psp_profile_enabled = 0;
    if (ret < 0 && stats->errors == 0)
        stats->errors = 1;
    if (measure_start)
        stats->measured_us = (uint64_t)(tick_us() - measure_start);
#if PSP_ME_RENDER_QUEUE
    /* Account for actual presentation, including the queued final frames. */
    if (me_warmup_presented_us && me_last_presented_us >= me_warmup_presented_us)
        stats->measured_us = me_last_presented_us - me_warmup_presented_us;
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    if (render) {
        int finish = rgb_finish();
        playback_diagnostic("rgb_finish_end", finish);
        if (finish < 0) {
            if (ret >= 0) { ret = -112; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
    if (rgb_warmup_us && rgb_last_us >= rgb_warmup_us)
        stats->measured_us = rgb_last_us - rgb_warmup_us;
#endif
    if (ret >= 0) video_stage = PSP_VIDEO_STAGE_CLEANUP;
#if PSP_ME_RENDER
    if (render) {
        int shutdown = me_render_shutdown();
        playback_diagnostic("me_shutdown_end", shutdown);
        if (shutdown < 0) {
            /* Keep the source/decode error when shutdown only reports the
             * already-failed pipeline. */
            if (ret >= 0) { ret = shutdown; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
#endif
#if PSP_AUDIO
    if (audio_started) {
        const int64_t audio_origin = audio.format ? source_origin_us(audio.format) : 0;
        audio_close(stats->eof && ret >= 0);
        if (output_count && (quit_requested || !stats->eof) &&
            audio.first_pts_us != AV_NOPTS_VALUE && audio.rate > 0) {
            int64_t played = audio_clock_us() - audio_origin;
            if (played < playback_start_ms * 1000) played = playback_start_ms * 1000;
            if (played >= 0 && played < playback_position_us) playback_position_us = played;
        }
        if (audio.error) {
            if (ret >= 0) { ret = audio.error; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
#endif
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec) avcodec_free_context(&codec);
    source_close(&format, &input_io);
    stats->total_frames = output_count;
    if (ret >= 0 || quit_requested) video_stage = PSP_VIDEO_STAGE_NONE;
    playback_diagnostic(psp_video_stage_name(video_stage), ret);
    playback_diagnostic_log_tail();
    save_results(run_path, stats, render, full, ret);
    return ret;
}


#ifndef PSP_VIDEO_LIBRARY
int psp_video_lab_main(void)
{
#if PSP_YUV_LUT
    yuv_rgb_init(&yuv_rgb_tables);
#endif
#if PSP_SPATIAL_LUT
    scale_map_init(&scale_map);
#endif
    install_exit_callback();
    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenInit();
    av_log_set_level(AV_LOG_ERROR);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

#if PSP_HEADLESS_BENCH
    profile_requested = PSP_HEADLESS_PROFILE;
    RunStats stats = {0};
    char path[128] = {0};
    run_decode(1, PSP_BENCH_FULL, path, sizeof(path), &stats);
    sceKernelExitGame();
    return 0;
#endif

    while (!quit_requested) {
        pspDebugScreenClear();
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("WiliMini decoder diagnostics\n%s\n\n", PSP_BUILD_ID);
        pspDebugScreenPrintf("X: decode 300 frames\n");
        pspDebugScreenPrintf("TRIANGLE: render 300 frames\n");
        pspDebugScreenPrintf("Hold L + TRIANGLE/X: sampled H264 profile\n");
        pspDebugScreenPrintf("SQUARE: decode full video\n");
        pspDebugScreenPrintf("CIRCLE: exit\n\n");
        pspDebugScreenPrintf("Input: rickroll.flv (MP4 / H.264)\n");
        pspDebugScreenPrintf("Results: WiliMini/results/run-XXX\n");

        SceCtrlData pad;
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            if (quit_requested) break;
            sceKernelDelayThread(15000);
        } while (!(pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE |
                                PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE)));
        if (quit_requested || (pad.Buttons & PSP_CTRL_CIRCLE)) break;

        profile_requested = (pad.Buttons & PSP_CTRL_LTRIGGER) != 0;
        int render = (pad.Buttons & PSP_CTRL_TRIANGLE) != 0;
        int full = (pad.Buttons & PSP_CTRL_SQUARE) != 0;
        RunStats stats = {0};
        char path[128] = {0};
        pspDebugScreenClear();
        pspDebugScreenPrintf("Decoding... HOME to stop\n");
        int result = run_decode(render, full, path, sizeof(path), &stats);

        pspDebugScreenInit();
        pspDebugScreenClear();
        pspDebugScreenPrintf("Result: %d\n", result);
        if (result < 0) {
            char error[96];
            av_strerror(result, error, sizeof(error));
            pspDebugScreenPrintf("Error: %s\n", error);
        }
        pspDebugScreenPrintf("Frames: %u, packets: %u\n", stats.frames, stats.packets);
        pspDebugScreenPrintf("Total: %llu ms\n",
                             (unsigned long long)(stats.measured_us / 1000));
        pspDebugScreenPrintf("Read: %llu ms, decode: %llu ms\n",
                             (unsigned long long)(stats.read_us / 1000),
                             (unsigned long long)(stats.decode_us / 1000));
        pspDebugScreenPrintf("Render: %llu ms, EOF: %u\n",
                             (unsigned long long)(stats.render_us / 1000), stats.eof);
        pspDebugScreenPrintf("Log: %s\n\nPress CIRCLE for menu\n", path);
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            sceKernelDelayThread(15000);
        } while (!quit_requested && !(pad.Buttons & PSP_CTRL_CIRCLE));
        while (!quit_requested && (pad.Buttons & PSP_CTRL_CIRCLE)) {
            sceCtrlReadBufferPositive(&pad, 1);
            sceKernelDelayThread(15000);
        }
    }
    sceKernelExitGame();
    return 0;
}

#endif

/* Public synchronous API: caller owns lifecycle and the HOME callback.
 * All decoder/ME/audio workers are joined before this returns. */
int psp_video_play(const PspVideoOptions *options, PspVideoResult *result)
{
    if (result) *result = (PspVideoResult){.error = -1, .error_stage = PSP_VIDEO_STAGE_SETUP};
    if (!options || !options->log_root || (!options->source && !options->path) ||
        (options->rgb_buffer_frames != 0 && options->rgb_buffer_frames != 2 &&
         options->rgb_buffer_frames != 4 && options->rgb_buffer_frames != 8) ||
        (options->cancel_button != PSP_VIDEO_CANCEL_CIRCLE &&
         options->cancel_button != PSP_VIDEO_CANCEL_CROSS) ||
        (options->path && strlen(options->path)>1017) ||
        (options->source && (!options->source->read_at || (options->source->size==0 || options->source->size < -1))) ||
        (options->audio_source && (!options->audio_source->read_at || (options->audio_source->size==0 || options->audio_source->size < -1)))) return -1;
    presenter = options->presenter && options->presenter->present ? options->presenter : NULL;
    seek_requested = seek_paused = 0;
    playback_seeking = 0; playback_seeks = 0;
    quality_requested = 0;
    native_seek_enabled = options->backend == PSP_VIDEO_BACKEND_NATIVE_ME;
    seek_target_ms = 0;
    if (presenter) {
        controls_lock = sceKernelCreateSema("player_controls", 0, 1, 1, NULL);
        if (controls_lock < 0) return controls_lock;
        pc_init(&controls, sceKernelGetSystemTimeWide(), options->start_paused);
        controls.position_ms = options->start_ms > 0 ? options->start_ms : 0;
        if (options->settings) controls.settings = *options->settings;
        if (controls.settings.quality_count < 0 || controls.settings.quality_count > 3) controls.settings.quality_count = 0;
        for (int i = 0; i < controls.settings.quality_count; ++i)
            if (controls.settings.qualities[i] == controls.settings.quality) controls.quality_index = i;
    }
    video_io_source=options->source;
    audio_io_source=options->audio_source;
    cancel_button = options->cancel_button == PSP_VIDEO_CANCEL_CROSS ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
    playback_start_ms = options->start_ms > 0 ? options->start_ms : 0;
    playback_position_us = 0;
    first_video_us = -1;
#if PSP_AUDIO && PSP_RGB_PACE
    rgb_slots = options->rgb_buffer_frames ? options->rgb_buffer_frames : RGB_SLOTS;
#endif
    char local_source[1024];
    if (options->source) video_source="custom:stream.mp4";
    else if (strncmp(options->path, "file:", 5)) {
        snprintf(local_source, sizeof(local_source), "file:%s", options->path);
        video_source = local_source;
    } else video_source = options->path;
    video_log_root = options->log_root;
    client_playback = 1;
    quit_requested = 0;
    profile_requested = options->diagnostic_profile != 0;
    yuv_rgb_init(&yuv_rgb_tables);
    scale_map_init(&scale_map);
    av_log_set_level(AV_LOG_ERROR);
    scePowerSetClockFrequency(333, 333, 166);
    SceUID control = sceKernelCreateThread("video_cancel", video_cancel_thread,
                                          0x12, 0x2000, PSP_THREAD_ATTR_USER, NULL);
    poll_controller = 1;
    if (control < 0) {
        poll_controller = client_playback = 0;
        if (result) result->error = control;
        if (controls_lock >= 0) sceKernelDeleteSema(controls_lock);
        controls_lock = -1; presenter = NULL;
        return control;
    }
    int started = sceKernelStartThread(control, 0, NULL);
    if (started < 0) {
        sceKernelDeleteThread(control); poll_controller = client_playback = 0;
        if (result) result->error = started;
        if (controls_lock >= 0) sceKernelDeleteSema(controls_lock);
        controls_lock = -1; presenter = NULL;
        return started;
    }
    RunStats stats = {0};
    char path[128] = {0};
    int ret;
#if BILI_PPSSPP_SCE_MPEG
    if (options->backend == PSP_VIDEO_BACKEND_PPSSPP_SCE_MPEG)
        ret = ppsspp_run_decode(options, &stats);
    else
#endif
    {
        WLOG_I(WLC_PLAYER, "backend=psp-native");
        ret = run_decode(1, options->sample ? 0 : 1, path, sizeof(path), &stats);
    }
    poll_controller = 0;
    if (control >= 0) {
        sceKernelWaitThreadEnd(control, NULL);
        sceKernelDeleteThread(control);
    }
    if (result) *result = (PspVideoResult){.error = quit_requested ? 0 : ret, .cancelled = quit_requested,
        .frames = stats.total_frames, .eof = stats.eof && !quit_requested, .error_stage = video_stage,
        .position_ms = playback_position_us / 1000, .seeks = playback_seeks};
    if (result) {
        result->seek_requested = seek_requested;
        result->seek_ms = seek_target_ms;
        result->seek_paused = controls_lock >= 0 ? controls.paused : seek_paused;
        result->quality_requested = quality_requested;
        if (controls_lock >= 0) result->settings = controls.settings;
    }
    if (controls_lock >= 0) sceKernelDeleteSema(controls_lock);
    controls_lock = -1; presenter = NULL;
    client_playback = 0;
    video_io_source = NULL;
    audio_io_source = NULL;
    return quit_requested ? 0 : ret;
}
void psp_video_stop(void) {
    quit_requested = 1;
    if(video_io_source && video_io_source->cancel)video_io_source->cancel(video_io_source->opaque);
    if(audio_io_source && audio_io_source->cancel)audio_io_source->cancel(audio_io_source->opaque);
}
int psp_video_me_backend(void) {
#if PSP_ME_RENDER
    return meLabBackend();
#else
    return -1;
#endif
}
