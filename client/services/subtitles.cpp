#include "subtitles.hpp"
#include "../api/wiliwili/bilibili/util/json.hpp"
#include <algorithm>
#include <cmath>
namespace pspapp {
std::vector<SubtitleCue> parseSubtitles(std::string_view input) {
    std::vector<SubtitleCue> cues;
    if (input.size() > 512 * 1024) return cues;
    try {
        auto root = nlohmann::json::parse(input);
        if (!root.contains("body") || !root["body"].is_array()) return cues;
        size_t bytes = 0;
        for (const auto& item : root["body"]) {
            if (!item.is_object() || !item.contains("from") || !item["from"].is_number() ||
                !item.contains("to") || !item["to"].is_number() ||
                !item.contains("content") || !item["content"].is_string()) continue;
            double from = item["from"].get<double>(), to = item["to"].get<double>();
            if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to <= from || to > 86400) continue;
            auto text = item["content"].get<std::string>();
            if (text.empty() || text.size() > 1024) continue;
            // Bound the whole track; never silently play a truncated timeline.
            bytes += text.size();
            if (bytes > 128 * 1024 || cues.size() == 2048) return {};
            cues.push_back({static_cast<int64_t>(from * 1000), static_cast<int64_t>(to * 1000), std::move(text)});
        }
        std::stable_sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    } catch (...) { return {}; }
    return cues;
}
int subtitleAt(const std::vector<SubtitleCue>& cues, int64_t ms) {
    auto end = std::upper_bound(cues.begin(), cues.end(), ms, [](int64_t t, const auto& c) { return t < c.startMs; });
    while (end != cues.begin()) {
        --end;
        if (ms < end->endMs) return static_cast<int>(end - cues.begin());
    }
    return -1;
}
}
