/* Native video output: FFmpeg reconstructs YUV420P frames on SC, the frame
 * pacer queues them, and the presenter converts and scales them with the GE
 * at their audio-clock time. Neither the CPU nor the Media Engine converts
 * colours, and no RGB frame buffers are allocated. */
#ifndef PSP_GE_PRESENT_H
#define PSP_GE_PRESENT_H
#include "frame_pacer.h"
static unsigned video_frames_queued;

static int video_output_start(void)
{
    video_frames_queued = 0;
    int ret = pacer_start();
    if (ret == AVERROR(EBUSY)) playback_diagnostic("pacer_buffer_retained_busy", ret);
    return ret;
}
static int video_output_submit(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 1280 || frame->height > 720)
        return -1;
    /* The GE reads the planes from memory. Writing back the whole 16 KiB data
     * cache is cheaper than walking the ~350 KiB of planes by range. */
    sceKernelDcacheWritebackAll();
    int ret = pacer_submit(frame, presentation_pts_us);
    if (ret >= 0) ++video_frames_queued;
    return ret;
}
static int video_output_shutdown(void)
{
    pacer_close();
    pacer_release();
    return 0;
}
#endif
