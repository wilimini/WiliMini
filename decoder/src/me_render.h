#ifndef PSP_LAB_ME_RENDER_H
#define PSP_LAB_ME_RENDER_H
#if PSP_ME_RENDER
#if PSP_ME_FRAME_PIPELINE
#include "me_bridge.h"
#include "me_frame_pipeline.h"
static unsigned me_submitted, me_completed;
#if PSP_PTS_PACE
static uint64_t me_total_lateness_us;
static unsigned me_late_frames, me_max_lateness_us;
#endif
static int mefp_hash_next;
static MefpStats mefp_final_stats;
#if PSP_AUDIO && PSP_RGB_PACE
static int mefp_rgb_acquire_cb(void **out_pixels)
{
    int slot = rgb_acquire();
    if (slot < 0) return slot;
    *out_pixels = rgb_slot_pixels((unsigned)slot);
    return 0;
}
static void mefp_rgb_publish_cb(int64_t pts)
{
    rgb_publish(pts);
}
static void mefp_rgb_release_cb(void)
{
    sceKernelSignalSema(rgb_space, 1);
}
#endif
static int me_render_start(void)
{
    me_submitted = me_completed = 0;
    mefp_hash_next = 0;
    memset(&mefp_final_stats, 0, sizeof(mefp_final_stats));
#if PSP_PTS_PACE
    me_total_lateness_us = 0;
    me_late_frames = me_max_lateness_us = 0;
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    int ret = rgb_start();
    if (ret == AVERROR(EBUSY)) playback_diagnostic("rgb_buffer_retained_busy", ret);
    if (ret < 0) return ret;
    ret = mefp_start((int)output_pixel_format,
                     mefp_rgb_acquire_cb, mefp_rgb_publish_cb, mefp_rgb_release_cb);
    if (ret < 0) {
        rgb_close();
        rgb_release();
        return ret;
    }
    return 0;
#else
    return mefp_start((int)output_pixel_format, NULL, NULL, NULL);
#endif
}
static int me_render_submit(const AVFrame *frame)
{
    int64_t pts = 0;
#if PSP_AUDIO && PSP_RGB_PACE
    pts = presentation_pts_us;
#endif
    int ret = mefp_render_submit(frame, pts, mefp_hash_next);
    mefp_hash_next = 0;
    if (ret >= 0) ++me_submitted;
    return ret;
}
static int me_render_finish(void)
{
    int ret = mefp_drain();
    mefp_get_stats(&mefp_final_stats);
    me_completed = mefp_final_stats.render_completed;
    return ret;
}
static int me_render_shutdown(void)
{
    int ret = mefp_drain();
    mefp_get_stats(&mefp_final_stats);
    me_completed = mefp_final_stats.render_completed;
#if PSP_AUDIO && PSP_RGB_PACE
    rgb_close();
#endif
    int shutdown = mefp_stop();
#if PSP_AUDIO && PSP_RGB_PACE
    if (shutdown >= 0) rgb_release();
#endif
    return ret < 0 ? ret : shutdown;
}
#else /* !PSP_ME_FRAME_PIPELINE */
#include "me_task.h"
#if PSP_VME_QPEL
#include "h264_vme_qpel.h"
#endif
#if PSP_ME_DMA
#include <pspdmac.h>
#endif
#if PSP_ME_RENDER_QUEUE
_Static_assert(PSP_ME_RENDER_QUEUE >= 2 && PSP_ME_RENDER_QUEUE <= 16, "presentation queue depth 2..16");
static uint64_t queue_worker_deadline;
static int64_t queue_worker_pts;
static uint64_t me_warmup_presented_us, me_last_presented_us;
#define me_render_start me_render_start_one
#define me_render_submit me_render_submit_one
#define me_render_finish me_render_finish_one
#endif
static MeJob me_job;
#if PSP_AUDIO && PSP_RGB_PACE
static void *me_output;
static int64_t me_rgb_pts;
#else
static uint32_t me_output[512 * 272] __attribute__((aligned(64)));
#endif
static AVFrame *me_frame;
static int me_pending;
#if PSP_PTS_PACE
static uint64_t me_presentation_deadline;
static uint64_t me_total_lateness_us;
static unsigned me_late_frames, me_max_lateness_us;
#endif
static unsigned me_submitted, me_completed;
static int me_render_finish(void)
{
    if (!me_pending) return 0;
    int ret = meLabWait();
    if (ret < 0) {
        if (!PSP_ME_RENDER_QUEUE) playback_diagnostic("me_task_wait_failed", ret);
        return ret;
    }
    if (!PSP_ME_RENDER_QUEUE && !me_completed) playback_diagnostic("me_first_task_completed", ret);
#if !PSP_ME_CACHE || !PSP_ME_DMA
    sceKernelDcacheInvalidateRange(me_output, 512 * 272 * rgb_pixel_bytes);
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    rgb_publish(me_rgb_pts);
#else
    uint8_t *dst = (void *)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u);
#if PSP_PTS_PACE
    wait_presentation(me_presentation_deadline);
#endif
#if PSP_ME_DMA
    ret = sceDmacMemcpy(dst + 512 * rgb_pixel_bytes,
                        (uint8_t *)me_output + 512 * rgb_pixel_bytes,
                        512 * 270 * rgb_pixel_bytes);
    if (ret < 0) return ret;
#else
    memcpy(dst + 512 * rgb_pixel_bytes,
           (uint8_t *)me_output + 512 * rgb_pixel_bytes,
           512 * 270 * rgb_pixel_bytes);
#endif
#if PSP_PTS_PACE
    uint64_t presented = sceKernelGetSystemTimeWide();
    unsigned late = presented > me_presentation_deadline ?
        (unsigned)(presented - me_presentation_deadline) : 0;
    me_total_lateness_us += late;
    if (late > 40000) ++me_late_frames;
    if (late > me_max_lateness_us) me_max_lateness_us = late;
#endif
#endif /* synchronous presentation */
#if PSP_ME_RENDER_QUEUE
    me_last_presented_us = sceKernelGetSystemTimeWide();
    if (me_completed + 1 == WARMUP_FRAMES) me_warmup_presented_us = me_last_presented_us;
    me_frame = NULL; /* Queue references are released on the decoder thread. */
#else
    av_frame_free(&me_frame);
#endif
    me_pending = 0;
    ++me_completed;
    return 0;
}
static int me_render_start(void)
{
    yuv_rgb_init(&me_tables);
    me_submitted = me_completed = 0;
#if PSP_PTS_PACE
    me_total_lateness_us = 0;
    me_late_frames = me_max_lateness_us = 0;
#endif
    int ret;
    switch (meLabBackend()) {
    case ME_BACKEND_CUSTOM_CORE: ret = meLabInit((unsigned)me_worker); break;
    case ME_BACKEND_MIST: ret = meLabInit((unsigned)me_task); break;
    default: return AVERROR(ENODEV);
    }
    if (ret < 0) return ret;
    meCoreSelectSystemTable(ret);
    sceKernelDcacheWritebackRange(&meCoreSystemTable, sizeof(meCoreSystemTable));
#if PSP_VME_RENDER || PSP_VME_QPEL
    if (meLabBackend() == ME_BACKEND_CUSTOM_CORE) {
        vme_contexts_init();
        memset(&me_job, 0, sizeof(me_job));
        me_job.operation = 0;
        int vret = meLabSubmit(&me_job);
        if (vret >= 0) vret = meLabWait();
        if (vret < 0) return vret;
    }
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    if (ret >= 0) ret = rgb_start();
    if (ret == AVERROR(EBUSY)) playback_diagnostic("rgb_buffer_retained_busy", ret);
#endif
    return ret;
}
static int me_render_submit(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0 || frame->width > 1280 || frame->height > 720) return -1;
    int ret = me_render_finish();
    if (ret < 0) return ret;
#if PSP_AUDIO && PSP_RGB_PACE
    int slot = rgb_acquire();
    if (slot < 0) return slot;
    me_output = rgb_slot_pixels((unsigned)slot);
#if PSP_ME_RENDER_QUEUE
    me_rgb_pts = queue_worker_pts;
#else
    me_rgb_pts = presentation_pts_us;
#endif
#endif
#if PSP_ME_RENDER_QUEUE
    me_frame = (AVFrame *)frame;
#else
    me_frame = av_frame_clone(frame);
#endif
    if (!me_frame) {
#if PSP_AUDIO && PSP_RGB_PACE
        sceKernelSignalSema(rgb_space, 1);
#endif
        return -1;
    }
#if PSP_PTS_PACE
#if PSP_ME_RENDER_QUEUE
    me_presentation_deadline = queue_worker_deadline;
#else
    me_presentation_deadline = presentation_deadline;
#endif
#endif
    me_job.operation = 1;
    me_job.value = 0; /* Do not report progress left over from the previous job. */
    me_job.width = frame->width;
    me_job.height = frame->height;
    for (int p = 0; p < 3; ++p) {
        me_job.planes[p] = frame->data[p];
        me_job.strides[p] = frame->linesize[p];
#if !PSP_ME_CACHE
        sceKernelDcacheWritebackRange(frame->data[p], frame->linesize[p] * (p ? (frame->height + 1) / 2 : frame->height));
#endif
    }
#if PSP_ME_CACHE
    /* Flush the 16 KiB CPU D-cache once, rather than scanning ~345 KiB. */
    sceKernelDcacheWritebackAll();
#endif
    me_job.output = me_output;
    me_job.output_format = (uint32_t)output_pixel_format;
    if (!PSP_ME_RENDER_QUEUE && !me_submitted) playback_diagnostic("me_first_submit_begin", 0);
    ret = meLabSubmit(&me_job);
    if (!PSP_ME_RENDER_QUEUE && (!me_submitted || ret < 0)) playback_diagnostic("me_submit_end", ret);
    if (ret < 0) {
#if PSP_AUDIO && PSP_RGB_PACE
        sceKernelSignalSema(rgb_space, 1);
#endif
#if !PSP_ME_RENDER_QUEUE
        av_frame_free(&me_frame);
#else
        me_frame = NULL;
#endif
        return ret;
    }
    me_pending = 1;
    ++me_submitted;
#if PSP_ME_RENDER == 1
    return me_render_finish();
#else
    return 0;
#endif
}
#if PSP_ME_RENDER_QUEUE
#undef me_render_start
#undef me_render_submit
#undef me_render_finish
#include "present_queue.h"
#endif
static int me_render_shutdown(void)
{
#if PSP_AUDIO && PSP_RGB_PACE
    rgb_close();
#endif
#if PSP_ME_RENDER_QUEUE
    int ret = present_shutdown();
    int shutdown = meLabShutdown();
#else
    int ret = 0;
    int shutdown = meLabShutdown();
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    // A timeout does not prove the ME stopped writing. Keep its output alive.
    if (shutdown >= 0) rgb_release();
#endif
    return ret < 0 ? ret : shutdown;
}
#if PSP_VME_QPEL
#define VME_QPEL_SLOTS 6
#define VME_QPEL_PLANE_BYTES (VME_HP_W * VME_HP_MAX_H)
#define VME_QPEL_SLOT_BYTES  (VME_QPEL_PLANE_BYTES * 3)
static uint8_t *vme_qpel_pool_mem;
static unsigned vme_qpel_free_mask;
static VmeHalfpelJob vme_hp_job __attribute__((aligned(64)));
static unsigned vme_qpel_refs_built;
static uint64_t vme_qpel_total_us;

static void vme_qpel_slot_free(void *opaque, uint8_t *data)
{
    (void)data;
    unsigned slot = (unsigned)(uintptr_t)opaque;
    if (slot < VME_QPEL_SLOTS) vme_qpel_free_mask |= (1u << slot);
}

static AVBufferRef *vme_qpel_ref_build(const uint8_t *luma, int stride,
                                       int width, int height,
                                       PspVmeHalfpelPlanes *out)
{
    if (!vme_qpel_pool_mem || !vme_qpel_free_mask) return NULL;
    if (width != VME_HP_W || height <= 0 || height > VME_HP_MAX_H || stride < VME_HP_W)
        return NULL;
    if (me_render_finish() < 0) return NULL;

    unsigned slot = 0;
    while (slot < VME_QPEL_SLOTS && !(vme_qpel_free_mask & (1u << slot))) ++slot;
    if (slot >= VME_QPEL_SLOTS) return NULL;

    uint8_t *base = vme_qpel_pool_mem + (size_t)slot * VME_QPEL_SLOT_BYTES;
    uint8_t *p20 = base;
    uint8_t *p02 = base + VME_QPEL_PLANE_BYTES;
    uint8_t *p22 = base + VME_QPEL_PLANE_BYTES * 2;

    AVBufferRef *ref = av_buffer_create(base, VME_QPEL_SLOT_BYTES,
                                        vme_qpel_slot_free,
                                        (void *)(uintptr_t)slot, 0);
    if (!ref) return NULL;
    vme_qpel_free_mask &= ~(1u << slot);

    uint64_t t0 = sceKernelGetSystemTimeWide();
    vme_hp_job.p20 = p20;
    vme_hp_job.p02 = p02;
    vme_hp_job.p22 = p22;
    sceKernelDcacheWritebackAll();

    memset(&me_job, 0, sizeof(me_job));
    me_job.operation = 2;
    me_job.planes[0] = luma;
    me_job.strides[0] = stride;
    me_job.width = width;
    me_job.height = height;
    me_job.output = &vme_hp_job;

    int ret = meLabSubmit(&me_job);
    if (ret >= 0) ret = meLabWait();
    if (ret < 0 || me_job.value != 0x4d450002u) {
        av_buffer_unref(&ref);
        return NULL;
    }
    sceKernelDcacheWritebackInvalidateAll();
    vme_qpel_total_us += sceKernelGetSystemTimeWide() - t0;
    ++vme_qpel_refs_built;

    out->p20 = p20;
    out->p02 = p02;
    out->p22 = p22;
    out->stride = VME_HP_W;
    out->width = width;
    out->height = height;
    return ref;
}

static void vme_qpel_start(void)
{
    vme_qpel_refs_built = 0;
    vme_qpel_total_us = 0;
    avpriv_psp_vme_ref_build = NULL;
    if (meLabBackend() != ME_BACKEND_CUSTOM_CORE) return;
    if (!vme_qpel_pool_mem)
        vme_qpel_pool_mem = memalign(64, (size_t)VME_QPEL_SLOTS * VME_QPEL_SLOT_BYTES);
    if (!vme_qpel_pool_mem) return;
    vme_qpel_free_mask = (1u << VME_QPEL_SLOTS) - 1u;
    avpriv_psp_vme_ref_build = vme_qpel_ref_build;
    WLOG_I(WLC_MEM, "vme_qpel_pool slots=%u bytes=%u",
           (unsigned)VME_QPEL_SLOTS, (unsigned)(VME_QPEL_SLOTS * VME_QPEL_SLOT_BYTES));
}

static void vme_qpel_stop(void)
{
    avpriv_psp_vme_ref_build = NULL;
}

static void vme_qpel_free(void)
{
    avpriv_psp_vme_ref_build = NULL;
    if (vme_qpel_pool_mem && vme_qpel_free_mask == ((1u << VME_QPEL_SLOTS) - 1u)) {
        free(vme_qpel_pool_mem);
        vme_qpel_pool_mem = NULL;
    }
}
#endif
#endif /* !PSP_ME_FRAME_PIPELINE */
#endif
#endif
