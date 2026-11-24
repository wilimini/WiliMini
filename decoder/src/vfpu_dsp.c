/* FFmpeg 6.1.2, 8-bit H.264 SC kernels: VFPU 16x16 integer-pel copy plus a
 * packed integer average (the measured winner among the VFPU variants; see
 * git history for the others). They serve frames the ME pipeline hands back
 * to FFmpeg. The decoder thread runs with PSP_THREAD_ATTR_VFPU enabled. */
#include <stdint.h>
#include <string.h>
#include "h264qpel.h"
#include "h264chroma.h"
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
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; col += 4) {
            uint32_t a, b;
            __asm__ volatile(
                "ulw %0, 0(%2)\n\tlw %1, 0(%3)"
                : "=&r"(a), "=&r"(b) : "r"(src + col), "r"(dst + col) : "memory");
            uint32_t result = (a | b) - (((a ^ b) & 0xfefefefeu) >> 1);
            __asm__ volatile("sw %0, 0(%1)" :: "r"(result), "r"(dst + col) : "memory");
        }
        src += stride;
        dst += stride;
    }
#endif
}

static void vfpu_put16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
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
}

void __real_ff_h264qpel_init(H264QpelContext *, int);
void __wrap_ff_h264qpel_init(H264QpelContext *c, int bit_depth)
{
    __real_ff_h264qpel_init(c, bit_depth);
    if (bit_depth == 8) {
        scalar_put16 = c->put_h264_qpel_pixels_tab[0][0];
        c->put_h264_qpel_pixels_tab[0][0] = vfpu_put16;
        c->avg_h264_qpel_pixels_tab[0][0] = vfpu_avg16;
    }
}

void __real_ff_h264chroma_init(H264ChromaContext *, int);
void __wrap_ff_h264chroma_init(H264ChromaContext *c, int bit_depth)
{
    __real_ff_h264chroma_init(c, bit_depth);
#if PSP_PACKED_CHROMA
    if (bit_depth == 8) packed_chroma_init(c);
#endif
}
