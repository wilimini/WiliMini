/* Concurrent luma motion prediction and residual addition on the ME CPU.
 * The main CPU retains entropy decoding, chroma reconstruction and deblocking.
 * Owned commands and an isolated output tile cross cores; reference frames
 * remain immutable, and edge-emulation scratch is copied before publication.
 * MIST / ME Custom Core: mcidclan, MIT; see experiments/me/LICENSE.MIST.txt. */
#include <pspkernel.h>
#include <me-core-mapper/me-core-mapper.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "h264dec.h"
#include "me_bridge.h"
#include "me_decode.h"
#include "full_render.h"
#if PSP_ME_DECODE_RENDER
#include <pspdmac.h>
#include <pspge.h>
#endif

#if PSP_ME_DECODE >= 2
#define TILE_STRIDE 1024 /* upper bound; actual frame stride is used */
#else
#define TILE_STRIDE 32
#endif
#define TILE_BYTES (TILE_STRIDE * 32)
#if PSP_ME_DECODE >= 3
#define QDEPTH 8
#else
#define QDEPTH 2
#endif
typedef struct __attribute__((aligned(64))) {
    qpel_mc_func function;
    const uint8_t *input;
    unsigned operation, stride, transform8;
    const uint8_t *planes[3];
    int plane_stride[3];
    const YuvRgbTables *tables;
    uint32_t *rgba;
    unsigned destination_offset, padding[2];
    unsigned offsets[16], nonzero[16];
    int16_t coefficients[256] __attribute__((aligned(64)));
    uint8_t source[TILE_BYTES];
} Prediction;
typedef struct __attribute__((aligned(64))) {
    volatile unsigned requested, stop;
    unsigned padding[14];
    volatile unsigned completed, ready, exited;
    unsigned consumer_padding[13];
    Prediction commands[QDEPTH];
    uint8_t output[TILE_BYTES] __attribute__((aligned(64)));
} DecodeQueue;
_Static_assert(offsetof(Prediction, offsets) == 64, "command header cache line");
_Static_assert(offsetof(Prediction, source) == 704, "owned coefficient layout");
_Static_assert(offsetof(DecodeQueue, completed) == 64, "separate consumer cache line");
static DecodeQueue queue;
static MeJob job;
static qpel_mc_func fallback[2][3][16], kernels[2][3][16];
static uint8_t *destination;
static int destination_stride, active, running, pending_copy;
static unsigned edge_begin, edge_end;
static unsigned decode_ready;
#if PSP_ME_DECODE_RENDER
static YuvRgbTables render_tables __attribute__((aligned(64)));
static uint32_t render_output[512 * 272] __attribute__((aligned(64)));
static AVFrame *render_frame;
static unsigned render_sequence;
unsigned me_decode_render_submitted, me_decode_render_completed;
#endif
void psp_me_retire(void);
void me_h264_idct_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct8_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct_dc_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct8_dc_add_8_c(uint8_t *, int16_t *, int);
static void residual(Prediction *p, uint8_t *out, unsigned segment)
{
    unsigned step = p->transform8 ? 4 : 1;
    for (unsigned i = 0; i < 16; i += step) {
        if (!p->nonzero[i]) continue;
        void (*fn)(uint8_t *, int16_t *, int);
        int dc = p->nonzero[i] == 1 && p->coefficients[i * 16];
        if (p->transform8) fn = dc ? me_h264_idct8_dc_add_8_c : me_h264_idct8_add_8_c;
        else fn = dc ? me_h264_idct_dc_add_8_c : me_h264_idct_add_8_c;
        fn = (void *)((unsigned)fn | segment);
        fn(out + p->offsets[i], p->coefficients + i * 16, p->stride);
    }
}
#if PSP_ME_DECODE_CHECK
static uint8_t expected[TILE_BYTES] __attribute__((aligned(64)));
#endif
unsigned me_decode_jobs, me_decode_blocks, me_decode_checked, me_decode_mismatches;
int me_decode_error;
static volatile DecodeQueue *control(void)
{
    return (void *)((unsigned)&queue | 0x40000000u);
}
static int drain(void)
{
    volatile DecodeQueue *q = control();
    unsigned begin = sceKernelGetSystemTimeLow();
    while (q->completed != q->requested) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) {
            me_decode_error = -201;
            return -1;
        }
    }
    return 0;
}
int me_task(int index, void *param);
/* Allegrex cache operation values and the duplicated ME hit operations follow
 * mcidclan's MIT me-lib.c. Batch the final output writebacks before one sync. */
static inline void output_writeback(void *address)
{
#if PSP_ME_DECODE_FAST
    __asm__ volatile("cache 0x1a, 0(%0)\n\tcache 0x1a, 0(%0)" :: "r"(address) : "memory");
#else
    meCoreDcacheWritebackRange(address, 64);
#endif
}
static inline void command_writeback(void *address, unsigned bytes)
{
#if PSP_ME_DECODE_FAST >= 3
    __asm__ volatile("sync" ::: "memory");
    for (unsigned offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("cache 0x1a, 0(%0)" :: "r"((uint8_t *)address + offset) : "memory");
#else
    sceKernelDcacheWritebackRange(address, bytes);
#endif
}
static inline void command_invalidate(void *address, unsigned bytes)
{
#if PSP_ME_DECODE_FAST >= 3
    __asm__ volatile("sync" ::: "memory");
    for (unsigned offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("cache 0x19, 0(%0)\n\tcache 0x19, 0(%0)" :: "r"((uint8_t *)address + offset) : "memory");
    __asm__ volatile("sync" ::: "memory");
#else
    meCoreDcacheInvalidateRange(address, bytes);
#endif
}
int __attribute__((noinline)) me_worker(int index, void *param)
{
    (void)index;
    MeJob *j = (void *)((unsigned)param | 0x20000000u);
    DecodeQueue *cached = (void *)((unsigned)j->output | 0x80000000u);
    volatile DecodeQueue *q = (void *)((unsigned)cached | 0x20000000u);
    unsigned done = 0;
    q->ready = 1;
    __asm__ volatile("sync" ::: "memory");
    while (!q->stop || done != q->requested) {
        if (done == q->requested) continue;
        Prediction *p = &cached->commands[done & (QDEPTH - 1)];
        command_invalidate(p, 64);
        if (p->operation == 3) meCoreDcacheWritebackInvalidateAll();
#if PSP_ME_DECODE_RENDER
        else if (p->operation == 4) {
            const uint8_t *planes[3];
            for (unsigned plane = 0; plane < 3; ++plane) {
                planes[plane] = (void *)((unsigned)p->planes[plane] | 0x80000000u);
                meCoreDcacheInvalidateRange((void *)planes[plane], p->plane_stride[plane] * (plane ? 180 : 360));
            }
            const YuvRgbTables *tables = (void *)((unsigned)p->tables | 0x80000000u);
            uint32_t *out = (void *)((unsigned)p->rgba | 0x80000000u);
            meCoreDcacheInvalidateRange((void *)tables, sizeof(*tables));
            full_render_planes(planes, p->plane_stride, out, tables);
            meCoreDcacheWritebackRange(out, sizeof(render_output));
        }
#endif
        else {
            if (p->operation == 5) {
                /* Prediction-only macroblock: publish the completed tile. */
            } else if (p->operation == 2) {
                command_invalidate(p, 64 + 128 + 512);
                residual(p, cached->output, 0x80000000u);
                meCoreDcacheWritebackRange(p->coefficients, sizeof(p->coefficients));
            } else {
                if (p->operation == 1) meCoreDcacheInvalidateRange(p->source, TILE_BYTES);
                qpel_mc_func fn = (void *)((unsigned)p->function | 0x80000000u);
                fn(cached->output + p->destination_offset,
                   (void *)((unsigned)p->input | 0x80000000u), p->stride);
            }
            unsigned rows = p->operation >= 2 ? 16 : 16 >> p->transform8;
            unsigned offset = p->operation >= 2 ? 0 : p->destination_offset;
#if PSP_ME_DECODE_FAST >= 2
            if (p->operation >= 2) {
#endif
            __asm__ volatile("sync" ::: "memory");
            for (unsigned row = 0; row < rows; ++row)
                output_writeback((void *)(((unsigned)cached->output + offset + row * p->stride) & ~63u));
#if PSP_ME_DECODE_FAST >= 2
            }
#endif
        }
        __asm__ volatile("sync" ::: "memory");
        q->completed = ++done;
    }
    q->exited = 1;
    j->done = 1;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}
int me_decode_start(void)
{
    memset(&queue, 0, sizeof(queue));
    me_decode_jobs = me_decode_blocks = me_decode_checked = me_decode_mismatches = 0;
    me_decode_error = active = running = pending_copy = 0;
    decode_ready = 0;
#if PSP_ME_DECODE_RENDER
    yuv_rgb_init(&render_tables);
    render_frame = NULL;
    me_decode_render_submitted = me_decode_render_completed = 0;
#endif
    int ret = meLabInit((unsigned)me_task);
    if (ret < 0) return ret;
    sceKernelDcacheWritebackInvalidateRange(&queue, sizeof(queue));
    job.output = (void *)&queue;
    ret = meLabSubmit(&job);
    if (ret < 0) return ret;
    running = 1;
    unsigned begin = sceKernelGetSystemTimeLow();
    while (!control()->ready) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) return -202;
    }
    return 0;
}
int me_decode_stop(void)
{
    int ret = 0;
    if (running) {
        psp_me_retire();
#if PSP_ME_DECODE_RENDER
        ret = me_decode_render_finish();
#endif
        int drained = drain();
        if (!ret) ret = drained;
        control()->stop = 1;
        __asm__ volatile("sync" ::: "memory");
        int wait = meLabWait();
        if (!ret) ret = wait;
        running = active = 0;
    }
    int shutdown = meLabShutdown();
    if (!ret) ret = shutdown;
    if (!ret) ret = me_decode_error;
    return ret;
}

/* Called only by the opt-in h264_mb object, immediately around motion
 * prediction. No live slice/coefficient context is passed to the ME. */
void psp_me_motion_begin(const H264Context *h, H264SliceContext *sl,
                         uint8_t *dst, int stride)
{
    active = 0;
    if (!running || me_decode_error || h->pixel_shift || CHROMA444(h) ||
        h->picture_structure != PICT_FRAME || FRAME_MBAFF(h) ||
        sl->qscale == 0) return;
    if (decode_ready) {
        if (control()->completed < decode_ready) return;
        decode_ready = 0;
    }
    unsigned type = h->cur_pic.mb_type[sl->mb_xy];
#if PSP_ME_DECODE < 3
    if (!IS_16X16(type)) return;
#else
    if (IS_8X8(type))
        for (unsigned i = 0; i < 4; ++i)
            if (!IS_SUB_8X8(sl->sub_mb_type[i])) return;
#endif
    /* Weighted prediction reads its result before motion_end. Keep it on SC.
     * Implicit B prediction with equal weights follows the normal put/avg path. */
    if (sl->pwt.use_weight == 1) return;
    unsigned cache_step = IS_16X16(type) ? 16 : 4;
    for (unsigned i = 0; i < 16; i += cache_step) {
        int r0 = sl->ref_cache[0][scan8[i]], r1 = sl->ref_cache[1][scan8[i]];
        if (sl->pwt.use_weight == 2 && r0 >= 0 && r1 >= 0 &&
            sl->pwt.implicit_weight[r0][r1][sl->mb_y & 1] != 32) return;
    }
    unsigned fraction = 0, expensive = 0;
    for (int list = 0; list < 2; ++list)
        if (USES_LIST(type, list))
            for (unsigned i = 0; i < 16; i += cache_step)
                if (sl->ref_cache[list][scan8[i]] >= 0) {
                    unsigned x = sl->mv_cache[list][scan8[i]][0] & 3;
                    unsigned y = sl->mv_cache[list][scan8[i]][1] & 3;
                    fraction |= x | y;
                    expensive |= x && y && ((x | y) & 1);
                }
#if PSP_ME_DECODE_SELECT
    if (!expensive) return;
#else
    (void)expensive;
#endif
    if (!fraction) return; /* retain the fast SC VFPU integer-pixel copy */
    if (PSP_ME_DECODE >= 2 && (stride > TILE_STRIDE || stride < 64 || (stride & 63))) return;
    destination = dst;
    destination_stride = stride;
    edge_begin = (unsigned)sl->edge_emu_buffer;
    edge_end = edge_begin + sl->edge_emu_buffer_allocated;
    active = 1;
    ++me_decode_blocks;
}
void psp_me_retire(void)
{
    if (!active && !pending_copy) return;
    if (drain() < 0) { active = pending_copy = 0; return; }
    unsigned output_stride = PSP_ME_DECODE >= 2 ? destination_stride : TILE_STRIDE;
#if PSP_ME_DECODE_FAST
    const uint8_t *output = (void *)((unsigned)queue.output | 0x40000000u);
#else
    const uint8_t *output = queue.output;
    for (unsigned row = 0; row < 16; ++row)
        sceKernelDcacheInvalidateRange(queue.output + row * output_stride, 64);
#endif
#if PSP_ME_DECODE_CHECK
    ++me_decode_checked;
    for (unsigned row = 0; row < 16; ++row)
        if (memcmp(output + row * output_stride, expected + row * output_stride, 16))
            ++me_decode_mismatches;
    if (me_decode_mismatches && !me_decode_error) me_decode_error = -203;
#endif
    for (unsigned row = 0; row < 16; ++row)
        memcpy(destination + row * destination_stride, output + row * output_stride, 16);
    active = pending_copy = 0;
}
void psp_me_motion_end(void)
{
#if PSP_ME_DECODE_CHECK == 2 && PSP_ME_DECODE >= 2
    if (active && !me_decode_error) {
        drain();
        for (unsigned row = 0; row < 16; ++row) {
            sceKernelDcacheInvalidateRange(queue.output + row * destination_stride, 64);
            if (memcmp(queue.output + row * destination_stride, expected + row * destination_stride, 16))
                me_decode_error = -204;
        }
    }
#endif
#if PSP_ME_DECODE == 1
    psp_me_retire();
#endif
}
void psp_me_slice_begin(void)
{
#if PSP_ME_DECODE >= 2
    if (!running || me_decode_error) return;
    psp_me_retire();
    sceKernelDcacheWritebackAll();
    volatile DecodeQueue *q = control();
    unsigned sent = q->requested;
    if (sent - q->completed == QDEPTH && drain() < 0) return;
    Prediction *p = &queue.commands[sent & (QDEPTH - 1)];
    p->operation = 3;
    command_writeback(p, 64);
    __asm__ volatile("sync" ::: "memory");
    q->requested = sent + 1;
    decode_ready = sent + 1;
#if !PSP_ME_DECODE_RENDER
    drain();
#endif
#endif
}
int psp_me_luma_submit(const H264Context *h, H264SliceContext *sl, int type)
{
#if PSP_ME_DECODE >= 2
    if (!active) return 0;
    if (sl->cbp & 15) {
        volatile DecodeQueue *q = control();
        unsigned sent = q->requested;
        if (sent - q->completed == QDEPTH && drain() < 0) return 0;
        Prediction *p = &queue.commands[sent & (QDEPTH - 1)];
        p->operation = 2;
        p->stride = destination_stride;
        p->transform8 = !!IS_8x8DCT(type);
        memcpy(p->coefficients, sl->mb, sizeof(p->coefficients));
        for (unsigned i = 0; i < 16; ++i) {
            p->offsets[i] = h->block_offset[i]; /* original frame stride */
            p->nonzero[i] = sl->non_zero_count_cache[scan8[i]];
        }
#if PSP_ME_DECODE_CHECK
        Prediction reference;
        memcpy(&reference, p, 64 + 128 + 512);
        residual(&reference, expected, 0);
#endif
        memset(sl->mb, 0, sizeof(p->coefficients));
        command_writeback(p, 64 + 128 + 512);
        __asm__ volatile("sync" ::: "memory");
        q->requested = sent + 1;
    }
#if PSP_ME_DECODE_FAST >= 2
    else {
        volatile DecodeQueue *q = control();
        unsigned sent = q->requested;
        if (sent - q->completed == QDEPTH && drain() < 0) return 0;
        Prediction *p = &queue.commands[sent & (QDEPTH - 1)];
        p->operation = 5;
        p->stride = destination_stride;
        command_writeback(p, 64);
        __asm__ volatile("sync" ::: "memory");
        q->requested = sent + 1;
    }
#endif
    pending_copy = 1;
    active = 0;
    return 1;
#else
    (void)h; (void)sl; (void)type;
    return 0;
#endif
}
static void predict(uint8_t *dst, const uint8_t *src, ptrdiff_t stride, unsigned average, unsigned size, unsigned xy)
{
    if (!active) { fallback[average][size][xy](dst, src, stride); return; }
    volatile DecodeQueue *q = control();
    unsigned sent = q->requested;
    if (sent - q->completed == QDEPTH && drain() < 0) return;
    Prediction *p = &queue.commands[sent & (QDEPTH - 1)];
    /* Include the full six-tap footprint, including edge-emulated input.
     * Owned snapshots allow SC to reuse edge scratch for chroma immediately. */
    unsigned width = 16 >> size;
    unsigned cols = width + ((xy & 3) ? 5 : 0), rows = width + ((xy & 12) ? 5 : 0);
    int top = (xy & 12) ? -2 : 0, left = (xy & 3) ? -2 : 0;
    unsigned input_stride = PSP_ME_DECODE >= 2 ? stride : TILE_STRIDE;
    int snapshot = PSP_ME_DECODE == 1 || ((unsigned)src >= edge_begin && (unsigned)src < edge_end);
    if (snapshot) {
        for (unsigned row = 0; row < rows; ++row)
            memcpy(p->source + (row + 2 + top) * input_stride + 2 + left,
                   src + ((int)row + top) * stride + left, cols);
        p->input = p->source + 2 * input_stride + 2;
    } else p->input = src;
    p->operation = snapshot;
    p->stride = input_stride;
    p->destination_offset = (unsigned)(dst - destination);
    p->transform8 = size;
    p->function = kernels[average][size][xy];
#if PSP_ME_DECODE_CHECK
    kernels[average][size][xy](expected + p->destination_offset, p->input, input_stride);
#endif
    command_writeback(p, 64);
    if (snapshot) sceKernelDcacheWritebackRange(p->source, TILE_BYTES);
    __asm__ volatile("sync" ::: "memory");
    q->requested = sent + 1;
    ++me_decode_jobs;
}
#define PHASE(S,N) \
static void put_##S##_##N(uint8_t *d, const uint8_t *s, ptrdiff_t st) { predict(d,s,st,0,S,N); } \
static void avg_##S##_##N(uint8_t *d, const uint8_t *s, ptrdiff_t st) { predict(d,s,st,1,S,N); }
#define PHASES(S) PHASE(S,0) PHASE(S,1) PHASE(S,2) PHASE(S,3) PHASE(S,4) PHASE(S,5) PHASE(S,6) PHASE(S,7) PHASE(S,8) PHASE(S,9) PHASE(S,10) PHASE(S,11) PHASE(S,12) PHASE(S,13) PHASE(S,14) PHASE(S,15)
PHASES(0) PHASES(1) PHASES(2)
#define FUNCTIONS(P,S) {P##_##S##_0,P##_##S##_1,P##_##S##_2,P##_##S##_3,P##_##S##_4,P##_##S##_5,P##_##S##_6,P##_##S##_7,P##_##S##_8,P##_##S##_9,P##_##S##_10,P##_##S##_11,P##_##S##_12,P##_##S##_13,P##_##S##_14,P##_##S##_15}
static const qpel_mc_func put_wrappers[3][16] = {FUNCTIONS(put,0),FUNCTIONS(put,1),FUNCTIONS(put,2)};
static const qpel_mc_func avg_wrappers[3][16] = {FUNCTIONS(avg,0),FUNCTIONS(avg,1),FUNCTIONS(avg,2)};
void __real_ff_h264qpel_init(H264QpelContext *, int);
void me_decode_qpel_init(H264QpelContext *c, int depth)
{
    if (depth != 8) return;
    H264QpelContext original;
    __real_ff_h264qpel_init(&original, 8);
    memcpy(kernels[0], original.put_h264_qpel_pixels_tab, sizeof(kernels[0]));
    memcpy(kernels[1], original.avg_h264_qpel_pixels_tab, sizeof(kernels[1]));
    memcpy(fallback[0], c->put_h264_qpel_pixels_tab, sizeof(fallback[0]));
    memcpy(fallback[1], c->avg_h264_qpel_pixels_tab, sizeof(fallback[1]));
    unsigned bytes = (PSP_ME_DECODE >= 3 ? 2 : 1) * sizeof(put_wrappers[0]);
    memcpy(c->put_h264_qpel_pixels_tab, put_wrappers, bytes);
    memcpy(c->avg_h264_qpel_pixels_tab, avg_wrappers, bytes);
}
#if PSP_ME_DECODE_RENDER
int me_decode_render_finish(void)
{
    if (!render_frame) return 0;
    unsigned begin = sceKernelGetSystemTimeLow();
    while (control()->completed < render_sequence) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) return -205;
        sceKernelDelayThread(10);
    }
    uint32_t *dst = (void *)((unsigned)sceGeEdramGetAddr() | 0x40000000u);
    int ret = sceDmacMemcpy(dst + 512, render_output + 512, 512 * 270 * 4);
    if (ret < 0) return ret;
    av_frame_free(&render_frame);
    ++me_decode_render_completed;
    return 0;
}
int me_decode_render(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width != 640 || frame->height != 360) return -206;
    int ret = me_decode_render_finish();
    if (ret < 0) return ret;
    if (drain() < 0) return me_decode_error;
    render_frame = av_frame_clone(frame);
    if (!render_frame) return -207;
    volatile DecodeQueue *q = control();
    unsigned sent = q->requested;
    Prediction *p = &queue.commands[sent & (QDEPTH - 1)];
    p->operation = 4;
    p->tables = &render_tables;
    p->rgba = render_output;
    for (unsigned plane = 0; plane < 3; ++plane) {
        p->planes[plane] = frame->data[plane];
        p->plane_stride[plane] = frame->linesize[plane];
        sceKernelDcacheWritebackRange(frame->data[plane], frame->linesize[plane] * (plane ? 180 : 360));
    }
    command_writeback(p, 64);
    __asm__ volatile("sync" ::: "memory");
    q->requested = render_sequence = sent + 1;
    ++me_decode_render_submitted;
    return 0;
}
#endif
