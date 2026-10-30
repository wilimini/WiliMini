/* FFmpeg 6.1.2, 8-bit H.264. Real-PSP experiments:
 * 0: original C (this file is not linked)
 * 1: VFPU 16x16 integer-pel copy
 * 2: VFPU chroma interpolation (slower; experimental)
 * 3: VFPU copy and average
 * 4: VFPU copy + packed integer average (measured winner)
 * 5: integer copy + packed integer average (control)
 * Decoder runs on the main thread with PSP_THREAD_ATTR_VFPU enabled.
 */
#include <stdint.h>
#include <string.h>
#include "h264qpel.h"
#include "h264chroma.h"
#include "me_decode.h"
#include "approx_qpel.h"
#include "packed_chroma.h"

static qpel_mc_func scalar_put16;

static void vfpu_avg16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
#if PSP_UNROLLED_AVG
    /* Keep the exact byte-wise rounding while removing the four-iteration
     * inner loop and its per-word address calculations. */
#define AVG_WORD(OFFSET) do { \
    uint32_t a, b; \
    __asm__ volatile("ulw %0, " #OFFSET "(%2)\n\tlw %1, " #OFFSET "(%3)" \
        : "=&r"(a), "=&r"(b) : "r"(src), "r"(dst) : "memory"); \
    uint32_t result = (a | b) - (((a ^ b) & 0xfefefefeu) >> 1); \
    __asm__ volatile("sw %0, " #OFFSET "(%1)" :: "r"(result), "r"(dst) : "memory"); \
} while (0)
    for (int row = 0; row < 16; ++row) {
        AVG_WORD(0); AVG_WORD(4); AVG_WORD(8); AVG_WORD(12);
        src += stride;
        dst += stride;
    }
#undef AVG_WORD
#else
#if PSP_VFPU_DSP != 4 && PSP_VFPU_DSP != 5
    __asm__ volatile("viim.s S710, 257" ::: "memory");
#endif
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; col += 4) {
            uint32_t a, b;
            __asm__ volatile(
                "ulw %0, 0(%2)\n\tlw %1, 0(%3)"
                : "=&r"(a), "=&r"(b) : "r"(src + col), "r"(dst + col) : "memory");
#if PSP_VFPU_DSP == 4 || PSP_VFPU_DSP == 5
            uint32_t result = (a | b) - (((a ^ b) & 0xfefefefeu) >> 1);
            __asm__ volatile("sw %0, 0(%1)" :: "r"(result), "r"(dst + col) : "memory");
#else
            __asm__ volatile(
                "mtv %0, S000\n\tmtv %1, S010\n\t"
                "vc2i.s C000, S000\n\tvc2i.s C010, S010\n\t"
                "vi2f.q C000, C000, 24\n\tvi2f.q C010, C010, 24\n\t"
                "vadd.q C000, C000, C010\n\tvadd.q C000, C000, C710[x,x,x,x]\n\t"
                "vf2iz.q C000, C000, 22\n\tvi2uc.q S000, C000\n\tsv.s S000, 0(%2)"
                :: "r"(a ^ 0x80808080u), "r"(b ^ 0x80808080u), "r"(dst + col) : "memory");
#endif
        }
        src += stride;
        dst += stride;
    }
#endif
}

static void vfpu_put16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
#if PSP_VFPU_DSP == 5
    for (int row = 0; row < 16; ++row) {
        uint32_t a, b, c, d;
        __asm__ volatile(
            "ulw %0, 0(%4)\n\tulw %1, 4(%4)\n\t"
            "ulw %2, 8(%4)\n\tulw %3, 12(%4)\n\t"
            "sw %0, 0(%5)\n\tsw %1, 4(%5)\n\tsw %2, 8(%5)\n\tsw %3, 12(%5)"
            : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d)
            : "r"(src), "r"(dst) : "memory");
        src += stride;
        dst += stride;
    }
#else
    /* VFPU word loads require 4-byte alignment, quad stores require 16. */
    if (((uintptr_t)dst | (uintptr_t)stride) & 15) {
        scalar_put16(dst, src, stride);
        return;
    }
    if ((uintptr_t)src & 3) {
        for (int row = 0; row < 16; ++row) {
            uint32_t a, b, c, d;
            __asm__ volatile(
                "ulw %0, 0(%4)\n\tulw %1, 4(%4)\n\t"
                "ulw %2, 8(%4)\n\tulw %3, 12(%4)\n\t"
#if PSP_INTEGER_UNALIGNED_COPY
                "sw %0, 0(%5)\n\tsw %1, 4(%5)\n\tsw %2, 8(%5)\n\tsw %3, 12(%5)"
#else
                "mtv %0, S000\n\tmtv %1, S001\n\tmtv %2, S002\n\tmtv %3, S003\n\t"
                "sv.q C000, 0(%5)"
#endif
                : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d)
                : "r"(src), "r"(dst) : "memory");
            src += stride;
            dst += stride;
        }
        return;
    }
    for (int row = 0; row < 16; ++row) {
        __asm__ volatile("ulv.q C000, 0(%0)\n\tsv.q C000, 0(%1)"
                         :: "r"(src), "r"(dst) : "memory");
        src += stride;
        dst += stride;
    }
#endif
}

void __real_ff_h264qpel_init(H264QpelContext *, int);
void __wrap_ff_h264qpel_init(H264QpelContext *c, int bit_depth)
{
    __real_ff_h264qpel_init(c, bit_depth);
    if (bit_depth == 8 && (PSP_VFPU_DSP == 1 || PSP_VFPU_DSP >= 3)) {
        scalar_put16 = c->put_h264_qpel_pixels_tab[0][0];
        c->put_h264_qpel_pixels_tab[0][0] = vfpu_put16;
        if (PSP_VFPU_DSP >= 3)
            c->avg_h264_qpel_pixels_tab[0][0] = vfpu_avg16;
    }
#if PSP_ME_DECODE
    me_decode_qpel_init(c, bit_depth);
#endif
#if PSP_APPROX_QPEL
    if (bit_depth == 8) approx_qpel_init(c);
#endif
}

static h264_chroma_mc_func scalar_chroma_put8, scalar_chroma_avg8;

static inline __attribute__((always_inline)) void vfpu_chroma8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                         int h, int x, int y, int average)
{
    if (!x || !y) {
        (average ? scalar_chroma_avg8 : scalar_chroma_put8)(dst, src, stride, h, x, y);
        return;
    }
    const int weights[4] __attribute__((aligned(16))) = {
        (8-x)*(8-y), x*(8-y), (8-x)*y, x*y
    };
    __asm__ volatile("lv.q C700, 0(%0)\n\tvi2f.q C700, C700, 0\n\tviim.s S710, 8224\n\t"
                     :: "r"(weights) : "memory");
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < 8; col += 4) {
            uint32_t a, b, c, d, result;
            __asm__ volatile(
                "ulw %0, 0(%4)\n\tulw %1, 1(%4)\n\t"
                "ulw %2, 0(%5)\n\tulw %3, 1(%5)\n\t"
                : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(d)
                : "r"(src + col), "r"(src + stride + col) : "memory");
            __asm__ volatile(
                "mtv %1, S000\n\tmtv %2, S010\n\tmtv %3, S020\n\tmtv %4, S030\n\t"
                "vc2i.s C000, S000\n\tvc2i.s C010, S010\n\tvc2i.s C020, S020\n\tvc2i.s C030, S030\n\t"
                "vi2f.q C000, C000, 24\n\tvi2f.q C010, C010, 24\n\t"
                "vi2f.q C020, C020, 24\n\tvi2f.q C030, C030, 24\n\t"
                "vscl.q C000, C000, S700\n\tvscl.q C010, C010, S701\n\t"
                "vscl.q C020, C020, S702\n\tvscl.q C030, C030, S703\n\t"
                "vadd.q C000, C000, C010\n\tvadd.q C020, C020, C030\n\t"
                "vadd.q C000, C000, C020\n\tvadd.q C000, C000, C710[x,x,x,x]\n\t"
                "vf2iz.q C000, C000, 17\n\tvi2uc.q S000, C000\n\tmfv %0, S000\n\t"
                : "=r"(result)
                : "r"(a ^ 0x80808080u), "r"(b ^ 0x80808080u),
                  "r"(c ^ 0x80808080u), "r"(d ^ 0x80808080u) : "memory");
            if (average) {
                uint32_t old;
                __asm__ volatile("lw %0, 0(%1)" : "=r"(old) : "r"(dst + col) : "memory");
                result = (old | result) - (((old ^ result) & 0xfefefefeu) >> 1);
            }
            __asm__ volatile("sw %0, 0(%1)" :: "r"(result), "r"(dst + col) : "memory");
        }
        dst += stride;
        src += stride;
    }
}

static void vfpu_chroma_put8(uint8_t *d, const uint8_t *s, ptrdiff_t stride, int h, int x, int y)
{ vfpu_chroma8(d, s, stride, h, x, y, 0); }
static void vfpu_chroma_avg8(uint8_t *d, const uint8_t *s, ptrdiff_t stride, int h, int x, int y)
{ vfpu_chroma8(d, s, stride, h, x, y, 1); }

/* Round eighth-pixel chroma positions to the nearest source pixel. Keep the
 * existing zero-phase kernels, including their stride/height handling. */
static h264_chroma_mc_func nearest_chroma_integer[2][4];
unsigned nearest_chroma_test_failures;
#define NEAREST_CHROMA(N) \
static void nearest_chroma_put_##N(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y) \
{ nearest_chroma_integer[0][N](d, s + (x >= 4) + (y >= 4) * st, st, h, 0, 0); } \
static void nearest_chroma_avg_##N(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y) \
{ nearest_chroma_integer[1][N](d, s + (x >= 4) + (y >= 4) * st, st, h, 0, 0); }
NEAREST_CHROMA(0)
NEAREST_CHROMA(1)
NEAREST_CHROMA(2)
NEAREST_CHROMA(3)
#undef NEAREST_CHROMA
static void nearest_chroma_init(H264ChromaContext *c)
{
    static const h264_chroma_mc_func put[4] = {
        nearest_chroma_put_0, nearest_chroma_put_1, nearest_chroma_put_2, nearest_chroma_put_3 };
    static const h264_chroma_mc_func avg[4] = {
        nearest_chroma_avg_0, nearest_chroma_avg_1, nearest_chroma_avg_2, nearest_chroma_avg_3 };
    for (unsigned i = 0; i < 4; ++i) {
        nearest_chroma_integer[0][i] = c->put_h264_chroma_pixels_tab[i];
        nearest_chroma_integer[1][i] = c->avg_h264_chroma_pixels_tab[i];
        c->put_h264_chroma_pixels_tab[i] = put[i];
        c->avg_h264_chroma_pixels_tab[i] = avg[i];
    }
    /* Exercise real PSP kernels against scalar nearest-neighbour sampling,
     * including source alignment, rounding thresholds and destination guards. */
    uint8_t source[48 * 20] __attribute__((aligned(16)));
    uint8_t target[48 * 20] __attribute__((aligned(16)));
    nearest_chroma_test_failures = 0;
    for (unsigned i = 0; i < sizeof(source); ++i)
        source[i] = (i * 73 + (i >> 3) * 19) & 255;
    for (unsigned st = 32; st <= 48; st += 16)
        for (unsigned offset = 0; offset < 4; ++offset)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned phase = 0; phase < 64; ++phase)
                    for (unsigned average = 0; average < 2; ++average) {
                        unsigned width = 8 >> size, height = width * 2;
                        unsigned x = phase & 7, y = phase >> 3;
                        memset(target, 91, sizeof(target));
                        (average ? avg[size] : put[size])(target, source + offset, st, height, x, y);
                        for (unsigned i = 0; i < sizeof(target); ++i) {
                            unsigned row = i / st, col = i % st, value = 91;
                            if (row < height && col < width) {
                                value = source[offset + (row + (y >= 4)) * st + col + (x >= 4)];
                                if (average) value = (91 + value + 1) >> 1;
                            }
                            if (target[i] != value) ++nearest_chroma_test_failures;
                        }
                    }
}

void __real_ff_h264chroma_init(H264ChromaContext *, int);
void __wrap_ff_h264chroma_init(H264ChromaContext *c, int bit_depth)
{
    __real_ff_h264chroma_init(c, bit_depth);
    if (bit_depth == 8 && PSP_VFPU_DSP == 2) {
        scalar_chroma_put8 = c->put_h264_chroma_pixels_tab[0];
        scalar_chroma_avg8 = c->avg_h264_chroma_pixels_tab[0];
        c->put_h264_chroma_pixels_tab[0] = vfpu_chroma_put8;
        c->avg_h264_chroma_pixels_tab[0] = vfpu_chroma_avg8;
    }
#if PSP_ME_DECODE == 4
    me_decode_chroma_init(c, bit_depth);
#endif
#if PSP_PACKED_CHROMA
    if (bit_depth == 8) packed_chroma_init(c);
#endif
    if (bit_depth == 8) nearest_chroma_init(c);
}
