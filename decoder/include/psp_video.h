#ifndef PSP_VIDEO_H
#define PSP_VIDEO_H
/* No FFmpeg or UI types cross this boundary. Call on a VFPU-enabled thread.
 * The ME bridge PRX must be loaded before native playback, including PPSSPP.
 * PSP_VIDEO_BACKEND_SCE_MPEG decodes with the firmware's sceMpeg library (user mode: the
 * AVCODEC and MPEGBASE AV modules must be loaded, see client/bootstrap.c). wili-server
 * streams use only this backend. BILI_PPSSPP_SCE_MPEG makes it the default under PPSSPP.
 * One player at a time. */
#include <stdint.h>
typedef enum {
    PSP_VIDEO_BACKEND_NATIVE_ME = 0,
    PSP_VIDEO_BACKEND_SCE_MPEG = 1,
    PSP_VIDEO_BACKEND_PPSSPP_SCE_MPEG = PSP_VIDEO_BACKEND_SCE_MPEG /* former name */
} PspVideoBackend;
typedef enum {
    PSP_VIDEO_CANCEL_CIRCLE = 0,
    PSP_VIDEO_CANCEL_CROSS = 1
} PspVideoCancelButton;
typedef enum {
    PSP_VIDEO_PIXEL_FORMAT_RGB565 = 0,
    PSP_VIDEO_PIXEL_FORMAT_RGBA8888 = 1
} PspVideoPixelFormat;
typedef struct {
    void *opaque;
    int64_t size; /* -1 denotes a live, non-seekable source. */
    /* Random access, called concurrently by video and audio. May block until
     * the requested range is available. Return bytes, zero at EOF, or a
     * negative value on failure. */
    int (*read_at)(void *opaque,int64_t offset,void *buffer,int bytes);
    void (*cancel)(void *opaque); /* interrupt pending reads; must not block */
    void (*first_frame)(void *opaque); /* first decoded frame queued */
    /* Temporarily interrupt outstanding reads for seek, without stopping the
     * download. Called with 0 only after all old readers have joined. */
    void (*interrupt_reads)(void *opaque, int interrupted);
} PspVideoSource;
typedef struct {
    int quality, quality_count, qualities[3];
    int danmaku_enabled, danmaku_density, danmaku_opacity;
    int subtitle_track, subtitle_count; /* 0 is off; tracks are 1-based. */
} PspVideoSettings;
typedef struct {
    int64_t position_ms, duration_ms, preview_ms;
    int paused, controls_visible, seeking, seekable, buffering;
    int menu_open, menu_row, quality_preview;
    PspVideoSettings settings;
    int startup_stage;
    int menu_value; /* Preview only; settings retain applied values until confirmation. */
} PspVideoStatus;
typedef struct {
    void *opaque;
    /* Called only by the presentation owner, after ME has finished the RGB
     * frame. NULL pixels means redraw the last clean frame (pause/OSD).
     * Copy pixels before returning; never retain a queue slot. This callback
     * owns DMA, GE completion and scanout switching. No network or disk I/O. */
    int (*present)(void *opaque, const void *pixels, PspVideoPixelFormat format,
                   const PspVideoStatus *status);
    void (*update_settings)(void *opaque, PspVideoSettings *settings);
} PspVideoPresenter;
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
    PspVideoPixelFormat rgb_pixel_format; /* RGB565 or RGBA8888; zero defaults to RGB565. */
    PspVideoBackend backend;
    PspVideoCancelButton cancel_button; /* 0 keeps Circle as the default. */
    int diagnostic_profile; /* Nonzero enables sampled H.264 timings; adds overhead. */
    const PspVideoPresenter *presenter; /* NULL preserves diagnostic display */
    int start_paused;
    const PspVideoSettings *settings;
    /* Streams that restart instead of seeking in place (wili-server). Such a
     * stream's own clock starts at zero when it is opened, so the player shows
     * timeline_offset_ms + elapsed, and takes the duration from
     * timeline_duration_ms because the stream carries none. With restart_seek
     * set a seek key ends the session with seek_requested set; the caller then
     * reopens the stream at seek_ms. All three are zero for ordinary sources. */
    int restart_seek;
    int64_t timeline_offset_ms, timeline_duration_ms;
    /* Optional, restart_seek streams on the SCE_MPEG backend only. With it a seek does not end the
     * session: the player stops reading, closes its decoder and demuxer, and calls reopen to get new
     * sources positioned at target_ms. The UI, controls, presenter and the session itself stay up.
     * The callee must release the old sources (nothing reads them any more) and write the new ones to
     * *video and *audio (audio may be NULL), valid until the next reopen or the end of the session.
     * Return 0, or a negative value to fall back to the behaviour without this hook: the session ends
     * with seek_requested set and the caller reopens. It runs on the player thread and may block, but
     * should poll psp_video_cancelled() and give up once that is nonzero. */
    int (*reopen)(void *opaque, int64_t target_ms, const PspVideoSource **video, const PspVideoSource **audio);
    void *reopen_opaque;
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
    unsigned seeks; /* in-session seeks completed */
    int seek_requested; /* legacy backend fallback; native seeks in session */
    int64_t seek_ms;
    int seek_paused;
    int quality_requested;
    PspVideoSettings settings;
} PspVideoResult;
int psp_video_play(const PspVideoOptions *options, PspVideoResult *result);
void psp_video_stop(void);
/* Nonzero once psp_video_stop() or an exit key has ended the session. */
int psp_video_cancelled(void);
/* Sample playback progress without acquiring playback locks or writing files. */
void psp_video_log_health(void);
/* ME_BACKEND_* of the bridge PRX loaded by the bootstrap, or -1 when native
 * ME is not in use. Call only after the bootstrap's bridge load succeeded. */
int psp_video_me_backend(void);
#endif
