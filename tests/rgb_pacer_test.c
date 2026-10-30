// Exercise the production queue with host threads and injected PSP failures.
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
#define PSP_AUDIO 1
#define PSP_RGB_PACE 1
#define PSP_RGB_NEAREST_PTS 1
#define PSP_THREAD_ATTR_USER 0
#define AV_NOPTS_VALUE INT64_MIN
#define AVERROR(e) (-(e))
#define WARMUP_FRAMES 30
static volatile int quit_requested;
static int failure, live_allocations, semaphores, thread_created, thread_started;
static size_t allocated_bytes;
static int (*worker_entry)(SceSize, void*);
static pthread_t worker;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int counts[3], active[3];

static void* queue_alloc(size_t alignment, size_t bytes) {
    if (failure == 1) return NULL;
    void* ptr = NULL;
    assert(!posix_memalign(&ptr, alignment, bytes));
    ++live_allocations;
    allocated_bytes = bytes;
    return ptr;
}
static void queue_free(void* ptr) {
    if (ptr) { --live_allocations; free(ptr); }
}
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
    (void)timeout;
    pthread_mutex_lock(&mutex);
    while (counts[id] < count) pthread_cond_wait(&changed, &mutex);
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
static uint64_t sceKernelGetSystemTimeWide(void) { return 1000000000; }
static int64_t audio_clock_us(void) { return 1000000000; }
static int audio_playback_failed(void) { return 0; }
static int sceDmacMemcpy(void* dst, const void* src, size_t bytes) {
    (void)dst; (void)src; assert(bytes == 512 * 270 * 4); return 0;
}
static void sceKernelDcacheWritebackInvalidateRange(void* ptr, size_t bytes) {
    assert((uintptr_t)ptr % 64 == 0 && bytes == allocated_bytes);
}
static const void *presenter;
static int playback_seeking;
static int playback_is_paused(void) { return 0; }
static int present_video(const uint32_t *pixels) { (void)pixels; return 0; }
#define memalign queue_alloc
#define free queue_free
#include "../decoder/src/rgb_pacer.h"
#undef free

int main(void) {
    for (failure = 1; failure <= 4; ++failure) {
        assert(rgb_start() < 0);
        rgb_close(); rgb_release();
        assert(!rgb_pixels && !live_allocations && !semaphores && !thread_created);
    }
    failure = 0;
    for (int repeat = 0; repeat < 3; ++repeat) for (unsigned slots = 2; slots <= 8; slots *= 2) {
        rgb_slots = slots;
        assert(rgb_start() == 0);
        assert(allocated_bytes == slots * 512 * 272 * 4);
        assert(rgb_start() == AVERROR(EBUSY));
        rgb_startup_ready = 1;
        for (int frame = 0; frame < 32; ++frame) {
            const int slot = rgb_acquire();
            assert(slot >= 0 && (unsigned)slot < slots);
            rgb_pixels[slot][0] = frame;
            rgb_publish(frame);
        }
        rgb_close();
        assert(rgb_consumed == 32 && !semaphores && !thread_created);
        // Allocation survives worker shutdown until the ME owner releases it.
        assert(live_allocations == 1);
        rgb_release();
        assert(!rgb_pixels && !live_allocations);
    }
    puts("RGB queues 2/4/8: repeated playback, alignment and failure cleanup passed");
}
