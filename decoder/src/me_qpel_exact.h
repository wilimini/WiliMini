/* Bit-exact 8-bit H.264 6-tap Luma Qpel MC kernels tuned for PSP Allegrex ME.
 *
 * Design:
 * - 100% ISO/IEC 14496-10 & FFmpeg bit-identical across all 15 sub-pel phases,
 *   block sizes 16/8/4, and put/avg variants.
 * - Zero VFPU instructions (safe on ME).
 * - Uses Allegrex hardware `min`/`max` (zero clip branches), 2-lane 16-bit SWAR
 *   with +4112 bias (strictly non-negative, zero cross-lane borrow), 4-byte
 *   word loads (`ulw`), SWAR byte averaging, sliding-window vertical 6-tap
 *   (no copy_block!), and fused quarter-pel / HV passes (no duplicate H pass!).
 */
#ifndef PSP_ME_QPEL_EXACT_H
#define PSP_ME_QPEL_EXACT_H

#include <stdint.h>
#include <stddef.h>

static inline int meq_clip_u8(int x)
{
#ifdef __PSP__
    int r;
    __asm__("max %0, %1, $zero\n\tmin %0, %0, %2" : "=&r"(r) : "r"(x), "r"(255));
    return r;
#else
    return x < 0 ? 0 : (x > 255 ? 255 : x);
#endif
}

static inline uint32_t meq_ulw(const uint8_t *p)
{
#ifdef __PSP__
    uint32_t v;
    __asm__ volatile("ulw %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
#else
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#endif
}

static inline void meq_usw(uint8_t *p, uint32_t v)
{
#ifdef __PSP__
    __asm__ volatile("usw %0, 0(%1)" :: "r"(v), "r"(p) : "memory");
#else
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
#endif
}

static inline uint32_t meq_avg4(uint32_t a, uint32_t b)
{
    return (a | b) - (((a ^ b) & 0xfefefefeu) >> 1);
}

static inline void meq_store4(uint8_t *dst, uint32_t val, int is_avg)
{
    if (is_avg) val = meq_avg4(meq_ulw(dst), val);
    meq_usw(dst, val);
}

/* Pack 2-lane biased 6-tap sums (bias = +4112 = 0x1010 per lane) into 4 clipped bytes.
 * lo holds pixels (0, 2); hi holds pixels (1, 3). */
static inline uint32_t meq_pack4_biased(uint32_t lo, uint32_t hi)
{
    int p0 = (int)((lo & 0xffffu) >> 5) - 128;
    int p1 = (int)((hi & 0xffffu) >> 5) - 128;
    int p2 = (int)(lo >> 21) - 128;
    int p3 = (int)(hi >> 21) - 128;
    return (uint32_t)meq_clip_u8(p0) |
           ((uint32_t)meq_clip_u8(p1) << 8) |
           ((uint32_t)meq_clip_u8(p2) << 16) |
           ((uint32_t)meq_clip_u8(p3) << 24);
}

/* Compute 2-lane biased vertical 6-tap on 4 pixels packed as (lo, hi).
 * Raw 6-tap in [-2550..+10710]; +0x10101010u (+4112 per lane) keeps both lanes
 * strictly in [+1562..+14822] with zero cross-lane borrow/overflow. */
static inline uint32_t meq_v6_4px(uint32_t r0l, uint32_t r0h,
                                  uint32_t r1l, uint32_t r1h,
                                  uint32_t r2l, uint32_t r2h,
                                  uint32_t r3l, uint32_t r3h,
                                  uint32_t r4l, uint32_t r4h,
                                  uint32_t r5l, uint32_t r5h)
{
    uint32_t lo = (r2l + r3l) * 20u + (r0l + r5l) + 0x10101010u - (r1l + r4l) * 5u;
    uint32_t hi = (r2h + r3h) * 20u + (r0h + r5h) + 0x10101010u - (r1h + r4h) * 5u;
    return meq_pack4_biased(lo, hi);
}

/* Fast horizontal 6-tap on 4 consecutive output pixels (src[-2..+6]).
 * Returns 4 packed clipped bytes. */
static inline uint32_t meq_h6_4px(const uint8_t *s)
{
    uint32_t w0 = meq_ulw(s - 2); /* s[-2], s[-1], s[0], s[1] */
    uint32_t w1 = meq_ulw(s + 2); /* s[2],  s[3],  s[4], s[5] */
    int s_2 = (int)(w0 & 0xffu);
    int s_1 = (int)((w0 >> 8) & 0xffu);
    int s0  = (int)((w0 >> 16) & 0xffu);
    int s1  = (int)(w0 >> 24);
    int s2  = (int)(w1 & 0xffu);
    int s3  = (int)((w1 >> 8) & 0xffu);
    int s4  = (int)((w1 >> 16) & 0xffu);
    int s5  = (int)(w1 >> 24);
    int s6  = (int)s[6];

    int p0 = ((s0 + s1) * 20 - (s_1 + s2) * 5 + (s_2 + s3) + 16) >> 5;
    int p1 = ((s1 + s2) * 20 - (s0  + s3) * 5 + (s_1 + s4) + 16) >> 5;
    int p2 = ((s2 + s3) * 20 - (s1  + s4) * 5 + (s0  + s5) + 16) >> 5;
    int p3 = ((s3 + s4) * 20 - (s2  + s5) * 5 + (s1  + s6) + 16) >> 5;
    return (uint32_t)meq_clip_u8(p0) |
           ((uint32_t)meq_clip_u8(p1) << 8) |
           ((uint32_t)meq_clip_u8(p2) << 16) |
           ((uint32_t)meq_clip_u8(p3) << 24);
}

/* Raw horizontal 6-tap into int16_t[4] (no shift/clip) for 2D HV passes. */
static inline void meq_h6_raw4(int16_t *dst, const uint8_t *s)
{
    uint32_t w0 = meq_ulw(s - 2);
    uint32_t w1 = meq_ulw(s + 2);
    int s_2 = (int)(w0 & 0xffu);
    int s_1 = (int)((w0 >> 8) & 0xffu);
    int s0  = (int)((w0 >> 16) & 0xffu);
    int s1  = (int)(w0 >> 24);
    int s2  = (int)(w1 & 0xffu);
    int s3  = (int)((w1 >> 8) & 0xffu);
    int s4  = (int)((w1 >> 16) & 0xffu);
    int s5  = (int)(w1 >> 24);
    int s6  = (int)s[6];

    dst[0] = (int16_t)((s0 + s1) * 20 - (s_1 + s2) * 5 + (s_2 + s3));
    dst[1] = (int16_t)((s1 + s2) * 20 - (s0  + s3) * 5 + (s_1 + s4));
    dst[2] = (int16_t)((s2 + s3) * 20 - (s1  + s4) * 5 + (s0  + s5));
    dst[3] = (int16_t)((s3 + s4) * 20 - (s2  + s5) * 5 + (s1  + s6));
}

/* ---------------- Phase 1, 2, 3: Horizontal (mc10, mc20, mc30) ---------------- */
static inline void meq_mc_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                            int size, int qmode, int is_avg)
{
    /* qmode: 0 = mc20 (half), 1 = mc10 (avg with src+0), 2 = mc30 (avg with src+1) */
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; x += 4) {
            uint32_t h = meq_h6_4px(src + x);
            if (qmode == 1)      h = meq_avg4(h, meq_ulw(src + x));
            else if (qmode == 2) h = meq_avg4(h, meq_ulw(src + x + 1));
            meq_store4(dst + x, h, is_avg);
        }
        dst += stride;
        src += stride;
    }
}

/* ---------------- Phase 4, 8, 12: Vertical (mc01, mc02, mc03) ---------------- */
/* Sliding-window 4-column vertical 6-tap: 1 ulw load per 4 output pixels, zero copy_block! */
static inline void meq_mc_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                            int size, int qmode, int is_avg)
{
    /* qmode: 0 = mc02 (half), 1 = mc01 (avg with row 0), 2 = mc03 (avg with row +1) */
    const uint32_t mask = 0x00ff00ffu;
    for (int x = 0; x < size; x += 4) {
        const uint8_t *s = src + x - 2 * stride;
        uint8_t *d = dst + x;
        uint32_t p0 = meq_ulw(s); s += stride;
        uint32_t p1 = meq_ulw(s); s += stride;
        uint32_t p2 = meq_ulw(s); s += stride;
        uint32_t p3 = meq_ulw(s); s += stride;
        uint32_t p4 = meq_ulw(s); s += stride;

        uint32_t r0l = p0 & mask, r0h = (p0 >> 8) & mask;
        uint32_t r1l = p1 & mask, r1h = (p1 >> 8) & mask;
        uint32_t r2l = p2 & mask, r2h = (p2 >> 8) & mask;
        uint32_t r3l = p3 & mask, r3h = (p3 >> 8) & mask;
        uint32_t r4l = p4 & mask, r4h = (p4 >> 8) & mask;

        for (int y = 0; y < size; ++y) {
            uint32_t p5 = meq_ulw(s); s += stride;
            uint32_t r5l = p5 & mask, r5h = (p5 >> 8) & mask;
            uint32_t v = meq_v6_4px(r0l, r0h, r1l, r1h, r2l, r2h, r3l, r3h, r4l, r4h, r5l, r5h);
            if (qmode == 1)      v = meq_avg4(v, p2);
            else if (qmode == 2) v = meq_avg4(v, p3);
            meq_store4(d, v, is_avg);
            d += stride;

            p2 = p3; p3 = p4; p4 = p5;
            r0l = r1l; r0h = r1h;
            r1l = r2l; r1h = r2h;
            r2l = r3l; r2h = r3h;
            r3l = r4l; r3h = r4h;
            r4l = r5l; r4h = r5h;
        }
    }
}

/* ---------------- Phase 5, 7, 13, 15: Diagonal H+V (mc11, mc31, mc13, mc33) ---------------- */
/* 1 fast H pass into halfH[] + 1 fused sliding-window V+SWAR-avg pass straight to dst! */
static inline void meq_mc_diag(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                               int size, int dx_v, int dy_h, int is_avg)
{
    uint32_t halfH[16 * 4]; /* max 16 rows * 4 words = 256 bytes */
    const int words = size >> 2;
    const uint8_t *sh = src + (dy_h ? stride : 0);
    for (int y = 0; y < size; ++y) {
        for (int w = 0; w < words; ++w)
            halfH[y * words + w] = meq_h6_4px(sh + (w << 2));
        sh += stride;
    }

    const uint32_t mask = 0x00ff00ffu;
    const uint8_t *sv_base = src + dx_v;
    for (int w = 0; w < words; ++w) {
        int x = w << 2;
        const uint8_t *s = sv_base + x - 2 * stride;
        uint8_t *d = dst + x;
        uint32_t p0 = meq_ulw(s); s += stride;
        uint32_t p1 = meq_ulw(s); s += stride;
        uint32_t p2 = meq_ulw(s); s += stride;
        uint32_t p3 = meq_ulw(s); s += stride;
        uint32_t p4 = meq_ulw(s); s += stride;

        uint32_t r0l = p0 & mask, r0h = (p0 >> 8) & mask;
        uint32_t r1l = p1 & mask, r1h = (p1 >> 8) & mask;
        uint32_t r2l = p2 & mask, r2h = (p2 >> 8) & mask;
        uint32_t r3l = p3 & mask, r3h = (p3 >> 8) & mask;
        uint32_t r4l = p4 & mask, r4h = (p4 >> 8) & mask;

        for (int y = 0; y < size; ++y) {
            uint32_t p5 = meq_ulw(s); s += stride;
            uint32_t r5l = p5 & mask, r5h = (p5 >> 8) & mask;
            uint32_t v = meq_v6_4px(r0l, r0h, r1l, r1h, r2l, r2h, r3l, r3h, r4l, r4h, r5l, r5h);
            uint32_t out = meq_avg4(halfH[y * words + w], v);
            meq_store4(d, out, is_avg);
            d += stride;

            r0l = r1l; r0h = r1h;
            r1l = r2l; r1h = r2h;
            r2l = r3l; r2h = r3h;
            r3l = r4l; r3h = r4h;
            r4l = r5l; r4h = r5h;
        }
    }
}

/* ---------------- Phase 6, 10, 14: 2D HV & HV+H (mc21, mc22, mc23) ---------------- */
/* Computes raw H ONLY ONCE into tmp[(size+5)*size], then fuses V-pass on tmp AND
 * reuses tmp[row 0 or +1] for halfH in mc21/mc23 (eliminates 100% of duplicate H pass!). */
static inline void meq_mc_hv_and_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                   int size, int hmode, int is_avg)
{
    /* hmode: 0 = mc22 (HV only), 1 = mc21 (avg HV with H at y), 2 = mc23 (avg HV with H at y+1) */
    int16_t tmp[21 * 16];
    const uint8_t *sr = src - 2 * stride;
    for (int y = 0; y < size + 5; ++y) {
        int16_t *tr = tmp + y * size;
        for (int x = 0; x < size; x += 4)
            meq_h6_raw4(tr + x, sr + x);
        sr += stride;
    }

    for (int x = 0; x < size; x += 4) {
        const int16_t *t = tmp + x;
        uint8_t *d = dst + x;
        int t0_0 = t[0], t0_1 = t[1], t0_2 = t[2], t0_3 = t[3]; t += size;
        int t1_0 = t[0], t1_1 = t[1], t1_2 = t[2], t1_3 = t[3]; t += size;
        int t2_0 = t[0], t2_1 = t[1], t2_2 = t[2], t2_3 = t[3]; t += size;
        int t3_0 = t[0], t3_1 = t[1], t3_2 = t[2], t3_3 = t[3]; t += size;
        int t4_0 = t[0], t4_1 = t[1], t4_2 = t[2], t4_3 = t[3]; t += size;

        for (int y = 0; y < size; ++y) {
            int t5_0 = t[0], t5_1 = t[1], t5_2 = t[2], t5_3 = t[3]; t += size;
            int q0 = meq_clip_u8(((t2_0 + t3_0) * 20 - (t1_0 + t4_0) * 5 + (t0_0 + t5_0) + 512) >> 10);
            int q1 = meq_clip_u8(((t2_1 + t3_1) * 20 - (t1_1 + t4_1) * 5 + (t0_1 + t5_1) + 512) >> 10);
            int q2 = meq_clip_u8(((t2_2 + t3_2) * 20 - (t1_2 + t4_2) * 5 + (t0_2 + t5_2) + 512) >> 10);
            int q3 = meq_clip_u8(((t2_3 + t3_3) * 20 - (t1_3 + t4_3) * 5 + (t0_3 + t5_3) + 512) >> 10);
            uint32_t hv = (uint32_t)q0 | ((uint32_t)q1 << 8) | ((uint32_t)q2 << 16) | ((uint32_t)q3 << 24);

            if (hmode == 1) {
                uint32_t hh = (uint32_t)meq_clip_u8((t2_0 + 16) >> 5) |
                              ((uint32_t)meq_clip_u8((t2_1 + 16) >> 5) << 8) |
                              ((uint32_t)meq_clip_u8((t2_2 + 16) >> 5) << 16) |
                              ((uint32_t)meq_clip_u8((t2_3 + 16) >> 5) << 24);
                hv = meq_avg4(hh, hv);
            } else if (hmode == 2) {
                uint32_t hh = (uint32_t)meq_clip_u8((t3_0 + 16) >> 5) |
                              ((uint32_t)meq_clip_u8((t3_1 + 16) >> 5) << 8) |
                              ((uint32_t)meq_clip_u8((t3_2 + 16) >> 5) << 16) |
                              ((uint32_t)meq_clip_u8((t3_3 + 16) >> 5) << 24);
                hv = meq_avg4(hh, hv);
            }
            meq_store4(d, hv, is_avg);
            d += stride;

            t0_0 = t1_0; t0_1 = t1_1; t0_2 = t1_2; t0_3 = t1_3;
            t1_0 = t2_0; t1_1 = t2_1; t1_2 = t2_2; t1_3 = t2_3;
            t2_0 = t3_0; t2_1 = t3_1; t2_2 = t3_2; t2_3 = t3_3;
            t3_0 = t4_0; t3_1 = t4_1; t3_2 = t4_2; t3_3 = t4_3;
            t4_0 = t5_0; t4_1 = t5_1; t4_2 = t5_2; t4_3 = t5_3;
        }
    }
}

/* ---------------- Phase 9, 11: 2D HV + V (mc12, mc32) ---------------- */
/* Fuses V-pass on raw H (producing HV) + sliding-window V-pass on src+dx (producing V)
 * into ONE pass directly into dst (zero copy_block, zero halfV/halfHV arrays!). */
static inline void meq_mc_hv_and_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                   int size, int dx_v, int is_avg)
{
    int16_t tmp[21 * 16];
    const uint8_t *sr = src - 2 * stride;
    for (int y = 0; y < size + 5; ++y) {
        int16_t *tr = tmp + y * size;
        for (int x = 0; x < size; x += 4)
            meq_h6_raw4(tr + x, sr + x);
        sr += stride;
    }

    const uint32_t mask = 0x00ff00ffu;
    const uint8_t *sv_base = src + dx_v;
    for (int x = 0; x < size; x += 4) {
        const int16_t *t = tmp + x;
        const uint8_t *s = sv_base + x - 2 * stride;
        uint8_t *d = dst + x;

        int t0_0 = t[0], t0_1 = t[1], t0_2 = t[2], t0_3 = t[3]; t += size;
        int t1_0 = t[0], t1_1 = t[1], t1_2 = t[2], t1_3 = t[3]; t += size;
        int t2_0 = t[0], t2_1 = t[1], t2_2 = t[2], t2_3 = t[3]; t += size;
        int t3_0 = t[0], t3_1 = t[1], t3_2 = t[2], t3_3 = t[3]; t += size;
        int t4_0 = t[0], t4_1 = t[1], t4_2 = t[2], t4_3 = t[3]; t += size;

        uint32_t p0 = meq_ulw(s); s += stride;
        uint32_t p1 = meq_ulw(s); s += stride;
        uint32_t p2 = meq_ulw(s); s += stride;
        uint32_t p3 = meq_ulw(s); s += stride;
        uint32_t p4 = meq_ulw(s); s += stride;

        uint32_t r0l = p0 & mask, r0h = (p0 >> 8) & mask;
        uint32_t r1l = p1 & mask, r1h = (p1 >> 8) & mask;
        uint32_t r2l = p2 & mask, r2h = (p2 >> 8) & mask;
        uint32_t r3l = p3 & mask, r3h = (p3 >> 8) & mask;
        uint32_t r4l = p4 & mask, r4h = (p4 >> 8) & mask;

        for (int y = 0; y < size; ++y) {
            int t5_0 = t[0], t5_1 = t[1], t5_2 = t[2], t5_3 = t[3]; t += size;
            uint32_t p5 = meq_ulw(s); s += stride;
            uint32_t r5l = p5 & mask, r5h = (p5 >> 8) & mask;

            int q0 = meq_clip_u8(((t2_0 + t3_0) * 20 - (t1_0 + t4_0) * 5 + (t0_0 + t5_0) + 512) >> 10);
            int q1 = meq_clip_u8(((t2_1 + t3_1) * 20 - (t1_1 + t4_1) * 5 + (t0_1 + t5_1) + 512) >> 10);
            int q2 = meq_clip_u8(((t2_2 + t3_2) * 20 - (t1_2 + t4_2) * 5 + (t0_2 + t5_2) + 512) >> 10);
            int q3 = meq_clip_u8(((t2_3 + t3_3) * 20 - (t1_3 + t4_3) * 5 + (t0_3 + t5_3) + 512) >> 10);
            uint32_t hv = (uint32_t)q0 | ((uint32_t)q1 << 8) | ((uint32_t)q2 << 16) | ((uint32_t)q3 << 24);
            uint32_t v  = meq_v6_4px(r0l, r0h, r1l, r1h, r2l, r2h, r3l, r3h, r4l, r4h, r5l, r5h);
            meq_store4(d, meq_avg4(v, hv), is_avg);
            d += stride;

            t0_0 = t1_0; t0_1 = t1_1; t0_2 = t1_2; t0_3 = t1_3;
            t1_0 = t2_0; t1_1 = t2_1; t1_2 = t2_2; t1_3 = t2_3;
            t2_0 = t3_0; t2_1 = t3_1; t2_2 = t3_2; t2_3 = t3_3;
            t3_0 = t4_0; t3_1 = t4_1; t3_2 = t4_2; t3_3 = t4_3;
            t4_0 = t5_0; t4_1 = t5_1; t4_2 = t5_2; t4_3 = t5_3;
            r0l = r1l; r0h = r1h;
            r1l = r2l; r1h = r2h;
            r2l = r3l; r2h = r3h;
            r3l = r4l; r3h = r4h;
            r4l = r5l; r4h = r5h;
        }
    }
}

/* Instantiate all 15 non-zero phases for sizes 16, 8, 4 (put + avg) */
#define MEQ_DEFINE_SIZE(S) \
static void meq_put_##S##_10(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 1, 0); } \
static void meq_put_##S##_20(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 0, 0); } \
static void meq_put_##S##_30(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 2, 0); } \
static void meq_put_##S##_01(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 1, 0); } \
static void meq_put_##S##_11(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 0, 0, 0); } \
static void meq_put_##S##_21(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 1, 0); } \
static void meq_put_##S##_31(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 1, 0, 0); } \
static void meq_put_##S##_02(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 0, 0); } \
static void meq_put_##S##_12(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_v(d, s, st, S, 0, 0); } \
static void meq_put_##S##_22(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 0, 0); } \
static void meq_put_##S##_32(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_v(d, s, st, S, 1, 0); } \
static void meq_put_##S##_03(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 2, 0); } \
static void meq_put_##S##_13(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 0, 1, 0); } \
static void meq_put_##S##_23(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 2, 0); } \
static void meq_put_##S##_33(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 1, 1, 0); } \
static void meq_avg_##S##_10(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 1, 1); } \
static void meq_avg_##S##_20(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 0, 1); } \
static void meq_avg_##S##_30(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_h(d, s, st, S, 2, 1); } \
static void meq_avg_##S##_01(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 1, 1); } \
static void meq_avg_##S##_11(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 0, 0, 1); } \
static void meq_avg_##S##_21(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 1, 1); } \
static void meq_avg_##S##_31(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 1, 0, 1); } \
static void meq_avg_##S##_02(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 0, 1); } \
static void meq_avg_##S##_12(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_v(d, s, st, S, 0, 1); } \
static void meq_avg_##S##_22(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 0, 1); } \
static void meq_avg_##S##_32(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_v(d, s, st, S, 1, 1); } \
static void meq_avg_##S##_03(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_v(d, s, st, S, 2, 1); } \
static void meq_avg_##S##_13(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 0, 1, 1); } \
static void meq_avg_##S##_23(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_hv_and_h(d, s, st, S, 2, 1); } \
static void meq_avg_##S##_33(uint8_t *d, const uint8_t *s, ptrdiff_t st) { meq_mc_diag(d, s, st, S, 1, 1, 1); }

MEQ_DEFINE_SIZE(16)
MEQ_DEFINE_SIZE(8)
MEQ_DEFINE_SIZE(4)
#undef MEQ_DEFINE_SIZE

#ifdef AVCODEC_H264QPEL_H
static void meq_install_qpel(H264QpelContext *c)
{
#define MEQ_BIND(IDX, S) do { \
    c->put_h264_qpel_pixels_tab[IDX][1]  = meq_put_##S##_10; \
    c->put_h264_qpel_pixels_tab[IDX][2]  = meq_put_##S##_20; \
    c->put_h264_qpel_pixels_tab[IDX][3]  = meq_put_##S##_30; \
    c->put_h264_qpel_pixels_tab[IDX][4]  = meq_put_##S##_01; \
    c->put_h264_qpel_pixels_tab[IDX][5]  = meq_put_##S##_11; \
    c->put_h264_qpel_pixels_tab[IDX][6]  = meq_put_##S##_21; \
    c->put_h264_qpel_pixels_tab[IDX][7]  = meq_put_##S##_31; \
    c->put_h264_qpel_pixels_tab[IDX][8]  = meq_put_##S##_02; \
    c->put_h264_qpel_pixels_tab[IDX][9]  = meq_put_##S##_12; \
    c->put_h264_qpel_pixels_tab[IDX][10] = meq_put_##S##_22; \
    c->put_h264_qpel_pixels_tab[IDX][11] = meq_put_##S##_32; \
    c->put_h264_qpel_pixels_tab[IDX][12] = meq_put_##S##_03; \
    c->put_h264_qpel_pixels_tab[IDX][13] = meq_put_##S##_13; \
    c->put_h264_qpel_pixels_tab[IDX][14] = meq_put_##S##_23; \
    c->put_h264_qpel_pixels_tab[IDX][15] = meq_put_##S##_33; \
    c->avg_h264_qpel_pixels_tab[IDX][1]  = meq_avg_##S##_10; \
    c->avg_h264_qpel_pixels_tab[IDX][2]  = meq_avg_##S##_20; \
    c->avg_h264_qpel_pixels_tab[IDX][3]  = meq_avg_##S##_30; \
    c->avg_h264_qpel_pixels_tab[IDX][4]  = meq_avg_##S##_01; \
    c->avg_h264_qpel_pixels_tab[IDX][5]  = meq_avg_##S##_11; \
    c->avg_h264_qpel_pixels_tab[IDX][6]  = meq_avg_##S##_21; \
    c->avg_h264_qpel_pixels_tab[IDX][7]  = meq_avg_##S##_31; \
    c->avg_h264_qpel_pixels_tab[IDX][8]  = meq_avg_##S##_02; \
    c->avg_h264_qpel_pixels_tab[IDX][9]  = meq_avg_##S##_12; \
    c->avg_h264_qpel_pixels_tab[IDX][10] = meq_avg_##S##_22; \
    c->avg_h264_qpel_pixels_tab[IDX][11] = meq_avg_##S##_32; \
    c->avg_h264_qpel_pixels_tab[IDX][12] = meq_avg_##S##_03; \
    c->avg_h264_qpel_pixels_tab[IDX][13] = meq_avg_##S##_13; \
    c->avg_h264_qpel_pixels_tab[IDX][14] = meq_avg_##S##_23; \
    c->avg_h264_qpel_pixels_tab[IDX][15] = meq_avg_##S##_33; \
} while (0)
    MEQ_BIND(0, 16);
    MEQ_BIND(1, 8);
    MEQ_BIND(2, 4);
#undef MEQ_BIND
}
#endif /* AVCODEC_H264QPEL_H */

#endif /* PSP_ME_QPEL_EXACT_H */
