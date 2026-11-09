#ifndef PSP_AUDIO_API_H
#define PSP_AUDIO_API_H
/* Audio-only playback for the music player. It reuses the decode and output
 * threads of psp_video_play but starts no video decoder, Media Engine work,
 * presenter or controller thread, and it returns to the caller after the open:
 * the UI keeps running while audio plays. psp_audio_open, psp_audio_seek and
 * psp_audio_close block (network, probing, prefill) and must be called from a
 * VFPU-enabled worker thread. psp_audio_status and psp_audio_pause are cheap
 * and may be called from any thread. One player at a time: do not run this
 * together with psp_video_play. */
#include <stdint.h>
#include "psp_video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PSP_AUDIO_IDLE = 0,   /* no session */
    PSP_AUDIO_OPENING,    /* opening or seeking: the position is the target */
    PSP_AUDIO_PLAYING,
    PSP_AUDIO_BUFFERING,  /* playing, but the PCM ring ran empty */
    PSP_AUDIO_PAUSED,
    PSP_AUDIO_ENDED,      /* every sample was submitted; close with drain */
    PSP_AUDIO_FAILED      /* decoder or output error; see PspAudioStatus.error */
} PspAudioState;

typedef struct {
    const PspVideoSource *source; /* seekable audio-only source (DASH audio) */
    int64_t start_ms;
    int start_paused;
    int64_t duration_hint_ms;     /* used when the container reports none */
} PspAudioOptions;

typedef struct {
    PspAudioState state;
    int64_t position_ms, duration_ms;
    int error;
    unsigned underruns;
} PspAudioStatus;

/* Returns 0 once the first PCM is buffered, or a negative error. A failed open
 * leaves no session; the caller still owns the source. */
int psp_audio_open(const PspAudioOptions *options);
/* Reopens the same source at the target, like a video seek: the download is
 * kept, the decoder and output restart. A negative result leaves a session
 * that must be closed. */
int psp_audio_seek(int64_t position_ms);
void psp_audio_pause(int paused);
void psp_audio_status(PspAudioStatus *status);
/* drain != 0 lets the last submitted blocks finish (natural end); otherwise
 * pending reads are cancelled. Joins the workers and releases the audio
 * channel. Safe to call without a session. */
void psp_audio_close(int drain);
/* HOME: same as psp_video_stop. Does not block; follow with psp_audio_close. */
void psp_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif
