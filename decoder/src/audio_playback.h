#ifndef PSP_AUDIO_PLAYBACK_H
#define PSP_AUDIO_PLAYBACK_H
#if PSP_AUDIO
#include <pspaudio.h>
#include <libavutil/samplefmt.h>

/* The decoder owns FFmpeg objects. Only immutable PCM crosses to the output
 * thread; neither thread touches ME state or the video decoder's buffers. */
#define AUDIO_SAMPLES 2048
#define AUDIO_BLOCKS 64
#define AUDIO_PREFILL 48
static struct {
    int16_t pcm[AUDIO_BLOCKS][AUDIO_SAMPLES * 2] __attribute__((aligned(64)));
    /* SRC returns when PCM is queued, not when its memory is reusable.
     * These buffers belong exclusively to the output thread. Four periods
     * exceed the observed one-period hardware queue and protect DMA reads
     * from producer reuse of a just-consumed software-ring slot. */
    int16_t dma_pcm[4][AUDIO_SAMPLES * 2] __attribute__((aligned(64)));
    unsigned valid[AUDIO_BLOCKS];
    AVFormatContext *format;
    VideoSourceIO input_io;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    int stream, rate, reserved;
    SceUID space, ready, go, decoder_thread, output_thread;
    volatile int stop, eof, input_eof, error;
    volatile unsigned produced, consumed;
    unsigned fill;
    uint64_t epoch_us;
    int64_t first_pts_us;
    uint64_t decode_us, convert_us, source_samples, submitted_samples;
    uint64_t first_output_us, first_return_us, last_output_us;
    unsigned underruns, max_wait_us, max_decode_us, blocks;
    unsigned adaptive_packets;
    unsigned max_output_gap_us, max_clock_drift_us, min_buffer_blocks;
    volatile unsigned clock_sequence;
    unsigned clock_played_samples;
    uint64_t clock_anchor_us;
    int rest_samples;
} audio;

static int audio_wait(SceUID sem)
{
    while (!audio.stop && !quit_requested) {
        SceUInt timeout = 100000;
        int ret = sceKernelWaitSema(sem, 1, &timeout);
        if (ret >= 0) return 0;
        if ((unsigned)ret != 0x800201a8u) return ret;
    }
    return -1;
}

static int audio_publish(void)
{
    unsigned slot = audio.produced % AUDIO_BLOCKS;
    audio.valid[slot] = audio.fill;
    ++audio.produced;
    audio.fill = 0;
    sceKernelSignalSema(audio.ready, 1);
    return 0;
}

static int audio_convert(const AVFrame *frame)
{
    if (frame->sample_rate != audio.rate || frame->ch_layout.nb_channels != 2)
        return AVERROR(EINVAL);
    const int64_t pts_us = frame->best_effort_timestamp == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
        av_rescale_q(frame->best_effort_timestamp,
                     audio.format->streams[audio.stream]->time_base, AV_TIME_BASE_Q);
    int skip = 0;
    if (pts_us != AV_NOPTS_VALUE && playback_start_ms > 0) {
        const int64_t delta = playback_start_ms * 1000 + source_origin_us(audio.format) - pts_us;
        if (delta > 0) {
            int64_t samples = (delta * audio.rate + 999999) / 1000000;
            if (samples >= frame->nb_samples) return 0;
            skip = (int)samples;
        }
    }
    if (audio.first_pts_us == AV_NOPTS_VALUE)
        audio.first_pts_us = (pts_us == AV_NOPTS_VALUE ? 0 : pts_us) + (int64_t)skip * 1000000 / audio.rate;
    for (int i = skip; i < frame->nb_samples; ) {
        if (!audio.fill && audio_wait(audio.space) < 0) return -1;
        unsigned count = AUDIO_SAMPLES - audio.fill;
        if (count > (unsigned)(frame->nb_samples - i)) count = frame->nb_samples - i;
        int16_t *dst = audio.pcm[audio.produced % AUDIO_BLOCKS] + audio.fill * 2;
        uint64_t start = sceKernelGetSystemTimeWide();
        for (unsigned j = 0; j < count; ++j) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                int sample;
                if (frame->format == AV_SAMPLE_FMT_FLTP) {
                    float value = ((const float *)frame->extended_data[ch])[i + j] * 32768.0f;
                    sample = value >= 32767.0f ? 32767 : value <= -32768.0f ? -32768 : (int)value;
                } else if (frame->format == AV_SAMPLE_FMT_S32P) {
                    sample = ((const int32_t *)frame->extended_data[ch])[i + j] >> 16;
                } else if (frame->format == AV_SAMPLE_FMT_S16P) {
                    sample = ((const int16_t *)frame->extended_data[ch])[i + j];
                } else return AVERROR(ENOSYS);
                dst[j * 2 + ch] = sample;
            }
        }
        audio.convert_us += sceKernelGetSystemTimeWide() - start;
        audio.fill += count;
        audio.source_samples += count;
        i += count;
        if (audio.fill == AUDIO_SAMPLES) audio_publish();
    }
    return 0;
}

static int audio_receive(void)
{
    for (;;) {
        uint64_t start = sceKernelGetSystemTimeWide();
        int ret = avcodec_receive_frame(audio.codec, audio.frame);
        unsigned elapsed = sceKernelGetSystemTimeWide() - start;
        audio.decode_us += elapsed;
        if (elapsed > audio.max_decode_us) audio.max_decode_us = elapsed;
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) return ret;
        ret = audio_convert(audio.frame);
        av_frame_unref(audio.frame);
        if (ret < 0) return ret;
    }
}

static int audio_decode_thread(SceSize size, void *arg)
{
    (void)size; (void)arg;
    int ret = 0;
    while (!audio.stop && !quit_requested) {
        ret = av_read_frame(audio.format, audio.packet);
        if (ret < 0) break;
        if (audio.packet->stream_index == audio.stream) {
            uint64_t start = sceKernelGetSystemTimeWide();
            ret = avcodec_send_packet(audio.codec, audio.packet);
            unsigned elapsed = sceKernelGetSystemTimeWide() - start;
            audio.decode_us += elapsed;
            if (elapsed > audio.max_decode_us) audio.max_decode_us = elapsed;
            if (ret >= 0) ret = audio_receive();
        }
        av_packet_unref(audio.packet);
        if (ret < 0) break;
    }
    if (ret == AVERROR_EOF && !audio.stop && !quit_requested) {
        audio.input_eof = 1;
        ret = avcodec_send_packet(audio.codec, NULL);
        if (ret >= 0) ret = audio_receive();
        if (ret >= 0 && audio.fill) {
            unsigned slot = audio.produced % AUDIO_BLOCKS;
            memset(audio.pcm[slot] + audio.fill * 2, 0,
                   (AUDIO_SAMPLES - audio.fill) * 2 * sizeof(int16_t));
            audio_publish();
        }
    }
    if (ret < 0 && !audio.stop && !quit_requested) audio.error = ret;
    audio.eof = 1;
    sceKernelSignalSema(audio.ready, 1);
    return 0;
}

static int audio_output_thread(SceSize size, void *arg)
{
    (void)size; (void)arg;
    if (audio_wait(audio.go) < 0) return 0;
    while (!audio.stop && !quit_requested && (uint64_t)sceKernelGetSystemTimeWide() < audio.epoch_us)
        sceKernelDelayThread(1000);
    while (!audio.stop && !quit_requested) {
        if (audio.eof && audio.consumed == audio.produced) break;
        uint64_t before = sceKernelGetSystemTimeWide();
        int empty = audio.consumed == audio.produced;
        if (audio_wait(audio.ready) < 0) break;
        if (audio.consumed == audio.produced) continue;
        unsigned waited = sceKernelGetSystemTimeWide() - before;
        if (empty && !audio.eof && waited > 1000) {
            ++audio.underruns;
            if (waited > audio.max_wait_us) audio.max_wait_us = waited;
        }
        unsigned slot = audio.consumed % AUDIO_BLOCKS;
        unsigned buffered = audio.produced - audio.consumed;
        if (!audio.eof && buffered < audio.min_buffer_blocks) audio.min_buffer_blocks = buffered;
        before = sceKernelGetSystemTimeWide();
        if (!audio.first_output_us) audio.first_output_us = before;
        int16_t *dma = audio.dma_pcm[audio.blocks % 4];
        memcpy(dma, audio.pcm[slot], sizeof(audio.dma_pcm[0]));
        sceKernelDcacheWritebackRange(dma, sizeof(audio.dma_pcm[0]));
        int ret = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, dma);
        if (ret < 0) { audio.error = ret; break; }
        uint64_t returned = sceKernelGetSystemTimeWide();
        if (!audio.first_return_us) audio.first_return_us = returned;
        if (audio.last_output_us && returned - audio.last_output_us > audio.max_output_gap_us)
            audio.max_output_gap_us = returned - audio.last_output_us;
        uint64_t expected = audio.first_return_us +
            (uint64_t)audio.blocks * AUDIO_SAMPLES * 1000000 / audio.rate;
        if (returned > expected && returned - expected > audio.max_clock_drift_us)
            audio.max_clock_drift_us = returned - expected;
        audio.last_output_us = returned;
        audio.submitted_samples += AUDIO_SAMPLES;
        int rest = sceAudioOutput2GetRestSample();
        audio.rest_samples = rest;
        unsigned played = audio.submitted_samples - AUDIO_SAMPLES;
        if (rest >= 0 && rest <= 2 * AUDIO_SAMPLES &&
            audio.submitted_samples >= (unsigned)rest)
            played = audio.submitted_samples - rest;
        ++audio.clock_sequence;
        __asm__ volatile("" ::: "memory");
        audio.clock_played_samples = played;
        audio.clock_anchor_us = sceKernelGetSystemTimeWide();
        __asm__ volatile("" ::: "memory");
        ++audio.clock_sequence;
        ++audio.blocks;
        ++audio.consumed;
        sceKernelSignalSema(audio.space, 1);
    }
    /* SRC can retain the last two blocks after the blocking call returns. */
    if (!audio.stop && !quit_requested)
        sceKernelDelayThread(2 * AUDIO_SAMPLES * 1000000u / audio.rate);
    return 0;
}

static int audio_open(const char *source, const PspVideoSource *stream, const char *run_path)
{
    (void)run_path;
    memset(&audio, 0, sizeof(audio));
    audio.space = audio.ready = audio.go = -1;
    audio.decoder_thread = audio.output_thread = -1;
    audio.first_pts_us = AV_NOPTS_VALUE;
    audio.min_buffer_blocks = AUDIO_BLOCKS;
    int ret = source_open(&audio.format, source, stream, &audio.input_io);
    if (ret < 0) return ret;
    source_memory("audio_source");
    ret = avformat_find_stream_info(audio.format, NULL);
    if (ret < 0) return ret;
    source_memory("audio_probe");
    audio.stream = av_find_best_stream(audio.format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio.stream < 0) return audio.stream;
    ret = source_start_at(audio.format, audio.stream, playback_start_ms);
    if (ret < 0) return ret;
    for (unsigned i = 0; i < audio.format->nb_streams; ++i)
        if ((int)i != audio.stream) audio.format->streams[i]->discard = AVDISCARD_ALL;
    const AVCodec *decoder = avcodec_find_decoder_by_name(PSP_AUDIO_FIXED ? "aac_fixed" : "aac");
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;
    audio.codec = avcodec_alloc_context3(decoder);
    if (!audio.codec) return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(audio.codec, audio.format->streams[audio.stream]->codecpar);
    if (ret < 0) return ret;
    audio.codec->pkt_timebase = audio.format->streams[audio.stream]->time_base;
    audio.codec->thread_count = 1;
    ret = avcodec_open2(audio.codec, decoder, NULL);
    if (ret < 0) return ret;
#if PSP_AUDIO_VFPU
    extern unsigned audio_vfpu_failures;
    if (audio_vfpu_failures) return -105;
#endif
    audio.rate = audio.codec->sample_rate;
    if ((audio.rate != 48000 && audio.rate != 44100) || audio.codec->ch_layout.nb_channels != 2) return AVERROR(EINVAL);
    audio.frame = av_frame_alloc();
    audio.packet = av_packet_alloc();
    if (!audio.frame || !audio.packet) return AVERROR(ENOMEM);
    for (int tries = 0; tries < 20; ++tries) {
        ret = sceAudioSRCChReserve(AUDIO_SAMPLES, audio.rate, 2);
        if (ret >= 0 || ((unsigned)ret != 0x80260002u && (unsigned)ret != 0x80268002u))
            break;
        sceKernelDelayThread(100000);
    }
    if (ret < 0) return ret;
    audio.reserved = 1;
    audio.space = sceKernelCreateSema("pcm_space", 0, AUDIO_BLOCKS, AUDIO_BLOCKS, NULL);
    audio.ready = sceKernelCreateSema("pcm_ready", 0, 0, AUDIO_BLOCKS + 1, NULL);
    audio.go = sceKernelCreateSema("pcm_start", 0, 0, 1, NULL);
    if (audio.space < 0 || audio.ready < 0 || audio.go < 0) return -101;
    audio.decoder_thread = sceKernelCreateThread("aac_decode", audio_decode_thread,
                           0x18, 128 * 1024, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    audio.output_thread = sceKernelCreateThread("pcm_output", audio_output_thread,
                           0x10, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (audio.decoder_thread < 0 || audio.output_thread < 0) return -102;
    ret = sceKernelStartThread(audio.decoder_thread, 0, NULL);
    if (ret < 0) return ret;
    ret = sceKernelStartThread(audio.output_thread, 0, NULL);
    if (ret < 0) return ret;
    while (audio.produced < AUDIO_PREFILL && !audio.eof && !audio.error && !quit_requested)
        sceKernelDelayThreadCB(10000);
    if (audio.error) return audio.error;
    if (quit_requested || !audio.produced) return -103;
#if PSP_AUDIO_VERIFY
    char pcm_path[192];
    snprintf(pcm_path, sizeof(pcm_path), "%s/audio-first.pcm", run_path);
    FILE *pcm = fopen(pcm_path, "wb");
    if (!pcm) return -106;
    size_t count = 8 * AUDIO_SAMPLES * 2;
    size_t written = fwrite(audio.pcm, sizeof(int16_t), count, pcm);
    fclose(pcm);
    if (written != count) return -107;
#endif
    audio.epoch_us = sceKernelGetSystemTimeWide() + 1000000;
#if PSP_RGB_PACE
    rgb_startup_ready = 1;
#endif
    return sceKernelSignalSema(audio.go, 1);
}

static void audio_close(int drain)
{
    if (drain && audio.output_thread >= 0 && !audio.error && !quit_requested) {
        SceUInt timeout = 10000000;
        if (sceKernelWaitThreadEndCB(audio.output_thread, &timeout) < 0)
            audio.error = -104;
    }
    audio.stop = 1;
    if (!drain) {
        if (video_io_source && video_io_source->cancel)
            video_io_source->cancel(video_io_source->opaque);
        if (audio_io_source && audio_io_source != video_io_source && audio_io_source->cancel)
            audio_io_source->cancel(audio_io_source->opaque);
    }
    SceUID threads[] = {audio.decoder_thread, audio.output_thread};
    for (unsigned i = 0; i < 2; ++i) if (threads[i] >= 0) {
        sceKernelWaitThreadEndCB(threads[i], NULL);
        sceKernelDeleteThread(threads[i]);
    }
    if (audio.reserved) {
        /* A stopped sample may still own two DMA periods in the SRC hardware.
         * Let those finish before release. The release can still report busy
         * while the last period drains, so retry before the next playback. */
        sceKernelDelayThread(2*AUDIO_SAMPLES*1000000u/audio.rate);
        int released=-1;
        for (int tries = 0; tries < 20; ++tries) {
            released=sceAudioSRCChRelease();
            if (released >= 0) break;
            sceKernelDelayThread(100000);
        }
        if(released<0)audio.error=released;
    }
    SceUID sems[] = {audio.space, audio.ready, audio.go};
    for (unsigned i = 0; i < 3; ++i) if (sems[i] >= 0) sceKernelDeleteSema(sems[i]);
    av_packet_free(&audio.packet);
    av_frame_free(&audio.frame);
    avcodec_free_context(&audio.codec);
    source_close(&audio.format,&audio.input_io);
}

static int64_t audio_clock_us(void)
{
    unsigned seq, samples;
    uint64_t anchor;
    do {
        seq = audio.clock_sequence;
        __asm__ volatile("" ::: "memory");
        samples = audio.clock_played_samples;
        anchor = audio.clock_anchor_us;
        __asm__ volatile("" ::: "memory");
    } while ((seq & 1) || seq != audio.clock_sequence);
    int64_t now = sceKernelGetSystemTimeWide();
    if (!anchor) return audio.first_pts_us + now - (int64_t)audio.epoch_us;
    int64_t elapsed = now - (int64_t)anchor;
    if (elapsed < 0) elapsed = 0;
    /* Do not run the media clock beyond PCM actually submitted to hardware. */
    int64_t block_us = (int64_t)AUDIO_SAMPLES * 1000000 / audio.rate;
    if (elapsed > block_us) elapsed = block_us;
    return audio.first_pts_us + (int64_t)samples * 1000000 / audio.rate + elapsed;
}
static int audio_playback_failed(void)
{
    return audio.stop || audio.error;
}
static uint64_t audio_video_deadline(int64_t pts, AVRational timebase)
{
    int64_t delta = av_rescale_q(pts, timebase, AV_TIME_BASE_Q) - audio_clock_us();
    int64_t deadline = sceKernelGetSystemTimeWide() + delta;
    return deadline > 0 ? deadline : 0;
}

static void audio_report(FILE *out)
{
    fprintf(out, "  \"audio_adaptive\": %d,\n  \"audio_adaptive_packets\": %u,\n",
            PSP_AUDIO_ADAPTIVE, audio.adaptive_packets);
    fprintf(out, "  \"audio_video_lead_us\": %u,\n", PSP_AUDIO_VIDEO_LEAD_US);
    fprintf(out, "  \"audio_eof\": %d,\n  \"audio_produced_blocks\": %u,\n"
            "  \"audio_consumed_blocks\": %u,\n", audio.input_eof, audio.produced, audio.consumed);
    fprintf(out, "  \"audio_present_immediate\": %d,\n", PSP_AUDIO_PRESENT_IMMEDIATE);
    fprintf(out, "  \"audio_rest_samples\": %d,\n", audio.rest_samples);
    fprintf(out, "  \"audio_dma_staging_blocks\": 4,\n");
    fprintf(out, "  \"audio_first_pts_us\": %lld,\n  \"audio_epoch_us\": %llu,\n",
            (long long)audio.first_pts_us, (unsigned long long)audio.epoch_us);
#if PSP_RGB_PACE
    fprintf(out, "  \"rgb_pace\": 1,\n  \"rgb_presented\": %u,\n  \"rgb_dropped\": %u,\n",
            rgb_presented, rgb_dropped);
    fprintf(out, "  \"rgb_nearest_pts\": %d,\n", PSP_RGB_NEAREST_PTS);
    fprintf(out, "  \"rgb_max_late_us\": %lld,\n  \"rgb_mean_late_us\": %lld,\n",
            (long long)rgb_max_late_us, (long long)(rgb_presented ? rgb_total_late_us / rgb_presented : 0));
#endif
#if PSP_AUDIO_VFPU
    extern unsigned audio_vfpu_failures, audio_vfpu_calls;
    fprintf(out, "  \"audio_vfpu_failures\": %u,\n  \"audio_vfpu_calls\": %u,\n",
            audio_vfpu_failures, audio_vfpu_calls);
#endif
    fprintf(out, "  \"audio_enabled\": 1,\n  \"audio_fixed\": %d,\n", PSP_AUDIO_FIXED);
    fprintf(out, "  \"audio_rate\": %d,\n  \"audio_error\": %d,\n", audio.rate, audio.error);
    fprintf(out, "  \"audio_source_samples\": %llu,\n  \"audio_submitted_samples\": %llu,\n",
            (unsigned long long)audio.source_samples, (unsigned long long)audio.submitted_samples);
    fprintf(out, "  \"audio_underruns\": %u,\n  \"audio_max_wait_us\": %u,\n", audio.underruns, audio.max_wait_us);
    fprintf(out, "  \"audio_decode_us\": %llu,\n  \"audio_convert_us\": %llu,\n",
            (unsigned long long)audio.decode_us, (unsigned long long)audio.convert_us);
    fprintf(out, "  \"audio_max_decode_us\": %u,\n  \"audio_blocks\": %u,\n", audio.max_decode_us, audio.blocks);
    fprintf(out, "  \"audio_max_output_gap_us\": %u,\n  \"audio_max_clock_drift_us\": %u,\n"
            "  \"audio_min_buffer_blocks\": %u,\n", audio.max_output_gap_us,
            audio.max_clock_drift_us, audio.min_buffer_blocks);
    fprintf(out, "  \"audio_first_output_us\": %llu,\n  \"audio_last_output_us\": %llu,\n",
            (unsigned long long)audio.first_output_us, (unsigned long long)audio.last_output_us);
}
#endif
#endif
