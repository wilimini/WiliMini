/* Video output for the native player: every decoded YUV420P frame goes to the
 * frame pacer, and the presenter converts it with the GE at its audio-clock
 * time.
 *
 * With PSP_ME_PIPELINE (the default) FFmpeg only entropy-decodes on SC and the
 * Media Engine reconstructs frames asynchronously (me_frame_pipeline.c), so a
 * queued frame carries the ME sequence number that completes its pixels.
 * Without it (diagnostic builds for environments lacking ME firmware) FFmpeg
 * reconstructs everything on SC. */
#ifndef PSP_VIDEO_OUTPUT_H
#define PSP_VIDEO_OUTPUT_H
#include "frame_pacer.h"
#if PSP_ME_PIPELINE
#include "me_bridge.h"
#include "me_frame_pipeline.h"
static MefpStats mefp_final_stats;
#endif
static unsigned video_frames_queued;

static int pacer_frame_ready(uint32_t seq)
{
#if PSP_ME_PIPELINE
    return mefp_seq_is_done(seq);
#else
    (void)seq;
    return 1;
#endif
}
static int video_output_start(void)
{
    video_frames_queued = 0;
    int ret = pacer_start();
    if (ret == AVERROR(EBUSY)) playback_diagnostic("pacer_buffer_retained_busy", ret);
    if (ret < 0) return ret;
#if PSP_ME_PIPELINE
    memset(&mefp_final_stats, 0, sizeof(mefp_final_stats));
    ret = mefp_start();
    if (ret < 0) {
        pacer_close();
        pacer_release();
        return ret;
    }
#endif
    return 0;
}
static int video_output_submit(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 1280 || frame->height > 720)
        return -1;
#if PSP_ME_PIPELINE
    const uint32_t ready_seq = mefp_prepare_present(frame->data[0]);
#else
    /* SC wrote the planes; the GE reads them from memory. */
    sceKernelDcacheWritebackAll();
    const uint32_t ready_seq = 0;
#endif
    int ret = pacer_submit(frame, ready_seq, presentation_pts_us);
    if (ret >= 0) ++video_frames_queued;
    return ret;
}
/* Waits for queued ME work, e.g. before a seek or at the end of playback. */
static int video_output_finish(void)
{
#if PSP_ME_PIPELINE
    int ret = mefp_drain();
    mefp_get_stats(&mefp_final_stats);
    return ret;
#else
    return 0;
#endif
}
static int video_output_shutdown(void)
{
    int ret = video_output_finish();
    pacer_close();
#if PSP_ME_PIPELINE
    int shutdown = mefp_stop();
    // A failed stop does not prove ME stopped writing. Keep its frames alive.
    if (shutdown >= 0) pacer_release();
    return ret < 0 ? ret : shutdown;
#else
    pacer_release();
    return ret;
#endif
}
#endif
