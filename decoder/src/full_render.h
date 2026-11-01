#ifndef PSP_LAB_FULL_RENDER_H
#define PSP_LAB_FULL_RENDER_H
#include "yuv_rgb.h"

#define DEFINE_FULL_RENDER(SUFFIX, TYPE, PIXEL, PIXEL_OFFSETS, CLEAR) \
static void full_render_planes_##SUFFIX(const uint8_t *const data[3], const int stride[3], \
                                        TYPE *dst, const YuvRgbTables *t) \
{ \
    for (int y = 0; y < 270; y += 3) { \
        int sy = y * 4 / 3; \
        for (int row = 0; row < 3; ++row) { \
            const uint8_t *yl = data[0] + (sy + row) * stride[0]; \
            const uint8_t *ul = data[1] + ((sy + row) / 2) * stride[1]; \
            const uint8_t *vl = data[2] + ((sy + row) / 2) * stride[2]; \
            TYPE *out = dst + (y + row + 1) * 512; \
            for (int group = 0; group < 160; ++group) { \
                int sx = group * 4, dx = group * 3, cx = group * 2; \
                int u = ul[cx], v = vl[cx]; \
                int r = t->v_red[v], g = t->u_green[u] + t->v_green[v], b = t->u_blue[u]; \
                out[dx] = PIXEL_OFFSETS(t, t->y[yl[sx]], r, g, b); \
                out[dx + 1] = PIXEL_OFFSETS(t, t->y[yl[sx + 1]], r, g, b); \
                out[dx + 2] = PIXEL(t, yl[sx + 2], ul[cx + 1], vl[cx + 1]); \
            } \
        } \
    } \
} \
static void full_render_scaled_##SUFFIX(const uint8_t *const data[3], const int stride[3], \
                                        int width, int height, TYPE *dst, const YuvRgbTables *t) \
{ \
    int dw = 480, dh = (int)((int64_t)height * 480 / width); \
    if (dh > 270) { dh = 270; dw = (int)((int64_t)width * 270 / height); } \
    if (dw < 1) dw = 1; \
    if (dh < 1) dh = 1; \
    int left = (480 - dw) / 2, top = 1 + (270 - dh) / 2; \
    for (int y = 0; y < 272; ++y) \
        for (int x = 0; x < 512; ++x) dst[y * 512 + x] = (CLEAR); \
    int sxmap[480]; \
    for (int x = 0; x < dw; ++x) sxmap[x] = x * width / dw; \
    for (int y = 0; y < dh; ++y) { \
        int sy = y * height / dh; \
        const uint8_t *yl = data[0] + sy * stride[0]; \
        const uint8_t *ul = data[1] + (sy / 2) * stride[1]; \
        const uint8_t *vl = data[2] + (sy / 2) * stride[2]; \
        for (int x = 0; x < dw; ++x) { \
            int sx = sxmap[x]; \
            dst[(top + y) * 512 + left + x] = PIXEL(t, yl[sx], ul[sx / 2], vl[sx / 2]); \
        } \
    } \
}

DEFINE_FULL_RENDER(565, uint16_t, yuv_rgb565_pixel, yuv_rgb565_pixel_offsets, 0)
DEFINE_FULL_RENDER(8888, uint32_t, yuv_rgb_pixel, yuv_rgb_pixel_offsets, 0xff000000u)

static void full_render_planes(const uint8_t *const data[3], const int stride[3],
                               uint16_t *dst, const YuvRgbTables *t)
{
    full_render_planes_565(data, stride, dst, t);
}

static void full_render_scaled(const uint8_t *const data[3], const int stride[3],
                               int width, int height, uint16_t *dst, const YuvRgbTables *t)
{
    full_render_scaled_565(data, stride, width, height, dst, t);
}
#undef DEFINE_FULL_RENDER
#endif
