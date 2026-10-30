/* Opt-in, deliberately non-bit-exact H.264 motion reconstruction.
 * Replace six-tap luma interpolation with packed bilinear interpolation.
 * Reference-frame errors can propagate; keep disabled in the exact decoder. */
#ifndef PSP_LAB_APPROX_QPEL_H
#define PSP_LAB_APPROX_QPEL_H
#if PSP_APPROX_QPEL
#if PSP_APPROX_QPEL != 1 && PSP_APPROX_QPEL != 2
#error Unsupported approximate interpolation mode
#endif
static qpel_mc_func approx_integer[2][3];

static inline __attribute__((always_inline)) uint32_t approx_load4(const uint8_t *p)
{
    uint32_t value;
    __asm__ volatile("ulw %0, 0(%1)" : "=r"(value) : "r"(p) : "memory");
    return value;
}

static inline __attribute__((always_inline)) void approx_bilinear(
    uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
    unsigned size, unsigned phase, unsigned average)
{
    const unsigned x = phase & 3, y = phase >> 2;
    const unsigned wa = (4 - x) * (4 - y), wb = x * (4 - y);
    const unsigned wc = (4 - x) * y, wd = x * y;
    for (unsigned row = 0; row < size; ++row) {
        for (unsigned col = 0; col < size; col += 4) {
            uint32_t a = approx_load4(src + col);
            uint32_t b = x ? approx_load4(src + col + 1) : 0;
            uint32_t c = y ? approx_load4(src + col + stride) : 0;
            uint32_t d = x && y ? approx_load4(src + col + stride + 1) : 0;
            const uint32_t mask = 0x00ff00ffu;
            uint32_t lo = ((a & mask) * wa + (b & mask) * wb +
                           (c & mask) * wc + (d & mask) * wd + 0x00080008u) >> 4;
            uint32_t hi = (((a >> 8) & mask) * wa + ((b >> 8) & mask) * wb +
                           ((c >> 8) & mask) * wc + ((d >> 8) & mask) * wd + 0x00080008u) >> 4;
            uint32_t value = (lo & mask) | ((hi & mask) << 8);
            if (average) {
                /* 4/8/16-pixel FFmpeg partition destinations are word aligned. */
                uint32_t old;
                __asm__ volatile("lw %0, 0(%1)" : "=r"(old) : "r"(dst + col) : "memory");
                value = (old | value) - (((old ^ value) & 0xfefefefeu) >> 1);
            }
            __asm__ volatile("sw %0, 0(%1)" :: "r"(value), "r"(dst + col) : "memory");
        }
        src += stride;
        dst += stride;
    }
}

static inline __attribute__((always_inline)) void approx_predict(
    uint8_t *d, const uint8_t *s, ptrdiff_t st, unsigned index, unsigned phase, unsigned avg)
{
#if PSP_APPROX_QPEL == 2
    s += ((phase & 3) >= 2) + ((phase >> 2) >= 2) * st;
    approx_integer[avg][index](d, s, st);
#else
    approx_bilinear(d, s, st, 16 >> index, phase, avg);
#endif
}
#define APPROX_PHASE(S,N) \
static void approx_put_##S##_##N(uint8_t *d,const uint8_t *s,ptrdiff_t st) \
{ approx_predict(d,s,st,S,N,0); } \
static void approx_avg_##S##_##N(uint8_t *d,const uint8_t *s,ptrdiff_t st) \
{ approx_predict(d,s,st,S,N,1); }
#define APPROX_SIZE(S) \
APPROX_PHASE(S,1) APPROX_PHASE(S,2) APPROX_PHASE(S,3) \
APPROX_PHASE(S,4) APPROX_PHASE(S,5) APPROX_PHASE(S,6) APPROX_PHASE(S,7) \
APPROX_PHASE(S,8) APPROX_PHASE(S,9) APPROX_PHASE(S,10) APPROX_PHASE(S,11) \
APPROX_PHASE(S,12) APPROX_PHASE(S,13) APPROX_PHASE(S,14) APPROX_PHASE(S,15)
APPROX_SIZE(0) APPROX_SIZE(1) APPROX_SIZE(2)
#define APPROX_PUT(S) { NULL, approx_put_##S##_1, approx_put_##S##_2, approx_put_##S##_3, \
approx_put_##S##_4, approx_put_##S##_5, approx_put_##S##_6, approx_put_##S##_7, \
approx_put_##S##_8, approx_put_##S##_9, approx_put_##S##_10, approx_put_##S##_11, \
approx_put_##S##_12, approx_put_##S##_13, approx_put_##S##_14, approx_put_##S##_15 }
#define APPROX_AVG(S) { NULL, approx_avg_##S##_1, approx_avg_##S##_2, approx_avg_##S##_3, \
approx_avg_##S##_4, approx_avg_##S##_5, approx_avg_##S##_6, approx_avg_##S##_7, \
approx_avg_##S##_8, approx_avg_##S##_9, approx_avg_##S##_10, approx_avg_##S##_11, \
approx_avg_##S##_12, approx_avg_##S##_13, approx_avg_##S##_14, approx_avg_##S##_15 }
static void approx_qpel_init(H264QpelContext *c)
{
    static const qpel_mc_func put[3][16] = { APPROX_PUT(0), APPROX_PUT(1), APPROX_PUT(2) };
    static const qpel_mc_func avg[3][16] = { APPROX_AVG(0), APPROX_AVG(1), APPROX_AVG(2) };
    for (unsigned size = 0; size < 3; ++size) {
        approx_integer[0][size] = c->put_h264_qpel_pixels_tab[size][0];
        approx_integer[1][size] = c->avg_h264_qpel_pixels_tab[size][0];
        for (unsigned phase = 1; phase < 16; ++phase) {
            c->put_h264_qpel_pixels_tab[size][phase] = put[size][phase];
            c->avg_h264_qpel_pixels_tab[size][phase] = avg[size][phase];
        }
    }
}
#undef APPROX_PHASE
#undef APPROX_SIZE
#undef APPROX_PUT
#undef APPROX_AVG
#endif
#endif
