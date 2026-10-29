#ifndef PSP_VIDEO_H
#define PSP_VIDEO_H
/* No FFmpeg or UI types cross this boundary. Call on a VFPU-enabled thread.
 * The ME bridge PRX must be loaded before native playback, including PPSSPP.
 * BILI_PPSSPP_SCE_MPEG enables the optional emulator HLE backend instead.
 * One player at a time. */
#include <stdint.h>
typedef enum {
    PSP_VIDEO_BACKEND_NATIVE_ME = 0,
    PSP_VIDEO_BACKEND_PPSSPP_SCE_MPEG = 1
} PspVideoBackend;
typedef struct {
    void *opaque;
    int64_t size; /* -1 denotes a live, non-seekable source. */
    /* Random access, called concurrently by video and audio. May block until
     * the requested range is available. Return bytes, zero at EOF, or a
     * negative value on failure. */
    int (*read_at)(void *opaque,int64_t offset,void *buffer,int bytes);
    void (*cancel)(void *opaque); /* interrupt pending reads; must not block */
    void (*first_frame)(void *opaque); /* first decoded frame queued */
} PspVideoSource;
typedef struct {
    const char *path;     /* seekable local MP4, unused when source is set */
    const char *log_root; /* existing directory for results/run-NNN */
    int sample;          /* diagnostic: stop after 300 measured frames */
    const PspVideoSource *source; /* optional seekable video/muxed source */
    /* Optional separate audio source. NULL reuses source, preserving the
     * progressive MP4 path; DASH supplies independent video and audio URLs. */
    const PspVideoSource *audio_source;
    int64_t start_ms; /* VOD resume position; live sources must start at zero. */
    int rgb_buffer_frames; /* 0 keeps the default 8; otherwise 2, 4 or 8. */
    PspVideoBackend backend;
} PspVideoOptions;
typedef enum {
    PSP_VIDEO_STAGE_NONE, PSP_VIDEO_STAGE_SETUP, PSP_VIDEO_STAGE_LOG,
    PSP_VIDEO_STAGE_ME_INIT, PSP_VIDEO_STAGE_ME_RENDER_INIT,
    PSP_VIDEO_STAGE_SOURCE, PSP_VIDEO_STAGE_VIDEO_INIT,
    PSP_VIDEO_STAGE_AUDIO_INIT, PSP_VIDEO_STAGE_PLAY, PSP_VIDEO_STAGE_CLEANUP
} PspVideoStage;
const char *psp_video_stage_name(PspVideoStage stage);
typedef struct {
    int error, cancelled;
    unsigned frames, eof;
    PspVideoStage error_stage;
    int64_t position_ms; /* Last played media position, excluding decoder preroll. */
} PspVideoResult;
int psp_video_play(const PspVideoOptions *options, PspVideoResult *result);
void psp_video_stop(void);
#endif
