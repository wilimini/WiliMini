#ifndef PSP_LAB_ME_TASK_H
#define PSP_LAB_ME_TASK_H
#include "me_bridge.h"
#include <me-core-mapper/me-core-mapper.h>
#include "full_render.h"
static YuvRgbTables me_tables __attribute__((aligned(64)));
/* MIST return convention from mcidclan/psp-media-engine-mist (MIT).
 * Task and data live in user RAM, outside the ME's local EDRAM alias. */
int me_task(int index, void *param);
int __attribute__((noinline)) me_worker(int index, void *param)
{
    (void)index;
    MeJob *j = (void *)((unsigned)param | 0x20000000u);
    j->value = 1;
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
        uint32_t *out = (void *)((unsigned)j->output | 0x80000000u);
        if (j->width == 640 && j->height == 360)
            full_render_planes(data, j->strides, out, &me_tables);
        else
            full_render_scaled(data, j->strides, j->width, j->height, out, &me_tables);
        j->value = 6;
#if PSP_ME_FULL_CACHE
        meCoreDcacheWritebackInvalidateAll();
#else
        meCoreDcacheWritebackRange(out, 512 * 272 * 4);
#endif
    }
    j->value = 0x4d450001;
    j->done = 1;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}
#endif
