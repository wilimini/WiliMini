#pragma once

#include <string>

#include "bilibili.h"
#include "services/app_error.hpp"

namespace pspapp {

/* DASH only: a source/API failure is returned to the caller for explicit retry. */
int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr);
int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr);

}  // namespace pspapp
