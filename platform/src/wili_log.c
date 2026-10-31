#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "wili_log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef __PSP__
#include <pspkernel.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#ifndef WILI_LOG_RING_LINES
#define WILI_LOG_RING_LINES 64
#endif

unsigned char wili_log_levels[WLC_COUNT] = {
    WILI_LOG_INFO, WILI_LOG_INFO, WILI_LOG_INFO, WILI_LOG_INFO, WILI_LOG_INFO,
    WILI_LOG_INFO, WILI_LOG_INFO, WILI_LOG_INFO, WILI_LOG_INFO
};

static const char *const channel_names[WLC_COUNT] = {
    "CLIENT", "LOADER", "NET", "AUTH", "UI", "PLAYER", "ME", "MEM", "IO"
};
static const char *const level_names[] = {"off", "error", "warn", "info", "debug", "verbose"};
static const char level_letters[] = "-EWIDV";

#if WILI_LOG_RING_LINES > 0
static char ring[WILI_LOG_RING_LINES][WILI_LOG_LINE_MAX];
static unsigned ring_total; /* lines ever written; slot = total % capacity */
#endif

/* Locking. A binary semaphore matches the rest of the PSP code (see bilibili.c). */
#if defined(__GNUC__)
#define WILI_LOG_UNUSED __attribute__((unused))
#else
#define WILI_LOG_UNUSED
#endif
#ifdef __PSP__
static SceUID log_lock = -1;
static void lock_create(void) { log_lock = sceKernelCreateSema("wili_log", 0, 1, 1, NULL); }
static void lock_destroy(void) {
    if (log_lock >= 0) sceKernelDeleteSema(log_lock);
    log_lock = -1;
}
WILI_LOG_UNUSED static void lock_take(void) { if (log_lock >= 0) sceKernelWaitSema(log_lock, 1, NULL); }
WILI_LOG_UNUSED static void lock_give(void) { if (log_lock >= 0) sceKernelSignalSema(log_lock, 1); }
static unsigned long long now_us(void) { return (unsigned long long)sceKernelGetSystemTimeWide(); }
#else
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_create(void) {}
static void lock_destroy(void) {}
WILI_LOG_UNUSED static void lock_take(void) { pthread_mutex_lock(&log_lock); }
WILI_LOG_UNUSED static void lock_give(void) { pthread_mutex_unlock(&log_lock); }
static unsigned long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000u;
}
#endif

void wili_log_init(void) {
#ifdef __PSP__
    if (log_lock < 0) lock_create();
#else
    lock_create();
#endif
}

void wili_log_shutdown(void) { lock_destroy(); }

const char *wili_log_channel_name(WiliLogChannel channel) {
    return (unsigned)channel < WLC_COUNT ? channel_names[channel] : "?";
}

void wili_log_set_level(WiliLogChannel channel, WiliLogLevel level) {
    if ((unsigned)channel < WLC_COUNT && level <= WILI_LOG_VERBOSE) wili_log_levels[channel] = (unsigned char)level;
}

void wili_log_set_all(WiliLogLevel level) {
    for (int i = 0; i < WLC_COUNT; ++i) wili_log_set_level((WiliLogChannel)i, level);
}

static int equals_nocase(const char *a, size_t length, const char *b) {
    if (strlen(b) != length) return 0;
    for (size_t i = 0; i < length; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int parse_level_n(const char *text, size_t length, WiliLogLevel *level) {
    for (int i = 0; i <= WILI_LOG_VERBOSE; ++i) {
        if (equals_nocase(text, length, level_names[i])) {
            *level = (WiliLogLevel)i;
            return 0;
        }
    }
    return -1;
}

int wili_log_parse_level(const char *text, WiliLogLevel *level) {
    if (!text || !level) return -1;
    return parse_level_n(text, strlen(text), level);
}

int wili_log_configure(const char *spec) {
    if (!spec) return -1;
    int applied = 0;
    while (*spec) {
        while (*spec == ' ' || *spec == ',') ++spec;
        const char *end = spec;
        while (*end && *end != ',') ++end;
        size_t length = (size_t)(end - spec);
        while (length > 0 && spec[length - 1] == ' ') --length;
        const char *equals = memchr(spec, '=', length);
        WiliLogLevel level;
        if (!equals) {
            if (length && parse_level_n(spec, length, &level) == 0) {
                wili_log_set_all(level);
                ++applied;
            }
        } else if (parse_level_n(equals + 1, length - (size_t)(equals + 1 - spec), &level) == 0) {
            const size_t name_length = (size_t)(equals - spec);
            for (int i = 0; i < WLC_COUNT; ++i) {
                if (equals_nocase(spec, name_length, channel_names[i])) {
                    wili_log_set_level((WiliLogChannel)i, level);
                    ++applied;
                    break;
                }
            }
        }
        spec = end;
    }
    return applied;
}

void wili_log_write(WiliLogChannel channel, WiliLogLevel level, const char *format, ...) {
    if ((unsigned)channel >= WLC_COUNT || level < WILI_LOG_ERROR || level > WILI_LOG_VERBOSE) return;
    char line[WILI_LOG_LINE_MAX];
    const unsigned long long ms = now_us() / 1000u;
    int used = snprintf(line, sizeof(line), "[%4llu.%03u] %c %s ", ms / 1000u, (unsigned)(ms % 1000u),
                        level_letters[level], channel_names[channel]);
    if (used < 0 || (size_t)used >= sizeof(line) - 2) return;
    va_list args;
    va_start(args, format);
    const int body = vsnprintf(line + used, sizeof(line) - (size_t)used - 1, format, args);
    va_end(args);
    if (body < 0) return;
    used += body;
    /* Truncated lines keep room for the newline written below. */
    if ((size_t)used > sizeof(line) - 2) used = (int)sizeof(line) - 2;
    while (used > 0 && line[used - 1] == '\n') --used; /* callers may still pass a trailing newline */
    line[used] = '\n';
    line[used + 1] = '\0';

    fputs(line, stdout);
#if WILI_LOG_RING_LINES > 0
    lock_take();
    memcpy(ring[ring_total % WILI_LOG_RING_LINES], line, (size_t)used + 2);
    ++ring_total;
    lock_give();
#endif
}

int wili_log_ring_capacity(void) { return WILI_LOG_RING_LINES; }

int wili_log_ring_count(void) {
#if WILI_LOG_RING_LINES > 0
    return ring_total < WILI_LOG_RING_LINES ? (int)ring_total : WILI_LOG_RING_LINES;
#else
    return 0;
#endif
}

int wili_log_dump(FILE *out) {
#if WILI_LOG_RING_LINES > 0
    if (!out) return 0;
    lock_take();
    const unsigned count = ring_total < WILI_LOG_RING_LINES ? ring_total : WILI_LOG_RING_LINES;
    const unsigned first = ring_total - count;
    for (unsigned i = 0; i < count; ++i) fputs(ring[(first + i) % WILI_LOG_RING_LINES], out);
    lock_give();
    return (int)count;
#else
    (void)out;
    return 0;
#endif
}
