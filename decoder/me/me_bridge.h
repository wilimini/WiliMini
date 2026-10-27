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
int meLabInit(uint32_t entry);
int meLabSubmit(MeJob *job);
int meLabWait(void);
int meLabShutdown(void);
#endif
