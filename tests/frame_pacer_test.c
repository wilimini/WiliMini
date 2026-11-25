// Exercise the production frame queue with host threads and injected PSP failures.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int SceUID;
typedef unsigned SceSize;
typedef unsigned SceUInt;
#define PSP_THREAD_ATTR_USER 0
#define AV_NOPTS_VALUE INT64_MIN
#define AVERROR(e) (-(e))
#define WARMUP_FRAMES 30

/* Minimal AVFrame: a reference count shared by the frames that refer to it. */
typedef struct { uint8_t *data[3]; int linesize[3]; int width, height; int *refs; } AVFrame;
static int live_frames, live_refs, failure;
static AVFrame *av_frame_alloc(void) {
    if (failure == 1) return NULL;
    ++live_frames;
    return calloc(1, sizeof(AVFrame));
}
static void av_frame_unref(AVFrame *frame) {
    if (frame->refs && --*frame->refs == 0) { free(frame->refs); --live_refs; }
    memset(frame, 0, sizeof(*frame));
}
static int av_frame_ref(AVFrame *dst, const AVFrame *src) {
    *dst = *src;
    ++*dst->refs;
    return 0;
}
static void av_frame_free(AVFrame **frame) {
    if (!*frame) return;
    av_frame_unref(*frame);
    free(*frame);
    *frame = NULL;
    --live_frames;
}
typedef enum { PSP_VIDEO_PIXEL_FORMAT_RGBA8888 = 1, PSP_VIDEO_PIXEL_FORMAT_YUV420P = 2 } PspVideoPixelFormat;
typedef struct { const uint8_t *planes[3]; int strides[3]; int width, height; } PspVideoYuvFrame;

static volatile int quit_requested;
static int semaphores, thread_created, thread_started;
static int (*worker_entry)(SceSize, void*);
static pthread_t worker;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int counts[3], active[3];

static int sceKernelCreateSema(const char* name, int attr, int count, int max, void* opt) {
    (void)name; (void)attr; (void)max; (void)opt;
    if (failure == 2) return -2;
    for (int i = 1; i <= 2; ++i) if (!active[i]) {
        active[i] = 1; counts[i] = count; ++semaphores; return i;
    }
    assert(0); return -1;
}
static int sceKernelDeleteSema(int id) {
    assert(active[id]); active[id] = 0; --semaphores; return 0;
}
static int sceKernelWaitSema(int id, int count, SceUInt* timeout) {
    pthread_mutex_lock(&mutex);
    if (timeout) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)*timeout * 1000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        while (counts[id] < count)
            if (pthread_cond_timedwait(&changed, &mutex, &until)) {
                pthread_mutex_unlock(&mutex);
                return (int)0x800201a8u; /* timeout */
            }
    } else {
        while (counts[id] < count) pthread_cond_wait(&changed, &mutex);
    }
    counts[id] -= count;
    pthread_mutex_unlock(&mutex);
    return 0;
}
static int sceKernelSignalSema(int id, int count) {
    pthread_mutex_lock(&mutex);
    counts[id] += count;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    return 0;
}
static void* run_worker(void* arg) { (void)arg; worker_entry(0, NULL); return NULL; }
static int sceKernelCreateThread(const char* name, int (*entry)(SceSize, void*), int priority,
                                 int stack, int attr, void* opt) {
    (void)name; (void)priority; (void)stack; (void)attr; (void)opt;
    if (failure == 3) return -3;
    worker_entry = entry; thread_created = 1; return 1;
}
static int sceKernelStartThread(int id, int bytes, void* arg) {
    (void)id; (void)bytes; (void)arg;
    if (failure == 4) return -4;
    assert(!pthread_create(&worker, NULL, run_worker, NULL)); thread_started = 1; return 0;
}
static int sceKernelWaitThreadEndCB(int id, void* opt) {
    (void)id; (void)opt;
    assert(thread_started); pthread_join(worker, NULL); thread_started = 0; return 0;
}
static int sceKernelDeleteThread(int id) { (void)id; thread_created = 0; return 0; }
static void sceKernelDelayThread(unsigned us) { usleep(us); }
static void sceKernelDelayThreadCB(unsigned us) { usleep(us); }
static uint64_t sceKernelGetSystemTimeWide(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}
static int64_t audio_clock_us(void) { return 1000000000; }
static int audio_playback_failed(void) { return 0; }
static int playback_seeking;
static int playback_is_paused(void) { return 0; }

static int presented_frames, last_presented;
static int present_video(const void *pixels, PspVideoPixelFormat format) {
    if (!pixels) return 0;
    assert(format == PSP_VIDEO_PIXEL_FORMAT_YUV420P);
    const PspVideoYuvFrame *frame = pixels;
    /* The test stores the frame number in width; presentation keeps order. */
    assert(frame->width > last_presented);
    last_presented = frame->width;
    ++presented_frames;
    return 0;
}
#include "../decoder/src/frame_pacer.h"

int main(void) {
    (void)presentation_pts_us;
    for (failure = 1; failure <= 4; ++failure) {
        assert(pacer_start() < 0);
        pacer_close(); pacer_release();
        assert(!live_frames && !semaphores && !thread_created);
    }
    failure = 0;
    for (int repeat = 0; repeat < 3; ++repeat) for (unsigned slots = 2; slots <= 8; slots *= 2) {
        pacer_slots = slots;
        presented_frames = last_presented = 0;
        assert(pacer_start() == 0);
        assert(live_frames == (int)slots);
        assert(pacer_start() == AVERROR(EBUSY));
        pacer_startup_ready = 1;
        for (int n = 1; n <= 32; ++n) {
            AVFrame decoded = {0};
            static uint8_t plane[16];
            decoded.data[0] = decoded.data[1] = decoded.data[2] = plane;
            decoded.linesize[0] = decoded.linesize[1] = decoded.linesize[2] = 16;
            decoded.width = n; decoded.height = 2;
            decoded.refs = malloc(sizeof(int)); *decoded.refs = 1; ++live_refs;
            assert(pacer_submit(&decoded, n) == 0);
            av_frame_unref(&decoded); /* the decoder drops its own reference */
        }
        assert(pacer_finish() == 0);
        // Every queued picture was released once the queue drained.
        assert(!live_refs);
        pacer_close();
        assert(pacer_consumed == 32 && !semaphores && !thread_created);
        assert(presented_frames + (int)pacer_dropped == 32);
        // Slots survive worker shutdown until the ME owner releases them.
        assert(live_frames == (int)slots);
        pacer_release();
        assert(!live_frames);
    }
    puts("Frame queues 2/4/8: presentation order, reference release and failure cleanup passed");
}
