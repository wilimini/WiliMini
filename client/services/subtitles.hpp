#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace pspapp {
struct SubtitleCue { int64_t startMs, endMs; std::string text; };
struct SubtitleTrack { std::string label; std::vector<SubtitleCue> cues; };
std::vector<SubtitleCue> parseSubtitles(std::string_view json);
std::vector<SubtitleTrack> loadSubtitles(const std::string& bvid, uint64_t cid, uint64_t aid = 0);
// Latest starting active cue wins if the source contains overlapping intervals.
int subtitleAt(const std::vector<SubtitleCue>& cues, int64_t positionMs);
}
