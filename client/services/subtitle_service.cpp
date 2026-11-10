#include "subtitles.hpp"
#include "bilibili_api.hpp"
#include "../bilibili.h"
#include "../api/wiliwili/bilibili/api.h"

namespace pspapp {
namespace {
std::string stringField(const nlohmann::json& object, const char* key) {
    const auto field = object.find(key);
    return field != object.end() && field->is_string() ? field->get<std::string>() : std::string{};
}
}

Result<std::vector<SubtitleTrack>> subtitleTracks(const std::string& bvid, uint64_t cid, uint64_t aid, HttpClient& http) {
    using Tracks = Result<std::vector<SubtitleTrack>>;
    constexpr auto op = ErrorOperation::Request;
    const AppError invalid{ErrorDomain::Api, ErrorKind::InvalidResponse, op};
    if (!cid || (bvid.empty() && !aid)) return {};
    ApiParams params{{"cid", std::to_string(cid)}};
    params.emplace_back(aid ? "aid" : "bvid", aid ? std::to_string(aid) : bvid);
    // Like Bilibili Evolved, use the session on the player endpoint directly.
    auto result = BilibiliApi{http}.get(bilibili::Api::PageDetail, params, false, op, false);
    if (transfer_cancel) return Tracks::failure({ErrorDomain::Network, ErrorKind::Cancelled, op});
    if (!result) return Tracks::failure(std::move(result.error));
    try {
        const auto& data = result.value.data;
        if (data.at("cid") != cid || (aid && data.at("aid") != aid) ||
            (!bvid.empty() && data.at("bvid") != bvid)) return Tracks::failure(invalid);
        const auto subtitle = data.find("subtitle");
        if (subtitle != data.end() && !subtitle->is_object() && !subtitle->is_null())
            return Tracks::failure(invalid);
        const auto* list = subtitle != data.end() && subtitle->contains("subtitles")
                               ? &subtitle->at("subtitles") : nullptr;
        std::vector<SubtitleTrack> tracks;
        if (list && !list->is_null()) {
            if (!list->is_array()) return Tracks::failure(invalid);
            for (const auto& entry : *list) {
                if (transfer_cancel) return Tracks::failure({ErrorDomain::Network, ErrorKind::Cancelled, op});
                std::string url = stringField(entry, "subtitle_url");
                if (url.rfind("//", 0) == 0) url = "https:" + url;
                if (url.rfind("https://", 0) != 0 || url.size() == 8) continue;
                std::string label = stringField(entry, "lan_doc");
                if (label.empty()) label = stringField(entry, "lan");
                if (label.empty()) label = "Subtitle";
                if (label.size() > 48) {
                    size_t end = 48;
                    while (end && (static_cast<unsigned char>(label[end]) & 0xc0) == 0x80) --end;
                    label.resize(end);
                }
                tracks.push_back({std::move(label), {}, std::move(url)});
                if (tracks.size() == 3) break;
            }
        }
        if (tracks.empty() && data.value("need_login_subtitle", false))
            return Tracks::failure({ErrorDomain::Auth, ErrorKind::LoginRequired, op});
        return {std::move(tracks), {}};
    } catch (...) {
        return Tracks::failure(invalid);
    }
}
}
