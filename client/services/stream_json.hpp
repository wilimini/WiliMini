#pragma once
#include "bilibili_api.hpp"
#include "community_service.hpp"
#include <cstddef>
#include <cstdint>

namespace pspapp {

// Episodes kept in RAM per sliding window for BrowseKind::Season (multiple of 3).
inline constexpr size_t kSeasonWindowSize = 60;

// Pull-parses /pgc/view/pc/season with nlohmann::json::sax_parse (from RAM or a
// spilled temporary file) without constructing a DOM tree, keeping only episodes
// in [windowOffset, windowOffset + windowSize).
void parseSeasonStream(const HttpResponse &response, uint64_t seasonId,
                       size_t windowOffset, size_t windowSize,
                       BrowsePage &out, StreamApiMeta &meta);

} // namespace pspapp
