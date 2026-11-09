#ifndef PSP_AUDIO_ONLY_H
#define PSP_AUDIO_ONLY_H
/* Public audio-only API (psp_audio.h). Included at the end of player.c so it
 * shares the single `audio` engine and its buffers with the video player. */
#if PSP_AUDIO
#include "psp_audio.h"

/* audio_api_lock guards the two flags below and every read of `audio` by
 * psp_audio_status; audio_close and audio_open run outside it, with
 * audio_api_running cleared so the status path never sees a half-built engine. */
static SceUID audio_api_lock = -1;
static int audio_api_session;  /* between a successful claim and release */
static int audio_api_running;  /* `audio` is open and its workers may run */
static int64_t audio_api_duration_ms, audio_api_target_ms;

#define AUDIO_ONLY_START_DELAY_US 100000

static void audio_api_enter(void) { sceKernelWaitSema(audio_api_lock, 1, NULL); }
static void audio_api_leave(void) { sceKernelSignalSema(audio_api_lock, 1); }

static void audio_api_interrupt_reads(int interrupted) {
    if (audio_io_source && audio_io_source->interrupt_reads)
        audio_io_source->interrupt_reads(audio_io_source->opaque, interrupted);
}

static void audio_api_release(void) {
    if (controls_lock >= 0) sceKernelDeleteSema(controls_lock);
    controls_lock = -1;
    audio_io_source = NULL;
    audio_api_enter();
    audio_api_session = audio_api_running = 0;
    audio_api_leave();
}

static int audio_api_start(int64_t start_ms) {
    playback_start_ms = start_ms;
    audio_start_delay_us = AUDIO_ONLY_START_DELAY_US;
    int ret = audio_open("custom:audio.m4a", audio_io_source, NULL, "");
    audio_start_delay_us = 1000000;
    if (ret < 0) { audio_close(0); return ret; }
    if (audio.format->duration != AV_NOPTS_VALUE && audio.format->duration > 0)
        audio_api_duration_ms = audio.format->duration / 1000;
    audio_api_enter();
    audio_api_running = 1;
    audio_api_leave();
    return 0;
}

int psp_audio_open(const PspAudioOptions *options) {
    if (!options || !options->source || !options->source->read_at ||
        options->source->size == 0 || options->source->size < -1) return -1;
    if (audio_api_lock < 0) {
        audio_api_lock = sceKernelCreateSema("audio_api", 0, 1, 1, NULL);
        if (audio_api_lock < 0) { int error = audio_api_lock; audio_api_lock = -1; return error; }
    }
    audio_api_enter();
    const int busy = audio_api_session || client_playback || controls_lock >= 0;
    if (!busy) audio_api_session = 1;
    audio_api_leave();
    if (busy) return -2;
    controls_lock = sceKernelCreateSema("audio_controls", 0, 1, 1, NULL);
    if (controls_lock < 0) {
        int error = controls_lock;
        controls_lock = -1;
        audio_api_release();
        return error;
    }
    pc_init(&controls, sceKernelGetSystemTimeWide(), options->start_paused);
    quit_requested = 0;
    playback_seeking = 0;
    video_io_source = NULL;
    audio_io_source = options->source;
    audio_api_duration_ms = options->duration_hint_ms > 0 ? options->duration_hint_ms : 0;
    audio_api_target_ms = options->start_ms > 0 ? options->start_ms : 0;
    int ret = audio_api_start(audio_api_target_ms);
    if (ret < 0) audio_api_release();
    return ret;
}

int psp_audio_seek(int64_t position_ms) {
    audio_api_enter();
    const int running = audio_api_running;
    if (running) {
        if (position_ms < 0) position_ms = 0;
        if (audio_api_duration_ms > 0 && position_ms > audio_api_duration_ms)
            position_ms = audio_api_duration_ms;
        audio_api_target_ms = position_ms;
        audio_api_running = 0;
    }
    audio_api_leave();
    if (!running) return -1;
    playback_seeking = 1;
    audio_close(0); /* interrupts reads without cancelling the download */
    const int error = audio.error;
    audio_api_interrupt_reads(0);
    playback_seeking = 0;
    if (error) return error;
    if (quit_requested) return AVERROR_EXIT;
    return audio_api_start(position_ms);
}

void psp_audio_pause(int paused) {
    if (controls_lock < 0) return;
    sceKernelWaitSema(controls_lock, 1, NULL);
    if (!!controls.paused != !!paused) pc_pause(&controls, sceKernelGetSystemTimeWide());
    sceKernelSignalSema(controls_lock, 1);
}

void psp_audio_status(PspAudioStatus *status) {
    if (!status) return;
    *status = (PspAudioStatus){0};
    if (audio_api_lock < 0) return;
    audio_api_enter();
    status->duration_ms = audio_api_duration_ms;
    if (!audio_api_session) {
        /* idle */
    } else if (!audio_api_running) {
        status->state = PSP_AUDIO_OPENING;
        status->position_ms = audio_api_target_ms;
    } else {
        status->underruns = audio.underruns;
        int64_t position = playback_start_ms;
        if (audio.rate > 0 && audio.epoch_us && audio.first_pts_us != AV_NOPTS_VALUE) {
            position = (audio_clock_us() - source_origin_us(audio.format)) / 1000;
            if (position < playback_start_ms) position = playback_start_ms;
        }
        if (audio_api_duration_ms > 0 && position > audio_api_duration_ms)
            position = audio_api_duration_ms;
        status->position_ms = position;
        if (audio.error) {
            status->state = PSP_AUDIO_FAILED;
            status->error = audio.error;
        } else if (audio.eof && audio.consumed == audio.produced) {
            status->state = PSP_AUDIO_ENDED;
        } else if (playback_is_paused()) {
            status->state = PSP_AUDIO_PAUSED;
        } else if (audio.consumed == audio.produced) {
            status->state = PSP_AUDIO_BUFFERING;
        } else {
            status->state = PSP_AUDIO_PLAYING;
        }
    }
    audio_api_leave();
}

void psp_audio_close(int drain) {
    if (audio_api_lock < 0) return;
    audio_api_enter();
    const int session = audio_api_session;
    audio_api_running = 0;
    audio_api_leave();
    if (!session) return;
    audio_close(drain);
    audio_api_release();
}

void psp_audio_stop(void) { psp_video_stop(); }

#endif
#endif
