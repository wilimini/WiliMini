#pragma once

#include <string>

#include "bilibili.h"

namespace pspapp {

/* Requests DASH first and falls back to the proven progressive MP4 path when
 * the response has no H.264/AAC pair or the range source cannot start. */
int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality = 16);
int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality = 16);

}  // namespace pspapp
