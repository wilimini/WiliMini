#ifndef WILI_HW_AVC_PLAYER_H
#define WILI_HW_AVC_PLAYER_H
/* Included by sce_mpeg_player.h (and so by player.c): the real-hardware segment of the PC stream
 * player. Demuxes with FFmpeg, hands each H.264 access unit to the AVC hardware (hw_avc.h), and
 * presents the pictures against the AAC clock exactly like the sceMpeg program-stream segment,
 * which stays for PPSSPP. */
#include "hw_avc.h"

/* The first SPS and PPS of an avcC record, without start codes, and its NAL length size. */
static int hw_avc_parameters(const AVCodecParameters *par, int *nal_length,
                             uint8_t *sps, int *sps_size, uint8_t *pps, int *pps_size) {
    const uint8_t *p = par->extradata;
    int size = par->extradata_size;
    if (size < 7 || p[0] != 1) return 0; /* no avcC: Annex B with parameter sets in band */
    *nal_length = (p[4] & 3) + 1;
    int at = 6;
    for (int group = 0; group < 2; ++group) {
        int count = group ? (at < size ? p[at++] : 0) : (p[5] & 31);
        for (int i = 0; i < count; ++i) {
            if (at + 2 > size) return -1;
            int n = AV_RB16(p + at); at += 2;
            if (n <= 0 || n > size - at) return -1;
            uint8_t *dst = group ? pps : sps;
            int *dst_size = group ? pps_size : sps_size;
            if (!*dst_size && n <= 256) { memcpy(dst, p + at, n); *dst_size = n; }
            at += n;
        }
    }
    return 0;
}

/* Rewrites one demuxed access unit as its slices with 4-byte lengths, which is what GetAvcNalAu
 * takes, and picks up in-band SPS/PPS; SEI, AUD and the rest are dropped, as Moonlight does.
 * Returns the output size (0 for an access unit without slices) or -1. */
static int hw_avc_access_unit(const uint8_t *in, int size, int nal_length, uint8_t *out, int capacity,
                              uint8_t *sps, int *sps_size, uint8_t *pps, int *pps_size, int *idr) {
    int at = 0, used = 0;
    *idr = 0;
    while (at < size) {
        int n = 0, begin;
        if (nal_length) {
            if (nal_length > size - at) return -1;
            for (int i = 0; i < nal_length; ++i) n = (n << 8) | in[at++];
            begin = at;
        } else {
            while (at + 3 <= size && !(in[at] == 0 && in[at + 1] == 0 && in[at + 2] == 1)) ++at;
            if (at + 3 > size) break;
            begin = at += 3;
            while (at + 3 <= size && !(in[at] == 0 && in[at + 1] == 0 && in[at + 2] <= 1)) ++at;
            if (at + 3 > size) at = size;
            n = at - begin;
            while (n > 0 && !in[begin + n - 1]) --n; /* trailing zero bytes */
        }
        if (n <= 0 || n > size - begin) return -1;
        if (nal_length) at = begin + n;
        const int type = in[begin] & 31;
        if (type == 1 || type == 5) {
            if (n > capacity - used - 4) return -1;
            AV_WB32(out + used, (uint32_t)n); used += 4;
            memcpy(out + used, in + begin, n); used += n;
            *idr |= type == 5;
        } else if (type == 7 && n <= 256) { memcpy(sps, in + begin, n); *sps_size = n; }
        else if (type == 8 && n <= 256) { memcpy(pps, in + begin, n); *pps_size = n; }
    }
    return used;
}

static int hw_avc_run_segment(const PspVideoOptions *options, RunStats *stats,
                              char *run_path, size_t run_path_size, int first_segment) {
    int ret = 0, audio_started = 0, opened = 0, failures = 0, need_idr = 1, eof = 0;
    unsigned segment_frames = 0, page = 0; /* frames shown since this segment started */
    HwAvc avc;
    memset(&avc, 0, sizeof(avc));
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    VideoSourceIO input = {0};
    uint8_t *rgba = NULL, *frame = NULL, *au = NULL;
    uint8_t sps[256], pps[256];
    int sps_size = 0, pps_size = 0, nal_length = 0, stream = -1, width = 0, height = 0;
    /* Output order is decode order (no B-frames), so pictures take the oldest pending time. */
    int64_t pts_queue[8];
    unsigned pts_head = 0, pts_count = 0;
    const char *backend_name = "psp-hw-avc";
    video_stage = PSP_VIDEO_STAGE_LOG;
    if (first_segment) {
        ret = select_run_dir(run_path, run_path_size);
        if (ret < 0) return ret;
    }
    stats->eof = 0;
    video_stage = PSP_VIDEO_STAGE_SOURCE;
    WLOG_I(WLC_PLAYER, "backend=%s", backend_name);
    ret = source_open(&format, SOURCE, video_io_source, &input);
    if (ret < 0) goto done;
    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) goto done;
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream < 0) { ret = stream; goto done; }
    video_origin_us = source_origin_us(format);
    playback_metadata(format->duration == AV_NOPTS_VALUE ? 0 : format->duration / 1000,
                      !format->pb || (format->pb->seekable & AVIO_SEEKABLE_NORMAL));
    AVCodecParameters *par = format->streams[stream]->codecpar;
    width = par->width; height = par->height;
    const int profile = par->profile == FF_PROFILE_UNKNOWN ? 0 : (par->profile & 0xff);
    if (par->codec_id != AV_CODEC_ID_H264 || width <= 0 || height <= 0 ||
        width > HW_AVC_MAX_W || height > HW_AVC_MAX_H || (profile && profile != 66 && profile != 77)) {
        WLOG_E(WLC_PLAYER, "hw-avc: unsupported stream %dx%d codec=%d profile=%d (needs H.264 "
               "Baseline or Main, at most %dx%d)", width, height, (int)par->codec_id, profile,
               HW_AVC_MAX_W, HW_AVC_MAX_H);
        ret = AVERROR(ENOSYS); goto done;
    }
    const int coded_height = (height + 15) & ~15;
    ret = source_start_at(format, stream, playback_start_ms);
    if (ret < 0) goto done;
    if (hw_avc_parameters(par, &nal_length, sps, &sps_size, pps, &pps_size) < 0) {
        ret = AVERROR_INVALIDDATA; goto done;
    }
    packet = av_packet_alloc();
    if (!packet) { ret = AVERROR(ENOMEM); goto done; }
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if ((int)i != stream) format->streams[i]->discard = AVDISCARD_ALL;
    video_stage = PSP_VIDEO_STAGE_VIDEO_INIT;
    const size_t bpp = rgb_pixel_bytes, frame_bytes = (size_t)512 * 272 * bpp;
    rgba = memalign(64, HW_AVC_PICTURES * HW_AVC_PICTURE_BYTES);
    frame = memalign(64, frame_bytes);
    au = memalign(64, HW_AVC_MAX_AU);
    if (!rgba || !frame || !au) { ret = AVERROR(ENOMEM); goto done; }
    void *const pictures[HW_AVC_PICTURES] = {
        rgba, rgba + HW_AVC_PICTURE_BYTES, rgba + 2 * HW_AVC_PICTURE_BYTES};
    memset(frame, 0, frame_bytes); /* the borders stay black: only the picture area is rewritten */
    int out_w = 480, out_h = (int)((int64_t)height * 480 / width);
    if (out_h > 272) { out_h = 272; out_w = (int)((int64_t)width * 272 / height); }
    if (out_w < 2) out_w = 2;
    if (out_h < 1) out_h = 1;
    const int direct = out_w == width && out_h == height;
    const int off_x = (480 - out_w) / 2, off_y = (272 - out_h) / 2;
    uint16_t xmap[480], ymap[272];
    if (!direct) {
        for (int x = 0; x < out_w; ++x) xmap[x] = (uint16_t)(x * width / out_w);
        for (int y = 0; y < out_h; ++y) ymap[y] = (uint16_t)(y * height / out_h);
    }
    video_stage = PSP_VIDEO_STAGE_AUDIO_INIT;
    audio_started = 1;
    ret = audio_open(SOURCE, audio_io_source ? audio_io_source : video_io_source,
                     audio_io_source ? NULL : format, "");
    if (ret < 0) goto done;
    video_stage = PSP_VIDEO_STAGE_PLAY;
    playback_seeking = 0; /* a soft reseek is complete once the new segment can play */
    const AVRational time_base = format->streams[stream]->time_base;
    while (!quit_requested && !soft_reseek_request) {
        int count = 0;
        if (!eof) {
            playback_operation_set("hw_read");
            av_packet_unref(packet);
            ret = av_read_frame(format, packet);
            if (ret == AVERROR_EOF) { eof = 1; ret = 0; }
            else if (ret < 0) break;
            else if (packet->stream_index != stream) continue;
        }
        if (!eof) {
            int idr = 0;
            const int size = hw_avc_access_unit(packet->data, packet->size, nal_length, au, HW_AVC_MAX_AU,
                                                sps, &sps_size, pps, &pps_size, &idr);
            if (size < 0) {
                WLOG_W(WLC_PLAYER, "hw-avc: malformed or oversized access unit (%d bytes)", packet->size);
                need_idr = 1;
                continue;
            }
            if (!size || (need_idr && !idr) || !sps_size || !pps_size) continue;
            if (!opened) {
                const int cabac = hw_avc_pps_cabac(pps, pps_size);
                const int main_mode = sps[1] != 66 || cabac == 1;
                WLOG_I(WLC_PLAYER, "hw-avc: stream %dx%d profile=%d level=%d entropy=%s",
                       width, height, sps[1], sps[3], cabac == 1 ? "CABAC" : cabac == 0 ? "CAVLC" : "?");
                ret = hw_avc_open(&avc, main_mode);
                if (ret < 0) break;
                opened = 1;
            }
            int64_t pts = packet->pts == AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (pts == AV_NOPTS_VALUE) pts = 0;
            if (pts_count == 8) { pts_head = (pts_head + 1) & 7; --pts_count; }
            pts_queue[(pts_head + pts_count++) & 7] =
                av_rescale_q(pts, time_base, AV_TIME_BASE_Q) - video_origin_us;
            playback_operation_set("hw_decode");
            count = hw_avc_decode(&avc, sps, sps_size, pps, pps_size, au, size, idr, coded_height, pictures);
            if (count < 0) {
                /* Like Moonlight: rebuild the session after an error instead of trusting its
                 * references, and wait for the next keyframe. */
                hw_avc_close(&avc);
                opened = 0; need_idr = 1; pts_count = 0;
                if (++failures > 3) { ret = count; break; }
                ret = 0;
                continue;
            }
            failures = 0; need_idr = 0;
        } else {
            if (opened) {
                playback_operation_set("hw_drain");
                count = hw_avc_drain(&avc, coded_height, pictures);
                if (count < 0) count = 0;
            }
            if (!count) { stats->eof = 1; ret = stats->total_frames ? 0 : AVERROR_INVALIDDATA; break; }
        }
        for (int i = 0; i < count && !quit_requested && !soft_reseek_request; ++i) {
            int64_t pts_us = 0;
            if (pts_count) { pts_us = pts_queue[pts_head]; pts_head = (pts_head + 1) & 7; --pts_count; }
            if (stats->total_frames < 3 || stats->total_frames % 60 == 0)
                WLOG_I(WLC_PLAYER, "hw_frame frames=%u pictures=%d pts_us=%lld audio_us=%lld",
                       stats->total_frames, count, (long long)pts_us, (long long)audio_clock_us());
            if (pts_us < playback_start_ms * 1000) continue;
            if (first_video_us < 0) first_video_us = pts_us;
            playback_operation_set("hw_audio_wait");
            while (!quit_requested && !soft_reseek_request && !audio.error) {
                int64_t clock = audio_clock_us();
                /* When audio ends before video, let the remaining video follow
                 * the same epoch instead of waiting on a stopped audio clock. */
                if (audio.eof && audio.consumed == audio.produced)
                    clock = audio.first_pts_us + playback_now_us() - audio.epoch_us;
                int paused = playback_is_paused();
                if ((!paused && pts_us <= clock - source_origin_us(audio.format)) ||
                    (presenter && paused && !segment_frames)) break;
                if (presenter && present_video(NULL, output_pixel_format) < 0) { ret = -113; break; }
                sceKernelDelayThreadCB(2000);
            }
            if (ret < 0 || quit_requested || soft_reseek_request) break;
            if (audio.error) { ret = audio.error; break; }
            playback_operation_set("hw_scale");
            /* CSC writes PSP RGBA8888 (red in the low byte); RGB565 output is PSP BGR565. */
            const uint32_t *source = (const uint32_t *)pictures[i];
            for (int y = 0; y < out_h; ++y) {
                const uint32_t *sp = source + (size_t)(direct ? y : ymap[y]) * HW_AVC_STRIDE;
                if (bpp == 4) {
                    uint32_t *dp = (uint32_t *)frame + (size_t)(y + off_y) * 512 + off_x;
                    if (direct) memcpy(dp, sp, (size_t)out_w * 4);
                    else for (int x = 0; x < out_w; ++x) dp[x] = sp[xmap[x]];
                } else {
                    uint16_t *dp = (uint16_t *)frame + (size_t)(y + off_y) * 512 + off_x;
                    for (int x = 0; x < out_w; ++x) {
                        const uint32_t c = sp[direct ? x : xmap[x]];
                        dp[x] = (uint16_t)(((c >> 3) & 0x1f) | ((c >> 5) & 0x7e0) | ((c >> 8) & 0xf800));
                    }
                }
            }
            /* The presenter and the DMA engine read this buffer through RAM, not our cache. */
            sceKernelDcacheWritebackRange(frame, frame_bytes);
            if (presenter) {
                playback_operation_set("hw_present");
                ret = present_video(frame, output_pixel_format);
                if (ret < 0) break;
            } else {
                void *vram = (void *)(0x44000000 + (size_t)page * frame_bytes);
                memcpy(vram, frame, frame_bytes);
                sceDisplayWaitVblankStart();
                sceDisplaySetFrameBuf(vram, 512,
                    output_pixel_format == PSP_VIDEO_PIXEL_FORMAT_RGB565
                        ? PSP_DISPLAY_PIXEL_FORMAT_565 : PSP_DISPLAY_PIXEL_FORMAT_8888,
                    PSP_DISPLAY_SETBUF_IMMEDIATE);
            }
            page ^= 1;
            ++segment_frames;
            ++stats->total_frames;
            playback_position_us = pts_us;
            if (stats->total_frames == 1 && video_io_source && video_io_source->first_frame)
                video_io_source->first_frame(video_io_source->opaque);
        }
        if (ret < 0) break;
        if (options->sample && stats->total_frames >= 300) break;
    }
 done:
    playback_operation_set("hw_cleanup_audio");
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
    playback_operation_set("hw_cleanup_decoder");
    if (opened) hw_avc_close(&avc);
    free(au); free(frame); free(rgba);
    av_packet_free(&packet);
    source_close(&format, &input);
    if (run_path[0]) {
        char path[224]; snprintf(path, sizeof(path), "%s/summary.json", run_path);
        FILE *log = fopen(path, "wb");
        if (log) {
            fprintf(log, "{\n  \"backend\": \"%s\",\n  \"frames\": %u,\n  \"first_video_us\": %lld,\n"
                    "  \"error\": %d,\n  \"eof\": %u\n}\n",
                    backend_name, stats->total_frames, (long long)first_video_us, ret, stats->eof);
            fclose(log);
        }
    }
    if (ret >= 0) video_stage = PSP_VIDEO_STAGE_NONE;
    return ret;
}
#endif
