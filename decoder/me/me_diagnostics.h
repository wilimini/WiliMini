#ifndef WILI_ME_DIAGNOSTICS_H
#define WILI_ME_DIAGNOSTICS_H
#include <stdint.h>

#define ME_DIAGNOSTICS_VERSION 1
#define ME_DIAGNOSTICS_CAPACITY 24
enum MeDiagnosticEvent {
    ME_DIAG_INIT = 1, ME_DIAG_PROFILE, ME_DIAG_BOOTSTRAP,
    ME_DIAG_BACKEND, ME_DIAG_DMA_BEGIN, ME_DIAG_DMA_END,
    ME_DIAG_INJECT_BEGIN, ME_DIAG_READY, ME_DIAG_SUBMIT,
    ME_DIAG_TRIGGER, ME_DIAG_WAIT_TIMEOUT, ME_DIAG_WAIT_DONE,
    ME_DIAG_SHUTDOWN, ME_DIAG_RESTORE_BEGIN, ME_DIAG_RESTORED,
    ME_DIAG_CORE_BOOT, ME_DIAG_CORE_READY, ME_DIAG_CORE_PROBE, ME_DIAG_CORE_STOP
};
/* a/b/c are event-specific values, described in decoder/README.md.
 * Fixed storage, no allocation, file I/O or ME-side logging. */
typedef struct {
    uint32_t sequence, time_us, event;
    int32_t result;
    uint32_t a, b, c;
} MeDiagnosticRecord;
typedef struct {
    uint32_t version, sequence, ready, pending;
    int32_t table;
    char build[96];
    MeDiagnosticRecord records[ME_DIAGNOSTICS_CAPACITY];
} MeDiagnostics;
int meLabGetDiagnostics(MeDiagnostics *out, unsigned size);
#endif
