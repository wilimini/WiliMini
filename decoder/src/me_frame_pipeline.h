#ifndef PSP_ME_FRAME_PIPELINE_H
#define PSP_ME_FRAME_PIPELINE_H

#include <stdint.h>

struct AVFrame;

typedef int (*MefpAcquireRgbFn)(void **out_pixels);
typedef void (*MefpPublishRgbFn)(int64_t pts);
typedef void (*MefpReleaseRgbFn)(void);

typedef struct {
    unsigned recon_frames;
    unsigned fallback_frames;
    unsigned render_submitted;
    unsigned render_completed;
    unsigned intra_mbs;
    unsigned inter_mbs;
    unsigned wait_us;
    uint64_t capture_us;
    uint64_t me_recon_us;
    uint64_t me_render_us;
    uint32_t frame_hash;
    int error;
} MefpStats;

int mefp_start(int output_format,
               unsigned max_inflight_renders,
               MefpAcquireRgbFn acquire_cb,
               MefpPublishRgbFn publish_cb,
               MefpReleaseRgbFn release_cb);
int mefp_render_submit(const struct AVFrame *frame, int64_t pts, int hash_frame);
void mefp_poll_renders(void);
uint32_t mefp_find_recon_seq(const uint8_t *y_plane);
int mefp_seq_is_done(uint32_t seq);
int mefp_take_sc_written(void);
int mefp_drain(void);
int mefp_stop(void);
void mefp_get_stats(MefpStats *out);

#endif /* PSP_ME_FRAME_PIPELINE_H */
