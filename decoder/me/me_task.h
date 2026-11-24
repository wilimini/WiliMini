#ifndef PSP_LAB_ME_TASK_H
#define PSP_LAB_ME_TASK_H
#include "me_bridge.h"
#include <me-core-mapper/me-core-mapper.h>
#include "full_render.h"
#if PSP_VME_RENDER || PSP_VME_QPEL
#include "vme_render.h"
#endif
static YuvRgbTables me_tables __attribute__((aligned(64)));
/* Shared C worker for MIST and Custom Core. MIST uses me_entry.S to return to
 * firmware; Custom Core calls this worker directly. Task data use an uncached
 * alias outside the ME's local EDRAM. */
int me_task(int index, void *param);
int __attribute__((noinline)) me_worker(int index, void *param)
{
    (void)index;
    MeJob *j = (void *)((unsigned)param | 0x20000000u);
    j->value = 1;
#if PSP_VME_RENDER || PSP_VME_QPEL
    if (j->operation == 0) {
        vme_me_wipe();
        j->value = 0x4d450001;
        j->done = 1;
        __asm__ volatile("sync" ::: "memory");
        return 0;
    }
#endif
    if (j->operation == 1) {
        const uint8_t *data[3];
#if PSP_ME_FULL_CACHE
        /* Input and LUT cache lines are read-only on ME. Write back its own
         * dirty stack/output lines before invalidating the complete cache. */
        meCoreDcacheWritebackInvalidateAll();
#endif
        for (int p = 0; p < 3; ++p) {
            data[p] = (void *)((unsigned)j->planes[p] | 0x80000000u);
#if !PSP_ME_FULL_CACHE
            meCoreDcacheInvalidateRange((void *)data[p], j->strides[p] * (p ? (j->height + 1) / 2 : j->height));
#endif
            j->value = 2 + p;
        }
#if !PSP_ME_FULL_CACHE
        meCoreDcacheInvalidateRange(&me_tables, sizeof(me_tables));
#endif
        j->value = 5;
        int used_vme_dma = 0;
        void *out = (void *)((unsigned)j->output | 0x80000000u);
        if (j->output_format == 0) {
            if (j->width == 640 && j->height == 360) {
#if PSP_VME_RENDER
                if (*(volatile int *)((uintptr_t)&me_vme_wiped | 0xA0000000u)) {
                    vme_render_565(data, j->strides, j->output);
                    used_vme_dma = 1;
                } else {
                    full_render_planes_565(data, j->strides, out, &me_tables);
                }
#else
                full_render_planes_565(data, j->strides, out, &me_tables);
#endif
            }
            else
                full_render_scaled_565(data, j->strides, j->width, j->height, out, &me_tables);
        } else if (j->width == 640 && j->height == 360) {
            full_render_planes_8888(data, j->strides, out, &me_tables);
        } else {
            full_render_scaled_8888(data, j->strides, j->width, j->height, out, &me_tables);
        }
        j->value = 6;
        if (!used_vme_dma) {
#if PSP_ME_FULL_CACHE
            meCoreDcacheWritebackInvalidateAll();
#else
            meCoreDcacheWritebackRange(out, 512 * 272 * (j->output_format == 0 ? 2 : 4));
#endif
        }
    }
#if PSP_VME_QPEL
    else if (j->operation == 2) {
        if (*(volatile int *)((uintptr_t)&me_vme_wiped | 0xA0000000u)) {
            VmeHalfpelJob *hj = (VmeHalfpelJob *)((uintptr_t)j->output | 0xA0000000u);
            const uint8_t *y_src = (const uint8_t *)((uintptr_t)j->planes[0] | 0x80000000u);
            vme_build_halfpel_640(y_src, j->strides[0], j->height, hj->p20, hj->p02, hj->p22);
            j->value = 0x4d450002;
            j->done = 1;
            __asm__ volatile("sync" ::: "memory");
            return 0;
        }
    }
#endif
    j->value = 0x4d450001;
    j->done = 1;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}
#endif
