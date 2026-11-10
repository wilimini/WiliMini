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
static volatile int audio_seek_aborted;
static int64_t audio_api_duration_ms, audio_api_target_ms;

#define AUDIO_ONLY_START_DELAY_US 100000

static void audio_api_enter(void) { sceKernelWaitSema(audio_api_lock, 1, NULL); }
static void audio_api_leave(void) { sceKernelSignalSema(audio_api_lock, 1); }

static void audio_api_interrupt_reads(int interrupted) {
    if (audio_io_source && audio_io_source->interrupt_reads)
        audio_io_source->interrupt_reads(audio_io_source->opaque, interrupted);
}

static void audio_stop_workers(void) {
    audio.stop = 1;
    audio_api_interrupt_reads(1);
    SceUID threads[] = {audio.decoder_thread, audio.output_thread};
    for (unsigned i = 0; i < 2; ++i) if (threads[i] >= 0) {
        sceKernelWaitThreadEndCB(threads[i], NULL);
        sceKernelDeleteThread(threads[i]);
    }
    audio.decoder_thread = audio.output_thread = -1;
    SceUID sems[] = {audio.space, audio.ready, audio.go};
    for (unsigned i = 0; i < 3; ++i) if (sems[i] >= 0) sceKernelDeleteSema(sems[i]);
    audio.space = audio.ready = audio.go = -1;
    audio_api_interrupt_reads(0);
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
    audio_seek_aborted = 0;
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
    const int session = audio_api_session;
    if (session) {
        if (position_ms < 0) position_ms = 0;
        if (audio_api_duration_ms > 0 && position_ms > audio_api_duration_ms)
            position_ms = audio_api_duration_ms;
        audio_api_target_ms = position_ms;
        audio_api_running = 0;
    }
    audio_api_leave();
    if (!session) return -1;
    playback_seeking = 1;
    audio_stop_workers();
    if (quit_requested || audio_seek_aborted || !audio.format || !audio.codec) {
        const int aborted = quit_requested || audio_seek_aborted;
        audio_seek_aborted = 0;
        playback_seeking = 0;
        return aborted ? AVERROR_EXIT : -1;
    }
    playback_start_ms = position_ms;
    audio_start_delay_us = AUDIO_ONLY_START_DELAY_US;
    av_packet_unref(audio.packet);
    av_frame_unref(audio.frame);
    if (audio.format->pb) {
        audio.format->pb->error = 0;
        audio.format->pb->eof_reached = 0;
        audio.format->pb->buf_ptr = audio.format->pb->buf_end;
    }
    const int64_t target = av_rescale_q(position_ms * 1000 + source_origin_us(audio.format),
                                        AV_TIME_BASE_Q, audio.format->streams[audio.stream]->time_base);
    int ret = av_seek_frame(audio.format, audio.stream, target, AVSEEK_FLAG_BACKWARD);
    if (ret >= 0 && !audio_seek_aborted) {
        avcodec_flush_buffers(audio.codec);
        audio.stop = audio.eof = audio.input_eof = audio.error = 0;
        audio.produced = audio.consumed = audio.fill = audio.blocks = 0;
        audio.first_pts_us = AV_NOPTS_VALUE;
        audio.epoch_us = 0;
        audio.min_buffer_blocks = AUDIO_BLOCKS;
        audio.source_samples = audio.submitted_samples = 0;
        audio.first_output_us = audio.first_return_us = audio.last_output_us = 0;
        audio.clock_sequence = 0;
        audio.clock_played_samples = 0;
        audio.clock_anchor_us = 0;
        audio.rest_samples = 0;
        ret = audio_start_workers("");
    } else if (ret >= 0) {
        ret = AVERROR_EXIT;
    }
    audio_start_delay_us = 1000000;
    playback_seeking = 0;
    audio_seek_aborted = 0;
    if (ret < 0) {
        audio_stop_workers();
        return ret;
    }
    audio_api_enter();
    audio_api_running = 1;
    audio_api_leave();
    return 0;
}

void psp_audio_interrupt(void) {
    audio_seek_aborted = 1;
    audio.stop = 1;
    audio_api_interrupt_reads(1);
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
