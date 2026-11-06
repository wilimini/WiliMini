#include "playback.hpp"
#include "services/bilibili_api.hpp"
#include "api/wiliwili/bilibili/api.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

#include "api/wiliwili/bilibili.h"
#include "api/wiliwili/bilibili/result/video_detail_result.h"

namespace pspapp {

namespace {

#ifdef BILI_DEVICE_LIVE
void liveLog(const char* format, ...) {
    FILE* file = std::fopen(CLIENT_PATH("/dash-live.txt"), "a");
    if (!file) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    std::fclose(file);
}
#else
void liveLog(const char*, ...) {}
#endif

const bilibili::DashMedia* selectVideo(const bilibili::VideoUrlResult& result, int quality) {
    const bilibili::DashMedia* selected = nullptr;
    for (const auto& item : result.dash.video) {
        if (item.codecid != 7 || (item.id != 16 && item.id != 32 && item.id != 64) ||
            item.width > 1280 || item.height > 720 || item.base_url.empty()) continue;
        if (!selected || (item.id <= quality && (selected->id > quality || item.id > selected->id)) ||
            (item.id > quality && selected->id > quality && item.id < selected->id) ||
            (item.id == selected->id && item.bandwidth < selected->bandwidth)) selected = &item;
    }
    return selected;
}

void fillQuality(BiliPlayback& playback, const bilibili::VideoUrlResult& result, int selected) {
    playback.quality = selected;
    playback.quality_count = 0;
    for (int id : {16, 32, 64}) {
        for (const auto& item : result.dash.video) {
            if (item.id == id && item.codecid == 7 && item.width <= 1280 && item.height <= 720 && !item.base_url.empty()) {
                playback.qualities[playback.quality_count++] = id;
                break;
            }
        }
    }
}
const bilibili::DashMedia* selectAudio(const bilibili::VideoUrlResult& result) {
    static constexpr int preference[] = {30280, 30232, 30216};
    for (int id : preference) {
        for (const auto& item : result.dash.audio) {
            if (item.id == id && !item.base_url.empty()) return &item;
        }
    }
    return nullptr;
}

void fillMediaUrl(BiliMediaUrl& target, const bilibili::DashMedia& source) {
    target = BiliMediaUrl{};
    target.url = source.base_url.c_str();
    const size_t count = source.backup_url.size() < (BILI_MEDIA_URLS - 1)
                             ? source.backup_url.size()
                             : (BILI_MEDIA_URLS - 1);
    for (size_t i = 0; i < count; ++i) target.backups[i] = source.backup_url[i].c_str();
    target.backup_count = static_cast<int>(count);
}

/* wili-server transcodes the video to an FLV the PSP can decode and the server
 * seeks by restarting at startMs. */
int prepareServerPlayback(const BiliVideo& video, bool pgc, const StreamTarget& stream,
                          BiliPlayback& playback, std::string& error, AppError* typedError) {
    WiliStreamRequest request{};
    request.bvid = pgc ? nullptr : video.bvid;
    request.cid = video.cid;
    request.start_ms = stream.startMs;
    request.pgc = pgc ? 1 : 0;
    request.preset = stream.preset;
    request.source_qn = stream.sourceQn;
    request.max_fps = stream.maxFps;
    request.profile = stream.profile;
    char url[640];
    if (wili_server_stream_url(&stream.server, &request, url, sizeof(url)) != 0) {
        error = "stream_url";
        if (typedError) *typedError = {ErrorDomain::Playback, ErrorKind::StreamServer,
                                       ErrorOperation::PreparePlayback, 0, 0, "bad_request"};
        return -1;
    }
    int status = 0;
    char serverCode[48];
    playback = BiliPlayback{};
    if (bili_prepare_stream_server(url, &playback, &status, serverCode, sizeof(serverCode)) < 0) {
        if (transfer_cancel) {
            error = "Cancelled";
            if (typedError) *typedError = {ErrorDomain::Playback, ErrorKind::Cancelled, ErrorOperation::PreparePlayback};
        } else {
            error = streamErrorCode(status, serverCode);
            if (typedError) *typedError = {ErrorDomain::Playback, ErrorKind::StreamServer,
                                           ErrorOperation::PreparePlayback, 0, status, error};
        }
        return -1;
    }
    playback.restart_seek = 1;
    playback.timeline_offset_ms = stream.startMs;
    playback.timeline_duration_ms = video.duration > 0 ? static_cast<long long>(video.duration) * 1000 : 0;
    // The source tier comes from Settings (Stream source); the on-screen quality menu has nothing to offer.
    playback.quality = 0;
    playback.quality_count = 0;
    liveLog("server stream ready offset=%lld", stream.startMs);
    return 0;
}

}  // namespace

const char* streamErrorCode(int httpStatus, const char* serverCode) {
    if (serverCode && serverCode[0]) return serverCode;
    switch (httpStatus) {
        case 0: return "unreachable";
        case 401: return "unauthorized";
        case 429: return "rate_limited";
        case 503: return "busy";
        default: return "upstream";
    }
}

int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality, AppError* typedError, const StreamTarget* stream) {
    liveLog("prepare bvid=%s cid=%lld", video.bvid, video.cid);
    if (stream) return prepareServerPlayback(video, false, *stream, playback, error, typedError);
    if (transfer_cancel) {
        error = "Cancelled";
        return -1;
    }
    auto response = BilibiliApi{}.get(bilibili::Api::PlayUrl2,
        {{"bvid", video.bvid}, {"cid", std::to_string(video.cid)}, {"qn", std::to_string(quality)},
         {"fnval", bilibili::BilibiliClient::FNVAL}, {"fnver", "0"}, {"fourk", "1"},
         {"from_client", "BROWSER"}, {"web_location", "1315873"},
         {"gaia_source", "view-card"}, {"is_main_page", "false"}, {"need_fragment", "false"},
         {"isGaiaAvoided", "true"}, {"voice_balance", "1"}},
        true, ErrorOperation::PreparePlayback, false);
    if (!response) {
        if (typedError) *typedError = response.error;
        error = errorMessage(response.error);
        return -1;
    }
    bilibili::VideoUrlResult result;
    try { result = response.value.data.get<bilibili::VideoUrlResult>(); }
    catch (...) {
        if (typedError) *typedError = {ErrorDomain::Api, ErrorKind::InvalidResponse,
                                       ErrorOperation::PreparePlayback, 0, 0, "playurl_parse_failed"};
        error = "播放地址响应无法识别"; return -1;
    }
    {
        const bilibili::DashMedia* videoMedia = selectVideo(result, quality);
        const bilibili::DashMedia* audioMedia = selectAudio(result);
        liveLog("dash videos=%zu audios=%zu selected_video=%d selected_audio=%d",
                result.dash.video.size(), result.dash.audio.size(),
                videoMedia ? videoMedia->id : 0, audioMedia ? audioMedia->id : 0);
        if (videoMedia && audioMedia) {
            BiliMediaUrl videoUrl{}, audioUrl{};
            fillMediaUrl(videoUrl, *videoMedia);
            fillMediaUrl(audioUrl, *audioMedia);
#ifdef BILI_DEVICE_LIVE
            std::fprintf(stderr, "DASH video id=%d codec=%d audio id=%d\n",
                         videoMedia->id, videoMedia->codecid, audioMedia->id);
#endif
            const int rangeResult = bili_prepare_range_playback(&video, &videoUrl, &audioUrl, &playback);
            liveLog("range result=%d", rangeResult);
            if (rangeResult == 0) {
                if (!transfer_cancel) { fillQuality(playback, result, videoMedia->id); return 0; }
                bili_finish_playback(&playback);
                error = "Cancelled";
                return -1;
            }
        }
    }

    if (transfer_cancel) {
        error = "Cancelled";
        return -1;
    }
    if (network_error[0]) error = "视频源请求失败";
    else error = "没有可播放的 H.264 / AAC DASH 视频流";
    if (typedError) *typedError = {ErrorDomain::Playback, ErrorKind::Unknown,
                                   ErrorOperation::PreparePlayback, -1, 0,
                                   network_error[0] ? network_error : "dash_unavailable"};

    return -1;
}

int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality, AppError* typedError, const StreamTarget* stream) {
    (void)episode;
    if (stream) return prepareServerPlayback(video, true, *stream, playback, error, typedError);
    auto response = BilibiliApi{}.get(bilibili::Api::SeasonUrl2,
        {{"cid",std::to_string(video.cid)},{"qn",std::to_string(quality)},{"gaia_source",""},{"from_client","PC_APP"},
         {"is_main_page","false"},{"need_fragment","false"},{"isGaiaAvoided","false"},
         {"voice_balance","1"},{"drm_tech_type","3"},{"fourk","1"},{"fnval",bilibili::BilibiliClient::FNVAL},{"fnver","0"}},
        false, ErrorOperation::PreparePlayback, false);
    if (!response) { if (typedError) *typedError = response.error; error = errorMessage(response.error); return -1; }
    try {
        const auto& info = response.value.data.at("video_info");
        auto result = info.get<bilibili::VideoUrlResult>();
        const auto *videoMedia = selectVideo(result, quality), *audioMedia = selectAudio(result);
        if (videoMedia && audioMedia) {
            BiliMediaUrl videoUrl{}, audioUrl{};
            fillMediaUrl(videoUrl,*videoMedia); fillMediaUrl(audioUrl,*audioMedia);
            const int code = bili_prepare_range_playback(&video,&videoUrl,&audioUrl,&playback);
            if (code == 0) { fillQuality(playback, result, videoMedia->id); return 0; }
        }
        error = network_error[0] ? network_error : "没有可播放的 H.264 / AAC 视频流";
    } catch (const std::exception&) { error = "服务器响应无法识别"; }
    if (typedError) *typedError = {ErrorDomain::Playback, ErrorKind::Unknown,
                                   ErrorOperation::PreparePlayback, -1, 0,
                                   network_error[0] ? network_error : "dash_unavailable"};
    return -1;
}

}  // namespace pspapp
