#include "subtitles.hpp"
#include "bilibili_api.hpp"
#include "../bilibili.h"
namespace pspapp {
std::vector<SubtitleTrack> loadSubtitles(const std::string& bvid, uint64_t cid, uint64_t aid) {
    std::vector<SubtitleTrack> tracks;
    if (!cid || (bvid.empty() && !aid)) return tracks;
    ApiParams params{{"cid", std::to_string(cid)}};
    params.emplace_back(bvid.empty() ? "aid" : "bvid", bvid.empty() ? std::to_string(aid) : bvid);
    auto result = BilibiliApi{}.get("https://api.bilibili.com/x/player/v2", params, false, ErrorOperation::Request, false);
    if (!result || transfer_cancel) return tracks;
    try {
        const auto& list = result.value.data.at("subtitle").at("subtitles");
        if (!list.is_array()) return tracks;
        unsigned attempts = 0;
        for (const auto& entry : list) {
            if (transfer_cancel || attempts++ == 3) break;
            std::string url = entry.value("subtitle_url", "");
            if (url.rfind("//", 0) == 0) url = "https:" + url;
            if (url.rfind("https://", 0) != 0) continue;
            auto response = defaultHttpClient().get(url, ErrorOperation::Request, false);
            if (!response) continue;
            auto cues = parseSubtitles(response.value.body);
            if (cues.empty()) continue;
            std::string label = entry.value("lan_doc", entry.value("lan", "Subtitle"));
            if (label.size() > 48) {
                size_t end = 48;
                while (end && (static_cast<unsigned char>(label[end]) & 0xc0) == 0x80) --end;
                label.resize(end);
            }
            tracks.push_back({std::move(label), std::move(cues)});
        }
    } catch (...) { /* Optional metadata never blocks playback on failure. */ }
    return tracks;
}
}
