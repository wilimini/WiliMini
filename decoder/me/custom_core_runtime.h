#ifndef WILI_CUSTOM_CORE_RUNTIME_H
#define WILI_CUSTOM_CORE_RUNTIME_H
#include <stdint.h>
#include "me_bridge.h"
enum { CORE_IDLE, CORE_JOB, CORE_STOP };
enum { CORE_STARTING, CORE_READY, CORE_RUNNING, CORE_STOPPED };
typedef struct { unsigned command, state; int result; } CustomCoreStatus;
typedef struct { unsigned image_bytes, reset, ram, high, low; } CustomCoreBoot;
static inline uintptr_t customCoreUncachedAddress(uintptr_t address)
{
    /* Kernel modules may be relocated to a physical or cached kernel address. */
    return (address & 0x1fffffffu) | 0xa0000000u;
}
int customCoreStart(uint32_t entry, CustomCoreBoot *boot);
void customCoreSubmit(MeJob *job);
void customCoreStop(void);
CustomCoreStatus customCoreStatus(void);
void customCoreRestore(void);
#endif
