#ifndef PSP_ME_BRIDGE_H
#define PSP_ME_BRIDGE_H
#include <stdint.h>
typedef struct __attribute__((aligned(64))) {
    uint32_t operation;
    const uint8_t *planes[3];
    int strides[3];
    uint32_t *output;
    volatile uint32_t done;
    volatile uint32_t value;
    int width, height;
} MeJob;
/* Execution backend implemented by the loaded bridge PRX. The loader selects
 * exactly one bridge before the client starts; the values are fixed because
 * client/preferences.hpp and client/me_backend_select.h use the same numbers. */
#define ME_BACKEND_MIST 0
#define ME_BACKEND_CUSTOM_CORE 1
int meLabBackend(void);
/* Returns the selected ME_CORE_* image table (>= 0), or a negative error.
 * Each client PRX must select its own mapper copy before submitting ME work.
 * entry is the MIST trampoline when meLabBackend() is ME_BACKEND_MIST, or the
 * returning C worker for ME_BACKEND_CUSTOM_CORE. */
int meLabInit(uint32_t entry);
int meLabSubmit(MeJob *job);
int meLabWait(void);
int meLabShutdown(void);
#endif
