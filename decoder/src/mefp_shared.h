/* Types shared by the SC (capture) and ME (reconstruction) halves of the
 * whole-frame H.264 pipeline. Internal to me_frame_pipeline.c and mefp_recon.c.
 *
 * Ownership: SC fills a MefpFrameSlot, publishes a MefpCmd and bumps
 * `requested`; ME reconstructs the slot in place and bumps `completed`.
 * Both sides access the ring through uncached addresses for control words and
 * flush/invalidate whole D-caches around command hand-off. */
#ifndef PSP_MEFP_SHARED_H
#define PSP_MEFP_SHARED_H

#include <stdint.h>
#include <stddef.h>
#include "h264dec.h"

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
    MEFP_OP_RECON = 1, /* reconstruct one captured frame slot */
    MEFP_OP_SYNC  = 2, /* no work: completes once every earlier command has */
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
} MefpCmd;

typedef struct __attribute__((aligned(64))) {
    /* Cache line 0: written by SC only */
    volatile unsigned requested;
    volatile unsigned stop;
    uint8_t          *emu_buf;     /* MEFP_EMU_BUF_BYTES of ME scratch */
    uint8_t          *bipred_buf;  /* MEFP_BIPRED_BUF_BYTES of ME scratch */
    unsigned          producer_pad[12];
    /* Cache line 1: written by ME only */
    volatile unsigned completed;
    volatile unsigned ready;
    volatile unsigned exited;
    volatile int      error;
    volatile unsigned me_intra_mbs;
    volatile unsigned me_inter_mbs;
    unsigned          consumer_pad[10];
    /* Cache line 2+: command ring */
    MefpCmd           cmds[MEFP_RING_DEPTH];
} MefpRing;

_Static_assert(offsetof(MefpRing, completed) == 64, "ring controls must be on separate cache lines");
_Static_assert(offsetof(MefpRing, cmds) == 128, "ring cmds must be 64-byte aligned");

/* ME scratch buffers. Custom Core places them in ME-local eDRAM (cached KSEG0
 * 0x80120000..0x80130000, clear of the low vectors and the ME stack at the top
 * of eDRAM). MIST keeps the firmware's eDRAM layout, so SC allocates them in
 * main RAM instead. */
#define MEFP_EMU_BUF_BYTES    (MEFP_MAX_STRIDE * 28)
#define MEFP_BIPRED_BUF_BYTES (MEFP_MAX_STRIDE * 32)
#define MEFP_EDRAM_EMU_BUF    0x80120000u
#define MEFP_EDRAM_BIPRED_BUF 0x80128000u

typedef struct __attribute__((aligned(64))) {
    H264QpelContext   qpel;
    H264ChromaContext chroma;
    H264PredContext   hpc;
    H264DSPContext    h264dsp;
} MefpDsp;

/* Defined in me_frame_pipeline.c; read by the ME worker. */
extern MefpDsp mefp_dsp;

/* mefp_recon.c */
void mefp_dsp_init(MefpDsp *dsp);
/* ME entry: Custom Core calls it directly; MIST enters via me_entry.S. */
int mefp_me_worker(int index, void *param);

#endif /* PSP_MEFP_SHARED_H */
