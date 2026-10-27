#ifndef PSP_LAB_SCALE_MAP_H
#define PSP_LAB_SCALE_MAP_H

#include <stdint.h>

typedef struct {
    uint16_t luma_x[480];
    uint16_t chroma_x[480];
    uint16_t source_y[270];
} ScaleMap;

static inline void scale_map_init(ScaleMap *map)
{
    for (int x = 0; x < 480; ++x) {
        map->luma_x[x] = x * 640 / 480;
        map->chroma_x[x] = map->luma_x[x] / 2;
    }
    for (int y = 0; y < 270; ++y)
        map->source_y[y] = y * 360 / 270;
}

#endif
