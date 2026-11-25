/* SC half of the whole-frame H.264 pipeline (8-bit 4:2:0 progressive).
 *
 * - SC runs demux, audio and H.264 CABAC/CAVLC. For eligible frames it never
 *   touches pixels: FFmpeg hooks capture compact per-macroblock commands
 *   (prediction modes, MC partitions, sparse residuals) into a frame slot.
 * - ME (mefp_recon.c) reconstructs each slot in place inside a persistent
 *   worker that consumes a FIFO command ring.
 * - Each in-flight slot holds AVFrame references to its target and reference
 *   pictures until ME completes that sequence number.
 * - Frames the pipeline cannot take (interlaced, deblocked, high bit depth...)
 *   fall back to FFmpeg's own reconstruction on SC after draining ME.
 * - The GE presenter waits on mefp_seq_is_done() before reading a frame.
 *
 * Both ME backends run the same worker. Custom Core calls it directly and
 * gives it eDRAM scratch; MIST enters through the me_entry.S trampoline and
 * uses main-RAM scratch so the firmware's eDRAM is left alone. */
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

#include "me_bridge.h"
#include "mefp_shared.h"
#include "me_frame_pipeline.h"

/* MIST trampoline in me_entry.S: calls mefp_me_worker, then returns to the
 * firmware handler. */
extern void me_mist_entry(void);

static MefpRing mefp_ring;
MefpDsp mefp_dsp;
static MeJob mefp_job __attribute__((aligned(64)));

/* SC-only state */
static MefpFrameSlot *mefp_slots[MEFP_FRAME_SLOTS];
static uint32_t       mefp_slot_seq[MEFP_FRAME_SLOTS];
static AVFrame       *mefp_slot_cur[MEFP_FRAME_SLOTS];
static AVFrame       *mefp_slot_refs[MEFP_FRAME_SLOTS][MEFP_MAX_HELD_REFS];
static int            mefp_slot_ref_count[MEFP_FRAME_SLOTS];
/* MIST scratch; NULL on Custom Core, which uses eDRAM. */
static uint8_t       *mefp_scratch;

static int            mefp_running;
static int            mefp_open_slot_idx = -1;
static int            mefp_open_slice_idx = -1;
static const AVFrame *mefp_open_cur_f;
static int            mefp_frame_fallback;
static int            mefp_any_sc_written;
static unsigned       mefp_next_slot;

/* Recently submitted target planes, so the presenter can find the sequence
 * number that finishes a decoded frame. */
#define MEFP_RECON_HIST 16
static const uint8_t *mefp_recon_y[MEFP_RECON_HIST];
static uint32_t       mefp_recon_seq[MEFP_RECON_HIST];
static unsigned       mefp_recon_hist_pos;
static unsigned       mefp_recon_hist_count;

static unsigned mefp_stat_recon_frames;
static unsigned mefp_stat_fallback_frames;
static unsigned mefp_stat_wait_us;
static uint64_t mefp_stat_capture_us;
static int      mefp_stat_error;

static inline volatile MefpRing *mefp_sc_ctrl(void)
{
    return (volatile MefpRing *)((uintptr_t)&mefp_ring | 0x40000000u);
}

static int mefp_wait_seq(uint32_t seq)
{
    if (!mefp_running) return 0;
    if (mefp_stat_error) return mefp_stat_error;
    unsigned t0 = sceKernelGetSystemTimeLow();
    while ((int)(mefp_sc_ctrl()->completed - seq) < 0) {
        if (sceKernelGetSystemTimeLow() - t0 > MEFP_WAIT_TIMEOUT) {
            mefp_stat_error = -211;
            return mefp_stat_error;
        }
    }
    mefp_stat_wait_us += sceKernelGetSystemTimeLow() - t0;
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
    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i)
        mefp_release_slot_refs(i);
    mefp_recon_hist_count = 0;
    sceKernelDcacheWritebackInvalidateAll();
    return ret;
}

static uint32_t mefp_find_recon_seq(const uint8_t *y_plane)
{
    if (!mefp_running || !y_plane || !mefp_recon_hist_count) return 0;
    unsigned n = mefp_recon_hist_count < MEFP_RECON_HIST ? mefp_recon_hist_count : MEFP_RECON_HIST;
    for (unsigned k = 0; k < n; ++k) {
        unsigned idx = (mefp_recon_hist_pos - 1u - k) & (MEFP_RECON_HIST - 1u);
        if (mefp_recon_y[idx] == y_plane) return mefp_recon_seq[idx];
    }
    return 0;
}

int mefp_seq_is_done(uint32_t seq)
{
    if (!seq || !mefp_running) return 1;
    return (int)(mefp_sc_ctrl()->completed - seq) >= 0;
}

uint32_t mefp_prepare_present(const uint8_t *y_plane)
{
    /* SC reconstructed a fallback frame: its pixels may still sit in the SC
     * D-cache, where neither GE nor ME can see them. */
    if (mefp_any_sc_written) {
        sceKernelDcacheWritebackAll();
        mefp_any_sc_written = 0;
    }
    return mefp_find_recon_seq(y_plane);
}

/* FFmpeg H.264 hooks (0004-psp-me-frame-pipeline.patch). They run on the
 * decoder thread for every slice/macroblock/frame. */

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

    const AVFrame *cur_f = h->cur_pic_ptr ? h->cur_pic_ptr->f : NULL;
    if (mefp_open_slot_idx >= 0 &&
        (mefp_open_cur_f != cur_f || !cur_f ||
         mefp_slots[mefp_open_slot_idx]->cur_data[0] != cur_f->data[0])) {
        /* The previous picture ended without field_end (decode error or a
         * flush); discard its capture. FFmpeg reuses a DPB slot's AVFrame for
         * later pictures, so the planes, not the AVFrame, identify it. */
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
    uint32_t seq = mefp_submit_cmd(cmd);
    mefp_slot_seq[idx] = seq;
    unsigned hidx = mefp_recon_hist_pos++ & (MEFP_RECON_HIST - 1u);
    mefp_recon_y[hidx] = mefp_slots[idx]->cur_data[0];
    mefp_recon_seq[hidx] = seq;
    if (mefp_recon_hist_count < MEFP_RECON_HIST) ++mefp_recon_hist_count;
    ++mefp_stat_recon_frames;
}


/* Public lifecycle */

static void mefp_free_slots(void)
{
    for (int i = 0; i < MEFP_FRAME_SLOTS; ++i) {
        if (mefp_slot_cur[i]) mefp_release_slot_refs(i);
        if (mefp_slots[i]) {
            free(mefp_slots[i]->mc_parts);
            free(mefp_slots[i]->coeff_pool);
        }
        free(mefp_slots[i]); mefp_slots[i] = NULL;
        av_frame_free(&mefp_slot_cur[i]);
        for (int r = 0; r < MEFP_MAX_HELD_REFS; ++r)
            av_frame_free(&mefp_slot_refs[i][r]);
    }
}

int mefp_start(void)
{
    const int backend = meLabBackend();
    if (backend != ME_BACKEND_CUSTOM_CORE && backend != ME_BACKEND_MIST) return AVERROR(ENODEV);
    /* A failed stop may have left ME writing into the previous scratch. */
    if (mefp_scratch) return AVERROR(EBUSY);

    memset(&mefp_ring, 0, sizeof(mefp_ring));
    memset(&mefp_dsp, 0, sizeof(mefp_dsp));
    memset(&mefp_job, 0, sizeof(mefp_job));
    mefp_open_slot_idx = mefp_open_slice_idx = -1;
    mefp_open_cur_f = NULL;
    mefp_frame_fallback = 0;
    mefp_any_sc_written = 0;
    mefp_next_slot = 0;
    mefp_recon_hist_pos = 0;
    mefp_recon_hist_count = 0;
    mefp_stat_recon_frames = mefp_stat_fallback_frames = 0;
    mefp_stat_wait_us = 0;
    mefp_stat_capture_us = 0;
    mefp_stat_error = 0;

    int ret;
    /* Two whole-frame command slots with compact pools that grow on demand. */
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

    unsigned entry;
    if (backend == ME_BACKEND_CUSTOM_CORE) {
        entry = (unsigned)mefp_me_worker;
        mefp_ring.emu_buf = (uint8_t *)MEFP_EDRAM_EMU_BUF;
        mefp_ring.bipred_buf = (uint8_t *)MEFP_EDRAM_BIPRED_BUF;
    } else {
        entry = (unsigned)me_mist_entry;
        mefp_scratch = memalign(64, MEFP_EMU_BUF_BYTES + MEFP_BIPRED_BUF_BYTES);
        if (!mefp_scratch) goto oom;
        mefp_ring.emu_buf = mefp_scratch;
        mefp_ring.bipred_buf = mefp_scratch + MEFP_EMU_BUF_BYTES;
    }

    mefp_dsp_init(&mefp_dsp);

    ret = meLabInit(entry);
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
            ret = meLabWait();
            if (meLabShutdown() >= 0 && ret >= 0) {
                free(mefp_scratch);
                mefp_scratch = NULL;
            }
            ret = -212;
            mefp_free_slots();
            return ret;
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
    free(mefp_scratch);
    mefp_scratch = NULL;
    mefp_free_slots();
    return ret;
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
    /* Only a clean stop proves ME no longer writes into the scratch. */
    if (ret >= 0) {
        free(mefp_scratch);
        mefp_scratch = NULL;
    }
    mefp_free_slots();
    return ret < 0 ? ret : mefp_stat_error;
}

void mefp_get_stats(MefpStats *out)
{
    if (!out) return;
    volatile MefpRing *ctrl = mefp_sc_ctrl();
    out->recon_frames    = mefp_stat_recon_frames;
    out->fallback_frames = mefp_stat_fallback_frames;
    out->intra_mbs       = ctrl->me_intra_mbs;
    out->inter_mbs       = ctrl->me_inter_mbs;
    out->wait_us         = mefp_stat_wait_us;
    out->capture_us      = mefp_stat_capture_us;
    out->error           = mefp_stat_error ? mefp_stat_error : ctrl->error;
}
