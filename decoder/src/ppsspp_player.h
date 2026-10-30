#ifndef WILI_PPSSPP_PLAYER_H
#define WILI_PPSSPP_PLAYER_H
#include <pspmpeg.h>
#include <libavutil/intreadwrite.h>

/* Included by player.c to share source IO, cancellation and the AAC clock.
 * Only this backend synthesizes PSMF/PS: PSP firmware keeps using the ME path.
 * PPSSPP's sceMpeg HLE accepts PS, not MP4 access units or GetAvcNalAu. */
#define EMU_RING_PACKETS 256
#define EMU_MAX_AU (4 * 1024 * 1024)
typedef struct {
    AVFormatContext *format;
    AVPacket *packet;
    int stream, nal_length, eof, error;
    uint8_t *parameters, *data;
    unsigned parameters_size, size, offset;
    int64_t pts, dts;
} EmuMux;

static void emu_timestamp(uint8_t *p, int prefix, int64_t value) {
    uint64_t t = (uint64_t)value & 0x1ffffffffULL;
    p[0] = (prefix << 4) | ((t >> 29) & 14) | 1;
    p[1] = t >> 22; p[2] = ((t >> 14) & 254) | 1;
    p[3] = t >> 7; p[4] = ((t << 1) & 254) | 1;
}

static int emu_parameters(EmuMux *mux, const AVCodecParameters *par) {
    const uint8_t *p = par->extradata;
    unsigned size = par->extradata_size;
    if (size > EMU_MAX_AU) return AVERROR_INVALIDDATA;
    if (!size) return 0; /* FLV/Annex B may carry SPS/PPS in-band. */
    if (size < 7 || p[0] != 1) {
        mux->parameters = av_memdup(p, size);
        if (!mux->parameters) return AVERROR(ENOMEM);
        mux->parameters_size = size;
        return 0;
    }
    mux->nal_length = (p[4] & 3) + 1;
    mux->parameters = av_malloc(size + 1024);
    if (!mux->parameters) return AVERROR(ENOMEM);
    unsigned at = 6, count = p[5] & 31;
    for (int group = 0; group < 2; ++group) {
        for (unsigned i = 0; i < count; ++i) {
            if (at + 2 > size) return AVERROR_INVALIDDATA;
            unsigned n = AV_RB16(p + at); at += 2;
            if (!n || n > size - at) return AVERROR_INVALIDDATA;
            AV_WB32(mux->parameters + mux->parameters_size, 1);
            mux->parameters_size += 4;
            memcpy(mux->parameters + mux->parameters_size, p + at, n);
            mux->parameters_size += n; at += n;
        }
        if (!group) {
            if (at >= size) return AVERROR_INVALIDDATA;
            count = p[at++];
        }
    }
    return 0;
}

static int emu_next_packet(EmuMux *mux) {
    av_freep(&mux->data);
    mux->size = mux->offset = 0;
    int ret;
    do {
        av_packet_unref(mux->packet);
        ret = av_read_frame(mux->format, mux->packet);
        if (ret < 0) return ret;
    } while (mux->packet->stream_index != mux->stream && !quit_requested);
    if (quit_requested) return AVERROR_EXIT;
    AVPacket *packet = mux->packet;
    unsigned extra = (packet->flags & AV_PKT_FLAG_KEY) ? mux->parameters_size : 0;
    unsigned size = packet->size;
    if (size > EMU_MAX_AU) return AVERROR_INVALIDDATA;
    /* At most four output bytes for every one-byte NAL length. */
    mux->data = av_malloc(size * 4 + extra + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!mux->data) return AVERROR(ENOMEM);
    if (extra) memcpy(mux->data, mux->parameters, extra);
    mux->size = extra;
    if (mux->nal_length) {
        unsigned at = 0;
        while (at < size) {
            if ((unsigned)mux->nal_length > size - at) return AVERROR_INVALIDDATA;
            unsigned n = 0;
            for (int i = 0; i < mux->nal_length; ++i) n = (n << 8) | packet->data[at++];
            if (!n || n > size - at) return AVERROR_INVALIDDATA;
            AV_WB32(mux->data + mux->size, 1); mux->size += 4;
            memcpy(mux->data + mux->size, packet->data + at, n);
            mux->size += n; at += n;
        }
    } else {
        memcpy(mux->data + mux->size, packet->data, size); mux->size += size;
    }
    AVRational tb = mux->format->streams[mux->stream]->time_base;
    int64_t pts = packet->pts == AV_NOPTS_VALUE ? packet->dts : packet->pts;
    int64_t dts = packet->dts == AV_NOPTS_VALUE ? pts : packet->dts;
    if (pts == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
    int64_t origin = av_rescale_q(source_origin_us(mux->format), AV_TIME_BASE_Q, (AVRational){1,90000});
    mux->pts = av_rescale_q(pts, tb, (AVRational){1,90000}) - origin + 90000;
    mux->dts = av_rescale_q(dts, tb, (AVRational){1,90000}) - origin + 90000;
    return 0;
}

/* Each callback packet is a complete MPEG-2 pack/PES, padded to 2048 bytes.
 * Keep packets bounded so streaming never needs a complete movie in memory. */
static SceInt32 emu_ring_read(void *output, SceInt32 packets, void *opaque) {
    EmuMux *mux = opaque;
    static const uint8_t pack[14] = {0,0,1,0xba,0x44,0,4,0,4,1,0,0,3,0xf8};
    int written = 0;
    while (written < packets && !quit_requested && !mux->error) {
        if (mux->offset == mux->size) {
            int ret = emu_next_packet(mux);
            if (ret < 0) {
                if (ret == AVERROR_EOF) mux->eof = 1;
                else mux->error = ret;
                break;
            }
        }
        uint8_t *p = (uint8_t *)output + written * 2048;
        memset(p, 0xff, 2048); memcpy(p, pack, sizeof(pack));
        int first = mux->offset == 0;
        unsigned header = first ? 19 : 9;
        unsigned n = mux->size - mux->offset;
        if (n > 2034 - header) n = 2034 - header;
        unsigned pad = 2034 - header - n;
        unsigned stuffing = pad < 6 ? pad : 0;
        p += 14;
        p[0]=0; p[1]=0; p[2]=1; p[3]=0xe0;
        AV_WB16(p+4, header + stuffing + n - 6);
        p[6]=0x80; p[7]=first ? 0xc0 : 0; p[8]=(first ? 10 : 0) + stuffing;
        if (first) { emu_timestamp(p+9,3,mux->pts); emu_timestamp(p+14,1,mux->dts); }
        memcpy(p + header + stuffing, mux->data + mux->offset, n);
        if (pad >= 6) {
            p += header + n;
            p[0]=0; p[1]=0; p[2]=1; p[3]=0xbe; AV_WB16(p+4,pad-6);
        }
        mux->offset += n; ++written;
    }
    return written;
}

static int ppsspp_run_decode(const PspVideoOptions *options, RunStats *stats) {
    int ret = 0, initialized = 0, created = 0, constructed = 0, audio_started = 0;
    SceMpeg mpeg = NULL;
    SceMpegRingbuffer ring = {0};
    SceMpegStream *stream = NULL;
    SceMpegAu au __attribute__((aligned(64)));
    void *state = NULL, *ring_data = NULL, *es = NULL;
    uint32_t *rgb = NULL;
    VideoSourceIO input = {0};
    EmuMux mux = {0};
    uint8_t header[2048] __attribute__((aligned(64))) = {0};
    int stride = 0, width = 0, height = 0;
    char run_path[192] = {0};
    video_stage = PSP_VIDEO_STAGE_LOG;
    ret = select_run_dir(run_path, sizeof(run_path));
    if (ret < 0) return ret;
    video_stage = PSP_VIDEO_STAGE_SOURCE;
    printf("PLAYER backend=ppsspp-sceMpeg\n");
    ret = source_open(&mux.format, SOURCE, video_io_source, &input);
    if (ret < 0) goto done;
    ret = avformat_find_stream_info(mux.format, NULL);
    if (ret < 0) goto done;
    mux.stream = av_find_best_stream(mux.format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (mux.stream < 0) { ret = mux.stream; goto done; }
    video_origin_us = source_origin_us(mux.format);
    playback_metadata(mux.format->duration == AV_NOPTS_VALUE ? 0 : mux.format->duration / 1000,
                      !mux.format->pb || (mux.format->pb->seekable & AVIO_SEEKABLE_NORMAL));
    AVCodecParameters *par = mux.format->streams[mux.stream]->codecpar;
    width = par->width; height = par->height; stride = (width + 15) & ~15;
    if (par->codec_id != AV_CODEC_ID_H264 || width <= 0 || height <= 0 || width > 1280 || height > 720) {
        ret = AVERROR(ENOSYS); goto done;
    }
    ret = source_start_at(mux.format, mux.stream, playback_start_ms);
    if (ret < 0) goto done;
    ret = emu_parameters(&mux, par);
    if (ret < 0) goto done;
    mux.packet = av_packet_alloc();
    if (!mux.packet) { ret = AVERROR(ENOMEM); goto done; }
    for (unsigned i = 0; i < mux.format->nb_streams; ++i)
        if ((int)i != mux.stream) mux.format->streams[i]->discard = AVDISCARD_ALL;
    video_stage = PSP_VIDEO_STAGE_VIDEO_INIT;
    ret = sceMpegInit();
    if (ret < 0) goto done;
    initialized = 1;
    int state_size = sceMpegQueryMemSize(0), ring_size = sceMpegRingbufferQueryMemSize(EMU_RING_PACKETS);
    if (state_size <= 0 || ring_size <= 0) { ret = -1; goto done; }
    state = memalign(64, state_size); ring_data = memalign(64, ring_size);
    rgb = memalign(64, stride * ((height + 15) & ~15) * 4);
    if (!state || !ring_data || !rgb) { ret = AVERROR(ENOMEM); goto done; }
    ret = sceMpegRingbufferConstruct(&ring, EMU_RING_PACKETS, ring_data, ring_size, emu_ring_read, &mux);
    if (ret < 0) goto done;
    constructed = 1;
    ret = sceMpegCreate(&mpeg, state, state_size, &ring, stride, 0, 0);
    if (ret < 0) goto done;
    created = 1;
    /* A streaming PSMF header with one AVC stream. PS timestamps are 90 kHz,
     * offset by one second as expected by PPSSPP's MediaEngine HLE. */
    memcpy(header,"PSMF0015",8); AV_WB32(header+8,2048); AV_WB32(header+12,0x7ffff800);
    AV_WB32(header+0x56,90000);
    AV_WB32(header+0x5c,0x7fffffff);
    AV_WB16(header+0x80,1); header[0x82]=0xe0;
    header[0x8e]=(width+15)/16; header[0x8f]=(height+15)/16;
    SceInt32 offset;
    ret = sceMpegQueryStreamOffset(&mpeg,header,&offset);
    if (ret < 0) goto done;
    stream = sceMpegRegistStream(&mpeg,0,0);
    if (!stream) { ret = -1; goto done; }
    es = sceMpegMallocAvcEsBuf(&mpeg);
    if (!es) { ret = AVERROR(ENOMEM); goto done; }
    ret = sceMpegInitAu(&mpeg,es,&au);
    if (ret < 0) goto done;
    SceMpegAvcMode mode = {-1,SCE_MPEG_AVC_FORMAT_8888};
    ret = sceMpegAvcDecodeMode(&mpeg,&mode);
    if (ret < 0) goto done;
    video_stage = PSP_VIDEO_STAGE_AUDIO_INIT;
    audio_started = 1;
    ret = audio_open(SOURCE, audio_io_source ? audio_io_source : video_io_source, "");
    if (ret < 0) goto done;
    video_stage = PSP_VIDEO_STAGE_PLAY;
    unsigned page = 0, empty = 0;
    while (!quit_requested) {
        int available = sceMpegRingbufferAvailableSize(&ring);
        if (available < 0) { ret = available; break; }
        if (!mux.eof && available) {
            ret = sceMpegRingbufferPut(&ring,available,available);
            if (ret < 0) break;
        }
        if (mux.error) { ret = mux.error; break; }
        SceInt32 unknown = 0, got = 0;
        ret = sceMpegGetAvcAu(&mpeg,stream,&au,&unknown);
        if (ret < 0) {
            if (mux.eof && (unsigned)ret == 0x80618001u) {
                stats->eof = 1;
                ret = stats->total_frames ? 0 : AVERROR_INVALIDDATA;
            }
            break;
        }
        void *pixels = rgb;
        ret = sceMpegAvcDecode(&mpeg,&au,stride,&pixels,&got);
        if (ret < 0) break;
        if (!got) {
            if (++empty > 64) { ret = AVERROR_INVALIDDATA; break; }
            continue;
        }
        empty = 0;
        /* PPSSPP updates the AU with the reordered output timestamp. */
        int64_t pts_us = (((int64_t)au.iPtsMSB << 32 | au.iPts) - 90000) * 1000000 / 90000;
        if (pts_us < playback_start_ms * 1000) continue;
        if (first_video_us < 0) first_video_us = pts_us;
        while (!quit_requested && !audio.error) {
            int64_t clock = audio_clock_us();
            /* When audio ends before video, let the remaining video follow
             * the same epoch instead of waiting on a stopped audio clock. */
            if (audio.eof && audio.consumed == audio.produced)
                clock = audio.first_pts_us + playback_now_us() - audio.epoch_us;
            int paused = playback_is_paused();
            if ((!paused && pts_us <= clock - source_origin_us(audio.format)) ||
                (presenter && paused && !stats->total_frames)) break;
            if (presenter && present_video(NULL) < 0) { ret = -113; break; }
            sceKernelDelayThreadCB(2000);
        }
        if (quit_requested) break;
        if (audio.error) { ret = audio.error; break; }
        uint32_t *vram = (uint32_t *)(0x44000000 + (presenter ? 2 : page) * 512 * 272 * 4);
        memset(vram,0,512*272*4);
        int w=480, h=(int)((int64_t)height*480/width);
        if (h>272) { h=272; w=(int)((int64_t)width*272/height); }
        for (int y=0;y<h;++y) {
            uint32_t *dst=vram+(y+(272-h)/2)*512+(480-w)/2;
            const uint32_t *src=rgb+(y*height/h)*stride;
            for (int x=0;x<w;++x) dst[x]=src[x*width/w];
        }
        if (presenter) {
            ret = present_video(vram);
            if (ret < 0) break;
        } else {
            sceDisplayWaitVblankStart();
            sceDisplaySetFrameBuf(vram,512,PSP_DISPLAY_PIXEL_FORMAT_8888,PSP_DISPLAY_SETBUF_IMMEDIATE);
        }
        page ^= 1;
        ++stats->total_frames;
        playback_position_us = pts_us;
        if (stats->total_frames == 1 && video_io_source && video_io_source->first_frame)
            video_io_source->first_frame(video_io_source->opaque);
        if (options->sample && stats->total_frames >= 300) break;
    }
 done:
    if (audio_started) {
        int64_t origin = audio.format ? source_origin_us(audio.format) : 0;
        audio_close(stats->eof && ret >= 0);
        if (ret >= 0 && audio.error) ret = audio.error;
        if (stats->total_frames && audio.first_pts_us != AV_NOPTS_VALUE && audio.rate) {
            int64_t position = audio_clock_us() - origin;
            if (stats->eof && ret >= 0)
                position = audio.first_pts_us - origin + (int64_t)audio.source_samples * 1000000 / audio.rate;
            if (position >= 0 && (!stats->eof || position > playback_position_us))
                playback_position_us = position;
        }
    }
    if (created) {
        if (es) sceMpegFreeAvcEsBuf(&mpeg,es);
        /* PSPSDK declares the first argument as SceMpeg, but the firmware
         * and PPSSPP ABI take the address of the handle, like Delete. */
        if (stream) sceMpegUnRegistStream((SceMpeg)&mpeg,stream);
        sceMpegDelete(&mpeg);
    }
    if (constructed) sceMpegRingbufferDestruct(&ring);
    if (initialized) sceMpegFinish();
    free(rgb); free(ring_data); free(state);
    av_packet_free(&mux.packet); av_free(mux.parameters); av_free(mux.data);
    source_close(&mux.format,&input);
    if (run_path[0]) {
        char path[224]; snprintf(path,sizeof(path),"%s/summary.json",run_path);
        FILE *log=fopen(path,"wb");
        if (log) {
            fprintf(log,"{\n  \"backend\": \"ppsspp-sceMpeg\",\n  \"frames\": %u,\n  \"rgb_presented\": %u,\n  \"first_video_us\": %lld,\n  \"error\": %d,\n",stats->total_frames,stats->total_frames,(long long)first_video_us,ret);
            if (audio_started) fprintf(log,
                "  \"audio_error\": %d,\n  \"audio_first_pts_us\": %lld,\n"
                "  \"audio_submitted_samples\": %llu,\n  \"audio_source_samples\": %llu,\n",
                audio.error, (long long)audio.first_pts_us,
                (unsigned long long)audio.submitted_samples, (unsigned long long)audio.source_samples);
            fprintf(log,"  \"eof\": %u\n}\n",stats->eof); fclose(log);
        }
    }
    if (ret >= 0) video_stage = PSP_VIDEO_STAGE_NONE;
    return ret;
}
#endif
