#pragma once

#include <string>

#include "bilibili.h"
#include "services/app_error.hpp"

namespace pspapp {

/* Set when wili-server is in use: the video is fetched and transcoded on the PC and
 * arrives as one FLV stream starting at startMs. */
struct StreamTarget {
    WiliServer server{};
    WiliStreamPreset preset = WILI_PRESET_BALANCED;
    int sourceQn = WILI_QN_DEFAULT;   // Bilibili tier the PC downloads
    int maxFps = WILI_FPS_AUTO;       // frame-rate cap; the picture size is fixed at 480x272
    WiliStreamProfile profile = WILI_PROFILE_AUTO;
    long long startMs = 0;
};

/* DASH only: a source/API failure is returned to the caller for explicit retry. */
int prepareEpisodePlayback(const BiliVideo& video, uint64_t episode, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr, const StreamTarget* stream = nullptr);
int preparePlayback(const BiliVideo& video, BiliPlayback& playback, std::string& error, int quality = 16, AppError* typedError = nullptr, const StreamTarget* stream = nullptr);

/* "unreachable", "unauthorized", "login_required", ... for an HTTP status and the
 * server's JSON error code (either may be empty/zero). */
const char* streamErrorCode(int httpStatus, const char* serverCode);

}  // namespace pspapp
