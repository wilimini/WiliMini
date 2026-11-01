#ifndef WILI_LOG_INTERNAL_H
#define WILI_LOG_INTERNAL_H
/* Shared by wili_log.c and wili_log_sink.c only. */
#include "wili_log.h"

/* Lines kept in RAM; 0 builds without a ring (the loader). Must be the same in every file of
 * the logger, so the default lives here. */
#ifndef WILI_LOG_RING_LINES
#define WILI_LOG_RING_LINES 64
#endif

/* Flags shared between logging threads, the writer and the UI. */
#define WILI_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define WILI_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Copies the line with sequence >= *seq into `line` (WILI_LOG_LINE_MAX bytes) and advances
 * *seq. Lines already overwritten are skipped and added to *lost. Returns 0 when no newer
 * line exists. The ring lock is held only while copying, never during file I/O. */
int wili_log_ring_next(unsigned *seq, char *line, unsigned *lost);
/* Sequence number the next logged line will get. */
unsigned wili_log_ring_total(void);
/* Set by wili_log_sink_start so a logged line wakes the writer; NULL while no sink exists. */
extern void (*wili_log_notify)(void);
/* Set when an ERROR line is logged; the writer flushes without waiting and clears it. */
extern volatile int wili_log_urgent;

#endif
