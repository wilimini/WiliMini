#ifndef WILI_LOG_H
#define WILI_LOG_H
/* Leveled, per-channel logging for the loader, client and decoder (C and C++).
 *
 *   WLOG_E(WLC_NET, "http %d", code);
 *
 * Every line is "[sec.ms] L CHANNEL message". Keeping the channel name as the
 * first word of the message preserves the old "NET ...", "MEM ..." prefixes that
 * tools/ and docs/ match on.
 *
 * Cost: a disabled call is two compares and no formatting. Calls above
 * WILI_LOG_MAX_LEVEL are removed at compile time, so production builds can
 * strip VERBOSE while diagnostic builds define a higher maximum.
 *
 * Sinks: console (stdout, visible in PSPLink) and an in-RAM ring of the most
 * recent lines (WILI_LOG_RING_LINES, 0 disables it) that wili_log_dump() writes
 * into last-error.txt or diagnostics. Formatting happens on the caller's stack;
 * only the ring copy takes a lock, and no call allocates or touches the Memory
 * Stick, so it is safe on render and playback threads. */
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WILI_LOG_OFF = 0,
    WILI_LOG_ERROR,
    WILI_LOG_WARN,
    WILI_LOG_INFO,
    WILI_LOG_DEBUG,
    WILI_LOG_VERBOSE
} WiliLogLevel;

/* Channel names double as the line prefix; keep wili_log_channel_name() in sync. */
typedef enum {
    WLC_CLIENT = 0,
    WLC_LOADER,
    WLC_NET,
    WLC_AUTH,
    WLC_UI,
    WLC_PLAYER,
    WLC_ME,
    WLC_MEM,
    WLC_IO,
    WLC_COUNT
} WiliLogChannel;

#ifndef WILI_LOG_MAX_LEVEL
#define WILI_LOG_MAX_LEVEL WILI_LOG_DEBUG
#endif

extern unsigned char wili_log_levels[WLC_COUNT];

#if defined(__GNUC__)
#define WILI_LOG_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define WILI_LOG_PRINTF(a, b)
#endif

void wili_log_write(WiliLogChannel channel, WiliLogLevel level, const char *format, ...)
    WILI_LOG_PRINTF(3, 4);

/* Call once from the main thread before other threads log. Without it logging
 * still works; ring access is simply unlocked. */
void wili_log_init(void);
void wili_log_shutdown(void);

const char *wili_log_channel_name(WiliLogChannel channel);
void wili_log_set_level(WiliLogChannel channel, WiliLogLevel level);
void wili_log_set_all(WiliLogLevel level);
/* Parses "error|warn|info|debug|verbose|off" (case-insensitive). Returns 0 on success. */
int wili_log_parse_level(const char *text, WiliLogLevel *level);
/* Applies a spec such as "info,net=debug,player=verbose". A bare level sets every
 * channel, "name=level" one channel. Unknown items are skipped. Returns the number
 * of items applied, or -1 on a null spec. */
int wili_log_configure(const char *spec);

/* Writes the buffered lines, oldest first. Returns the number written. */
int wili_log_dump(FILE *out);
/* Number of lines currently held, and the capacity (0 when the ring is disabled). */
int wili_log_ring_count(void);
int wili_log_ring_capacity(void);

#define WLOG(channel, level, ...)                                              \
    do {                                                                       \
        if ((level) <= WILI_LOG_MAX_LEVEL && (level) <= wili_log_levels[channel]) \
            wili_log_write((channel), (level), __VA_ARGS__);                   \
    } while (0)

#define WLOG_E(channel, ...) WLOG(channel, WILI_LOG_ERROR, __VA_ARGS__)
#define WLOG_W(channel, ...) WLOG(channel, WILI_LOG_WARN, __VA_ARGS__)
#define WLOG_I(channel, ...) WLOG(channel, WILI_LOG_INFO, __VA_ARGS__)
#define WLOG_D(channel, ...) WLOG(channel, WILI_LOG_DEBUG, __VA_ARGS__)
#define WLOG_V(channel, ...) WLOG(channel, WILI_LOG_VERBOSE, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
