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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <wili_log.h>
#include <wili_results.h>
#include "playback_diagnostics.h"
#define WARMUP_FRAMES 30
static volatile int quit_requested;
static const volatile int *host_cancel_requested;
static int playback_cleanup_error;
#include "psp_video.h"
#include "playback_controls.h"
static PlaybackControls controls;
static SceUID controls_lock = -1;
static const PspVideoPresenter *presenter;
static int seek_requested;
static volatile int playback_seeking;
static volatile int seek_pending; /* re-target requested while a seek runs; guarded by controls_lock */
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
static int present_video(const void *pixels, PspVideoPixelFormat format);
static void playback_metadata(int64_t duration_ms, int seekable) {
    if (controls_lock < 0) return;
    sceKernelWaitSema(controls_lock, 1, NULL);
    controls.duration_ms = duration_ms > 0 ? duration_ms : 0;
    controls.seekable = seekable && duration_ms > 0;
    sceKernelSignalSema(controls_lock, 1);
}
#include "video_output.h"

#include "psp_video.h"
#include "source_io.h"
static const char *video_source = "file:./cache/video.mp4";
static const char *video_log_root = ".";
static const PspVideoSource *video_io_source;
static const PspVideoSource *audio_io_source;
static int client_playback;
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
    uint64_t last_tick = 0;
    while (poll_controller && !quit_requested) {
        if (host_cancel_requested && __atomic_load_n(host_cancel_requested, __ATOMIC_ACQUIRE)) {
            psp_video_stop();
            break;
        }
        SceCtrlData pad = {0};
        sceCtrlPeekBufferPositive(&pad, 1);
        /* Playback has no button input for long stretches; reset the idle
         * timers so the backlight and auto-suspend do not kick in. Paused
         * playback is left to sleep normally. */
        uint64_t now = sceKernelGetSystemTimeWide();
        if (!controls.paused && now - last_tick >= 5000000) {
            scePowerTick(PSP_POWER_TICK_ALL);
            last_tick = now;
        }
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
            /* Seek keys stay live while a native seek is in flight so repeated
             * presses accumulate; only the pre-play stages ignore them. */
            if (video_stage != PSP_VIDEO_STAGE_PLAY ||
                (playback_seeking && !native_seek_enabled))
                keys &= ~(PC_LEFT | PC_RIGHT | PC_REWIND | PC_FORWARD);
            int chained = 0;
            sceKernelWaitSema(controls_lock, 1, NULL);
            if (presenter && presenter->update_settings) presenter->update_settings(presenter->opaque, &controls.settings);
            int action = pc_input(&controls, keys, sceKernelGetSystemTimeWide());
            if (action == PC_QUALITY) quality_requested = controls.settings.qualities[controls.quality_index];
            if (action == PC_SEEK) {
                seek_target_ms = controls.preview_ms;
                seek_paused = controls.paused;
                seek_requested = 1;
                /* A seek is already running: the player thread re-targets
                 * it instead of starting a second one. */
                if (native_seek_enabled && playback_seeking) {
                    chained = 1;
                    seek_pending = 1;
                    controls.seeking = 0;
                    controls.position_ms = seek_target_ms;
                    controls.visible = 1;
                }
            }
            sceKernelSignalSema(controls_lock, 1);
            if (action == PC_SEEK && !chained) {
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
} RunStats;

int psp_profile_enabled;
PspProfileStage psp_profile_stages[PSP_PROFILE_STAGES];
static int profile_requested;
static unsigned timer_pair_us;
typedef struct { int64_t pts, dts; unsigned us; int result; } PacketRecord;
static PacketRecord *packet_records;
static unsigned packet_record_count;
static unsigned packet_records_truncated;
static FrameRecord *records;
#include "audio_playback.h"
static void interrupt_source_reads(int interrupted) {
    if (video_io_source && video_io_source->interrupt_reads)
        video_io_source->interrupt_reads(video_io_source->opaque, interrupted);
    if (audio_io_source && audio_io_source != video_io_source && audio_io_source->interrupt_reads)
        audio_io_source->interrupt_reads(audio_io_source->opaque, interrupted);
}
static void begin_playback_seek(void) {
    playback_seeking = 1;
    audio.stop = 1;
    interrupt_source_reads(1);
    sceKernelWaitSema(controls_lock, 1, NULL);
    controls.seeking = 0;
    controls.position_ms = seek_target_ms;
    controls.visible = 1;
    sceKernelSignalSema(controls_lock, 1);
}
static int present_video(const void *pixels, PspVideoPixelFormat format) {
    if (!presenter || !presenter->present) return 0;
    int64_t position = playback_start_ms;
    if (!playback_seeking && audio.rate > 0 && audio.epoch_us && audio.first_pts_us != AV_NOPTS_VALUE) {
        position = (audio_clock_us() - video_origin_us) / 1000;
        if (position < playback_start_ms) position = playback_start_ms;
    }
    sceKernelWaitSema(controls_lock, 1, NULL);
    if (!controls.paused && !playback_seeking) controls.position_ms = position;
    PspVideoStatus status = {controls.position_ms, controls.duration_ms, controls.preview_ms,
        controls.paused, controls.visible, controls.seeking, controls.seekable, playback_seeking,
        controls.menu_open, controls.menu_row,
        controls.settings.quality_count > 0 ? controls.settings.qualities[controls.quality_index] : 0,
        controls.settings, video_stage, controls.menu_value};
    sceKernelSignalSema(controls_lock, 1);
    return presenter->present(presenter->opaque, pixels, format, &status);
}
static int seek_in_session(AVFormatContext *format, AVCodecContext *codec,
                           AVPacket *packet, AVFrame *frame, const char *run_path) {
    /* Stop producers before retiring ME and consuming every queued frame.
     * The presentation owner keeps the last clean image on screen. */
    int ret = video_output_finish();
    if (ret < 0) return ret;
    ret = pacer_finish();
    if (ret < 0) return ret;
    audio_close(0);
    if (audio.error) return audio.error;
    if (quit_requested) return AVERROR_EXIT;
    pacer_startup_ready = 0;
    interrupt_source_reads(0);
    sceKernelWaitSema(controls_lock, 1, NULL);
    seek_pending = 0; /* the target read below already includes it */
    playback_start_ms = seek_target_ms;
    sceKernelSignalSema(controls_lock, 1);
    playback_position_us = playback_start_ms * 1000;
    av_packet_unref(packet);
    av_frame_unref(frame);
    if (format->pb) { format->pb->error = 0; format->pb->eof_reached = 0; }
    ret = av_seek_frame(format, -1, playback_position_us + source_origin_us(format), AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return ret;
    avcodec_flush_buffers(codec);
    ret = audio_open(video_source, audio_io_source ? audio_io_source : video_io_source,
                     audio_io_source ? NULL : format, run_path);
    if (ret < 0) return ret;
    sceKernelWaitSema(controls_lock, 1, NULL);
    const int again = seek_pending;
    if (!again) controls.position_ms = playback_start_ms;
    controls.visible = 1;
    controls.hide_at = sceKernelGetSystemTimeWide() + 3000000;
    if (!again) playback_seeking = 0; /* cleared under the lock so a late press cannot be lost */
    sceKernelSignalSema(controls_lock, 1);
    ++pacer_generation;
    pacer_seek_preview = 1;
    if (!again) seek_requested = 0;
    ++playback_seeks;
    WLOG_I(WLC_PLAYER, "seek=%u target_ms=%lld session=retained%s", playback_seeks,
           (long long)playback_start_ms, again ? " retarget=1" : "");
    return 0;
}

static unsigned tick_us(void)
{
    return (unsigned)sceKernelGetSystemTimeWide();
}

unsigned psp_profile_tick(void)
{
    return tick_us();
}


/* The run folder of the playback in progress ("" if none), and where its log starts.
 * results_enabled is the "Save logs and results" setting (or a profiling run); the lab build
 * always saves results. When off nothing is created or written under results/. */
static char current_run_path[192];
static unsigned run_log_mark;
static int results_enabled = 1;

static void remove_run_dir(const char *path)
{
    /* Names are collected first: removing while reading a directory can skip entries. */
    char names[16][64];
    int count = 0;
    SceUID dir = sceIoDopen(path);
    if (dir >= 0) {
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0 && count < 16) {
            if (!FIO_S_ISDIR(entry.d_stat.st_mode) &&
                snprintf(names[count], sizeof(names[0]), "%s", entry.d_name) < (int)sizeof(names[0]))
                ++count; /* a longer name is not ours; the folder then stays and is retried later */
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    for (int i = 0; i < count; ++i) {
        char file[256];
        if (snprintf(file, sizeof(file), "%s/%s", path, names[i]) < (int)sizeof(file)) sceIoRemove(file);
    }
    sceIoRmdir(path);
}

/* Creates results/run-NNN for this playback and removes the oldest runs beyond
 * WILI_RESULTS_KEEP. Profiling runs keep every folder because they are experiment
 * evidence. Playback must not fail because of its logs: when no numbered folder can be
 * made, results/latest is reused (and overwritten) instead. */
/* Streams this playback's log into its run folder next to summary.json, from the moment the
 * playback began (run_log_mark). Needs "Save logs and results" on; otherwise it is a no-op. */
static void start_run_log(void)
{
    char file[224];
    if (snprintf(file, sizeof(file), "%s/log.txt", current_run_path) < (int)sizeof(file))
        wili_log_sink_start(WILI_LOG_SINK_RUN, file, run_log_mark, 0);
}

static void stop_run_log(void)
{
    wili_log_sink_stop(WILI_LOG_SINK_RUN); /* writes what is pending, then closes */
    current_run_path[0] = 0;
}

static int select_run_dir(char *path, size_t length)
{
    static int scan[1024];
    int count = 0;
    current_run_path[0] = 0;
    if (!results_enabled) {
        path[0] = 0; /* callers treat an empty folder as "do not write results" */
        return 0;
    }
    snprintf(path, length, "%s/results", ROOT);
    sceIoMkdir(path, 0777);
    SceUID dir = sceIoDopen(path);
    if (dir >= 0) {
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0) {
            const int index = FIO_S_ISDIR(entry.d_stat.st_mode) ? wili_run_index(entry.d_name) : -1;
            if (index > 0 && count < (int)(sizeof(scan) / sizeof(scan[0]))) scan[count++] = index;
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    int next = 1, drop[WILI_RESULTS_PRUNE_STEP];
    const int drops = wili_run_plan(scan, count, profile_requested ? 0 : WILI_RESULTS_KEEP, &next, drop,
                                    WILI_RESULTS_PRUNE_STEP);
    WLOG_D(WLC_IO, "results scan folders=%d next=%d prune=%d", count, next, drops);
    for (int attempt = 0; drops >= 0 && attempt < 8; ++attempt) {
        snprintf(path, length, "%s/results/run-%03d", ROOT, next + attempt);
        /* Some filesystems (PPSSPP's among them) accept mkdir on an existing folder, which
         * would mix two runs; check first, as the original numbering did. */
        SceIoStat existing;
        if (sceIoGetstat(path, &existing) >= 0 || sceIoMkdir(path, 0777) < 0) continue;
        for (int i = 0; i < drops; ++i) {
            char old[192];
            snprintf(old, sizeof(old), "%s/results/run-%03d", ROOT, drop[i]);
            remove_run_dir(old);
        }
        if (drops) WLOG_I(WLC_IO, "results pruned=%d kept_newest=%d", drops, WILI_RESULTS_KEEP);
        snprintf(current_run_path, sizeof(current_run_path), "%s", path);
        start_run_log();
        return 0;
    }
    WLOG_W(WLC_IO, "results run folder unavailable; reusing results/latest");
    snprintf(path, length, "%s/results/latest", ROOT);
    sceIoMkdir(path, 0777);
    snprintf(current_run_path, sizeof(current_run_path), "%s", path);
    start_run_log();
    return 0;
}


#if BILI_PPSSPP_SCE_MPEG
#include "ppsspp_player.h"
#endif

static void save_results(const char *path, const RunStats *s, int full, int error_code)
{
    if (!path[0]) return; /* results are off */
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
    snprintf(file, sizeof(file), "%s/presentation.csv", path);
    FILE *present = fopen(file, "w");
    if (present) {
        fputs("output_index,pts_us,audio_us,wall_us,dropped,target_us\n", present);
        for (unsigned i = 0; diagnostic_audio_valid && i < pacer_record_count; ++i)
            fprintf(present, "%u,%lld,%lld,%llu,%d,%lld\n", i,
                    (long long)pacer_records[i].pts, (long long)pacer_records[i].audio,
                    (unsigned long long)pacer_records[i].wall, pacer_records[i].dropped,
                    (long long)pacer_records[i].target);
        fclose(present);
    }
    snprintf(file, sizeof(file), "%s/frames.csv", path);
    FILE *out = fopen(file, "w");
    if (out) {
        fputs("output_index,pts,receive_us,render_us\n", out);
        for (unsigned i = 0; records && i < s->frames && i < MAX_RECORDS; ++i)
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
        fprintf(out, "  \"audio_stats_valid\": %s,\n", diagnostic_audio_valid ? "true" : "false");
        if (diagnostic_audio_valid) audio_report(out);
        fprintf(out, "  \"skip_frame\": %d,\n  \"media_us\": %lld,\n",
                PSP_SKIP_FRAME, (long long)s->media_us);
        fprintf(out, "  \"skip_loop_filter\": %d,\n", PSP_SKIP_LOOP_FILTER);
        fprintf(out, "  \"packed_chroma\": %d,\n", PSP_PACKED_CHROMA);
        fprintf(out, "  \"integer_unaligned_copy\": %d,\n", PSP_INTEGER_UNALIGNED_COPY);
        fprintf(out, "  \"unrolled_avg\": %d,\n", PSP_UNROLLED_AVG);
        fprintf(out, "  \"cabac_movn\": %d,\n",
                avpriv_psp_cabac_mode ? avpriv_psp_cabac_mode() : 0);
        fprintf(out, "  \"me_pipeline\": %d,\n  \"frames_queued\": %u,\n", PSP_ME_PIPELINE, video_frames_queued);
        fprintf(out, "  \"ge_wait_us\": %llu,\n  \"ge_present_us\": %llu,\n",
                (unsigned long long)pacer_wait_us, (unsigned long long)pacer_present_us);
#if PSP_ME_PIPELINE
        fprintf(out,
                "  \"mefp_recon_frames\": %u,\n"
                "  \"mefp_fallback_frames\": %u,\n"
                "  \"mefp_intra_mbs\": %u,\n"
                "  \"mefp_inter_mbs\": %u,\n"
                "  \"mefp_wait_us\": %u,\n"
                "  \"mefp_capture_us\": %llu,\n"
                "  \"mefp_error\": %d,\n",
                mefp_final_stats.recon_frames,
                mefp_final_stats.fallback_frames,
                mefp_final_stats.intra_mbs,
                mefp_final_stats.inter_mbs,
                mefp_final_stats.wait_us,
                (unsigned long long)mefp_final_stats.capture_us,
                mefp_final_stats.error);
#endif
        fprintf(out, "  \"profiling\": %s,\n  \"packet_records_truncated\": %s,\n",
                profile_requested ? "true" : "false",
                packet_records_truncated ? "true" : "false");
        fprintf(out, "  \"error_stage\": \"%s\",\n", psp_video_stage_name(video_stage));
        fprintf(out,
                "  \"build\": \"%s\",\n"
                "  \"full\": %s,\n"
                "  \"cpu_mhz\": %d,\n  \"bus_mhz\": %d,\n"
                "  \"warmup_frames\": %d,\n  \"frames\": %u,\n"
                "  \"packets\": %u,\n  \"read_us\": %llu,\n"
                "  \"decode_us\": %llu,\n  \"render_us\": %llu,\n"
                "  \"measured_us\": %llu,\n  \"eof\": %s,\n"
                "  \"errors\": %u,\n  \"error_code\": %d,\n"
                "  \"log_truncated\": %s\n}\n",
                PSP_BUILD_ID, full ? "true" : "false",
                scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(),
                WARMUP_FRAMES, s->frames, s->packets,
                (unsigned long long)s->read_us, (unsigned long long)s->decode_us,
                (unsigned long long)s->render_us, (unsigned long long)s->measured_us,
                s->eof ? "true" : "false", s->errors, error_code,
                s->log_truncated ? "true" : "false");
        fclose(out);
    }
}

static int receive_frames(AVCodecContext *codec, AVFrame *frame,
                          RunStats *s, int full,
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

        const int64_t pts = frame->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE)
            presentation_pts_us = av_rescale_q(pts, codec->pkt_timebase, AV_TIME_BASE_Q);
        begin = tick_us();
        ret = video_output_submit(frame);
        if (!*output_count && video_io_source && video_io_source->first_frame)
            video_io_source->first_frame(video_io_source->opaque);
        const unsigned render_us = tick_us() - begin;
        if (ret < 0) {
            if (!playback_seeking) ++s->errors;
            av_frame_unref(frame);
            return ret;
        }

        if (*output_count >= WARMUP_FRAMES) {
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
                s->media_boundary_pts != AV_NOPTS_VALUE)
                s->media_us = av_rescale_q(frame->best_effort_timestamp -
                                           s->media_boundary_pts,
                                           s->media_time_base, AV_TIME_BASE_Q);
            if (records) {
                if (s->frames < MAX_RECORDS) {
                    records[s->frames] = (FrameRecord){frame->best_effort_timestamp, elapsed, render_us};
                } else {
                    s->log_truncated = 1;
                }
            }
            ++s->frames;
            s->render_us += render_us;
        } else {
            s->media_boundary_pts = frame->best_effort_timestamp;
        }
        ++*output_count;
        if (*output_count == 1) source_memory("video_first_frame");
        if (*output_count == WARMUP_FRAMES) source_memory("decode");
        av_frame_unref(frame);
        if (*output_count == WARMUP_FRAMES) {
            if (video_output_finish() < 0) return -1;
            memset(psp_profile_stages, 0, sizeof(psp_profile_stages));
            psp_profile_enabled = profile_requested;
            *measure_start = tick_us();
        }

        if (!full && s->frames >= SAMPLE_FRAMES)
            return 1;
    }
    return -1;
}

static int run_decode(int full, char *run_path, size_t path_size, RunStats *stats)
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
    int audio_started = 0;

    psp_profile_enabled = 0;
    stats->media_boundary_pts = AV_NOPTS_VALUE;
    memset(psp_profile_stages, 0, sizeof(psp_profile_stages));
    packet_record_count = packet_records_truncated = 0;
    diagnostic_audio_valid = 0;
    diagnostic_path[0] = 0;
    timer_pair_us = ~0u;
    for (int i = 0; i < 256; ++i) {
        unsigned start = tick_us();
        unsigned pair = tick_us() - start;
        if (pair < timer_pair_us) timer_pair_us = pair;
    }
    video_stage = PSP_VIDEO_STAGE_LOG;
    if (select_run_dir(run_path, path_size) < 0)
        return -2;
    if (run_path[0]) {
        records = malloc(MAX_RECORDS * sizeof(*records));
        if (!records) {
            stats->log_truncated = 1;
            WLOG_W(WLC_MEM, "frame diagnostics allocation failed");
        }
        if (profile_requested) {
            packet_records = malloc(MAX_RECORDS * sizeof(*packet_records));
            if (!packet_records) {
                packet_records_truncated = 1;
                WLOG_W(WLC_MEM, "packet diagnostics allocation failed");
            }
        }
        pacer_records = malloc(PACER_RECORD_LIMIT * sizeof(*pacer_records));
        if (!pacer_records) WLOG_W(WLC_MEM, "presentation diagnostics allocation failed");
    }
    playback_diagnostic_begin(run_path);
    video_stage = PSP_VIDEO_STAGE_ME_RENDER_INIT;
    playback_diagnostic("video_output_init_begin", 0);
    ret = video_output_start();
    playback_diagnostic("video_output_init_end", ret);
    if (ret < 0) goto done;

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
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) { ret = AVERROR(ENOMEM); goto done; }
    audio_started = 1;
    diagnostic_audio_valid = 1;
    video_stage = PSP_VIDEO_STAGE_AUDIO_INIT;
    ret = audio_open(SOURCE, audio_io_source ? audio_io_source : video_io_source,
                     audio_io_source ? NULL : format, run_path);
    source_memory("audio_init_end");
    playback_diagnostic("audio_init_end", ret);
    if (ret < 0) goto done;

    video_stage = PSP_VIDEO_STAGE_PLAY;
decode_again:
    while (!quit_requested) {
        if (playback_seeking) {
            source_memory("seek_begin");
            ret = seek_in_session(format, codec, packet, frame, run_path);
            source_memory("seek_end");
            if (ret < 0) goto done;
            catchup_to_key = 0;
            stats->eof = 0;
            if (playback_seeking) continue; /* re-targeted while seeking */
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
        /* Keep I/P reconstruction, but shed B reconstruction when the packet
         * no longer has enough time before its audio-clock presentation. */
        codec->skip_frame = (enum AVDiscard)PSP_SKIP_FRAME;
        if (!client_playback && packet->pts != AV_NOPTS_VALUE &&
            (uint64_t)sceKernelGetSystemTimeWide() + PSP_AUDIO_VIDEO_LEAD_US >
                audio_video_deadline(packet->pts, codec->pkt_timebase)) {
            codec->skip_frame = AVDISCARD_BIDIR;
            ++audio.adaptive_packets;
        }
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
        ret = avcodec_send_packet(codec, packet);
        elapsed = tick_us() - begin;
        if (output_count >= WARMUP_FRAMES)
            stats->decode_us += elapsed;
        if (packet_records && output_count >= WARMUP_FRAMES) {
            if (packet_record_count < MAX_RECORDS) {
                PacketRecord *record = &packet_records[packet_record_count++];
                *record = (PacketRecord){packet->pts, packet->dts, elapsed, ret};
            } else packet_records_truncated = 1;
        }
        av_packet_unref(packet);
        if (ret < 0) { ++stats->errors; goto done; }
        ret = receive_frames(codec, frame, stats, full,
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
            ret = receive_frames(codec, frame, stats, 1,
                                 &output_count, &measure_start);
            if (ret == AVERROR_EOF) ret = 0;
        }
    }

done:
    if (playback_seeking && !quit_requested && ret >= 0) goto decode_again;
    source_memory("cleanup_begin");
    playback_diagnostic("cleanup_begin", ret);
    {
        unsigned begin = tick_us();
        int finish = video_output_finish();
        stats->render_us += tick_us() - begin;
        if (finish < 0) {
            if (ret >= 0) { ret = finish; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
    psp_profile_enabled = 0;
    if (ret < 0 && stats->errors == 0)
        stats->errors = 1;
    if (measure_start)
        stats->measured_us = (uint64_t)(tick_us() - measure_start);
    {
        int finish = pacer_finish();
        playback_diagnostic("pacer_finish_end", finish);
        if (finish < 0) {
            if (ret >= 0) { ret = -112; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
    if (pacer_warmup_us && pacer_last_us >= pacer_warmup_us)
        stats->measured_us = pacer_last_us - pacer_warmup_us;
    if (ret >= 0) video_stage = PSP_VIDEO_STAGE_CLEANUP;
    {
        int shutdown = video_output_shutdown();
        playback_diagnostic("video_output_shutdown_end", shutdown);
        if (shutdown < 0) {
            playback_cleanup_error = shutdown;
            /* Keep the source/decode error when shutdown only reports the
             * already-failed pipeline. */
            if (ret >= 0) { ret = shutdown; video_stage = PSP_VIDEO_STAGE_CLEANUP; }
            ++stats->errors;
        }
    }
    if (audio_started) {
        const int64_t audio_origin = audio.format ? source_origin_us(audio.format) : 0;
        audio_close(stats->eof && ret >= 0);
        source_memory("audio_closed");
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
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec) avcodec_free_context(&codec);
    source_memory("video_codec_closed");
    source_close(&format, &input_io);
    source_memory("video_source_closed");
    stats->total_frames = output_count;
    if (ret >= 0 || quit_requested) video_stage = PSP_VIDEO_STAGE_NONE;
    playback_diagnostic(psp_video_stage_name(video_stage), ret);
    save_results(run_path, stats, full, ret);
    free(records);
    records = NULL;
    free(packet_records);
    packet_records = NULL;
    /* The presentation worker has joined, independently of ME shutdown. */
    free(pacer_records);
    pacer_records = NULL;
    pacer_record_count = 0;
    return ret;
}



/* Public synchronous API: caller owns lifecycle and the HOME callback.
 * All decoder/ME/audio workers are joined before this returns. */
int psp_video_play(const PspVideoOptions *options, PspVideoResult *result)
{
    if (result) *result = (PspVideoResult){.error = -1, .error_stage = PSP_VIDEO_STAGE_SETUP};
    if (!options || !options->log_root || (!options->source && !options->path) ||
        (options->buffer_frames != 0 && options->buffer_frames != 2 &&
         options->buffer_frames != 4 && options->buffer_frames != 8) ||
        /* Native playback presents every frame through the GE presenter. */
        (options->backend == PSP_VIDEO_BACKEND_NATIVE_ME &&
         (!options->presenter || !options->presenter->present)) ||
        (options->cancel_button != PSP_VIDEO_CANCEL_CIRCLE &&
         options->cancel_button != PSP_VIDEO_CANCEL_CROSS) ||
        (options->path && strlen(options->path)>1017) ||
        (options->source && (!options->source->read_at || (options->source->size==0 || options->source->size < -1))) ||
        (options->audio_source && (!options->audio_source->read_at || (options->audio_source->size==0 || options->audio_source->size < -1)))) return -1;
    if (options->cancel_requested && __atomic_load_n(options->cancel_requested, __ATOMIC_ACQUIRE)) {
        if (result) {
            *result = (PspVideoResult){.cancelled = 1, .position_ms = options->start_ms};
            if (options->settings) result->settings = *options->settings;
        }
        return 0;
    }
    host_cancel_requested = options->cancel_requested;
    playback_cleanup_error = 0;
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
    pacer_slots = options->buffer_frames ? options->buffer_frames : PACER_MAX_SLOTS;
    char local_source[1024];
    if (options->source) video_source="custom:stream.mp4";
    else if (strncmp(options->path, "file:", 5)) {
        snprintf(local_source, sizeof(local_source), "file:%s", options->path);
        video_source = local_source;
    } else video_source = options->path;
    video_log_root = options->log_root;
    client_playback = 1;
    quit_requested = host_cancel_requested && __atomic_load_n(host_cancel_requested, __ATOMIC_ACQUIRE);
    profile_requested = options->diagnostic_profile != 0;
    current_run_path[0] = 0;
    results_enabled = wili_log_files() || profile_requested;
    run_log_mark = wili_log_mark();
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
        ret = run_decode(options->sample ? 0 : 1, path, sizeof(path), &stats);
    }
    poll_controller = 0;
    stop_run_log();
    if (control >= 0) {
        sceKernelWaitThreadEnd(control, NULL);
        sceKernelDeleteThread(control);
    }
    if (playback_cleanup_error < 0) video_stage = PSP_VIDEO_STAGE_CLEANUP;
    const int final_error = playback_cleanup_error < 0 ? playback_cleanup_error : quit_requested ? 0 : ret;
    if (result) *result = (PspVideoResult){.error = final_error,
        .cancelled = quit_requested && playback_cleanup_error >= 0, .cleanup_error = playback_cleanup_error,
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
    host_cancel_requested = NULL;
    return final_error;
}
void psp_video_stop(void) {
    quit_requested = 1;
    if(video_io_source && video_io_source->cancel)video_io_source->cancel(video_io_source->opaque);
    if(audio_io_source && audio_io_source->cancel)audio_io_source->cancel(audio_io_source->opaque);
}
int psp_video_me_backend(void) {
#if PSP_ME_PIPELINE
    return meLabBackend();
#else
    return -1;
#endif
}
#include "audio_only.h"
