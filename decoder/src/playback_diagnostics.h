/* Included by player.c. Only the decoder thread writes this file. */
#ifndef WILI_PLAYBACK_DIAGNOSTICS_H
#define WILI_PLAYBACK_DIAGNOSTICS_H
#include <kubridge.h>
#include <psprtc.h>
#include "build_diagnostics.h"
#include <wili_log.h>
#if PSP_ME_RENDER
#include "me_bridge.h"
#include "me_diagnostics.h"
#endif
static char diagnostic_path[192];
static unsigned diagnostic_sequence;
static int diagnostic_audio_valid;
static int diagnostic_model;
static unsigned diagnostic_devkit;
static ScePspDateTime diagnostic_clock;
static unsigned diagnostic_attempt;

/* The loader chooses the bridge PRX from the user's setting; report what it started. */
static const char *playback_me_backend_name(void)
{
#if PSP_ME_RENDER
    switch (meLabBackend()) {
    case ME_BACKEND_MIST: return "mist";
    case ME_BACKEND_CUSTOM_CORE: return "custom-core";
    default: return "unknown";
    }
#else
    return "none";
#endif
}

static void playback_diagnostic(const char *stage, int result)
{
    if (!diagnostic_path[0]) return;
    FILE *out = fopen(diagnostic_path, "a");
    if (!out) return;
    fprintf(out, "checkpoint=%s result=%d uptime_us=%llu free_bytes=%u max_free_bytes=%u\n",
            stage, result, (unsigned long long)sceKernelGetSystemTimeWide(),
            (unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());
#if PSP_ME_RENDER
    MeDiagnostics snapshot;
    int ret = meLabGetDiagnostics(&snapshot, sizeof(snapshot));
    if (ret == 0 && snapshot.version == ME_DIAGNOSTICS_VERSION) {
        fprintf(out, "bridge_build=%s bridge_schema=%u ready=%u pending=%u table=%d sequence=%u\n",
                snapshot.build, (unsigned)snapshot.version, (unsigned)snapshot.ready, (unsigned)snapshot.pending,
                (int)snapshot.table, (unsigned)snapshot.sequence);
        static const char *names[] = {"unknown", "init", "profile", "bootstrap", "backend",
            "dma_begin", "dma_end", "inject_begin", "ready", "submit", "trigger",
            "wait_timeout", "wait_done", "shutdown", "restore_begin", "restored",
            "core_boot", "core_ready", "core_probe", "core_stop"};
        unsigned first = snapshot.sequence > ME_DIAGNOSTICS_CAPACITY ?
            snapshot.sequence - ME_DIAGNOSTICS_CAPACITY : 0;
        if (first > diagnostic_sequence)
            fprintf(out, "bridge_records_overwritten=%u\n", first - diagnostic_sequence);
        if (first < diagnostic_sequence) first = diagnostic_sequence;
        for (unsigned i = first; i < snapshot.sequence; ++i) {
            const MeDiagnosticRecord *r = &snapshot.records[i % ME_DIAGNOSTICS_CAPACITY];
            fprintf(out, "bridge_seq=%u time_us=%u event=%s result=%d a=%08x b=%08x c=%08x\n",
                    (unsigned)r->sequence, (unsigned)r->time_us, r->event < sizeof(names) / sizeof(names[0]) ? names[r->event] : "unknown",
                    (int)r->result, (unsigned)r->a, (unsigned)r->b, (unsigned)r->c);
        }
        diagnostic_sequence = snapshot.sequence;
    } else fprintf(out, "bridge_snapshot_error=%d schema=%u\n", ret, ret == 0 ? (unsigned)snapshot.version : 0);
#endif
    /* Close each checkpoint so an ordinary failure does not need final cleanup
     * to save the preceding stage. A hard lock/power cut may still lose writes. */
    fclose(out);
}
/* Append the newest log lines so one file shows what the player reported around a failure. */
static void playback_diagnostic_log_tail(void)
{
    if (!diagnostic_path[0]) return;
    FILE *out = fopen(diagnostic_path, "a");
    if (!out) return;
    fputs("recent_log_begin\n", out);
    const int lines = wili_log_dump(out);
    fprintf(out, "recent_log_end lines=%d\n", lines);
    fclose(out);
}
static void playback_diagnostic_begin(const char *run_path)
{
    diagnostic_sequence = 0;
    ++diagnostic_attempt;
    diagnostic_model = kuKernelGetModel();
    diagnostic_devkit = sceKernelDevkitVersion();
    memset(&diagnostic_clock, 0, sizeof(diagnostic_clock));
    int rtc_result = sceRtcGetCurrentClockLocalTime(&diagnostic_clock);
    snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/diagnostics.txt", run_path);
    FILE *out = fopen(diagnostic_path, "w");
    if (!out) { diagnostic_path[0] = 0; return; }
    fprintf(out, "diagnostics_schema=1\nbuild=%s\ncompiled_utc=%s\nbuild_type=%s\n"
            "model_id=%d\ndevkit_version=%08x\nattempt_in_process=%u\n"
            "device_local_time=%04u-%02u-%02uT%02u:%02u:%02u\nrtc_result=%d\n"
            "backend=psp-native\nme_backend=%s\nme_render=%d me_dma=%d me_cache=%d me_full_cache=%d me_queue=%d\n",
            WILI_DIAGNOSTIC_BUILD, WILI_DIAGNOSTIC_COMPILED_UTC, WILI_DIAGNOSTIC_BUILD_TYPE,
            diagnostic_model, diagnostic_devkit, diagnostic_attempt,
            diagnostic_clock.year, diagnostic_clock.month, diagnostic_clock.day,
            diagnostic_clock.hour, diagnostic_clock.minute, diagnostic_clock.second, rtc_result,
            playback_me_backend_name(), PSP_ME_RENDER, PSP_ME_DMA, PSP_ME_CACHE, PSP_ME_FULL_CACHE, PSP_ME_RENDER_QUEUE);
    fclose(out);
    playback_diagnostic("begin", 0);
}
#endif
