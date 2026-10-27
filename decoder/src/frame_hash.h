#ifndef PSP_LAB_FRAME_HASH_H
#define PSP_LAB_FRAME_HASH_H

#include <stdint.h>

static inline uint32_t hash_visible_plane(uint32_t hash, const uint8_t *data,
                                          int width, int height, int stride)
{
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = data + y * stride;
        for (int x = 0; x < width; ++x)
            hash = (hash ^ row[x]) * 16777619u;
    }
    return hash;
}

#endif
