/* Whole-frame SC (CABAC) <-> ME + VME2 (pixel reconstruction + RGB565 render)
 * parallel pipeline for 8-bit 4:2:0 progressive H.264 on Sony PSP (Custom Core).
 *
 * Design invariants:
 * - SC runs demux, audio, RGB pacer, and H.264 CABAC/CAVLC only. SC never
 *   reads or writes YUV pixel planes on pipelined frames.
 * - ME owns a persistent worker loop with a FIFO command ring (RECON + RENDER).
 *   ME runs all Intra prediction, Motion Compensation (including emulated edges
 *   and weighted prediction), pure-integer IDCT (zero VFPU instructions), and
 *   VME2 RGB565 rendering directly to the pacer's RGB565 slot.
 * - Each in-flight frame holds AVFrame references to its target and reference
 *   pictures until ME completes that sequence number.
 */
#include <pspkernel.h>
#include <me-core-mapper/me-core-mapper.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "h264dec.h"
#include "me_bridge.h"
#include "full_render.h"
#include "frame_hash.h"
#if PSP_VME_RENDER
#include "vme_render.h"
#endif
#include "me_qpel_exact.h"
#include "me_frame_pipeline.h"

#define MEFP_RING_DEPTH     8
#define MEFP_FRAME_SLOTS    2
#define MEFP_MAX_MBS        1680
#define MEFP_MAX_SLICES     8
#define MEFP_INIT_MC_PARTS  2048
#define MEFP_MAX_MC_PARTS   26880
#define MEFP_INIT_COEFFS    65536
#define MEFP_MAX_COEFFS     655360
#define MEFP_MAX_HELD_REFS  32
#define MEFP_MAX_STRIDE     1024
#define MEFP_MAX_UVSTRIDE   512
#define MEFP_WAIT_TIMEOUT   1500000u /* 1.5 s safety timeout */

enum {
    MEFP_OP_RECON  = 1,
    MEFP_OP_RENDER = 2,
    MEFP_OP_SYNC   = 3,
};

enum {
    MEFP_MB_INTER     = 0,
    MEFP_MB_INTRA16   = 1,
    MEFP_MB_INTRA4    = 2,
    MEFP_MB_INTRA8    = 3,
    MEFP_MB_PCM       = 4,
};

#define MEFP_FLAG_DCT8   (1u << 0)

enum {
    MEFP_PART_16x16 = 0,
    MEFP_PART_16x8  = 1,
    MEFP_PART_8x16  = 2,
    MEFP_PART_8x8   = 3,
    MEFP_PART_8x4   = 4,
    MEFP_PART_4x8   = 5,
    MEFP_PART_4x4   = 6,
};

typedef struct {
    uint8_t x_off;      /* in chroma pixels (0,2,4,6); luma = 2*x_off */
    uint8_t y_off;      /* in chroma pixels (0,2,4,6); luma = 2*y_off */
    uint8_t shape;      /* MEFP_PART_* */
    uint8_t dir_mask;   /* bit0=list0, bit1=list1 */
    uint8_t ref0;
    uint8_t ref1;
    uint8_t pad0;
    uint8_t pad1;
    int16_t mv[2][2];   /* raw sl->mv_cache[list][scan8[n]][0..1] */
} MefpMcPart;

typedef struct {
    uint8_t  mb_x;
    uint8_t  mb_y;
    uint8_t  kind;
    uint8_t  flags;     /* bit0: DCT8, bits1..3: slice_idx */
    uint8_t  chroma_pred_mode;
    uint8_t  intra16_pred_mode;
    uint8_t  mc_count;
    uint8_t  pad0;
    uint16_t tl_avail;
    uint16_t tr_avail;
    uint16_t mc_start;
    uint16_t intra4_start;
    uint32_t coeff_start;
    uint32_t luma_res_mask;
    uint16_t chroma_res_mask;
    uint16_t pad1;
} MefpMbCmd;

typedef struct {
    uint8_t *ref_data[2][16][3];
    uint8_t  ref_count[2];
    uint8_t  use_weight;
    uint8_t  use_weight_chroma;
    uint8_t  luma_log2_denom;
    uint8_t  chroma_log2_denom;
    int16_t  luma_weight[16][2][2];
    int16_t  chroma_weight[16][2][2][2];
    int16_t  implicit_weight[16][16][2];
} MefpSliceInfo;

typedef struct __attribute__((aligned(64))) {
    uint8_t *cur_data[3];
    int      linesize;
    int      uvlinesize;
    int      mb_width;
    int      mb_height;
    int      block_offset[48];
    unsigned mb_count;
    unsigned slice_count;
    unsigned mc_count;
    unsigned intra4_count;
    unsigned coeff_count;
    unsigned mc_cap;
    unsigned coeff_cap;
    MefpMcPart   *mc_parts;
    int16_t      *coeff_pool;
    MefpSliceInfo slices[MEFP_MAX_SLICES];
    MefpMbCmd     mbs[MEFP_MAX_MBS];
    uint8_t       intra4_modes[MEFP_MAX_MBS * 16];
} MefpFrameSlot;

typedef struct __attribute__((aligned(64))) {
    unsigned       op;
    MefpFrameSlot *slot;        /* MEFP_OP_RECON */
    const uint8_t *planes[3];   /* MEFP_OP_RENDER */
    int            strides[3];  /* MEFP_OP_RENDER */
    int            width;       /* MEFP_OP_RENDER */
    int            height;      /* MEFP_OP_RENDER */
    void          *rgb_dst;     /* MEFP_OP_RENDER */
    int            output_fmt;  /* MEFP_OP_RENDER */
    int            hash_frame;  /* MEFP_OP_RENDER */
    int            sc_written;  /* MEFP_OP_RENDER: invalidate ME cache if SC decoded */
} MefpCmd;

typedef struct __attribute__((aligned(64))) {
    /* Cache line 0: SC producer controls */
    volatile unsigned requested;
    volatile unsigned stop;
    unsigned          producer_pad[14];
    /* Cache line 1: ME consumer controls & telemetry */
    volatile unsigned completed;
    volatile unsigned ready;
    volatile unsigned exited;
    volatile int      error;
    volatile uint32_t frame_hash;
    volatile uint32_t me_recon_us_lo;
    volatile uint32_t me_recon_us_hi;
    volatile uint32_t me_render_us_lo;
    volatile uint32_t me_render_us_hi;
    volatile unsigned me_intra_mbs;
    volatile unsigned me_inter_mbs;
    volatile unsigned me_trace_op;
    volatile unsigned me_trace_mb;
    unsigned          consumer_pad[3];
    /* Cache line 2+: command ring */
    MefpCmd           cmds[MEFP_RING_DEPTH];
} MefpRing;

_Static_assert(offsetof(MefpRing, completed) == 64, "ring controls must be on separate cache lines");
_Static_assert(offsetof(MefpRing, cmds) == 128, "ring cmds must be 64-byte aligned");

typedef struct __attribute__((aligned(64))) {
    H264QpelContext   qpel;
    H264ChromaContext chroma;
    H264PredContext   hpc;
    H264DSPContext    h264dsp;
    YuvRgbTables      yuv_tables;
} MefpDsp;

/* Shared static state */
static MefpRing mefp_ring;
static MefpDsp  mefp_dsp;
static MeJob    mefp_job __attribute__((aligned(64)));

/* ME-only scratchpads placed in ME's dedicated 2 MiB eDRAM (cached KSEG0
 * 0x80120000..0x80130000, above VME_EDRAM_OUT16 and ~832 KiB below ME stack).
 * Saves 60 KiB of Main RAM BSS and avoids Main RAM bus contention on edge/bipred. */
#define MEFP_EDRAM_EMU_BUF    0x80120000u /* 28 KiB: MEFP_MAX_STRIDE * 28 */
#define MEFP_EDRAM_BIPRED_BUF 0x80128000u /* 32 KiB: MEFP_MAX_STRIDE * 32 */

/* SC-only state */
static MefpFrameSlot *mefp_slots[MEFP_FRAME_SLOTS];
static uint32_t       mefp_slot_seq[MEFP_FRAME_SLOTS];
static AVFrame       *mefp_slot_cur[MEFP_FRAME_SLOTS];
static AVFrame       *mefp_slot_refs[MEFP_FRAME_SLOTS][MEFP_MAX_HELD_REFS];
static int            mefp_slot_ref_count[MEFP_FRAME_SLOTS];

typedef struct {
    uint32_t seq;
    int64_t  pts;
    AVFrame *frame;
} MefpPendingRender;

static MefpPendingRender mefp_renders[MEFP_RING_DEPTH];
static unsigned          mefp_render_head, mefp_render_tail;

static int              mefp_running;
static int              mefp_output_format;
static unsigned         mefp_max_inflight_renders = 2;
static MefpAcquireRgbFn mefp_acquire_cb;
static MefpPublishRgbFn mefp_publish_cb;
static MefpReleaseRgbFn mefp_release_cb;

static int            mefp_open_slot_idx = -1;
static int            mefp_open_slice_idx = -1;
static const AVFrame *mefp_open_cur_f;
static int            mefp_frame_fallback;
static int            mefp_any_sc_written;
static unsigned       mefp_next_slot;

/* SC stats */
static unsigned mefp_stat_recon_frames;
static unsigned mefp_stat_fallback_frames;
static unsigned mefp_stat_render_submitted;
static unsigned mefp_stat_render_completed;
static unsigned mefp_stat_wait_us;
static uint64_t mefp_stat_capture_us;
static int      mefp_stat_error;

extern void __real_ff_h264qpel_init(H264QpelContext *, int);
extern void __real_ff_h264chroma_init(H264ChromaContext *, int);
extern void ff_h264_pred_init(H264PredContext *, int, int, int);
extern void ff_h264dsp_init(H264DSPContext *, int, int);

static inline volatile MefpRing *mefp_sc_ctrl(void)
{
    return (volatile MefpRing *)((uintptr_t)&mefp_ring | 0x40000000u);
}

/* =========================================================================
 * Pure-integer Allegrex DSP kernels for ME (ZERO VFPU, ZERO libc calls)
 * ========================================================================= */

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

/* =========================================================================
 * ME-side Motion Compensation & Whole-Frame Reconstruction
 * ========================================================================= */

/* ME-side Motion Compensation & Whole-Frame Reconstruction */

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

    uint8_t *emu_buf    = (uint8_t *)MEFP_EDRAM_EMU_BUF;
    uint8_t *bipred_buf = (uint8_t *)MEFP_EDRAM_BIPRED_BUF;
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

/* =========================================================================
 * Persistent ME Worker (Custom Core)
 * ========================================================================= */

static int __attribute__((noinline)) mefp_me_worker(int index, void *param)
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
#if PSP_VME_RENDER
    vme_me_wipe();
#endif

    unsigned done = 0;
    q->frame_hash = 2166136261u;
    q->ready = 1;
    __asm__ volatile("sync" ::: "memory");

    while (!q->stop || done != q->requested) {
        if (done == q->requested) {
            __asm__ volatile("nop; nop; nop; nop");
            continue;
        }
        MefpCmd *cmd = &cached->cmds[done & (MEFP_RING_DEPTH - 1)];
        /* Flush any previous ME writes and drop stale lines for cmd/slot.
         * ME D-cache is 16 KiB (~10 us total), guaranteeing complete coherency. */
        meCoreDcacheWritebackInvalidateAll();

        q->me_trace_op = cmd->op;
        if (cmd->op == MEFP_OP_RECON) {
            MefpFrameSlot *slot = (MefpFrameSlot *)((uintptr_t)cmd->slot | 0x80000000u);
            me_reconstruct_frame(dsp, slot, q);
        } else if (cmd->op == MEFP_OP_RENDER) {
            const uint8_t *planes[3];
            for (int p = 0; p < 3; ++p)
                planes[p] = (const uint8_t *)((uintptr_t)cmd->planes[p] | 0x80000000u);
            if (cmd->hash_frame) {
                uint32_t h = q->frame_hash;
                h = hash_visible_plane(h, planes[0], cmd->width, cmd->height, cmd->strides[0]);
                h = hash_visible_plane(h, planes[1], cmd->width / 2, cmd->height / 2, cmd->strides[1]);
                h = hash_visible_plane(h, planes[2], cmd->width / 2, cmd->height / 2, cmd->strides[2]);
                q->frame_hash = h;
            }
            int used_vme_dma = 0;
            void *out = (void *)((uintptr_t)cmd->rgb_dst | 0x80000000u);
            if (cmd->output_fmt == 0) {
                if (cmd->width == 640 && cmd->height == 360) {
#if PSP_VME_RENDER
                    if (*(volatile int *)((uintptr_t)&me_vme_wiped | 0xA0000000u)) {
                        vme_render_565(planes, cmd->strides, cmd->rgb_dst);
                        used_vme_dma = 1;
                    } else {
                        full_render_planes_565(planes, cmd->strides, out, &dsp->yuv_tables);
                    }
#else
                    full_render_planes_565(planes, cmd->strides, out, &dsp->yuv_tables);
#endif
                } else {
                    full_render_scaled_565(planes, cmd->strides, cmd->width, cmd->height, out, &dsp->yuv_tables);
                }
            } else if (cmd->width == 640 && cmd->height == 360) {
                full_render_planes_8888(planes, cmd->strides, out, &dsp->yuv_tables);
            } else {
                full_render_scaled_8888(planes, cmd->strides, cmd->width, cmd->height, out, &dsp->yuv_tables);
            }
            if (!used_vme_dma)
                meCoreDcacheWritebackRange(out, 512 * 272 * (cmd->output_fmt == 0 ? 2 : 4));
        } else if (cmd->op == MEFP_OP_SYNC) {
            /* Already wrote back & invalidated ME D-cache above. */
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

/* =========================================================================
 * SC Ring & Reference Management
 * ========================================================================= */

void mefp_poll_renders(void)
{
    if (!mefp_running) return;
    unsigned comp = mefp_sc_ctrl()->completed;
    while (mefp_render_head != mefp_render_tail) {
        MefpPendingRender *r = &mefp_renders[mefp_render_head & (MEFP_RING_DEPTH - 1)];
        if ((int)(comp - r->seq) < 0) break;
        if (mefp_publish_cb) mefp_publish_cb(r->pts);
        av_frame_unref(r->frame);
        ++mefp_render_head;
        ++mefp_stat_render_completed;
    }
}

static int mefp_wait_seq(uint32_t seq)
{
    if (!mefp_running) return 0;
    if (mefp_stat_error) return mefp_stat_error;
    unsigned t0 = sceKernelGetSystemTimeLow();
    while ((int)(mefp_sc_ctrl()->completed - seq) < 0) {
        mefp_poll_renders();
        if (sceKernelGetSystemTimeLow() - t0 > MEFP_WAIT_TIMEOUT) {
            mefp_stat_error = -211;
            return mefp_stat_error;
        }
    }
    mefp_stat_wait_us += sceKernelGetSystemTimeLow() - t0;
    mefp_poll_renders();
    if (mefp_sc_ctrl()->error) {
        mefp_stat_error = mefp_sc_ctrl()->error;
        return mefp_stat_error;
    }
    return 0;
}

static MefpCmd *mefp_reserve_cmd(void)
{
    volatile MefpRing *ctrl = mefp_sc_ctrl();
    unsigned req = ctrl->requested;
    if (req - ctrl->completed >= MEFP_RING_DEPTH) {
        if (mefp_wait_seq(req - MEFP_RING_DEPTH + 1) < 0) return NULL;
    }
    return &mefp_ring.cmds[req & (MEFP_RING_DEPTH - 1)];
}

static uint32_t mefp_submit_cmd(MefpCmd *cmd)
{
    (void)cmd;
    sceKernelDcacheWritebackInvalidateAll();
    __asm__ volatile("sync" ::: "memory");
    volatile MefpRing *ctrl = mefp_sc_ctrl();
    uint32_t seq = ctrl->requested + 1u;
    ctrl->requested = seq;
    __asm__ volatile("sync" ::: "memory");
    return seq;
}

static void mefp_release_slot_refs(int idx)
{
    if (mefp_slot_cur[idx]) av_frame_unref(mefp_slot_cur[idx]);
    for (int i = 0; i < mefp_slot_ref_count[idx]; ++i)
        if (mefp_slot_refs[idx][i]) av_frame_unref(mefp_slot_refs[idx][i]);
    mefp_slot_ref_count[idx] = 0;
}

static void mefp_abort_open_slot(void)
{
    if (mefp_open_slot_idx >= 0) {
        mefp_release_slot_refs(mefp_open_slot_idx);
        mefp_open_slot_idx = -1;
        mefp_open_slice_idx = -1;
        mefp_open_cur_f = NULL;
    }
}

int mefp_drain(void)
{
    if (!mefp_running) return 0;
    mefp_abort_open_slot();
    MefpCmd *cmd = mefp_reserve_cmd();
    if (!cmd) return mefp_stat_error;
    memset(cmd, 0, sizeof(*cmd));
    cmd->op = MEFP_OP_SYNC;
    uint32_t seq = mefp_submit_cmd(cmd);
    int ret = mefp_wait_seq(seq);
    mefp_poll_renders();
    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i)
        mefp_release_slot_refs(i);
    sceKernelDcacheWritebackInvalidateAll();
    return ret;
}

/* =========================================================================
 * FFmpeg H.264 Hooks (SC side)
 * ========================================================================= */

static int mefp_hold_ref_frame(int slot_idx, const AVFrame *ref_f)
{
    if (!ref_f || !ref_f->data[0]) return 0;
    for (int i = 0; i < mefp_slot_ref_count[slot_idx]; ++i) {
        if (mefp_slot_refs[slot_idx][i]->data[0] == ref_f->data[0])
            return 0;
    }
    if (mefp_slot_ref_count[slot_idx] >= MEFP_MAX_HELD_REFS) return -1;
    AVFrame *dst = mefp_slot_refs[slot_idx][mefp_slot_ref_count[slot_idx]];
    if (av_frame_ref(dst, ref_f) < 0) return -1;
    ++mefp_slot_ref_count[slot_idx];
    return 0;
}

static int mefp_hook_slice_begin(const H264Context *h, H264SliceContext *sl)
{
    if (!mefp_running || mefp_stat_error) return 0;
    mefp_poll_renders();

    const AVFrame *cur_f = h->cur_pic_ptr ? h->cur_pic_ptr->f : NULL;
    if (mefp_open_slot_idx >= 0 && mefp_open_cur_f != cur_f) {
        /* Previous frame errored before field_end; discard stale capture. */
        mefp_abort_open_slot();
    }

    int eligible =
        cur_f && cur_f->data[0] && cur_f->data[1] && cur_f->data[2] &&
        cur_f->format == AV_PIX_FMT_YUV420P &&
        h->pixel_shift == 0 &&
        !CHROMA444(h) && !CHROMA422(h) &&
        h->picture_structure == PICT_FRAME &&
        !FRAME_MBAFF(h) &&
        !sl->is_complex &&
        !sl->deblocking_filter &&
        !(h->flags & AV_CODEC_FLAG_GRAY) &&
        !h->ps.sps->transform_bypass &&
        h->mb_width > 0 && h->mb_height > 0 &&
        h->mb_width * h->mb_height <= MEFP_MAX_MBS &&
        sl->linesize > 0 && sl->linesize <= MEFP_MAX_STRIDE &&
        sl->uvlinesize > 0 && sl->uvlinesize <= MEFP_MAX_UVSTRIDE &&
        sl->ref_count[0] <= 16 && sl->ref_count[1] <= 16;

    if (!eligible) {
        mefp_abort_open_slot();
        mefp_drain();
        mefp_frame_fallback = 1;
        mefp_any_sc_written = 1;
        ++mefp_stat_fallback_frames;
        return 0;
    }

    if (mefp_open_slot_idx < 0) {
        int idx = (int)(mefp_next_slot & (MEFP_FRAME_SLOTS - 1));
        if (mefp_slot_seq[idx]) {
            if (mefp_wait_seq(mefp_slot_seq[idx]) < 0) {
                mefp_frame_fallback = 1;
                return 0;
            }
            mefp_release_slot_refs(idx);
        }
        MefpFrameSlot *s = mefp_slots[idx];
        s->cur_data[0] = cur_f->data[0];
        s->cur_data[1] = cur_f->data[1];
        s->cur_data[2] = cur_f->data[2];
        s->linesize    = (int)sl->linesize;
        s->uvlinesize  = (int)sl->uvlinesize;
        s->mb_width    = h->mb_width;
        s->mb_height   = h->mb_height;
        memcpy(s->block_offset, h->block_offset, sizeof(s->block_offset));
        s->mb_count     = 0;
        s->slice_count  = 0;
        s->mc_count     = 0;
        s->intra4_count = 0;
        s->coeff_count  = 0;

        if (av_frame_ref(mefp_slot_cur[idx], cur_f) < 0) {
            mefp_frame_fallback = 1;
            ++mefp_stat_fallback_frames;
            return 0;
        }
        mefp_open_slot_idx = idx;
        mefp_open_cur_f = cur_f;
        mefp_frame_fallback = 0;
        ++mefp_next_slot;
    }

    if (mefp_frame_fallback) return 0;

    MefpFrameSlot *s = mefp_slots[mefp_open_slot_idx];
    if (s->slice_count >= MEFP_MAX_SLICES) {
        mefp_abort_open_slot();
        mefp_drain();
        mefp_frame_fallback = 1;
        mefp_any_sc_written = 1;
        ++mefp_stat_fallback_frames;
        return 0;
    }

    int sidx = (int)s->slice_count++;
    mefp_open_slice_idx = sidx;
    MefpSliceInfo *si = &s->slices[sidx];
    si->ref_count[0]      = (uint8_t)sl->ref_count[0];
    si->ref_count[1]      = (uint8_t)sl->ref_count[1];
    si->use_weight        = (uint8_t)sl->pwt.use_weight;
    si->use_weight_chroma = (uint8_t)sl->pwt.use_weight_chroma;
    si->luma_log2_denom   = (uint8_t)sl->pwt.luma_log2_weight_denom;
    si->chroma_log2_denom = (uint8_t)sl->pwt.chroma_log2_weight_denom;

    for (unsigned list = 0; list < sl->list_count && list < 2; ++list) {
        for (unsigned r = 0; r < sl->ref_count[list] && r < 16; ++r) {
            const H264Ref *ref = &sl->ref_list[list][r];
            si->ref_data[list][r][0] = ref->data[0];
            si->ref_data[list][r][1] = ref->data[1];
            si->ref_data[list][r][2] = ref->data[2];
            if (ref->parent && ref->parent->f) {
                if (mefp_hold_ref_frame(mefp_open_slot_idx, ref->parent->f) < 0) {
                    mefp_abort_open_slot();
                    mefp_drain();
                    mefp_frame_fallback = 1;
                    mefp_any_sc_written = 1;
                    ++mefp_stat_fallback_frames;
                    return 0;
                }
            }
        }
    }
    if (sl->pwt.use_weight == 1) {
        for (int r = 0; r < 16; ++r) {
            for (int l = 0; l < 2; ++l) {
                si->luma_weight[r][l][0] = (int16_t)sl->pwt.luma_weight[r][l][0];
                si->luma_weight[r][l][1] = (int16_t)sl->pwt.luma_weight[r][l][1];
                for (int c = 0; c < 2; ++c) {
                    si->chroma_weight[r][l][c][0] = (int16_t)sl->pwt.chroma_weight[r][l][c][0];
                    si->chroma_weight[r][l][c][1] = (int16_t)sl->pwt.chroma_weight[r][l][c][1];
                }
            }
        }
    } else if (sl->pwt.use_weight == 2) {
        for (int r0 = 0; r0 < 16; ++r0)
            for (int r1 = 0; r1 < 16; ++r1) {
                si->implicit_weight[r0][r1][0] = (int16_t)sl->pwt.implicit_weight[r0][r1][0];
                si->implicit_weight[r0][r1][1] = (int16_t)sl->pwt.implicit_weight[r0][r1][1];
            }
    }
    return 1;
}

static inline void mefp_emit_mc(MefpFrameSlot *s, const H264SliceContext *sl,
                                int n, uint8_t shape, uint8_t x_off, uint8_t y_off,
                                int list0, int list1)
{
    MefpMcPart *pt = &s->mc_parts[s->mc_count++];
    pt->x_off    = x_off;
    pt->y_off    = y_off;
    pt->shape    = shape;
    pt->dir_mask = (uint8_t)((list0 ? 1u : 0u) | (list1 ? 2u : 0u));
    pt->pad0 = pt->pad1 = 0;
    const int s8 = scan8[n];
    if (list0) {
        pt->ref0     = (uint8_t)sl->ref_cache[0][s8];
        pt->mv[0][0] = sl->mv_cache[0][s8][0];
        pt->mv[0][1] = sl->mv_cache[0][s8][1];
    } else {
        pt->ref0 = 0; pt->mv[0][0] = 0; pt->mv[0][1] = 0;
    }
    if (list1) {
        pt->ref1     = (uint8_t)sl->ref_cache[1][s8];
        pt->mv[1][0] = sl->mv_cache[1][s8][0];
        pt->mv[1][1] = sl->mv_cache[1][s8][1];
    } else {
        pt->ref1 = 0; pt->mv[1][0] = 0; pt->mv[1][1] = 0;
    }
}

static inline uint16_t mefp_pack_chroma(const H264Context *h, H264SliceContext *sl,
                                        int mb_type, MefpFrameSlot *s)
{
    if (!(sl->cbp & 0x30)) return 0;
    if (sl->non_zero_count_cache[scan8[CHROMA_DC_BLOCK_INDEX + 0]])
        h->h264dsp.h264_chroma_dc_dequant_idct(
            sl->mb + 256,
            h->ps.pps->dequant4_coeff[IS_INTRA(mb_type) ? 1 : 4][sl->chroma_qp[0]][0]);
    if (sl->non_zero_count_cache[scan8[CHROMA_DC_BLOCK_INDEX + 1]])
        h->h264dsp.h264_chroma_dc_dequant_idct(
            sl->mb + 512,
            h->ps.pps->dequant4_coeff[IS_INTRA(mb_type) ? 2 : 5][sl->chroma_qp[1]][0]);

    uint16_t mask = 0;
    int16_t *cp = s->coeff_pool + s->coeff_count;
    for (int j = 0; j < 2; ++j) {
        for (int k = 0; k < 4; ++k) {
            int idx = (j + 1) * 16 + k;
            int16_t *blk = sl->mb + (j + 1) * 256 + k * 16;
            if (sl->non_zero_count_cache[scan8[idx]]) {
                memcpy(cp, blk, 16 * sizeof(int16_t));
                memset(blk, 0, 16 * sizeof(int16_t));
                cp += 16;
                mask |= (uint16_t)(2u << (j * 8 + k * 2));
            } else if (blk[0]) {
                *cp++ = blk[0];
                blk[0] = 0;
                mask |= (uint16_t)(1u << (j * 8 + k * 2));
            }
        }
    }
    s->coeff_count = (unsigned)(cp - s->coeff_pool);
    return mask;
}

static int mefp_ensure_pools(MefpFrameSlot *s, unsigned need_mc, unsigned need_coeff)
{
    if (s->mc_count + need_mc > s->mc_cap) {
        unsigned ncap = s->mc_cap ? s->mc_cap * 2 : MEFP_INIT_MC_PARTS;
        while (ncap < s->mc_count + need_mc) ncap *= 2;
        if (ncap > MEFP_MAX_MC_PARTS) ncap = MEFP_MAX_MC_PARTS;
        if (s->mc_count + need_mc > ncap) return -1;
        MefpMcPart *nbuf = memalign(64, ncap * sizeof(MefpMcPart));
        if (!nbuf) return -1;
        if (s->mc_parts && s->mc_count)
            memcpy(nbuf, s->mc_parts, s->mc_count * sizeof(MefpMcPart));
        free(s->mc_parts);
        s->mc_parts = nbuf;
        s->mc_cap = ncap;
    }
    if (s->coeff_count + need_coeff > s->coeff_cap) {
        unsigned ncap = s->coeff_cap ? s->coeff_cap * 2 : MEFP_INIT_COEFFS;
        while (ncap < s->coeff_count + need_coeff) ncap *= 2;
        if (ncap > MEFP_MAX_COEFFS) ncap = MEFP_MAX_COEFFS;
        if (s->coeff_count + need_coeff > ncap) return -1;
        int16_t *nbuf = memalign(64, ncap * sizeof(int16_t));
        if (!nbuf) return -1;
        if (s->coeff_pool && s->coeff_count)
            memcpy(nbuf, s->coeff_pool, s->coeff_count * sizeof(int16_t));
        free(s->coeff_pool);
        s->coeff_pool = nbuf;
        s->coeff_cap = ncap;
    }
    return 0;
}

static int mefp_hook_capture_mb(const H264Context *h, H264SliceContext *sl)
{
    if (mefp_open_slot_idx < 0 || mefp_frame_fallback) return 0;

    unsigned t0 = sceKernelGetSystemTimeLow();
    MefpFrameSlot *s = mefp_slots[mefp_open_slot_idx];
    const int mb_xy   = sl->mb_xy;
    const int mb_type = h->cur_pic.mb_type[mb_xy];

    if (s->mb_count >= MEFP_MAX_MBS ||
        s->intra4_count + 16 > MEFP_MAX_MBS * 16 ||
        mefp_ensure_pools(s, 16, 384) < 0) {
        mefp_stat_error = AVERROR(ENOMEM);
        return 0;
    }

    h->list_counts[mb_xy] = (uint8_t)sl->list_count;
    sl->mb_linesize   = sl->linesize;
    sl->mb_uvlinesize = sl->uvlinesize;

    MefpMbCmd *mb = &s->mbs[s->mb_count++];
    mb->mb_x              = (uint8_t)sl->mb_x;
    mb->mb_y              = (uint8_t)sl->mb_y;
    mb->flags             = (uint8_t)((mefp_open_slice_idx & 7) << 1);
    mb->chroma_pred_mode  = (uint8_t)sl->chroma_pred_mode;
    mb->intra16_pred_mode = (uint8_t)sl->intra16x16_pred_mode;
    mb->mc_start          = (uint16_t)s->mc_count;
    mb->mc_count          = 0;
    mb->tl_avail          = (uint16_t)sl->topleft_samples_available;
    mb->tr_avail          = (uint16_t)sl->topright_samples_available;
    mb->intra4_start      = (uint16_t)s->intra4_count;
    mb->coeff_start       = s->coeff_count;
    mb->luma_res_mask     = 0;
    mb->chroma_res_mask   = 0;
    mb->pad0 = 0; mb->pad1 = 0;

    if (IS_INTRA_PCM(mb_type)) {
        mb->kind = MEFP_MB_PCM;
        uint8_t *dst_pcm = (uint8_t *)(s->coeff_pool + s->coeff_count);
        memcpy(dst_pcm, sl->intra_pcm_ptr, 384);
        s->coeff_count += 192;
        mefp_stat_capture_us += sceKernelGetSystemTimeLow() - t0;
        return 1;
    }

    if (IS_INTRA(mb_type)) {
        if (IS_INTRA4x4(mb_type)) {
            if (IS_8x8DCT(mb_type)) {
                mb->kind = MEFP_MB_INTRA8;
                uint8_t *modes = s->intra4_modes + s->intra4_count;
                int16_t *cp = s->coeff_pool + s->coeff_count;
                uint32_t lmask = 0;
                for (int k = 0; k < 4; ++k) {
                    int i = k * 4;
                    modes[k] = (uint8_t)sl->intra4x4_pred_mode_cache[scan8[i]];
                    int nnz = sl->non_zero_count_cache[scan8[i]];
                    if (nnz) {
                        int16_t *blk = sl->mb + i * 16;
                        if (nnz == 1 && blk[0]) {
                            *cp++ = blk[0];
                            blk[0] = 0;
                            lmask |= (1u << (2 * k));
                        } else {
                            memcpy(cp, blk, 64 * sizeof(int16_t));
                            memset(blk, 0, 64 * sizeof(int16_t));
                            cp += 64;
                            lmask |= (2u << (2 * k));
                        }
                    }
                }
                s->intra4_count += 4;
                s->coeff_count = (unsigned)(cp - s->coeff_pool);
                mb->luma_res_mask = lmask;
            } else {
                mb->kind = MEFP_MB_INTRA4;
                uint8_t *modes = s->intra4_modes + s->intra4_count;
                int16_t *cp = s->coeff_pool + s->coeff_count;
                uint32_t lmask = 0;
                for (int i = 0; i < 16; ++i) {
                    modes[i] = (uint8_t)sl->intra4x4_pred_mode_cache[scan8[i]];
                    int nnz = sl->non_zero_count_cache[scan8[i]];
                    if (nnz) {
                        int16_t *blk = sl->mb + i * 16;
                        if (nnz == 1 && blk[0]) {
                            *cp++ = blk[0];
                            blk[0] = 0;
                            lmask |= (1u << (2 * i));
                        } else {
                            memcpy(cp, blk, 16 * sizeof(int16_t));
                            memset(blk, 0, 16 * sizeof(int16_t));
                            cp += 16;
                            lmask |= (2u << (2 * i));
                        }
                    }
                }
                s->intra4_count += 16;
                s->coeff_count = (unsigned)(cp - s->coeff_pool);
                mb->luma_res_mask = lmask;
            }
        } else {
            mb->kind = MEFP_MB_INTRA16;
            if (sl->non_zero_count_cache[scan8[LUMA_DC_BLOCK_INDEX]]) {
                h->h264dsp.h264_luma_dc_dequant_idct(
                    sl->mb, sl->mb_luma_dc[0],
                    h->ps.pps->dequant4_coeff[0][sl->qscale][0]);
            }
            int16_t *cp = s->coeff_pool + s->coeff_count;
            uint32_t lmask = 0;
            for (int i = 0; i < 16; ++i) {
                int16_t *blk = sl->mb + i * 16;
                if (sl->non_zero_count_cache[scan8[i]]) {
                    memcpy(cp, blk, 16 * sizeof(int16_t));
                    memset(blk, 0, 16 * sizeof(int16_t));
                    cp += 16;
                    lmask |= (2u << (2 * i));
                } else if (blk[0]) {
                    *cp++ = blk[0];
                    blk[0] = 0;
                    lmask |= (1u << (2 * i));
                }
            }
            s->coeff_count = (unsigned)(cp - s->coeff_pool);
            mb->luma_res_mask = lmask;
        }
        mb->chroma_res_mask = mefp_pack_chroma(h, sl, mb_type, s);
        mefp_stat_capture_us += sceKernelGetSystemTimeLow() - t0;
        return 1;
    }

    /* INTER macroblock */
    mb->kind = MEFP_MB_INTER;
    unsigned mc0 = s->mc_count;
    if (IS_16X16(mb_type)) {
        mefp_emit_mc(s, sl, 0, MEFP_PART_16x16, 0, 0,
                     IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
    } else if (IS_16X8(mb_type)) {
        mefp_emit_mc(s, sl, 0, MEFP_PART_16x8, 0, 0,
                     IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
        mefp_emit_mc(s, sl, 8, MEFP_PART_16x8, 0, 4,
                     IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1));
    } else if (IS_8X16(mb_type)) {
        mefp_emit_mc(s, sl, 0, MEFP_PART_8x16, 0, 0,
                     IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1));
        mefp_emit_mc(s, sl, 4, MEFP_PART_8x16, 4, 0,
                     IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1));
    } else {
        for (int i = 0; i < 4; ++i) {
            const int sub = sl->sub_mb_type[i];
            const int n   = 4 * i;
            const uint8_t x_off = (uint8_t)((i & 1) << 2);
            const uint8_t y_off = (uint8_t)((i & 2) << 1);
            const int l0 = IS_DIR(sub, 0, 0), l1 = IS_DIR(sub, 0, 1);
            if (IS_SUB_8X8(sub)) {
                mefp_emit_mc(s, sl, n, MEFP_PART_8x8, x_off, y_off, l0, l1);
            } else if (IS_SUB_8X4(sub)) {
                mefp_emit_mc(s, sl, n,     MEFP_PART_8x4, x_off, y_off,     l0, l1);
                mefp_emit_mc(s, sl, n + 2, MEFP_PART_8x4, x_off, y_off + 2, l0, l1);
            } else if (IS_SUB_4X8(sub)) {
                mefp_emit_mc(s, sl, n,     MEFP_PART_4x8, x_off,     y_off, l0, l1);
                mefp_emit_mc(s, sl, n + 1, MEFP_PART_4x8, x_off + 2, y_off, l0, l1);
            } else {
                for (int j = 0; j < 4; ++j) {
                    uint8_t sx = (uint8_t)(x_off + 2 * (j & 1));
                    uint8_t sy = (uint8_t)(y_off + (j & 2));
                    mefp_emit_mc(s, sl, n + j, MEFP_PART_4x4, sx, sy, l0, l1);
                }
            }
        }
    }
    mb->mc_count = (uint8_t)(s->mc_count - mc0);

    if (sl->cbp & 15) {
        int16_t *cp = s->coeff_pool + s->coeff_count;
        uint32_t lmask = 0;
        if (IS_8x8DCT(mb_type)) {
            mb->flags |= MEFP_FLAG_DCT8;
            for (int k = 0; k < 4; ++k) {
                int i = k * 4;
                int nnz = sl->non_zero_count_cache[scan8[i]];
                if (nnz) {
                    int16_t *blk = sl->mb + i * 16;
                    if (nnz == 1 && blk[0]) {
                        *cp++ = blk[0];
                        blk[0] = 0;
                        lmask |= (1u << (2 * k));
                    } else {
                        memcpy(cp, blk, 64 * sizeof(int16_t));
                        memset(blk, 0, 64 * sizeof(int16_t));
                        cp += 64;
                        lmask |= (2u << (2 * k));
                    }
                }
            }
        } else {
            for (int i = 0; i < 16; ++i) {
                int nnz = sl->non_zero_count_cache[scan8[i]];
                if (nnz) {
                    int16_t *blk = sl->mb + i * 16;
                    if (nnz == 1 && blk[0]) {
                        *cp++ = blk[0];
                        blk[0] = 0;
                        lmask |= (1u << (2 * i));
                    } else {
                        memcpy(cp, blk, 16 * sizeof(int16_t));
                        memset(blk, 0, 16 * sizeof(int16_t));
                        cp += 16;
                        lmask |= (2u << (2 * i));
                    }
                }
            }
        }
        s->coeff_count = (unsigned)(cp - s->coeff_pool);
        mb->luma_res_mask = lmask;
    }

    mb->chroma_res_mask = mefp_pack_chroma(h, sl, mb_type, s);
    mefp_stat_capture_us += sceKernelGetSystemTimeLow() - t0;
    return 1;
}

static void mefp_hook_frame_end(const H264Context *h, int error)
{
    (void)h;
    if (mefp_open_slot_idx < 0) return;
    int idx = mefp_open_slot_idx;
    mefp_open_slot_idx = -1;
    mefp_open_slice_idx = -1;
    mefp_open_cur_f = NULL;

    if (error < 0 || mefp_frame_fallback || mefp_stat_error) {
        mefp_release_slot_refs(idx);
        return;
    }

    MefpCmd *cmd = mefp_reserve_cmd();
    if (!cmd) {
        mefp_release_slot_refs(idx);
        return;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->op   = MEFP_OP_RECON;
    cmd->slot = mefp_slots[idx];
    mefp_slot_seq[idx] = mefp_submit_cmd(cmd);
    ++mefp_stat_recon_frames;
    mefp_poll_renders();
}

/* =========================================================================
 * Public Lifecycle & Render API
 * ========================================================================= */

int mefp_start(int output_format,
               unsigned max_inflight_renders,
               MefpAcquireRgbFn acquire_cb,
               MefpPublishRgbFn publish_cb,
               MefpReleaseRgbFn release_cb)
{
    if (meLabBackend() != ME_BACKEND_CUSTOM_CORE) return AVERROR(ENODEV);

    memset(&mefp_ring, 0, sizeof(mefp_ring));
    memset(&mefp_dsp, 0, sizeof(mefp_dsp));
    memset(&mefp_job, 0, sizeof(mefp_job));
    mefp_render_head = mefp_render_tail = 0;
    mefp_open_slot_idx = mefp_open_slice_idx = -1;
    mefp_open_cur_f = NULL;
    mefp_frame_fallback = 0;
    mefp_any_sc_written = 0;
    mefp_next_slot = 0;
    mefp_stat_recon_frames = mefp_stat_fallback_frames = 0;
    mefp_stat_render_submitted = mefp_stat_render_completed = 0;
    mefp_stat_wait_us = 0;
    mefp_stat_capture_us = 0;
    mefp_stat_error = 0;

    mefp_output_format = output_format;
    mefp_max_inflight_renders = max_inflight_renders >= 1 && max_inflight_renders <= 4
        ? max_inflight_renders : 1;
    mefp_acquire_cb = acquire_cb;
    mefp_publish_cb = publish_cb;
    mefp_release_cb = release_cb;

    /* Allocate 2 whole-frame command slots + compact growable pools */
    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i) {
        mefp_slots[i] = memalign(64, sizeof(MefpFrameSlot));
        mefp_slot_cur[i] = av_frame_alloc();
        mefp_slot_seq[i] = 0;
        mefp_slot_ref_count[i] = 0;
        if (!mefp_slots[i] || !mefp_slot_cur[i]) goto oom;
        memset(mefp_slots[i], 0, sizeof(MefpFrameSlot));
        mefp_slots[i]->mc_parts = memalign(64, MEFP_INIT_MC_PARTS * sizeof(MefpMcPart));
        mefp_slots[i]->coeff_pool = memalign(64, MEFP_INIT_COEFFS * sizeof(int16_t));
        if (!mefp_slots[i]->mc_parts || !mefp_slots[i]->coeff_pool) goto oom;
        mefp_slots[i]->mc_cap = MEFP_INIT_MC_PARTS;
        mefp_slots[i]->coeff_cap = MEFP_INIT_COEFFS;
        for (int r = 0; r < MEFP_MAX_HELD_REFS; ++r) {
            mefp_slot_refs[i][r] = av_frame_alloc();
            if (!mefp_slot_refs[i][r]) goto oom;
        }
    }
    for (int i = 0; i < MEFP_RING_DEPTH; ++i) {
        mefp_renders[i].frame = av_frame_alloc();
        if (!mefp_renders[i].frame) goto oom;
    }

    /* Populate pure-integer ME DSP tables (ZERO VFPU!) */
    __real_ff_h264qpel_init(&mefp_dsp.qpel, 8);
    mefp_dsp.qpel.put_h264_qpel_pixels_tab[0][0] = mefp_put16;
    mefp_dsp.qpel.avg_h264_qpel_pixels_tab[0][0] = mefp_avg16;
    mefp_dsp.qpel.put_h264_qpel_pixels_tab[1][0] = mefp_put8;
    mefp_dsp.qpel.avg_h264_qpel_pixels_tab[1][0] = mefp_avg8;
    meq_install_qpel(&mefp_dsp.qpel);
    __real_ff_h264chroma_init(&mefp_dsp.chroma, 8);
    mefp_dsp.chroma.put_h264_chroma_pixels_tab[0] = mefp_chroma_put8;
    mefp_dsp.chroma.put_h264_chroma_pixels_tab[1] = mefp_chroma_put4;
    mefp_dsp.chroma.avg_h264_chroma_pixels_tab[0] = mefp_chroma_avg8;
    mefp_dsp.chroma.avg_h264_chroma_pixels_tab[1] = mefp_chroma_avg4;
    ff_h264_pred_init(&mefp_dsp.hpc, AV_CODEC_ID_H264, 8, 1);
    ff_h264dsp_init(&mefp_dsp.h264dsp, 8, 1);
    yuv_rgb_init(&mefp_dsp.yuv_tables);

#if PSP_VME_RENDER
    vme_contexts_init();
#endif

    int ret = meLabInit((unsigned)mefp_me_worker);
    if (ret < 0) goto fail;
    meCoreSelectSystemTable(ret);
    sceKernelDcacheWritebackRange(&meCoreSystemTable, sizeof(meCoreSystemTable));
    sceKernelDcacheWritebackInvalidateAll();

    mefp_job.operation = 1;
    mefp_job.output = (void *)&mefp_ring;
    ret = meLabSubmit(&mefp_job);
    if (ret < 0) {
        meLabShutdown();
        goto fail;
    }

    unsigned t0 = sceKernelGetSystemTimeLow();
    while (!mefp_sc_ctrl()->ready) {
        if (sceKernelGetSystemTimeLow() - t0 > 500000u) {
            mefp_sc_ctrl()->stop = 1;
            meLabWait();
            meLabShutdown();
            ret = -212;
            goto fail;
        }
    }

    mefp_running = 1;
    avpriv_psp_mefp_slice_begin = mefp_hook_slice_begin;
    avpriv_psp_mefp_capture_mb  = mefp_hook_capture_mb;
    avpriv_psp_mefp_frame_end   = mefp_hook_frame_end;
    return 0;

oom:
    ret = AVERROR(ENOMEM);
fail:
    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i) {
        if (mefp_slots[i]) {
            free(mefp_slots[i]->mc_parts);
            free(mefp_slots[i]->coeff_pool);
        }
        free(mefp_slots[i]); mefp_slots[i] = NULL;
        av_frame_free(&mefp_slot_cur[i]);
        for (int r = 0; r < MEFP_MAX_HELD_REFS; ++r)
            av_frame_free(&mefp_slot_refs[i][r]);
    }
    for (int i = 0; i < MEFP_RING_DEPTH; ++i)
        av_frame_free(&mefp_renders[i].frame);
    return ret;
}

int mefp_render_submit(const AVFrame *frame, int64_t pts, int hash_frame)
{
    if (!mefp_running) return -1;
    if (mefp_stat_error) return mefp_stat_error;
    if (frame->format != AV_PIX_FMT_YUV420P ||
        frame->width <= 0 || frame->height <= 0 ||
        frame->width > 1280 || frame->height > 720)
        return -1;

    mefp_poll_renders();
    /* Bound renders queued on ME so rgb_pacer receives frames promptly
     * and rgb_acquire() can never stall on unpublished completed renders. */
    if (mefp_render_tail - mefp_render_head >= mefp_max_inflight_renders) {
        MefpPendingRender *oldest = &mefp_renders[mefp_render_head & (MEFP_RING_DEPTH - 1)];
        int wret = mefp_wait_seq(oldest->seq);
        if (wret < 0) return wret;
    }

    void *rgb_dst = NULL;
    if (mefp_acquire_cb) {
        int aret = mefp_acquire_cb(&rgb_dst);
        if (aret < 0) return aret;
    }

    MefpPendingRender *pr = &mefp_renders[mefp_render_tail & (MEFP_RING_DEPTH - 1)];
    if (av_frame_ref(pr->frame, frame) < 0) {
        if (mefp_release_cb) mefp_release_cb();
        return AVERROR(ENOMEM);
    }

    MefpCmd *cmd = mefp_reserve_cmd();
    if (!cmd) {
        av_frame_unref(pr->frame);
        if (mefp_release_cb) mefp_release_cb();
        return mefp_stat_error ? mefp_stat_error : -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->op         = MEFP_OP_RENDER;
    cmd->planes[0]  = frame->data[0];
    cmd->planes[1]  = frame->data[1];
    cmd->planes[2]  = frame->data[2];
    cmd->strides[0] = frame->linesize[0];
    cmd->strides[1] = frame->linesize[1];
    cmd->strides[2] = frame->linesize[2];
    cmd->width      = frame->width;
    cmd->height     = frame->height;
    cmd->rgb_dst    = rgb_dst;
    cmd->output_fmt = mefp_output_format;
    cmd->hash_frame = hash_frame;
    cmd->sc_written = mefp_any_sc_written;

    pr->pts = pts;
    pr->seq = mefp_submit_cmd(cmd);
    ++mefp_render_tail;
    ++mefp_stat_render_submitted;
    return 0;
}

int mefp_stop(void)
{
    avpriv_psp_mefp_slice_begin = NULL;
    avpriv_psp_mefp_capture_mb  = NULL;
    avpriv_psp_mefp_frame_end   = NULL;

    int ret = 0;
    if (mefp_running) {
        ret = mefp_drain();
        mefp_sc_ctrl()->stop = 1;
        __asm__ volatile("sync" ::: "memory");
        int wret = meLabWait();
        if (ret >= 0 && wret < 0) ret = wret;
        mefp_running = 0;
    }
    int sret = meLabShutdown();
    if (ret >= 0 && sret < 0) ret = sret;

    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i) {
        mefp_release_slot_refs(i);
        if (mefp_slots[i]) {
            free(mefp_slots[i]->mc_parts);
            free(mefp_slots[i]->coeff_pool);
        }
        free(mefp_slots[i]); mefp_slots[i] = NULL;
        av_frame_free(&mefp_slot_cur[i]);
        for (int r = 0; r < MEFP_MAX_HELD_REFS; ++r)
            av_frame_free(&mefp_slot_refs[i][r]);
    }
    for (int i = 0; i < MEFP_RING_DEPTH; ++i)
        av_frame_free(&mefp_renders[i].frame);

    return ret < 0 ? ret : mefp_stat_error;
}

void mefp_get_stats(MefpStats *out)
{
    if (!out) return;
    volatile MefpRing *ctrl = mefp_sc_ctrl();
    out->recon_frames     = mefp_stat_recon_frames;
    out->fallback_frames  = mefp_stat_fallback_frames;
    out->render_submitted = mefp_stat_render_submitted;
    out->render_completed = mefp_stat_render_completed;
    out->intra_mbs        = ctrl->me_intra_mbs;
    out->inter_mbs        = ctrl->me_inter_mbs;
    out->wait_us          = mefp_stat_wait_us;
    out->capture_us       = mefp_stat_capture_us;
    out->me_recon_us      = ((uint64_t)ctrl->me_recon_us_hi << 32) | ctrl->me_recon_us_lo;
    out->me_render_us     = ((uint64_t)ctrl->me_render_us_hi << 32) | ctrl->me_render_us_lo;
    out->frame_hash       = ctrl->frame_hash;
    out->error            = mefp_stat_error ? mefp_stat_error : ctrl->error;
}
