/* Batched FFmpeg inter reconstruction. MIST transport: mcidclan, MIT;
 * see experiments/me/LICENSE.MIST.txt. FFmpeg numerical kernels retain LGPL.
 * Pixel ownership changes only at fallback/filter boundaries, not per MB. */
#include <pspkernel.h>
#include <pspdmac.h>
#include <pspge.h>
#include <me-core-mapper/me-core-mapper.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "h264dec.h"
#include "me_decode.h"
#include "me_bridge.h"
#include "full_render.h"

#define DEPTH 2
#define MOTIONS 96 /* 16 subpartitions, two lists, Y/Cb/Cr */
#define MAX_STRIDE 1024
#define FILTERS 128
_Static_assert(PSP_ME_PIPE_BATCH >= 1 && PSP_ME_PIPE_BATCH <= 40, "batch must fit one row");
typedef struct {
    void *function;
    uint8_t *dst;
    const uint8_t *src;
    int stride;
    unsigned chroma, height, x, y;
} Motion;
typedef struct __attribute__((aligned(64))) {
    unsigned count, cbp, transform8, mb_x, mb_y;
    uint8_t *dst[3];
    int stride[3], qmul[2];
    unsigned chroma_dc[2];
    uint16_t offsets[48];
    uint8_t nonzero[48];
    Motion motion[MOTIONS];
    int16_t coefficients[384] __attribute__((aligned(64)));
#if PSP_ME_DECODE_CHECK
    uint8_t expected[384], prediction[384];
#endif
} Reconstruction;
typedef struct {
    void *function;
    uint8_t *pix[3], *border;
    int stride, uvstride, alpha, beta;
    unsigned kind;
    int8_t tc[4];
} Filter;
typedef struct __attribute__((aligned(64))) {
    unsigned operation, count;
    const uint8_t *planes[3];
    int stride[3];
    unsigned rows;
    Reconstruction blocks[PSP_ME_PIPE_BATCH];
    Filter filters[FILTERS] __attribute__((aligned(64)));
} Batch;
typedef struct __attribute__((aligned(64))) {
    volatile unsigned requested, stop;
    unsigned producer_pad[14];
    volatile unsigned completed, ready, exited, mismatches;
    volatile int error;
    volatile unsigned failure_mb, failure_pixel, failure_values;
    volatile unsigned trace_operation, trace_index, trace_function, trace_address;
    unsigned consumer_pad[4];
    Batch batches[DEPTH];
} Pipeline;
_Static_assert(offsetof(Pipeline, completed) == 64, "queue controls must not share a cache line");
_Static_assert(offsetof(Pipeline, batches) == 128, "queue payload must start on a cache line");
_Static_assert(offsetof(Batch, blocks) % 64 == 0, "blocks must be cache aligned");
_Static_assert(offsetof(Reconstruction, coefficients) % 64 == 0, "coefficients must be cache aligned");
static Pipeline pipeline;
static MeJob job;
static int running, active, owned, filtering;
static unsigned pending, filter_pending;
static unsigned first_row, last_row;
static const H264Context *picture;
static Reconstruction *current;
static uint8_t *border_buffers[2];
static unsigned border_bytes[2];
static uintptr_t edge_begin, edge_end;
static qpel_mc_func qfallback[2][3][16], qkernels[2][3][16];
static h264_chroma_mc_func cfallback[2][4], ckernels[2][4];
unsigned me_decode_jobs, me_decode_blocks, me_decode_checked, me_decode_mismatches;
int me_decode_error;
unsigned me_pipeline_batches, me_pipeline_handoffs, me_pipeline_wait_us;
unsigned me_pipeline_max_batch, me_pipeline_fallback;
unsigned me_pipeline_failure_mb, me_pipeline_failure_pixel, me_pipeline_failure_values;
unsigned me_pipeline_filters, me_pipeline_filter_rows;
unsigned me_pipeline_trace[4];
static YuvRgbTables tables __attribute__((aligned(64)));
static uint32_t rgba[512 * 272] __attribute__((aligned(64)));
static AVFrame *render_frame;
static unsigned render_sequence;
unsigned me_decode_render_submitted, me_decode_render_completed;
#if PSP_ME_DECODE_CHECK
static uint8_t reference[3][16 * MAX_STRIDE] __attribute__((aligned(64)));
static int16_t reference_coefficients[768] __attribute__((aligned(64)));
static uint8_t published_expected[40 * 23][384] __attribute__((aligned(64)));
static uint8_t published_valid[40 * 23];
#endif
void me_h264_idct_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct8_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct_dc_add_8_c(uint8_t *, int16_t *, int);
void me_h264_idct8_dc_add_8_c(uint8_t *, int16_t *, int);
void me_h264_chroma_dc_dequant_idct_8_c(int16_t *, int);
int me_task(int, void *);
void psp_me_retire(void);

static volatile Pipeline *control(void)
{ return (void *)((unsigned)&pipeline | 0x40000000u); }
static unsigned cache_span(unsigned bytes) { return (bytes + 63) & ~63u; }
static void invalidate_pixels(const void *ptr, unsigned bytes)
{
    unsigned start = (unsigned)ptr & ~63u;
    sceKernelDcacheInvalidateRange((void *)start, cache_span((unsigned)ptr - start + bytes));
}
static int wait_sequence(unsigned seq)
{
    if (me_decode_error) return -1;
    unsigned begin = sceKernelGetSystemTimeLow();
    while (control()->completed < seq) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) {
            me_pipeline_trace[0] = control()->trace_operation;
            me_pipeline_trace[1] = control()->trace_index;
            me_pipeline_trace[2] = control()->trace_function;
            me_pipeline_trace[3] = control()->trace_address;
            me_decode_error = -211;
            return -1;
        }
    }
    me_pipeline_wait_us += sceKernelGetSystemTimeLow() - begin;
    me_decode_mismatches = control()->mismatches;
    me_pipeline_failure_mb = control()->failure_mb;
    me_pipeline_failure_pixel = control()->failure_pixel;
    me_pipeline_failure_values = control()->failure_values;
    if (control()->error) me_decode_error = control()->error;
    return me_decode_error ? -1 : 0;
}
static Batch *reserve(void)
{
    unsigned sent = control()->requested;
    if (sent - control()->completed == DEPTH && wait_sequence(sent - DEPTH + 1) < 0)
        return NULL;
    return &pipeline.batches[sent & (DEPTH - 1)];
}
static unsigned publish(Batch *b, unsigned operation, unsigned count)
{
    b->operation = operation;
    b->count = count;
    unsigned bytes = operation == 0 ?
        (unsigned)((uint8_t *)&b->blocks[count] - (uint8_t *)b) : 64;
    if (operation == 0) {
        sceKernelDcacheWritebackRange(b, 64);
        for (unsigned i = 0; i < count; ++i) {
            Reconstruction *r = &b->blocks[i];
            sceKernelDcacheWritebackRange(r, cache_span(offsetof(Reconstruction, motion) + r->count * sizeof(Motion)));
            if (r->cbp) sceKernelDcacheWritebackRange(r->coefficients, sizeof(r->coefficients));
#if PSP_ME_DECODE_CHECK
            sceKernelDcacheWritebackRange(r->expected, sizeof(r->expected) + sizeof(r->prediction));
#endif
        }
    } else if (operation == 3) {
        sceKernelDcacheWritebackRange(b, 64);
        sceKernelDcacheWritebackRange(b->filters, cache_span(count * sizeof(Filter)));
    } else sceKernelDcacheWritebackRange(b, bytes);
    __asm__ volatile("sync" ::: "memory");
    unsigned seq = control()->requested + 1;
    control()->requested = seq;
    return seq;
}
static void flush(void)
{
    if (!pending) return;
    Batch *b = &pipeline.batches[control()->requested & (DEPTH - 1)];
    if (pending > me_pipeline_max_batch) me_pipeline_max_batch = pending;
    publish(b, 0, pending);
    ++me_pipeline_batches;
    pending = 0;
}
static void flush_filters(void)
{
    if (!filter_pending) return;
    Batch *b = &pipeline.batches[control()->requested & (DEPTH - 1)];
    publish(b, 3, filter_pending);
    filter_pending = 0;
}
static void add_residual(Reconstruction *b, uint8_t *out[3], int16_t *coeff, unsigned segment)
{
    unsigned step = b->transform8 ? 4 : 1;
    if (b->cbp & 15)
        for (unsigned i = 0; i < 16; i += step) {
            if (!b->nonzero[i]) continue;
            int dc = b->nonzero[i] == 1 && coeff[i * 16];
            void (*fn)(uint8_t *, int16_t *, int);
            if (b->transform8) fn = dc ? me_h264_idct8_dc_add_8_c : me_h264_idct8_add_8_c;
            else fn = dc ? me_h264_idct_dc_add_8_c : me_h264_idct_add_8_c;
            fn = (void *)((unsigned)fn | segment);
            fn(out[0] + b->offsets[i], coeff + i * 16, b->stride[0]);
        }
    if (b->cbp & 0x30)
        for (unsigned plane = 1; plane < 3; ++plane) {
            int16_t *block = coeff + 256 + (plane - 1) * 64;
            if (b->chroma_dc[plane - 1]) {
                void (*dc)(int16_t *, int) = (void *)((unsigned)me_h264_chroma_dc_dequant_idct_8_c | segment);
                dc(block, b->qmul[plane - 1]);
            }
            for (unsigned i = 0; i < 4; ++i) {
                unsigned nnz = b->nonzero[plane * 16 + i];
                void (*fn)(uint8_t *, int16_t *, int) = NULL;
                /* Chroma nnz counts AC separately from transformed DC. */
                if (nnz) fn = me_h264_idct_add_8_c;
                else if (block[i * 16]) fn = me_h264_idct_dc_add_8_c;
                if (fn) {
                    fn = (void *)((unsigned)fn | segment);
                    fn(out[plane] + b->offsets[plane * 16 + i], block + i * 16, b->stride[plane]);
                }
            }
        }
}
int __attribute__((noinline)) me_worker(int index, void *param)
{
    (void)index;
    MeJob *j = (void *)((unsigned)param | 0x20000000u);
    Pipeline *cached = (void *)((unsigned)j->output | 0x80000000u);
    volatile Pipeline *q = (void *)((unsigned)cached | 0x20000000u);
    unsigned done = 0;
    q->ready = 1;
    __asm__ volatile("sync" ::: "memory");
    while (!q->stop || done != q->requested) {
        if (done == q->requested) continue;
        Batch *batch = &cached->batches[done & (DEPTH - 1)];
        meCoreDcacheInvalidateRange(batch, 64);
        q->trace_operation = batch->operation;
        if (batch->operation == 1) {
            if (batch->count)
                for (unsigned p = 0; p < 3; ++p) {
                    unsigned address = (unsigned)batch->planes[p] | 0x80000000u;
                    unsigned start = address & ~63u;
                    meCoreDcacheWritebackRange((void *)start,
                        cache_span(address - start + batch->stride[p] * batch->rows / (p ? 2 : 1)));
                }
            meCoreDcacheWritebackInvalidateAll();
        } else if (batch->operation == 2) {
            const uint8_t *planes[3];
            for (unsigned p = 0; p < 3; ++p)
                planes[p] = (void *)((unsigned)batch->planes[p] | 0x80000000u);
            const YuvRgbTables *t = (void *)((unsigned)&tables | 0x80000000u);
            uint32_t *out = (void *)((unsigned)rgba | 0x80000000u);
            meCoreDcacheInvalidateRange((void *)t, sizeof(tables));
            for (unsigned p = 0; p < 3; ++p)
                meCoreDcacheInvalidateRange((void *)planes[p], batch->stride[p] * (p ? 180 : 360));
            full_render_planes(planes, batch->stride, out, t);
            meCoreDcacheWritebackRange(out, sizeof(rgba));
        } else if (batch->operation == 3) {
            meCoreDcacheInvalidateRange(batch->filters, cache_span(batch->count * sizeof(Filter)));
            for (unsigned i = 0; i < batch->count; ++i) {
                Filter *f = &batch->filters[i];
#if PSP_ME_DECODE_CHECK
                q->trace_index = i;
                q->trace_function = (unsigned)f->function;
                q->trace_address = (unsigned)f->pix[0];
                __asm__ volatile("sync" ::: "memory");
#endif
                uint8_t *pix = (void *)((unsigned)f->pix[0] | 0x80000000u);
                if (f->kind == 2) {
                    uint8_t *border = (void *)((unsigned)f->border | 0x80000000u);
                    for (unsigned x = 0; x < 16; ++x) border[x] = pix[15 * f->stride + x];
                    for (unsigned p = 1; p < 3; ++p) {
                        const uint8_t *chroma = (void *)((unsigned)f->pix[p] | 0x80000000u);
                        for (unsigned x = 0; x < 8; ++x) border[16 + (p - 1) * 8 + x] = chroma[7 * f->uvstride + x];
                    }
                } else if (f->kind == 1) {
                    void (*fn)(uint8_t *, ptrdiff_t, int, int) = (void *)((unsigned)f->function | 0x80000000u);
                    fn(pix, f->stride, f->alpha, f->beta);
                } else {
                    void (*fn)(uint8_t *, ptrdiff_t, int, int, int8_t *) = (void *)((unsigned)f->function | 0x80000000u);
                    fn(pix, f->stride, f->alpha, f->beta, f->tc);
                }
            }
        } else {
            for (unsigned n = 0; n < batch->count; ++n) {
                Reconstruction *b = &batch->blocks[n];
                meCoreDcacheInvalidateRange(b, 64);
                meCoreDcacheInvalidateRange(b, cache_span(offsetof(Reconstruction, motion) + b->count * sizeof(Motion)));
                if (b->cbp) meCoreDcacheInvalidateRange(b->coefficients, sizeof(b->coefficients));
#if PSP_ME_DECODE_CHECK
                meCoreDcacheInvalidateRange(b->expected, sizeof(b->expected) + sizeof(b->prediction));
#endif
                uint8_t *out[3];
                for (unsigned p = 0; p < 3; ++p)
                    out[p] = (void *)((unsigned)b->dst[p] | 0x80000000u);
                for (unsigned i = 0; i < b->count; ++i) {
                    Motion *m = &b->motion[i];
                    uint8_t *dst = (void *)((unsigned)m->dst | 0x80000000u);
                    const uint8_t *src = (void *)((unsigned)m->src | 0x80000000u);
                    if (m->chroma) {
                        h264_chroma_mc_func fn = (void *)((unsigned)m->function | 0x80000000u);
                        fn(dst, src, m->stride, m->height, m->x, m->y);
                    } else {
                        qpel_mc_func fn = (void *)((unsigned)m->function | 0x80000000u);
                        fn(dst, src, m->stride);
                    }
                }
#if PSP_ME_DECODE_CHECK
                unsigned pos = 0;
                for (unsigned p = 0; p < 3; ++p)
                    for (unsigned y = 0; y < (p ? 8u : 16u); ++y)
                        for (unsigned x = 0; x < (p ? 8u : 16u); ++x)
                            if (out[p][y * b->stride[p] + x] != b->prediction[pos++]) {
                                if (!q->mismatches) {
                                    q->failure_mb = b->mb_y * 256 + b->mb_x;
                                    q->failure_pixel = (1u << 24) | (p << 16) | (y << 8) | x;
                                    q->failure_values = (out[p][y * b->stride[p] + x] << 8) | b->prediction[pos - 1];
                                }
                                ++q->mismatches;
                            }
#endif
                add_residual(b, out, b->coefficients, 0x80000000u);
#if PSP_ME_DECODE_CHECK
                pos = 0;
                for (unsigned p = 0; p < 3; ++p)
                    for (unsigned y = 0; y < (p ? 8u : 16u); ++y)
                        for (unsigned x = 0; x < (p ? 8u : 16u); ++x)
                            if (out[p][y * b->stride[p] + x] != b->expected[pos++]) {
                                if (!q->mismatches) {
                                    q->failure_mb = b->mb_y * 256 + b->mb_x;
                                    q->failure_pixel = (p << 16) | (y << 8) | x;
                                    q->failure_values = (out[p][y * b->stride[p] + x] << 8) | b->expected[pos - 1];
                                }
                                ++q->mismatches;
                            }
                if (q->mismatches) q->error = -213;
#endif
                if (b->cbp) meCoreDcacheWritebackInvalidateRange(b->coefficients, sizeof(b->coefficients));
            }
        }
        __asm__ volatile("sync" ::: "memory");
        q->completed = ++done;
    }
    meCoreDcacheWritebackInvalidateAll();
    q->exited = 1;
    j->done = 1;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}
int me_decode_start(void)
{
    memset(&pipeline, 0, sizeof(pipeline));
    me_decode_jobs = me_decode_blocks = me_decode_checked = me_decode_mismatches = 0;
    me_pipeline_batches = me_pipeline_handoffs = me_pipeline_wait_us = 0;
    me_pipeline_max_batch = me_pipeline_fallback = 0;
    me_pipeline_failure_mb = me_pipeline_failure_pixel = me_pipeline_failure_values = 0;
    me_decode_error = active = owned = running = pending = filtering = filter_pending = 0;
    me_pipeline_filters = me_pipeline_filter_rows = 0;
    border_buffers[0] = border_buffers[1] = NULL;
#if PSP_ME_DECODE_CHECK
    memset(published_valid, 0, sizeof(published_valid));
#endif
    picture = NULL;
    render_frame = NULL;
    me_decode_render_submitted = me_decode_render_completed = 0;
    yuv_rgb_init(&tables);
    int ret = meLabInit((unsigned)me_task);
    if (ret < 0) return ret;
    sceKernelDcacheWritebackInvalidateRange(&pipeline, sizeof(pipeline));
    job.output = (void *)&pipeline;
    ret = meLabSubmit(&job);
    if (ret < 0) return ret;
    running = 1;
    unsigned begin = sceKernelGetSystemTimeLow();
    while (!control()->ready)
        if (sceKernelGetSystemTimeLow() - begin > 500000) return -212;
    return 0;
}
void psp_me_retire(void)
{
    if (!running || !owned) return;
    flush();
    flush_filters();
    Batch *fence = reserve();
    if (!fence) return;
    unsigned top = first_row * 16;
    if (top >= 4) top -= 4;
    unsigned bottom = (last_row + 1) * 16;
    for (unsigned p = 0; p < 3; ++p) {
        fence->planes[p] = picture->cur_pic.f->data[p] + top / (p ? 2 : 1) * picture->cur_pic.f->linesize[p];
        fence->stride[p] = picture->cur_pic.f->linesize[p];
    }
    fence->rows = bottom - top;
    unsigned seq = publish(fence, 1, 1);
    if (wait_sequence(seq) < 0) return;
    /* The main CPU may have prefetched clean destination lines. Invalidate
     * pixel planes only; CABAC metadata remains dirty on the main CPU. */
    for (unsigned p = 0; p < 3; ++p)
        invalidate_pixels(picture->cur_pic.f->data[p] + top / (p ? 2 : 1) * picture->cur_pic.f->linesize[p],
            picture->cur_pic.f->linesize[p] * (bottom - top) / (p ? 2 : 1));
    for (unsigned i = 0; i < 2; ++i)
        if (border_buffers[i]) invalidate_pixels(border_buffers[i], border_bytes[i]);
#if PSP_ME_DECODE_CHECK
    if (!PSP_ME_PIPE_FILTER)
        for (unsigned mb = 0; mb < 40 * 23; ++mb) {
            if (!published_valid[mb]) continue;
            unsigned pos = 0;
            for (unsigned p = 0; p < 3; ++p) {
                unsigned width = p ? 8 : 16;
                const uint8_t *pixels = picture->cur_pic.f->data[p] + (mb / 40) * width * picture->cur_pic.f->linesize[p] + (mb % 40) * width;
                for (unsigned y = 0; y < width; ++y)
                    for (unsigned x = 0; x < width; ++x)
                        if (pixels[y * picture->cur_pic.f->linesize[p] + x] != published_expected[mb][pos++]) {
                            if (!me_decode_error) {
                                me_pipeline_failure_mb = (mb / 40) * 256 + mb % 40;
                                me_pipeline_failure_pixel = (p << 16) | (y << 8) | x;
                                me_pipeline_failure_values = (pixels[y * picture->cur_pic.f->linesize[p] + x] << 8) | published_expected[mb][pos - 1];
                            }
                            ++me_decode_mismatches;
                            me_decode_error = -219;
                        }
            }
            published_valid[mb] = 0;
        }
#endif
    owned = active = 0;
    ++me_pipeline_handoffs;
}
void psp_me_slice_begin(void)
{ psp_me_retire(); }
static int take_ownership(const H264Context *h, unsigned row)
{
    if (owned) {
        if (row < first_row) first_row = row;
        if (row > last_row) last_row = row;
        return 0;
    }
    sceKernelDcacheWritebackAll();
    Batch *fence = reserve();
    if (!fence) return -1;
    publish(fence, 1, 0);
    picture = h;
    first_row = last_row = row;
    owned = 1;
    return 0;
}
void psp_me_filter_begin(const H264Context *h, H264SliceContext *sl)
{
    filtering = 0;
    if (!PSP_ME_PIPE_FILTER || !running || me_decode_error || h->pixel_shift ||
        CHROMA444(h) || CHROMA422(h) || FRAME_MBAFF(h) ||
        h->picture_structure != PICT_FRAME || h->postpone_filter || !sl->deblocking_filter) {
        psp_me_retire();
        return;
    }
    flush();
    if (take_ownership(h, sl->mb_y) < 0) return;
    for (unsigned i = 0; i < 2; ++i) {
        border_buffers[i] = (void *)sl->top_borders[i];
        border_bytes[i] = sl->top_borders_allocated[i];
    }
    filtering = 1;
    ++me_pipeline_filter_rows;
}
void psp_me_filter_end(void)
{
    flush_filters();
    filtering = 0;
}
static Filter *new_filter(void)
{
    Batch *b = filter_pending ? &pipeline.batches[control()->requested & (DEPTH - 1)] : reserve();
    if (!b) return NULL;
    Filter *f = &b->filters[filter_pending++];
    return f;
}
int psp_me_backup(H264SliceContext *sl, const uint8_t *y, const uint8_t *cb, const uint8_t *cr, int stride, int uvstride)
{
    if (!filtering) return 0;
    Filter *f = new_filter();
    if (!f) return 1;
    f->kind = 2;
    f->pix[0] = (void *)y; f->pix[1] = (void *)cb; f->pix[2] = (void *)cr;
    f->border = sl->top_borders[1][sl->mb_x];
    f->stride = stride; f->uvstride = uvstride;
    if (filter_pending == FILTERS) flush_filters();
    return 1;
}
static int eligible(const H264Context *h, const H264SliceContext *sl)
{
    unsigned type = h->cur_pic.mb_type[sl->mb_xy];
    if (h->mb_width != 40 || h->mb_height != 23 || h->pixel_shift || CHROMA444(h) || CHROMA422(h) ||
        h->picture_structure != PICT_FRAME || FRAME_MBAFF(h) ||
        !IS_INTER(type) || sl->qscale == 0 || sl->is_complex ||
        sl->linesize > MAX_STRIDE || sl->linesize & 63 || sl->uvlinesize & 63 ||
        sl->pwt.use_weight == 1) return 0;
    for (unsigned i = 0; i < 16; ++i) {
        int r0 = sl->ref_cache[0][scan8[i]], r1 = sl->ref_cache[1][scan8[i]];
        if (sl->pwt.use_weight == 2 && r0 >= 0 && r1 >= 0 &&
            sl->pwt.implicit_weight[r0][r1][sl->mb_y & 1] != 32) return 0;
        for (unsigned list = 0; list < sl->list_count; ++list) {
            int r = sl->ref_cache[list][scan8[i]];
            if (r < 0) continue;
            if (sl->ref_list[list][r].data[0] == h->cur_pic.f->data[0]) return 0;
            int x = sl->mb_x * 16 + (sl->mv_cache[list][scan8[i]][0] >> 2);
            int y = sl->mb_y * 16 + (sl->mv_cache[list][scan8[i]][1] >> 2);
            /* Conservative bound: no live edge scratch may cross cores. */
            /* FFmpeg's edge test reserves 16 pixels even for a 4/8-pixel
             * partition, plus its partition origin (up to 12 pixels). */
            if (x < 4 || y < 4 || x + 32 >= h->width || y + 32 >= h->height) return 0;
        }
    }
    return 1;
}
void psp_me_before_mb(const H264Context *h, H264SliceContext *sl)
{
    active = 0;
    if (!running || me_decode_error || !eligible(h, sl)) {
        psp_me_retire();
        ++me_pipeline_fallback;
        return;
    }
    if (take_ownership(h, sl->mb_y) < 0) return;
    Batch *batch = pending ? &pipeline.batches[control()->requested & (DEPTH - 1)] : reserve();
    if (!batch) return;
    current = &batch->blocks[pending];
    current->count = 0;
    edge_begin = (uintptr_t)sl->edge_emu_buffer;
    edge_end = edge_begin + sl->edge_emu_buffer_allocated;
    current->mb_x = sl->mb_x; current->mb_y = sl->mb_y;
    for (unsigned p = 0; p < 3; ++p) {
        current->stride[p] = h->cur_pic.f->linesize[p];
        current->dst[p] = h->cur_pic.f->data[p] + sl->mb_y * (p ? 8 : 16) * current->stride[p] + sl->mb_x * (p ? 8 : 16);
    }
    active = 1;
    ++me_decode_blocks;
}
void psp_me_motion_begin(const H264Context *h, H264SliceContext *sl, uint8_t *dst, int stride)
{ (void)h; (void)sl; (void)dst; (void)stride; }
void psp_me_motion_end(void) {}
int psp_me_luma_submit(const H264Context *h, H264SliceContext *sl, int type)
{
    if (!active) return 0;
    current->cbp = sl->cbp;
    current->transform8 = !!IS_8x8DCT(type);
#if PSP_ME_DECODE_CHECK
    memcpy(reference_coefficients, sl->mb, sizeof(reference_coefficients));
#endif
    if (sl->cbp) {
        for (unsigned i = 0; i < 48; ++i) {
            current->offsets[i] = h->block_offset[i];
            current->nonzero[i] = sl->non_zero_count_cache[scan8[i]];
        }
        for (unsigned p = 0; p < 2; ++p) {
            current->qmul[p] = h->ps.pps->dequant4_coeff[4 + p][sl->chroma_qp[p]][0];
            current->chroma_dc[p] = sl->non_zero_count_cache[scan8[CHROMA_DC_BLOCK_INDEX + p]];
        }
        memcpy(current->coefficients, sl->mb, 512);
        memcpy(current->coefficients + 256, sl->mb + 256, 128);
        memcpy(current->coefficients + 320, sl->mb + 512, 128);
        memset(sl->mb, 0, 256 * sizeof(*sl->mb));
        memset(sl->mb + 256, 0, 64 * sizeof(*sl->mb));
        memset(sl->mb + 512, 0, 64 * sizeof(*sl->mb));
    }
#if PSP_ME_DECODE_CHECK
    unsigned pos = 0;
    for (unsigned p = 0; p < 3; ++p)
        for (unsigned y = 0; y < (p ? 8u : 16u); ++y) {
            unsigned width = p ? 8 : 16;
            memcpy(current->prediction + pos, reference[p] + y * current->stride[p], width);
            pos += width;
        }
    /* Use the original aggregate FFmpeg DSP entry points as the independent
     * reference, including its original coefficient layout and chroma NNZ. */
    if (sl->cbp & 15) {
        if (IS_8x8DCT(type))
            h->h264dsp.h264_idct8_add4(reference[0], h->block_offset, reference_coefficients,
                                      current->stride[0], sl->non_zero_count_cache);
        else h->h264dsp.h264_idct_add16(reference[0], h->block_offset, reference_coefficients,
                                       current->stride[0], sl->non_zero_count_cache);
    }
    if (sl->cbp & 0x30) {
        for (unsigned p = 0; p < 2; ++p)
            if (current->chroma_dc[p])
                h->h264dsp.h264_chroma_dc_dequant_idct(reference_coefficients + 256 * (p + 1), current->qmul[p]);
        uint8_t *out[2] = {reference[1], reference[2]};
        h->h264dsp.h264_idct_add8(out, h->block_offset, reference_coefficients,
                                 current->stride[1], sl->non_zero_count_cache);
    }
#endif
    return 1;
}
int psp_me_chroma_submit(void) { return active; }
void psp_me_commit(void)
{
    if (!active) return;
#if PSP_ME_DECODE_CHECK
    unsigned pos = 0;
    for (unsigned p = 0; p < 3; ++p)
        for (unsigned y = 0; y < (p ? 8u : 16u); ++y) {
            unsigned width = p ? 8 : 16;
            memcpy(current->expected + pos, reference[p] + y * current->stride[p], width);
            pos += width;
        }
    ++me_decode_checked;
    if (!PSP_ME_PIPE_FILTER) {
        unsigned mb = current->mb_y * 40 + current->mb_x;
        memcpy(published_expected[mb], current->expected, sizeof(current->expected));
        published_valid[mb] = 1;
    }
#endif
    active = 0;
    if (++pending == PSP_ME_PIPE_BATCH) flush();
}
static void capture(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                    void *function, unsigned chroma, unsigned height, unsigned x, unsigned y)
{
    if ((uintptr_t)src >= edge_begin && (uintptr_t)src < edge_end) {
        me_decode_error = -218; /* a mutable edge scratch pointer is forbidden */
        return;
    }
    if (current->count == MOTIONS) { me_decode_error = -214; return; }
    Motion *m = &current->motion[current->count++];
    m->function = function;
    m->dst = dst; m->src = src; m->stride = stride;
    m->chroma = chroma; m->height = height; m->x = x; m->y = y;
    ++me_decode_jobs;
#if PSP_ME_DECODE_CHECK
    unsigned p = 0;
    if (chroma) {
        uintptr_t address = (uintptr_t)dst;
        uintptr_t cb = (uintptr_t)current->dst[1], cr = (uintptr_t)current->dst[2];
        p = address >= cb && address < cb + 8 * stride ? 1 : 2;
        if (!(address >= (uintptr_t)current->dst[p] && address < (uintptr_t)current->dst[p] + 8 * stride)) {
            (void)cr; me_decode_error = -215; return;
        }
    }
    uint8_t *out = reference[p] + (dst - current->dst[p]);
    if (chroma) ((h264_chroma_mc_func)function)(out, src, stride, height, x, y);
    else ((qpel_mc_func)function)(out, src, stride);
#endif
}
static void predict(uint8_t *d, const uint8_t *s, ptrdiff_t st, unsigned avg, unsigned size, unsigned xy)
{
    if (!active) { qfallback[avg][size][xy](d, s, st); return; }
    capture(d, s, st, (void *)qkernels[avg][size][xy], 0, 0, 0, 0);
}
static void chroma(uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y, unsigned avg, unsigned size)
{
    if (!active) { cfallback[avg][size](d, s, st, h, x, y); return; }
    capture(d, s, st, (void *)ckernels[avg][size], 1, h, x, y);
}
#define PHASE(S,N) \
static void put_##S##_##N(uint8_t *d,const uint8_t *s,ptrdiff_t st){predict(d,s,st,0,S,N);} \
static void avg_##S##_##N(uint8_t *d,const uint8_t *s,ptrdiff_t st){predict(d,s,st,1,S,N);}
#define PHASES(S) PHASE(S,0) PHASE(S,1) PHASE(S,2) PHASE(S,3) PHASE(S,4) PHASE(S,5) PHASE(S,6) PHASE(S,7) PHASE(S,8) PHASE(S,9) PHASE(S,10) PHASE(S,11) PHASE(S,12) PHASE(S,13) PHASE(S,14) PHASE(S,15)
PHASES(0) PHASES(1) PHASES(2)
#define FUNCTIONS(P,S) {P##_##S##_0,P##_##S##_1,P##_##S##_2,P##_##S##_3,P##_##S##_4,P##_##S##_5,P##_##S##_6,P##_##S##_7,P##_##S##_8,P##_##S##_9,P##_##S##_10,P##_##S##_11,P##_##S##_12,P##_##S##_13,P##_##S##_14,P##_##S##_15}
static const qpel_mc_func qw[2][3][16] = {
 {FUNCTIONS(put,0),FUNCTIONS(put,1),FUNCTIONS(put,2)},
 {FUNCTIONS(avg,0),FUNCTIONS(avg,1),FUNCTIONS(avg,2)}};
#define CHROMA_WRAPPERS(S) \
static void cput##S(uint8_t*d,const uint8_t*s,ptrdiff_t st,int h,int x,int y){chroma(d,s,st,h,x,y,0,S);} \
static void cavg##S(uint8_t*d,const uint8_t*s,ptrdiff_t st,int h,int x,int y){chroma(d,s,st,h,x,y,1,S);}
CHROMA_WRAPPERS(0) CHROMA_WRAPPERS(1) CHROMA_WRAPPERS(2) CHROMA_WRAPPERS(3)
static const h264_chroma_mc_func cw[2][4] = {{cput0,cput1,cput2,cput3},{cavg0,cavg1,cavg2,cavg3}};
void __real_ff_h264qpel_init(H264QpelContext *, int);
void __real_ff_h264chroma_init(H264ChromaContext *, int);
void me_decode_qpel_init(H264QpelContext *c, int depth)
{
    if (depth != 8) return;
    H264QpelContext scalar;
    __real_ff_h264qpel_init(&scalar, 8);
    memcpy(qkernels[0], scalar.put_h264_qpel_pixels_tab, sizeof(qkernels[0]));
    memcpy(qkernels[1], scalar.avg_h264_qpel_pixels_tab, sizeof(qkernels[1]));
    memcpy(qfallback[0], c->put_h264_qpel_pixels_tab, sizeof(qfallback[0]));
    memcpy(qfallback[1], c->avg_h264_qpel_pixels_tab, sizeof(qfallback[1]));
    memcpy(c->put_h264_qpel_pixels_tab, qw[0], sizeof(qw[0]));
    memcpy(c->avg_h264_qpel_pixels_tab, qw[1], sizeof(qw[1]));
}
void me_decode_chroma_init(H264ChromaContext *c, int depth)
{
    if (depth != 8) return;
    H264ChromaContext scalar;
    __real_ff_h264chroma_init(&scalar, 8);
    memcpy(ckernels[0], scalar.put_h264_chroma_pixels_tab, sizeof(ckernels[0]));
    memcpy(ckernels[1], scalar.avg_h264_chroma_pixels_tab, sizeof(ckernels[1]));
    memcpy(cfallback[0], c->put_h264_chroma_pixels_tab, sizeof(cfallback[0]));
    memcpy(cfallback[1], c->avg_h264_chroma_pixels_tab, sizeof(cfallback[1]));
    memcpy(c->put_h264_chroma_pixels_tab, cw[0], sizeof(cw[0]));
    memcpy(c->avg_h264_chroma_pixels_tab, cw[1], sizeof(cw[1]));
}
typedef void (*EdgeKernel)(uint8_t *, ptrdiff_t, int, int, int8_t *);
typedef void (*IntraEdgeKernel)(uint8_t *, ptrdiff_t, int, int);
static EdgeKernel edge_kernels[4];
static IntraEdgeKernel intra_edge_kernels[4];
static void filter_edge(uint8_t *pix, ptrdiff_t stride, int alpha, int beta, int8_t *tc, unsigned index)
{
    if (!filtering) { edge_kernels[index](pix, stride, alpha, beta, tc); return; }
    Filter *f = new_filter();
    if (!f) return;
    f->kind = 0; f->function = (void *)edge_kernels[index];
    f->pix[0] = pix; f->stride = stride; f->alpha = alpha; f->beta = beta;
    memcpy(f->tc, tc, 4);
    ++me_pipeline_filters;
    if (filter_pending == FILTERS) flush_filters();
}
static void filter_intra_edge(uint8_t *pix, ptrdiff_t stride, int alpha, int beta, unsigned index)
{
    if (!filtering) { intra_edge_kernels[index](pix, stride, alpha, beta); return; }
    Filter *f = new_filter();
    if (!f) return;
    f->kind = 1; f->function = (void *)intra_edge_kernels[index];
    f->pix[0] = pix; f->stride = stride; f->alpha = alpha; f->beta = beta;
    ++me_pipeline_filters;
    if (filter_pending == FILTERS) flush_filters();
}
#define EDGE_WRAPPERS(N) \
static void edge##N(uint8_t *p,ptrdiff_t s,int a,int b,int8_t *t){filter_edge(p,s,a,b,t,N);} \
static void iedge##N(uint8_t *p,ptrdiff_t s,int a,int b){filter_intra_edge(p,s,a,b,N);}
EDGE_WRAPPERS(0) EDGE_WRAPPERS(1) EDGE_WRAPPERS(2) EDGE_WRAPPERS(3)
void __real_ff_h264dsp_init(H264DSPContext *, int, int);
void __wrap_ff_h264dsp_init(H264DSPContext *c, int depth, int chroma_idc)
{
    __real_ff_h264dsp_init(c, depth, chroma_idc);
    if (!PSP_ME_PIPE_FILTER || depth != 8 || chroma_idc != 1) return;
#define INSTALL_EDGE(FIELD,N) \
    edge_kernels[N] = c->FIELD; c->FIELD = edge##N; \
    intra_edge_kernels[N] = c->FIELD##_intra; c->FIELD##_intra = iedge##N
    INSTALL_EDGE(h264_v_loop_filter_luma,0);
    INSTALL_EDGE(h264_h_loop_filter_luma,1);
    INSTALL_EDGE(h264_v_loop_filter_chroma,2);
    INSTALL_EDGE(h264_h_loop_filter_chroma,3);
#undef INSTALL_EDGE
}
int me_decode_render_finish(void)
{
    if (!render_frame) return 0;
    if (wait_sequence(render_sequence) < 0) return me_decode_error;
    uint32_t *dst = (void *)((unsigned)sceGeEdramGetAddr() | 0x40000000u);
    int ret = sceDmacMemcpy(dst + 512, rgba + 512, 512 * 270 * 4);
    if (ret < 0) return ret;
    av_frame_free(&render_frame);
    ++me_decode_render_completed;
    return 0;
}
int me_decode_render(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width != 640 || frame->height != 360) return -216;
    psp_me_retire();
    int ret = me_decode_render_finish();
    if (ret < 0) return ret;
    render_frame = av_frame_clone(frame);
    if (!render_frame) return -217;
    Batch *b = reserve();
    if (!b) return me_decode_error;
    for (unsigned p = 0; p < 3; ++p) {
        b->planes[p] = frame->data[p];
        b->stride[p] = frame->linesize[p];
        sceKernelDcacheWritebackRange(frame->data[p], frame->linesize[p] * (p ? 180 : 360));
    }
    render_sequence = publish(b, 2, 0);
    ++me_decode_render_submitted;
    return 0;
}
int me_decode_stop(void)
{
    int ret = 0;
    if (running) {
        psp_me_retire();
        ret = me_decode_render_finish();
        if (wait_sequence(control()->requested) < 0) ret = me_decode_error;
        control()->stop = 1;
        __asm__ volatile("sync" ::: "memory");
        int waited = meLabWait();
        if (!ret) ret = waited;
        running = 0;
    }
    int shutdown = meLabShutdown();
    if (!ret) ret = shutdown;
    if (!ret) ret = me_decode_error;
    return ret;
}
