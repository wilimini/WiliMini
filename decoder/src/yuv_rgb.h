#ifndef PSP_LAB_YUV_RGB_H
#define PSP_LAB_YUV_RGB_H

#include <stdint.h>

typedef struct {
    int y[256];
    int u_green[256];
    int u_blue[256];
    int v_red[256];
    int v_green[256];
#if PSP_CLIP_LUT
    uint8_t clip[1024];
#endif
} YuvRgbTables;

static inline void yuv_rgb_init(YuvRgbTables *tables)
{
    for (int i = 0; i < 256; ++i) {
        tables->y[i] = 298 * (i - 16) + 128;
        tables->u_green[i] = -55 * (i - 128);
        tables->u_blue[i] = 541 * (i - 128);
        tables->v_red[i] = 459 * (i - 128);
        tables->v_green[i] = -136 * (i - 128);
    }
#if PSP_CLIP_LUT
    for (int i = 0; i < 1024; ++i) {
        int value = i - 384;
        tables->clip[i] = value < 0 ? 0 : value > 255 ? 255 : value;
    }
#endif
}

static inline int yuv_rgb_clamp(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

static inline uint32_t yuv_rgb_pixel_offsets(const YuvRgbTables *tables,
                                             int luma, int red, int green, int blue)
{
#if PSP_CLIP_LUT
    int r = tables->clip[((luma + red) >> 8) + 384];
    int g = tables->clip[((luma + green) >> 8) + 384];
    int b = tables->clip[((luma + blue) >> 8) + 384];
#else
    (void)tables;
    int r = yuv_rgb_clamp((luma + red) >> 8);
    int g = yuv_rgb_clamp((luma + green) >> 8);
    int b = yuv_rgb_clamp((luma + blue) >> 8);
#endif
    return 0xff000000u | ((uint32_t)b << 16) |
           ((uint32_t)g << 8) | (uint32_t)r;
}

static inline uint32_t yuv_rgb_pixel(const YuvRgbTables *tables,
                                     uint8_t y, uint8_t u, uint8_t v)
{
    return yuv_rgb_pixel_offsets(tables, tables->y[y], tables->v_red[v],
                                 tables->u_green[u] + tables->v_green[v],
                                 tables->u_blue[u]);
}

#endif
