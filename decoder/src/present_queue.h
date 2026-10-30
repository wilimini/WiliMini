/* Single decoder producer and independent timed ME presentation consumer.
 * All FFmpeg reference/pool operations stay on the decoder thread. */
#ifndef PSP_LAB_PRESENT_QUEUE_H
#define PSP_LAB_PRESENT_QUEUE_H
typedef struct { AVFrame *frame; uint64_t deadline; int64_t pts; } PresentFrame;
static PresentFrame present_frames[PSP_ME_RENDER_QUEUE];
static SceUID present_slots = -1, present_work = -1, present_lock = -1, present_drained = -1;
static SceUID present_thread = -1;
static unsigned present_head, present_tail, present_count;
static int present_error, present_stop, present_started;

static int present_worker(SceSize args, void *param)
{
    (void)args; (void)param;
    for (;;) {
        sceKernelWaitSema(present_work,1,NULL);
        sceKernelWaitSema(present_lock,1,NULL);
        if (present_stop && !present_count) {
            sceKernelSignalSema(present_lock,1);
            return 0;
        }
        PresentFrame item = present_frames[present_tail];
        present_tail = (present_tail + 1) % PSP_ME_RENDER_QUEUE;
        int error = present_error;
        sceKernelSignalSema(present_lock,1);
        if (!error) {
            queue_worker_deadline = item.deadline;
            queue_worker_pts = item.pts;
            error = me_render_submit_one(item.frame);
            if (!error) error = me_render_finish_one();
        }
        sceKernelWaitSema(present_lock,1,NULL);
        if (error) present_error = error;
        unsigned remaining = --present_count;
        sceKernelSignalSema(present_lock,1);
        sceKernelSignalSema(present_slots,1);
        if (!remaining) sceKernelSignalSema(present_drained,1);
    }
}
static int me_render_start(void)
{
    int ret = me_render_start_one();
    if (ret < 0) return ret;
    present_head = present_tail = present_count = 0;
    present_error = present_stop = present_started = 0;
    me_warmup_presented_us = me_last_presented_us = 0;
    memset(present_frames,0,sizeof(present_frames));
    present_slots = sceKernelCreateSema("present_slots",0,PSP_ME_RENDER_QUEUE,PSP_ME_RENDER_QUEUE,NULL);
    present_work = sceKernelCreateSema("present_work",0,0,PSP_ME_RENDER_QUEUE,NULL);
    present_lock = sceKernelCreateSema("present_lock",0,1,1,NULL);
    present_drained = sceKernelCreateSema("present_drained",0,0,1,NULL);
    if (present_slots < 0 || present_work < 0 || present_lock < 0 || present_drained < 0)
        return -93;
    present_thread = sceKernelCreateThread("fast_present",present_worker,0x18,0x8000,
        PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,NULL);
    if (present_thread < 0) return present_thread;
    ret = sceKernelStartThread(present_thread,0,NULL);
    if (ret >= 0) present_started = 1;
    return ret;
}
static int me_render_submit(const AVFrame *frame)
{
    int ret = sceKernelWaitSema(present_slots,1,NULL);
    if (ret < 0) return ret;
    sceKernelWaitSema(present_lock,1,NULL);
    int error = present_error;
    sceKernelSignalSema(present_lock,1);
    if (error) { sceKernelSignalSema(present_slots,1); return error; }
    /* A slot becomes reusable only after its frame has been presented.
     * Releasing here avoids concurrent FFmpeg buffer-pool operations. */
    av_frame_free(&present_frames[present_head].frame);
    AVFrame *copy = av_frame_clone(frame);
    if (!copy) { sceKernelSignalSema(present_slots,1); return AVERROR(ENOMEM); }
    sceKernelWaitSema(present_lock,1,NULL);
    present_frames[present_head] = (PresentFrame){copy,presentation_deadline,presentation_pts_us};
    present_head = (present_head + 1) % PSP_ME_RENDER_QUEUE;
    ++present_count;
    sceKernelSignalSema(present_lock,1);
    sceKernelSignalSema(present_work,1);
    return 0;
}
static int me_render_finish(void)
{
    if (!present_started) return 0;
    for (;;) {
        sceKernelWaitSema(present_lock,1,NULL);
        unsigned count = present_count;
        sceKernelSignalSema(present_lock,1);
        if (!count) break;
        sceKernelWaitSema(present_drained,1,NULL);
    }
    if (!present_error)
        for (unsigned i = 0; i < PSP_ME_RENDER_QUEUE; ++i)
            av_frame_free(&present_frames[i].frame);
    return present_error;
}
static int present_shutdown(void)
{
    int ret = me_render_finish();
    if (present_started) {
        sceKernelWaitSema(present_lock,1,NULL);
        present_stop = 1;
        sceKernelSignalSema(present_lock,1);
        sceKernelSignalSema(present_work,1);
        sceKernelWaitThreadEnd(present_thread,NULL);
        present_started = 0;
    }
    if (present_thread >= 0) { sceKernelDeleteThread(present_thread); present_thread = -1; }
    if (present_slots >= 0) { sceKernelDeleteSema(present_slots); present_slots = -1; }
    if (present_work >= 0) { sceKernelDeleteSema(present_work); present_work = -1; }
    if (present_lock >= 0) { sceKernelDeleteSema(present_lock); present_lock = -1; }
    if (present_drained >= 0) { sceKernelDeleteSema(present_drained); present_drained = -1; }
    return ret;
}
#endif
