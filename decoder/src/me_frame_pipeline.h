#ifndef PSP_ME_FRAME_PIPELINE_H
#define PSP_ME_FRAME_PIPELINE_H

#include <stdint.h>

/* Whole-frame SC (entropy decoding) <-> ME (reconstruction) H.264 pipeline.
 * All functions run on the decoder thread, except mefp_seq_is_done(), which
 * the presentation thread may call while playback is running. */

typedef struct {
    unsigned recon_frames;    /* frames reconstructed on ME */
    unsigned fallback_frames; /* frames FFmpeg reconstructed on SC */
    unsigned intra_mbs;
    unsigned inter_mbs;
    unsigned wait_us;         /* SC time spent waiting for ME */
    uint64_t capture_us;      /* SC time spent capturing macroblocks */
    int error;
} MefpStats;

struct AVCodecContext;
struct AVFrame;

/* Starts the ME worker on the loaded bridge (Custom Core or MIST) and installs
 * the FFmpeg hooks. */
int mefp_start(void);
/* Custom AVCodecContext.get_buffer2 callback that allocates 64-byte-aligned,
 * 64-byte-padded plane buffers so ME cache writebacks never share a D-cache
 * line with adjacent heap metadata or AVBuffer structs. */
int mefp_get_buffer2(struct AVCodecContext *avctx, struct AVFrame *frame, int flags);
/* Call once per decoded frame before presenting it. Returns the sequence
 * number that must complete before its planes may be read, or 0 when the
 * frame is already complete in memory. */
uint32_t mefp_prepare_present(const uint8_t *y_plane);
int mefp_seq_is_done(uint32_t seq);
/* Waits for all queued ME work and releases held reference frames. */
int mefp_drain(void);
/* Stops the worker and removes the hooks. */
int mefp_stop(void);
void mefp_get_stats(MefpStats *out);

#endif /* PSP_ME_FRAME_PIPELINE_H */
