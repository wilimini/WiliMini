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

}  // namespace

int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality) {
    liveLog("prepare bvid=%s cid=%lld", video.bvid, video.cid);
    if (transfer_cancel) {
        error = "Cancelled";
        return -1;
    }
    bool completed = false;
    bilibili::VideoUrlResult result;
    std::string apiError;
    int apiCode = 0;
    bilibili::BilibiliClient::get_video_url(
        video.bvid, static_cast<uint64_t>(video.cid), quality,
        [&](bilibili::VideoUrlResult value) {
            result = std::move(value);
            completed = true;
        },
        [&](const std::string& message, int code) {
            apiError = message;
            apiCode = code;
            liveLog("api error=%s code=%d", message.c_str(), code);
        });

    if (completed) {
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
            liveLog("range result=%d error=%s", rangeResult, network_error);
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
    network_error[0] = 0;
    liveLog("fallback progressive");
    const int fallbackResult = bili_prepare_playback(&video, &playback);
    liveLog("fallback result=%d error=%s", fallbackResult, network_error);
    if (fallbackResult == 0) {
        playback.quality = playback.qualities[0] = 16; playback.quality_count = 1;
        return 0;
    }
    if (network_error[0]) {
        error = network_error;
    } else if (!apiError.empty()) {
        error = apiError + " (" + std::to_string(apiCode) + ")";
    } else {
        error = "DASH and progressive MP4 are unavailable";
    }
    return -1;
}

int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality) {
    auto response = BilibiliApi{}.get(bilibili::Api::SeasonUrl2,
        {{"cid",std::to_string(video.cid)},{"qn",std::to_string(quality)},{"gaia_source",""},{"from_client","PC_APP"},
         {"is_main_page","false"},{"need_fragment","false"},{"isGaiaAvoided","false"},
         {"voice_balance","1"},{"drm_tech_type","3"},{"fourk","1"},{"fnval",bilibili::BilibiliClient::FNVAL},{"fnver","0"}},
        false, ErrorOperation::PreparePlayback);
    if (!response) { error = errorMessage(response.error); return -1; }
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
        if (!result.durl.empty() && !transfer_cancel) {
            BiliStream stream{};
            const auto document = nlohmann::json{{"code",0},{"data",info}}.dump();
            if (bili_parse_stream(document.c_str(),&stream) == 0) {
                const std::string key = "ep" + std::to_string(episode);
                const int code = bili_prepare_stream_playback(&stream,key.c_str(),&playback);
                if (code == 0) {
                    playback.quality = playback.qualities[0] = result.quality;
                    playback.quality_count = 1;
                }
                return code;
            }
        }
        error = network_error[0] ? network_error : "没有可播放的 H.264 / AAC 视频流";
    } catch (const std::exception&) { error = "服务器响应无法识别"; }
    return -1;
}

}  // namespace pspapp
