#ifndef WILI_RESULTS_H
#define WILI_RESULTS_H
/* Bookkeeping for results/run-NNN folders. Pure functions, no I/O, so the rules can be
 * tested on the host; decoder/src/player.c does the directory calls.
 *
 * Folders are named run-001, run-002, ... (3 or more digits). The next number is the
 * highest existing one plus one, so removing old folders never reuses a number.
 * FAT memory sticks list short names in uppercase (RUN-001), so matching ignores case. */
#include <stddef.h>

/* Newest runs kept (including the one being created). 0 keeps every run. Diagnostic and
 * profiling runs are never pruned; see decoder/src/player.c. */
#ifndef WILI_RESULTS_KEEP
#define WILI_RESULTS_KEEP 20
#endif
/* Old folders deleted per playback, so the first run after an upgrade does not spend a
 * long time deleting hundreds of folders on the Memory Stick before the video starts. */
#ifndef WILI_RESULTS_PRUNE_STEP
#define WILI_RESULTS_PRUNE_STEP 8
#endif
#define WILI_RUN_INDEX_MAX 999999

/* "run-" (any case) followed by 3 to 6 digits and nothing else, otherwise -1. Returns 1..999999. */
static inline int wili_run_index(const char *name) {
    if (!name || (name[0] | 0x20) != 'r' || (name[1] | 0x20) != 'u' || (name[2] | 0x20) != 'n' || name[3] != '-')
        return -1;
    int digits = 0, value = 0;
    for (const char *p = name + 4; *p; ++p, ++digits) {
        if (*p < '0' || *p > '9' || digits >= 6) return -1;
        value = value * 10 + (*p - '0');
    }
    return digits >= 3 && value >= 1 ? value : -1;
}

/* Given the existing run numbers, picks the number for the new run and which old runs to
 * delete so that at most `keep` remain after it is created. At most `drop_cap` folders
 * (the lowest numbers) are returned per call. Returns the drop count, or -1 when the
 * numbering is exhausted. */
static inline int wili_run_plan(const int *indices, int count, int keep,
                                int *next_index, int *drop, int drop_cap) {
    int highest = 0;
    for (int i = 0; i < count; ++i)
        if (indices[i] > highest) highest = indices[i];
    if (highest >= WILI_RUN_INDEX_MAX) return -1;
    *next_index = highest + 1;
    if (keep <= 0 || count < keep || drop_cap <= 0) return 0;
    int wanted = count - (keep - 1);
    if (wanted > drop_cap) wanted = drop_cap;
    int dropped = 0;
    int floor_value = 0; /* every chosen index is above this, so each pass picks the next lowest */
    while (dropped < wanted) {
        int lowest = 0;
        for (int i = 0; i < count; ++i)
            if (indices[i] > floor_value && (lowest == 0 || indices[i] < lowest)) lowest = indices[i];
        if (lowest == 0) break;
        drop[dropped++] = lowest;
        floor_value = lowest;
    }
    return dropped;
}

#endif
