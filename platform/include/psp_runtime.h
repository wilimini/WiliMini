#ifndef PSP_RUNTIME_H
#define PSP_RUNTIME_H

#include <pspiofilemgr.h>

#ifndef BILI_PPSSPP_SCE_MPEG
#define BILI_PPSSPP_SCE_MPEG 0
#endif

typedef enum {
    PSP_RUNTIME_PSP = 0,
    PSP_RUNTIME_PPSSPP = 1
} PspRuntime;

static inline PspRuntime psp_runtime_detect(void)
{
    return sceIoDevctl("kemulator:", 0x00000003, NULL, 0, NULL, 0) == 0
        ? PSP_RUNTIME_PPSSPP
        : PSP_RUNTIME_PSP;
}

#endif
