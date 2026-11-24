/* ME half of the whole-frame H.264 pipeline: intra prediction, exact motion
 * compensation (6-tap qpel, packed chroma, emulated edges, weighted
 * prediction) and integer IDCT, run by a persistent worker on the Media
 * Engine. This file must stay free of VFPU code and libc calls: everything it
 * reaches runs on ME with its own caches. */
#include <stdint.h>
#include <stddef.h>
#include <me-core-mapper/me-core-mapper.h>

#include "me_bridge.h"
#include "mefp_shared.h"
#include "me_qpel_exact.h"

extern void __real_ff_h264qpel_init(H264QpelContext *, int);
extern void __real_ff_h264chroma_init(H264ChromaContext *, int);
extern void ff_h264_pred_init(H264PredContext *, int, int, int);
extern void ff_h264dsp_init(H264DSPContext *, int, int);

/* Pure-integer Allegrex DSP kernels (no VFPU, no libc calls) */

static inline uint8_t mefp_clip_u8(int a)
{
    return (uint8_t)meq_clip_u8(a);
}

static void mefp_put16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    if (((uintptr_t)dst | (uintptr_t)stride) & 3) {
        for (int r = 0; r < 16; ++r) {
            for (int c = 0; c < 16; ++c) dst[c] = src[c];
            dst += stride; src += stride;
        }
        return;
    }
    for (int r = 0; r < 16; ++r) {
        uint32_t a, b, c, d;
        __asm__ volatile(
            "ulw %0, 0(%4)\n\tulw %1, 4(%4)\n\t"
            "ulw %2, 8(%4)\n\tulw %3, 12(%4)\n\t"
            "sw %0, 0(%5)\n\tsw %1, 4(%5)\n\tsw %2, 8(%5)\n\tsw %3, 12(%5)"
            : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d)
            : "r"(src), "r"(dst) : "memory");
        src += stride; dst += stride;
    }
}

static void mefp_avg16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    if (((uintptr_t)dst | (uintptr_t)stride) & 3) {
        for (int r = 0; r < 16; ++r) {
            for (int c = 0; c < 16; ++c) dst[c] = (uint8_t)((dst[c] + src[c] + 1) >> 1);
            dst += stride; src += stride;
        }
        return;
    }
#define MEFP_AVG_WORD(OFF) do { \
    uint32_t a, b; \
    __asm__ volatile("ulw %0, " #OFF "(%2)\n\tlw %1, " #OFF "(%3)" \
                     : "=&r"(a), "=&r"(b) : "r"(src), "r"(dst) : "memory"); \
    uint32_t res = (a | b) - (((a ^ b) & 0xfefefefeu) >> 1); \
    __asm__ volatile("sw %0, " #OFF "(%1)" :: "r"(res), "r"(dst) : "memory"); \
} while (0)
    for (int r = 0; r < 16; ++r) {
        MEFP_AVG_WORD(0); MEFP_AVG_WORD(4); MEFP_AVG_WORD(8); MEFP_AVG_WORD(12);
        src += stride; dst += stride;
    }
#undef MEFP_AVG_WORD
}

static void mefp_put8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    if (((uintptr_t)dst | (uintptr_t)stride) & 3) {
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) dst[c] = src[c];
            dst += stride; src += stride;
        }
        return;
    }
    for (int r = 0; r < 8; ++r) {
        uint32_t a, b;
        __asm__ volatile(
            "ulw %0, 0(%2)\n\tulw %1, 4(%2)\n\t"
            "sw %0, 0(%3)\n\tsw %1, 4(%3)"
            : "=&r"(a), "=&r"(b)
            : "r"(src), "r"(dst) : "memory");
        src += stride; dst += stride;
    }
}

static void mefp_avg8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    if (((uintptr_t)dst | (uintptr_t)stride) & 3) {
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) dst[c] = (uint8_t)((dst[c] + src[c] + 1) >> 1);
            dst += stride; src += stride;
        }
        return;
    }
    for (int r = 0; r < 8; ++r) {
        uint32_t a0, b0, a1, b1;
        __asm__ volatile(
            "ulw %0, 0(%4)\n\tlw %1, 0(%5)\n\t"
            "ulw %2, 4(%4)\n\tlw %3, 4(%5)"
            : "=&r"(a0), "=&r"(b0), "=&r"(a1), "=&r"(b1)
            : "r"(src), "r"(dst) : "memory");
        uint32_t r0 = (a0 | b0) - (((a0 ^ b0) & 0xfefefefeu) >> 1);
        uint32_t r1 = (a1 | b1) - (((a1 ^ b1) & 0xfefefefeu) >> 1);
        __asm__ volatile("sw %0, 0(%2)\n\tsw %1, 4(%2)" :: "r"(r0), "r"(r1), "r"(dst) : "memory");
        src += stride; dst += stride;
    }
}

static inline uint32_t mefp_ulw(const uint8_t *p)
{
    uint32_t v;
    __asm__ volatile("ulw %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline uint32_t mefp_chroma_mix4(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d,
    unsigned wa, unsigned wb, unsigned wc, unsigned wd, unsigned round, unsigned shift)
{
    const uint32_t mask = 0x00ff00ffu;
    uint32_t lo = ((a & mask) * wa + (b & mask) * wb + (c & mask) * wc + (d & mask) * wd + round) >> shift;
    uint32_t hi = (((a >> 8) & mask) * wa + ((b >> 8) & mask) * wb +
                   ((c >> 8) & mask) * wc + ((d >> 8) & mask) * wd + round) >> shift;
    return (lo & mask) | ((hi & mask) << 8);
}

static inline void mefp_chroma_store4(uint8_t *d, uint32_t value, unsigned avg)
{
    if (avg) {
        uint32_t old = mefp_ulw(d);
        value = (old | value) - (((old ^ value) & 0xfefefefeu) >> 1);
    }
    __asm__ volatile("usw %0, 0(%1)" :: "r"(value), "r"(d) : "memory");
}

static inline void mefp_chroma_exact(uint8_t *d, const uint8_t *s, ptrdiff_t st,
                                     int h, int x, int y, unsigned width, unsigned avg)
{
    if (!x && !y) {
        if (!(((uintptr_t)d | (uintptr_t)st) & 3)) {
            for (int row = 0; row < h; ++row) {
                for (unsigned col = 0; col < width; col += 4) {
                    uint32_t val = mefp_ulw(s + col);
                    if (avg) {
                        uint32_t old;
                        __asm__ volatile("lw %0, 0(%1)" : "=r"(old) : "r"(d + col) : "memory");
                        val = (old | val) - (((old ^ val) & 0xfefefefeu) >> 1);
                    }
                    __asm__ volatile("sw %0, 0(%1)" :: "r"(val), "r"(d + col) : "memory");
                }
                d += st; s += st;
            }
            return;
        }
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; ++col)
                d[col] = avg ? (uint8_t)((d[col] + s[col] + 1) >> 1) : s[col];
            d += st; s += st;
        }
        return;
    }
    if (x && y) {
        unsigned wa = (8 - x) * (8 - y), wb = x * (8 - y), wc = (8 - x) * y, wd = x * y;
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; col += 4) {
                uint32_t val = mefp_chroma_mix4(
                    mefp_ulw(s + col), mefp_ulw(s + col + 1),
                    mefp_ulw(s + col + st), mefp_ulw(s + col + st + 1),
                    wa, wb, wc, wd, 0x00200020u, 6);
                mefp_chroma_store4(d + col, val, avg);
            }
            d += st; s += st;
        }
    } else {
        unsigned frac = x ? (unsigned)x : (unsigned)y;
        ptrdiff_t step = x ? 1 : st;
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; col += 4) {
                uint32_t val = mefp_chroma_mix4(
                    mefp_ulw(s + col), mefp_ulw(s + col + step),
                    0, 0, 8 - frac, frac, 0, 0, 0x00040004u, 3);
                mefp_chroma_store4(d + col, val, avg);
            }
            d += st; s += st;
        }
    }
}

static void mefp_chroma_put8(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y)
{ mefp_chroma_exact(d, s, st, h, x, y, 8, 0); }
static void mefp_chroma_put4(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y)
{ mefp_chroma_exact(d, s, st, h, x, y, 4, 0); }
static void mefp_chroma_avg8(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y)
{ mefp_chroma_exact(d, s, st, h, x, y, 8, 1); }
static void mefp_chroma_avg4(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y)
{ mefp_chroma_exact(d, s, st, h, x, y, 4, 1); }

static void mefp_emulated_edge_mc(uint8_t *buf, const uint8_t *src,
                                  ptrdiff_t buf_linesize, ptrdiff_t src_linesize,
                                  int block_w, int block_h,
                                  int src_x, int src_y, int w, int h)
{
    int x, y;
    if (!w || !h) return;

    if (src_y >= h) {
        src -= src_y * src_linesize;
        src += (h - 1) * src_linesize;
        src_y = h - 1;
    } else if (src_y <= -block_h) {
        src -= src_y * src_linesize;
        src += (1 - block_h) * src_linesize;
        src_y = 1 - block_h;
    }
    if (src_x >= w) {
        src -= (1 + src_x - w);
        src_x = w - 1;
    } else if (src_x <= -block_w) {
        src += (1 - block_w - src_x);
        src_x = 1 - block_w;
    }

    int start_y = src_y < 0 ? -src_y : 0;
    int start_x = src_x < 0 ? -src_x : 0;
    int end_y   = (h - src_y) < block_h ? (h - src_y) : block_h;
    int end_x   = (w - src_x) < block_w ? (w - src_x) : block_w;

    int copy_w = end_x - start_x;
    src += start_y * src_linesize + start_x;
    buf += start_x;

    for (y = 0; y < start_y; ++y) {
        for (x = 0; x < copy_w; ++x) buf[x] = src[x];
        buf += buf_linesize;
    }
    for (; y < end_y; ++y) {
        for (x = 0; x < copy_w; ++x) buf[x] = src[x];
        src += src_linesize;
        buf += buf_linesize;
    }
    src -= src_linesize;
    for (; y < block_h; ++y) {
        for (x = 0; x < copy_w; ++x) buf[x] = src[x];
        buf += buf_linesize;
    }

    buf -= block_h * buf_linesize + start_x;
    while (block_h--) {
        uint8_t left_val  = buf[start_x];
        uint8_t right_val = buf[end_x - 1];
        for (x = 0; x < start_x; ++x) buf[x] = left_val;
        for (x = end_x; x < block_w; ++x) buf[x] = right_val;
        buf += buf_linesize;
    }
}

/* Exact 8-bit H.264 4x4 IDCT add (matches FFmpeg h264idct_template.c, zero VFPU) */
static void mefp_idct4_add(uint8_t *dst, int16_t *block, int stride)
{
    block[0] += 32;
    for (int i = 0; i < 4; ++i) {
        const unsigned z0 = block[i + 0] + (unsigned)block[i + 8];
        const unsigned z1 = block[i + 0] - (unsigned)block[i + 8];
        const unsigned z2 = (block[i + 4] >> 1) - (unsigned)block[i + 12];
        const unsigned z3 = block[i + 4] + (unsigned)(block[i + 12] >> 1);
        block[i + 0]  = (int16_t)(z0 + z3);
        block[i + 4]  = (int16_t)(z1 + z2);
        block[i + 8]  = (int16_t)(z1 - z2);
        block[i + 12] = (int16_t)(z0 - z3);
    }
    for (int i = 0; i < 4; ++i) {
        const unsigned z0 = block[0 + 4 * i] + (unsigned)block[2 + 4 * i];
        const unsigned z1 = block[0 + 4 * i] - (unsigned)block[2 + 4 * i];
        const unsigned z2 = (block[1 + 4 * i] >> 1) - (unsigned)block[3 + 4 * i];
        const unsigned z3 = block[1 + 4 * i] + (unsigned)(block[3 + 4 * i] >> 1);
        dst[i + 0 * stride] = mefp_clip_u8(dst[i + 0 * stride] + ((int)(z0 + z3) >> 6));
        dst[i + 1 * stride] = mefp_clip_u8(dst[i + 1 * stride] + ((int)(z1 + z2) >> 6));
        dst[i + 2 * stride] = mefp_clip_u8(dst[i + 2 * stride] + ((int)(z1 - z2) >> 6));
        dst[i + 3 * stride] = mefp_clip_u8(dst[i + 3 * stride] + ((int)(z0 - z3) >> 6));
    }
}

static void mefp_idct4_dc_add(uint8_t *dst, int16_t dc_raw, int stride)
{
    int dc = (dc_raw + 32) >> 6;
    for (int j = 0; j < 4; ++j) {
        dst[0] = mefp_clip_u8(dst[0] + dc);
        dst[1] = mefp_clip_u8(dst[1] + dc);
        dst[2] = mefp_clip_u8(dst[2] + dc);
        dst[3] = mefp_clip_u8(dst[3] + dc);
        dst += stride;
    }
}

/* Exact 8-bit H.264 8x8 IDCT add (matches FFmpeg h264idct_template.c, zero VFPU) */
static void mefp_idct8_add(uint8_t *dst, int16_t *block, int stride)
{
    block[0] += 32;
    for (int i = 0; i < 8; ++i) {
        const unsigned a0 = block[i + 0 * 8] + (unsigned)block[i + 4 * 8];
        const unsigned a2 = block[i + 0 * 8] - (unsigned)block[i + 4 * 8];
        const unsigned a4 = (block[i + 2 * 8] >> 1) - (unsigned)block[i + 6 * 8];
        const unsigned a6 = (block[i + 6 * 8] >> 1) + (unsigned)block[i + 2 * 8];
        const unsigned b0 = a0 + a6;
        const unsigned b2 = a2 + a4;
        const unsigned b4 = a2 - a4;
        const unsigned b6 = a0 - a6;

        const int a1 = -block[i + 3 * 8] + (unsigned)block[i + 5 * 8] - block[i + 7 * 8] - (block[i + 7 * 8] >> 1);
        const int a3 =  block[i + 1 * 8] + (unsigned)block[i + 7 * 8] - block[i + 3 * 8] - (block[i + 3 * 8] >> 1);
        const int a5 = -block[i + 1 * 8] + (unsigned)block[i + 7 * 8] + block[i + 5 * 8] + (block[i + 5 * 8] >> 1);
        const int a7 =  block[i + 3 * 8] + (unsigned)block[i + 5 * 8] + block[i + 1 * 8] + (block[i + 1 * 8] >> 1);

        const int b1 = (a7 >> 2) + (unsigned)a1;
        const int b3 = (unsigned)a3 + (a5 >> 2);
        const int b5 = (a3 >> 2) - (unsigned)a5;
        const int b7 = (unsigned)a7 - (a1 >> 2);

        block[i + 0 * 8] = (int16_t)(b0 + b7);
        block[i + 7 * 8] = (int16_t)(b0 - b7);
        block[i + 1 * 8] = (int16_t)(b2 + b5);
        block[i + 6 * 8] = (int16_t)(b2 - b5);
        block[i + 2 * 8] = (int16_t)(b4 + b3);
        block[i + 5 * 8] = (int16_t)(b4 - b3);
        block[i + 3 * 8] = (int16_t)(b6 + b1);
        block[i + 4 * 8] = (int16_t)(b6 - b1);
    }
    for (int i = 0; i < 8; ++i) {
        const unsigned a0 = block[0 + i * 8] + (unsigned)block[4 + i * 8];
        const unsigned a2 = block[0 + i * 8] - (unsigned)block[4 + i * 8];
        const unsigned a4 = (block[2 + i * 8] >> 1) - (unsigned)block[6 + i * 8];
        const unsigned a6 = (block[6 + i * 8] >> 1) + (unsigned)block[2 + i * 8];
        const unsigned b0 = a0 + a6;
        const unsigned b2 = a2 + a4;
        const unsigned b4 = a2 - a4;
        const unsigned b6 = a0 - a6;

        const int a1 = -(unsigned)block[3 + i * 8] + block[5 + i * 8] - block[7 + i * 8] - (block[7 + i * 8] >> 1);
        const int a3 =  (unsigned)block[1 + i * 8] + block[7 + i * 8] - block[3 + i * 8] - (block[3 + i * 8] >> 1);
        const int a5 = -(unsigned)block[1 + i * 8] + block[7 + i * 8] + block[5 + i * 8] + (block[5 + i * 8] >> 1);
        const int a7 =  (unsigned)block[3 + i * 8] + block[5 + i * 8] + block[1 + i * 8] + (block[1 + i * 8] >> 1);

        const unsigned b1 = (a7 >> 2) + (unsigned)a1;
        const unsigned b3 = (unsigned)a3 + (a5 >> 2);
        const unsigned b5 = (a3 >> 2) - (unsigned)a5;
        const unsigned b7 = (unsigned)a7 - (a1 >> 2);

        dst[i + 0 * stride] = mefp_clip_u8(dst[i + 0 * stride] + ((int)(b0 + b7) >> 6));
        dst[i + 1 * stride] = mefp_clip_u8(dst[i + 1 * stride] + ((int)(b2 + b5) >> 6));
        dst[i + 2 * stride] = mefp_clip_u8(dst[i + 2 * stride] + ((int)(b4 + b3) >> 6));
        dst[i + 3 * stride] = mefp_clip_u8(dst[i + 3 * stride] + ((int)(b6 + b1) >> 6));
        dst[i + 4 * stride] = mefp_clip_u8(dst[i + 4 * stride] + ((int)(b6 - b1) >> 6));
        dst[i + 5 * stride] = mefp_clip_u8(dst[i + 5 * stride] + ((int)(b4 - b3) >> 6));
        dst[i + 6 * stride] = mefp_clip_u8(dst[i + 6 * stride] + ((int)(b2 - b5) >> 6));
        dst[i + 7 * stride] = mefp_clip_u8(dst[i + 7 * stride] + ((int)(b0 - b7) >> 6));
    }
}

static void mefp_idct8_dc_add(uint8_t *dst, int16_t dc_raw, int stride)
{
    int dc = (dc_raw + 32) >> 6;
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i)
            dst[i] = mefp_clip_u8(dst[i] + dc);
        dst += stride;
    }
}


/* Motion compensation and whole-frame reconstruction */

static void me_mc_dir_part(const MefpDsp *dsp, const MefpFrameSlot *s,
                           const MefpSliceInfo *sl, int list, int refn,
                           int mvx, int mvy,
                           int square, int height, int delta,
                           uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr,
                           int x_off_full, int y_off_full,
                           int qidx, int cidx, int is_avg,
                           uint8_t *emu_buf)
{
    const int linesize   = s->linesize;
    const int uvlinesize = s->uvlinesize;
    const int pic_width  = s->mb_width * 16;
    const int pic_height = s->mb_height * 16;

    const int mx      = mvx + x_off_full * 8;
    const int my      = mvy + y_off_full * 8;
    const int luma_xy = (mx & 3) + ((my & 3) << 2);
    const int full_mx = mx >> 2;
    const int full_my = my >> 2;

    const uint8_t *ref_y  = (const uint8_t *)((uintptr_t)sl->ref_data[list][refn][0] | 0x80000000u);
    const uint8_t *ref_cb = (const uint8_t *)((uintptr_t)sl->ref_data[list][refn][1] | 0x80000000u);
    const uint8_t *ref_cr = (const uint8_t *)((uintptr_t)sl->ref_data[list][refn][2] | 0x80000000u);

    const uint8_t *src_y = ref_y + full_mx + full_my * linesize;
    int extra_width  = (mx & 7) ? -3 : 0;
    int extra_height = (my & 7) ? -3 : 0;
    int emu = 0;

    if (full_mx      < 0 - extra_width  ||
        full_my      < 0 - extra_height ||
        full_mx + 16 > pic_width  + extra_width ||
        full_my + 16 > pic_height + extra_height) {
        mefp_emulated_edge_mc(emu_buf, src_y - 2 - 2 * linesize,
                              linesize, linesize,
                              21, 21, full_mx - 2, full_my - 2,
                              pic_width, pic_height);
        src_y = emu_buf + 2 + 2 * linesize;
        emu   = 1;
    }

    qpel_mc_func qfn = is_avg
        ? dsp->qpel.avg_h264_qpel_pixels_tab[qidx][luma_xy]
        : dsp->qpel.put_h264_qpel_pixels_tab[qidx][luma_xy];
    qfn = (qpel_mc_func)((uintptr_t)qfn | 0x80000000u);
    qfn(dest_y, src_y, linesize);
    if (!square)
        qfn(dest_y + delta, src_y + delta, linesize);

    const uint8_t *src_cb = ref_cb + (mx >> 3) + (my >> 3) * uvlinesize;
    const uint8_t *src_cr = ref_cr + (mx >> 3) + (my >> 3) * uvlinesize;

    h264_chroma_mc_func cfn = is_avg
        ? dsp->chroma.avg_h264_chroma_pixels_tab[cidx]
        : dsp->chroma.put_h264_chroma_pixels_tab[cidx];
    cfn = (h264_chroma_mc_func)((uintptr_t)cfn | 0x80000000u);

    const int ch = height >> 1;
    if (emu) {
        mefp_emulated_edge_mc(emu_buf, src_cb, uvlinesize, uvlinesize,
                              9, 9, mx >> 3, my >> 3,
                              pic_width >> 1, pic_height >> 1);
        src_cb = emu_buf;
    }
    cfn(dest_cb, src_cb, uvlinesize, ch, mx & 7, my & 7);

    if (emu) {
        mefp_emulated_edge_mc(emu_buf, src_cr, uvlinesize, uvlinesize,
                              9, 9, mx >> 3, my >> 3,
                              pic_width >> 1, pic_height >> 1);
        src_cr = emu_buf;
    }
    cfn(dest_cr, src_cr, uvlinesize, ch, mx & 7, my & 7);
}

static void me_exec_mc_part(const MefpDsp *dsp, const MefpFrameSlot *s,
                            const MefpSliceInfo *sl, const MefpMbCmd *mb,
                            const MefpMcPart *pt,
                            uint8_t *mb_y_dst, uint8_t *mb_cb_dst, uint8_t *mb_cr_dst,
                            uint8_t *emu_buf, uint8_t *bipred_buf)
{
    static const uint8_t shape_square[7] = { 1, 0, 0, 1, 0, 0, 1 };
    static const uint8_t shape_height[7] = { 16, 8, 16, 8, 4, 8, 4 };
    static const uint8_t shape_qidx[7]   = { 0, 1, 1, 1, 2, 2, 2 };
    static const uint8_t shape_cidx[7]   = { 0, 0, 1, 1, 1, 2, 2 };
    static const uint8_t shape_widx[7]   = { 0, 0, 1, 1, 1, 2, 2 };

    const int sh     = pt->shape;
    const int square = shape_square[sh];
    const int height = shape_height[sh];
    const int qidx   = shape_qidx[sh];
    const int cidx   = shape_cidx[sh];
    const int widx   = shape_widx[sh];

    const int linesize   = s->linesize;
    const int uvlinesize = s->uvlinesize;
    int delta = 0;
    if (sh == MEFP_PART_16x8)      delta = 8;
    else if (sh == MEFP_PART_8x16) delta = 8 * linesize;
    else if (sh == MEFP_PART_8x4)  delta = 4;
    else if (sh == MEFP_PART_4x8)  delta = 4 * linesize;

    uint8_t *dest_y  = mb_y_dst  + 2 * pt->x_off + 2 * pt->y_off * linesize;
    uint8_t *dest_cb = mb_cb_dst + pt->x_off     + pt->y_off     * uvlinesize;
    uint8_t *dest_cr = mb_cr_dst + pt->x_off     + pt->y_off     * uvlinesize;
    const int x_full = pt->x_off + 8 * mb->mb_x;
    const int y_full = pt->y_off + 8 * mb->mb_y;
    const int list0  = pt->dir_mask & 1;
    const int list1  = (pt->dir_mask >> 1) & 1;

    const int weighted =
        (sl->use_weight == 1) ||
        (sl->use_weight == 2 && list0 && list1 &&
         sl->implicit_weight[pt->ref0][pt->ref1][mb->mb_y & 1] != 32);

    if (!weighted) {
        int avg = 0;
        if (list0) {
            me_mc_dir_part(dsp, s, sl, 0, pt->ref0, pt->mv[0][0], pt->mv[0][1],
                           square, height, delta, dest_y, dest_cb, dest_cr,
                           x_full, y_full, qidx, cidx, 0, emu_buf);
            avg = 1;
        }
        if (list1) {
            me_mc_dir_part(dsp, s, sl, 1, pt->ref1, pt->mv[1][0], pt->mv[1][1],
                           square, height, delta, dest_y, dest_cb, dest_cr,
                           x_full, y_full, qidx, cidx, avg, emu_buf);
        }
        return;
    }

    const int ch = height >> 1;
    if (list0 && list1) {
        uint8_t *tmp_cb = bipred_buf;
        uint8_t *tmp_cr = bipred_buf + 16;
        uint8_t *tmp_y  = bipred_buf + 16 * uvlinesize;

        me_mc_dir_part(dsp, s, sl, 0, pt->ref0, pt->mv[0][0], pt->mv[0][1],
                       square, height, delta, dest_y, dest_cb, dest_cr,
                       x_full, y_full, qidx, cidx, 0, emu_buf);
        me_mc_dir_part(dsp, s, sl, 1, pt->ref1, pt->mv[1][0], pt->mv[1][1],
                       square, height, delta, tmp_y, tmp_cb, tmp_cr,
                       x_full, y_full, qidx, cidx, 0, emu_buf);

        h264_biweight_func lw_avg = (h264_biweight_func)((uintptr_t)dsp->h264dsp.biweight_h264_pixels_tab[widx] | 0x80000000u);
        h264_biweight_func cw_avg = (h264_biweight_func)((uintptr_t)dsp->h264dsp.biweight_h264_pixels_tab[widx + 1] | 0x80000000u);

        if (sl->use_weight == 2) {
            int w0 = sl->implicit_weight[pt->ref0][pt->ref1][mb->mb_y & 1];
            int w1 = 64 - w0;
            lw_avg(dest_y, tmp_y, linesize, height, 5, w0, w1, 0);
            cw_avg(dest_cb, tmp_cb, uvlinesize, ch, 5, w0, w1, 0);
            cw_avg(dest_cr, tmp_cr, uvlinesize, ch, 5, w0, w1, 0);
        } else {
            int r0 = pt->ref0, r1 = pt->ref1;
            lw_avg(dest_y, tmp_y, linesize, height,
                   sl->luma_log2_denom,
                   sl->luma_weight[r0][0][0], sl->luma_weight[r1][1][0],
                   sl->luma_weight[r0][0][1] + sl->luma_weight[r1][1][1]);
            cw_avg(dest_cb, tmp_cb, uvlinesize, ch,
                   sl->chroma_log2_denom,
                   sl->chroma_weight[r0][0][0][0], sl->chroma_weight[r1][1][0][0],
                   sl->chroma_weight[r0][0][0][1] + sl->chroma_weight[r1][1][0][1]);
            cw_avg(dest_cr, tmp_cr, uvlinesize, ch,
                   sl->chroma_log2_denom,
                   sl->chroma_weight[r0][0][1][0], sl->chroma_weight[r1][1][1][0],
                   sl->chroma_weight[r0][0][1][1] + sl->chroma_weight[r1][1][1][1]);
        }
    } else {
        int list = list1 ? 1 : 0;
        int refn = list1 ? pt->ref1 : pt->ref0;
        me_mc_dir_part(dsp, s, sl, list, refn, pt->mv[list][0], pt->mv[list][1],
                       square, height, delta, dest_y, dest_cb, dest_cr,
                       x_full, y_full, qidx, cidx, 0, emu_buf);

        h264_weight_func lw_op = (h264_weight_func)((uintptr_t)dsp->h264dsp.weight_h264_pixels_tab[widx] | 0x80000000u);
        lw_op(dest_y, linesize, height,
              sl->luma_log2_denom, sl->luma_weight[refn][list][0], sl->luma_weight[refn][list][1]);
        if (sl->use_weight_chroma) {
            h264_weight_func cw_op = (h264_weight_func)((uintptr_t)dsp->h264dsp.weight_h264_pixels_tab[widx + 1] | 0x80000000u);
            cw_op(dest_cb, uvlinesize, ch,
                  sl->chroma_log2_denom, sl->chroma_weight[refn][list][0][0], sl->chroma_weight[refn][list][0][1]);
            cw_op(dest_cr, uvlinesize, ch,
                  sl->chroma_log2_denom, sl->chroma_weight[refn][list][1][0], sl->chroma_weight[refn][list][1][1]);
        }
    }
}

static void me_apply_chroma_residual(uint8_t *dest_cb, uint8_t *dest_cr,
                                     const int *bo, int uvlinesize,
                                     uint16_t mask, int16_t **cp_inout)
{
    if (!mask) return;
    int16_t *cp = *cp_inout;
    for (int k = 0; k < 4; ++k) {
        unsigned code = (mask >> (2 * k)) & 3u;
        if (code == 1u) {
            mefp_idct4_dc_add(dest_cb + bo[16 + k], *cp++, uvlinesize);
        } else if (code == 2u) {
            mefp_idct4_add(dest_cb + bo[16 + k], cp, uvlinesize);
            cp += 16;
        }
    }
    for (int k = 0; k < 4; ++k) {
        unsigned code = (mask >> (8 + 2 * k)) & 3u;
        if (code == 1u) {
            mefp_idct4_dc_add(dest_cr + bo[32 + k], *cp++, uvlinesize);
        } else if (code == 2u) {
            mefp_idct4_add(dest_cr + bo[32 + k], cp, uvlinesize);
            cp += 16;
        }
    }
    *cp_inout = cp;
}

static void me_reconstruct_frame(const MefpDsp *dsp, MefpFrameSlot *s, volatile MefpRing *q)
{
    uint8_t *cur_y  = (uint8_t *)((uintptr_t)s->cur_data[0] | 0x80000000u);
    uint8_t *cur_cb = (uint8_t *)((uintptr_t)s->cur_data[1] | 0x80000000u);
    uint8_t *cur_cr = (uint8_t *)((uintptr_t)s->cur_data[2] | 0x80000000u);
    const int linesize   = s->linesize;
    const int uvlinesize = s->uvlinesize;
    const int *bo = s->block_offset;

    uint8_t *emu_buf    = q->emu_buf;
    uint8_t *bipred_buf = q->bipred_buf;
    const MefpMcPart *mc_parts = (const MefpMcPart *)((uintptr_t)s->mc_parts | 0x80000000u);
    int16_t *coeff_base        = (int16_t *)((uintptr_t)s->coeff_pool | 0x80000000u);

    unsigned intra_cnt = 0, inter_cnt = 0;

    for (unsigned idx = 0; idx < s->mb_count; ++idx) {
        const MefpMbCmd *mb = &s->mbs[idx];
        uint8_t *dest_y  = cur_y  + (mb->mb_y * 16) * linesize   + mb->mb_x * 16;
        uint8_t *dest_cb = cur_cb + (mb->mb_y * 8)  * uvlinesize + mb->mb_x * 8;
        uint8_t *dest_cr = cur_cr + (mb->mb_y * 8)  * uvlinesize + mb->mb_x * 8;
        int16_t *cp = coeff_base + mb->coeff_start;

        if (mb->kind == MEFP_MB_INTER) {
            ++inter_cnt;
            const MefpSliceInfo *sl = &s->slices[(mb->flags >> 1) & 7u];
            for (unsigned p = 0; p < mb->mc_count; ++p) {
                me_exec_mc_part(dsp, s, sl, mb, &mc_parts[mb->mc_start + p],
                                dest_y, dest_cb, dest_cr, emu_buf, bipred_buf);
            }
            if (mb->luma_res_mask) {
                if (mb->flags & MEFP_FLAG_DCT8) {
                    for (int k = 0; k < 4; ++k) {
                        unsigned code = (mb->luma_res_mask >> (2 * k)) & 3u;
                        if (code == 1u) {
                            mefp_idct8_dc_add(dest_y + bo[k * 4], *cp++, linesize);
                        } else if (code == 2u) {
                            mefp_idct8_add(dest_y + bo[k * 4], cp, linesize);
                            cp += 64;
                        }
                    }
                } else {
                    for (int i = 0; i < 16; ++i) {
                        unsigned code = (mb->luma_res_mask >> (2 * i)) & 3u;
                        if (code == 1u) {
                            mefp_idct4_dc_add(dest_y + bo[i], *cp++, linesize);
                        } else if (code == 2u) {
                            mefp_idct4_add(dest_y + bo[i], cp, linesize);
                            cp += 16;
                        }
                    }
                }
            }
            me_apply_chroma_residual(dest_cb, dest_cr, bo, uvlinesize, mb->chroma_res_mask, &cp);
        } else if (mb->kind == MEFP_MB_INTRA16) {
            ++intra_cnt;
            void (*p8)(uint8_t *, ptrdiff_t) = (void (*)(uint8_t *, ptrdiff_t))((uintptr_t)dsp->hpc.pred8x8[mb->chroma_pred_mode] | 0x80000000u);
            void (*p16)(uint8_t *, ptrdiff_t) = (void (*)(uint8_t *, ptrdiff_t))((uintptr_t)dsp->hpc.pred16x16[mb->intra16_pred_mode] | 0x80000000u);
            p8(dest_cb, uvlinesize);
            p8(dest_cr, uvlinesize);
            p16(dest_y, linesize);
            if (mb->luma_res_mask) {
                for (int i = 0; i < 16; ++i) {
                    unsigned code = (mb->luma_res_mask >> (2 * i)) & 3u;
                    if (code == 1u) {
                        mefp_idct4_dc_add(dest_y + bo[i], *cp++, linesize);
                    } else if (code == 2u) {
                        mefp_idct4_add(dest_y + bo[i], cp, linesize);
                        cp += 16;
                    }
                }
            }
            me_apply_chroma_residual(dest_cb, dest_cr, bo, uvlinesize, mb->chroma_res_mask, &cp);
        } else if (mb->kind == MEFP_MB_INTRA8) {
            ++intra_cnt;
            void (*p8)(uint8_t *, ptrdiff_t) = (void (*)(uint8_t *, ptrdiff_t))((uintptr_t)dsp->hpc.pred8x8[mb->chroma_pred_mode] | 0x80000000u);
            p8(dest_cb, uvlinesize);
            p8(dest_cr, uvlinesize);
            const uint8_t *modes = s->intra4_modes + mb->intra4_start;
            for (int k = 0; k < 4; ++k) {
                int i = k * 4;
                uint8_t *ptr = dest_y + bo[i];
                int dir = modes[k];
                void (*p8l)(uint8_t *, int, int, ptrdiff_t) =
                    (void (*)(uint8_t *, int, int, ptrdiff_t))((uintptr_t)dsp->hpc.pred8x8l[dir] | 0x80000000u);
                p8l(ptr, (mb->tl_avail << i) & 0x8000, (mb->tr_avail << i) & 0x4000, linesize);
                unsigned code = (mb->luma_res_mask >> (2 * k)) & 3u;
                if (code == 1u) {
                    mefp_idct8_dc_add(ptr, *cp++, linesize);
                } else if (code == 2u) {
                    mefp_idct8_add(ptr, cp, linesize);
                    cp += 64;
                }
            }
            me_apply_chroma_residual(dest_cb, dest_cr, bo, uvlinesize, mb->chroma_res_mask, &cp);
        } else if (mb->kind == MEFP_MB_INTRA4) {
            ++intra_cnt;
            void (*p8)(uint8_t *, ptrdiff_t) = (void (*)(uint8_t *, ptrdiff_t))((uintptr_t)dsp->hpc.pred8x8[mb->chroma_pred_mode] | 0x80000000u);
            p8(dest_cb, uvlinesize);
            p8(dest_cr, uvlinesize);
            const uint8_t *modes = s->intra4_modes + mb->intra4_start;
            for (int i = 0; i < 16; ++i) {
                uint8_t *ptr = dest_y + bo[i];
                int dir = modes[i];
                uint8_t *topright = NULL;
                uint32_t tr_buf;
                if (dir == DIAG_DOWN_LEFT_PRED || dir == VERT_LEFT_PRED) {
                    if (!((mb->tr_avail << i) & 0x8000)) {
                        tr_buf = ptr[3 - linesize] * 0x01010101u;
                        topright = (uint8_t *)&tr_buf;
                    } else {
                        topright = ptr + 4 - linesize;
                    }
                }
                void (*p4)(uint8_t *, const uint8_t *, ptrdiff_t) =
                    (void (*)(uint8_t *, const uint8_t *, ptrdiff_t))((uintptr_t)dsp->hpc.pred4x4[dir] | 0x80000000u);
                p4(ptr, topright, linesize);
                unsigned code = (mb->luma_res_mask >> (2 * i)) & 3u;
                if (code == 1u) {
                    mefp_idct4_dc_add(ptr, *cp++, linesize);
                } else if (code == 2u) {
                    mefp_idct4_add(ptr, cp, linesize);
                    cp += 16;
                }
            }
            me_apply_chroma_residual(dest_cb, dest_cr, bo, uvlinesize, mb->chroma_res_mask, &cp);
        } else if (mb->kind == MEFP_MB_PCM) {
            ++intra_cnt;
            const uint8_t *pcm = (const uint8_t *)cp;
            for (int r = 0; r < 16; ++r)
                for (int c = 0; c < 16; ++c)
                    dest_y[r * linesize + c] = pcm[r * 16 + c];
            const uint8_t *pcb = pcm + 256;
            const uint8_t *pcr = pcm + 320;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    dest_cb[r * uvlinesize + c] = pcb[r * 8 + c];
                    dest_cr[r * uvlinesize + c] = pcr[r * 8 + c];
                }
        }
    }

    q->me_intra_mbs += intra_cnt;
    q->me_inter_mbs += inter_cnt;
}


/* Runs on SC before the worker starts: FFmpeg's C intra/weight functions plus
 * the integer qpel/chroma kernels above, so ME never executes VFPU code. */
void mefp_dsp_init(MefpDsp *dsp)
{
    __real_ff_h264qpel_init(&dsp->qpel, 8);
    dsp->qpel.put_h264_qpel_pixels_tab[0][0] = mefp_put16;
    dsp->qpel.avg_h264_qpel_pixels_tab[0][0] = mefp_avg16;
    dsp->qpel.put_h264_qpel_pixels_tab[1][0] = mefp_put8;
    dsp->qpel.avg_h264_qpel_pixels_tab[1][0] = mefp_avg8;
    meq_install_qpel(&dsp->qpel);
    __real_ff_h264chroma_init(&dsp->chroma, 8);
    dsp->chroma.put_h264_chroma_pixels_tab[0] = mefp_chroma_put8;
    dsp->chroma.put_h264_chroma_pixels_tab[1] = mefp_chroma_put4;
    dsp->chroma.avg_h264_chroma_pixels_tab[0] = mefp_chroma_avg8;
    dsp->chroma.avg_h264_chroma_pixels_tab[1] = mefp_chroma_avg4;
    ff_h264_pred_init(&dsp->hpc, AV_CODEC_ID_H264, 8, 1);
    ff_h264dsp_init(&dsp->h264dsp, 8, 1);
}

/* Persistent ME worker. One MeJob starts it; it then consumes the command
 * ring until SC sets `stop` and every requested command has completed. */
int __attribute__((noinline)) mefp_me_worker(int index, void *param)
{
    (void)index;
    MeJob *j = (MeJob *)((uintptr_t)param | 0xA0000000u);
    /* Operation 0 is Custom Core's meLabInit probe job: return immediately. */
    if (j->operation == 0) {
        j->value = 0x4d450001u;
        j->done = 1;
        __asm__ volatile("sync" ::: "memory");
        return 0;
    }

    MefpRing *cached = (MefpRing *)((uintptr_t)j->output | 0x80000000u);
    volatile MefpRing *q = (volatile MefpRing *)((uintptr_t)j->output | 0xA0000000u);
    const MefpDsp *dsp = (const MefpDsp *)((uintptr_t)&mefp_dsp | 0x80000000u);

    meCoreDcacheWritebackInvalidateAll();

    unsigned done = 0;
    q->ready = 1;
    __asm__ volatile("sync" ::: "memory");

    while (!q->stop || done != q->requested) {
        if (done == q->requested) {
            __asm__ volatile("nop; nop; nop; nop");
            continue;
        }
        MefpCmd *cmd = &cached->cmds[done & (MEFP_RING_DEPTH - 1)];
        /* Drop stale lines for the command and its slot. The ME D-cache is
         * 16 KiB, so a whole-cache operation takes about 10 us. */
        meCoreDcacheWritebackInvalidateAll();
        if (cmd->op == MEFP_OP_RECON) {
            MefpFrameSlot *slot = (MefpFrameSlot *)((uintptr_t)cmd->slot | 0x80000000u);
            me_reconstruct_frame(dsp, slot, q);
            /* Publish the reconstructed planes before `completed` advances:
             * the GE presenter and SC may read them right after. */
            meCoreDcacheWritebackInvalidateAll();
        }
        __asm__ volatile("sync" ::: "memory");
        q->completed = ++done;
    }

    meCoreDcacheWritebackInvalidateAll();
    q->exited = 1;
    j->value = 0x4d450001u;
    j->done = 1;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}
