#pragma once

#include <string>

#include "bilibili.h"
#include "services/app_error.hpp"

namespace pspapp {

/* DASH only: a source/API failure is returned to the caller for explicit retry.
 * `lossless` asks for the FLAC audio track; when the response has none (the video
 * or the account does not offer it) AAC is used and playback.lossless stays 0. */
int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr, bool lossless = false);
/* Audio track only, for music mode. `video.cid` selects the part. */
int prepareAudioPlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, AppError* typedError = nullptr, bool lossless = false);
int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr, bool lossless = false);

}  // namespace pspapp
