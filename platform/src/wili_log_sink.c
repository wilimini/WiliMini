/* Streaming file sinks for the log ring; see wili_log.h. Never logs from the writer
 * thread (no recursion, no lock cycles). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "wili_log.h"
#include "wili_log_internal.h"

#include <stdio.h>
#include <string.h>

#ifdef __PSP__
#include <pspkernel.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* Longest wait before pending lines are written (an ERROR line skips it), and how often an
 * idle writer wakes to notice a stop or a switched-off setting. */
#ifndef WILI_LOG_SINK_FLUSH_MS
#define WILI_LOG_SINK_FLUSH_MS 1000
#endif
#ifndef WILI_LOG_SINK_IDLE_MS
#define WILI_LOG_SINK_IDLE_MS 2000
#endif
#define WILI_LOG_SINK_STEP_MS 50
#define WILI_LOG_SINK_PATH_MAX 256

#if WILI_LOG_RING_LINES > 0

typedef struct {
    FILE *file;
    char path[WILI_LOG_SINK_PATH_MAX];
    unsigned next;       /* next ring sequence to write */
    unsigned bytes;      /* size of the current file */
    unsigned max_bytes;  /* rotate at this size; 0 = never */
    unsigned lost;       /* lines overwritten before they could be written */
} SinkSlot;

static SinkSlot slots[WILI_LOG_SINK_COUNT];
static volatile int stop_requested;
static int thread_running;

/* ---- platform: wake-up, sink lock, delay, thread ---- */
#ifdef __PSP__
static SceUID wake_sema = -1, sink_lock = -1, thread_id = -1;
static int platform_create(void) {
    wake_sema = sceKernelCreateSema("wili_log_wake", 0, 0, 1, NULL);
    sink_lock = sceKernelCreateSema("wili_log_sink", 0, 1, 1, NULL);
    return wake_sema >= 0 && sink_lock >= 0 ? 0 : -1;
}
static void platform_destroy(void) {
    if (wake_sema >= 0) sceKernelDeleteSema(wake_sema);
    if (sink_lock >= 0) sceKernelDeleteSema(sink_lock);
    wake_sema = sink_lock = -1;
}
static void lock_take(void) { sceKernelWaitSema(sink_lock, 1, NULL); }
static void lock_give(void) { sceKernelSignalSema(sink_lock, 1); }
static void wake_signal(void) { if (wake_sema >= 0) sceKernelSignalSema(wake_sema, 1); } /* full count is fine */
static void wake_wait(unsigned ms) { SceUInt timeout = ms * 1000u; sceKernelWaitSema(wake_sema, 1, &timeout); }
static void delay_ms(unsigned ms) { sceKernelDelayThread(ms * 1000u); }
static int sink_main(SceSize args, void *argp);
static int thread_start(void) {
    /* Low priority (larger number than the video and UI threads) and a roomy stack for stdio. */
    thread_id = sceKernelCreateThread("wili_log_sink", sink_main, 0x40, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (thread_id < 0) return -1;
    if (sceKernelStartThread(thread_id, 0, NULL) < 0) {
        sceKernelDeleteThread(thread_id);
        thread_id = -1;
        return -1;
    }
    return 0;
}
static void thread_join(void) {
    SceUInt timeout = 3000000u; /* a wedged write must not hang the exit */
    if (sceKernelWaitThreadEnd(thread_id, &timeout) < 0) sceKernelTerminateDeleteThread(thread_id);
    else sceKernelDeleteThread(thread_id);
    thread_id = -1;
}
#else
static pthread_mutex_t sink_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cond = PTHREAD_COND_INITIALIZER;
static int wake_flag;
static pthread_t thread_handle;
static int platform_create(void) { return 0; }
static void platform_destroy(void) {}
static void lock_take(void) { pthread_mutex_lock(&sink_mutex); }
static void lock_give(void) { pthread_mutex_unlock(&sink_mutex); }
static void wake_signal(void) {
    pthread_mutex_lock(&wake_mutex);
    wake_flag = 1;
    pthread_cond_signal(&wake_cond);
    pthread_mutex_unlock(&wake_mutex);
}
static void wake_wait(unsigned ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000u;
    until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&wake_mutex);
    while (!wake_flag && pthread_cond_timedwait(&wake_cond, &wake_mutex, &until) == 0) {}
    wake_flag = 0;
    pthread_mutex_unlock(&wake_mutex);
}
static void delay_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&t, NULL);
}
static void *sink_main(void *arg);
static int thread_start(void) { return pthread_create(&thread_handle, NULL, sink_main, NULL) == 0 ? 0 : -1; }
static void thread_join(void) { pthread_join(thread_handle, NULL); }
#endif

/* ---- slots (callers hold the sink lock) ---- */
static void slot_close(SinkSlot *s) {
    if (s->file) fclose(s->file);
    s->file = NULL;
}

static void slot_rotate(SinkSlot *s) {
    char old[WILI_LOG_SINK_PATH_MAX + 3];
    slot_close(s);
    snprintf(old, sizeof(old), "%s.1", s->path);
    remove(old); /* a Memory Stick cannot rename over an existing file */
    if (rename(s->path, old)) remove(s->path);
    s->file = fopen(s->path, "w");
    s->bytes = 0;
}

static void slot_drain(SinkSlot *s) {
    if (!s->file) return;
    char line[WILI_LOG_LINE_MAX];
    int wrote = 0, failed = 0;
    while (!failed && wili_log_ring_next(&s->next, line, &s->lost)) {
        if (s->lost) {
            char note[48];
            const int n = snprintf(note, sizeof(note), "log_lines_dropped=%u\n", s->lost);
            s->lost = 0;
            if (fwrite(note, 1, (size_t)n, s->file) != (size_t)n) failed = 1;
            s->bytes += (unsigned)n;
        }
        const size_t length = strlen(line);
        if (fwrite(line, 1, length, s->file) != length) failed = 1;
        s->bytes += (unsigned)length;
        wrote = 1;
    }
    if (wrote && !failed && fflush(s->file)) failed = 1;
    if (failed) { slot_close(s); return; }       /* disk full or card removed: stop writing */
    if (s->max_bytes && s->bytes >= s->max_bytes) slot_rotate(s);
}

static void drain_all(void) {
    for (int i = 0; i < WILI_LOG_SINK_COUNT; ++i) slot_drain(&slots[i]);
}

static void close_all(void) {
    for (int i = 0; i < WILI_LOG_SINK_COUNT; ++i) {
        slot_drain(&slots[i]);
        slot_close(&slots[i]);
    }
}

static unsigned backlog(void) {
    unsigned worst = 0;
    const unsigned total = wili_log_ring_total();
    lock_take(); /* slots change under this lock when a sink is started or stopped */
    for (int i = 0; i < WILI_LOG_SINK_COUNT; ++i)
        if (slots[i].file && total - slots[i].next > worst) worst = total - slots[i].next;
    lock_give();
    return worst;
}

#ifdef __PSP__
static int sink_main(SceSize args, void *argp) {
    (void)args; (void)argp;
#else
static void *sink_main(void *arg) {
    (void)arg;
#endif
    while (!WILI_LOAD(&stop_requested)) {
        wake_wait(WILI_LOG_SINK_IDLE_MS);
        if (WILI_LOAD(&stop_requested)) break;
        /* Coalesce lines into one write, but never let the ring overflow while waiting. */
        for (unsigned waited = 0; waited < WILI_LOG_SINK_FLUSH_MS && !WILI_LOAD(&wili_log_urgent) &&
                                  !WILI_LOAD(&stop_requested) && backlog() < WILI_LOG_RING_LINES / 2;
             waited += WILI_LOG_SINK_STEP_MS)
            delay_ms(WILI_LOG_SINK_STEP_MS);
        WILI_STORE(&wili_log_urgent, 0);
        lock_take();
        if (wili_log_files()) drain_all();
        else close_all(); /* the setting was switched off */
        lock_give();
    }
    lock_take();
    close_all();
    lock_give();
#ifdef __PSP__
    return 0;
#else
    return NULL;
#endif
}

static int ensure_thread(void) {
    if (thread_running) return 0;
    if (platform_create() < 0) { platform_destroy(); return -1; }
    WILI_STORE(&stop_requested, 0);
    if (thread_start() < 0) { platform_destroy(); return -1; }
    thread_running = 1;
    WILI_STORE(&wili_log_notify, wake_signal);
    return 0;
}

int wili_log_sink_start(WiliLogSink sink, const char *path, unsigned from_mark, unsigned max_bytes) {
    if (!wili_log_files()) return -2;
    if ((unsigned)sink >= WILI_LOG_SINK_COUNT || !path || strlen(path) >= WILI_LOG_SINK_PATH_MAX) return -1;
    if (ensure_thread() < 0) return -1;
    SinkSlot *s = &slots[sink];
    lock_take();
    slot_drain(s);
    slot_close(s);
    s->file = fopen(path, max_bytes ? "a" : "w");
    if (!s->file) {
        lock_give();
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->next = from_mark;
    s->lost = 0;
    s->max_bytes = max_bytes;
    s->bytes = 0;
    if (max_bytes) {
        fseek(s->file, 0, SEEK_END);
        const long size = ftell(s->file);
        s->bytes = size > 0 ? (unsigned)size : 0;
        if (s->bytes >= max_bytes) slot_rotate(s);
        if (!s->file) {
            lock_give();
            return -1;
        }
    }
    lock_give();
    wake_signal();
    return 0;
}

void wili_log_sink_stop(WiliLogSink sink) {
    if ((unsigned)sink >= WILI_LOG_SINK_COUNT || !thread_running) return;
    lock_take();
    slot_drain(&slots[sink]);
    slot_close(&slots[sink]);
    lock_give();
}

int wili_log_sink_active(WiliLogSink sink) {
    if ((unsigned)sink >= WILI_LOG_SINK_COUNT || !thread_running) return 0;
    lock_take();
    const int active = slots[sink].file != NULL;
    lock_give();
    return active;
}

void wili_log_sink_shutdown(void) {
    if (!thread_running) return;
    WILI_STORE(&wili_log_notify, (void (*)(void))NULL);
    WILI_STORE(&stop_requested, 1);
    wake_signal();
    thread_join();
    platform_destroy();
    thread_running = 0;
}

#else /* no ring buffer (the loader): nothing to stream from */

int wili_log_sink_start(WiliLogSink sink, const char *path, unsigned from_mark, unsigned max_bytes) {
    (void)sink; (void)path; (void)from_mark; (void)max_bytes;
    return -1;
}
void wili_log_sink_stop(WiliLogSink sink) { (void)sink; }
int wili_log_sink_active(WiliLogSink sink) { (void)sink; return 0; }
void wili_log_sink_shutdown(void) {}

#endif
