#ifndef PSP_LAB_ME_RENDER_H
#define PSP_LAB_ME_RENDER_H
#if PSP_ME_RENDER
#include "me_task.h"
#if PSP_ME_DMA
#include <pspdmac.h>
#endif
#if PSP_ME_RENDER_QUEUE
_Static_assert(PSP_ME_RENDER_QUEUE >= 2 && PSP_ME_RENDER_QUEUE <= 16, "presentation queue depth 2..16");
static uint64_t queue_worker_deadline;
static uint64_t me_warmup_presented_us, me_last_presented_us;
#define me_render_start me_render_start_one
#define me_render_submit me_render_submit_one
#define me_render_finish me_render_finish_one
#endif
static MeJob me_job;
#if PSP_AUDIO && PSP_RGB_PACE
static uint32_t *me_output;
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
static unsigned me_checked, me_mismatches;
static unsigned me_submitted, me_completed;
#if PSP_ME_CHECK
static uint32_t me_expected[512 * 272] __attribute__((aligned(64)));
#endif
static int me_render_finish(void)
{
    if (!me_pending) return 0;
    int ret = meLabWait();
    if (ret < 0) return ret;
#if !PSP_ME_CACHE || !PSP_ME_DMA || PSP_ME_CHECK
    sceKernelDcacheInvalidateRange(me_output, 512 * 272 * sizeof(uint32_t));
#endif
#if PSP_ME_CHECK
    full_render_planes((const uint8_t *const *)me_frame->data, me_frame->linesize,
                       me_expected, &me_tables);
    ++me_checked;
    for (int y = 1; y <= 270; ++y) {
        if (memcmp(me_expected + y * 512, me_output + y * 512, 480 * 4)) {
            ++me_mismatches;
            return -10;
        }
    }
#endif
#if PSP_AUDIO && PSP_RGB_PACE
    rgb_publish(me_rgb_pts);
#else
    uint32_t *dst = (void *)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u);
#if PSP_PTS_PACE
    wait_presentation(me_presentation_deadline);
#endif
#if PSP_ME_DMA
    ret = sceDmacMemcpy(dst + 512, me_output + 512, 512 * 270 * 4);
    if (ret < 0) return ret;
#else
    memcpy(dst + 512, me_output + 512, 512 * 270 * 4);
#endif
#if PSP_ME_CHECK
    for (int y = 1; y <= 270; ++y) {
        if (memcmp(dst + y * 512, me_output + y * 512, 480 * 4)) {
            ++me_mismatches;
            return -11;
        }
    }
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
    me_checked = me_mismatches = 0;
    me_submitted = me_completed = 0;
#if PSP_PTS_PACE
    me_total_lateness_us = 0;
    me_late_frames = me_max_lateness_us = 0;
#endif
    int ret = meLabInit((unsigned)me_task);
    if (ret < 0) return ret;
    meCoreSelectSystemTable(ret);
    sceKernelDcacheWritebackRange(&meCoreSystemTable, sizeof(meCoreSystemTable));
#if PSP_AUDIO && PSP_RGB_PACE
    if (ret >= 0) ret = rgb_start();
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
    me_output = rgb_pixels[slot];
    me_rgb_pts = presentation_pts_us;
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
    ret = meLabSubmit(&me_job);
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
#endif
#endif
