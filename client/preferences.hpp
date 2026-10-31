#pragma once
#include <cstddef>
#include <string>
#include "memory_budget.hpp"
#include "localization.hpp"

namespace pspapp {
enum class SettingItem : std::size_t {
    DarkTheme,
    BodySize,
    ShowCovers,
    SlowRepeat,
    PlaybackBufferFrames,
    MeBackend,
    ConfirmWithCircle,
    Language,
    LogLevel,
    Wifi,
    CoverCache,
    Help,
    About,
    Count
};

// Values match ME_BACKEND_* in decoder/me/me_bridge.h and client/me_backend_select.h.
// The bootstrap loads the matching bridge PRX, so a change applies on next launch.
enum class MeBackend : int { Mist = 0, CustomCore = 1 };
const char* meBackendCode(MeBackend backend);

// Values match WiliLogLevel in platform/include/wili_log.h. Verbose is compiled out of
// production builds, so it is not offered. Applied immediately, no restart needed.
enum class LogVerbosity : int { Error = 1, Warn = 2, Info = 3, Debug = 4 };
const char* logVerbosityCode(LogVerbosity level);

struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
    int playbackBufferFrames = defaultPlaybackBufferFrames();
    MeBackend meBackend = MeBackend::CustomCore;
    bool confirmWithCircle = false;
    Locale locale = Locale::ZhHans;
    LogVerbosity logLevel = LogVerbosity::Info;
    // Optional per-channel overrides such as "net=debug,player=warn". Edited by hand in
    // settings.json and preserved on save; see wili_log_configure().
    std::string logChannels;

    const char* confirmButton() const {
        return buttonGlyph(confirmWithCircle ? ControllerButton::Circle : ControllerButton::Cross);
    }
    const char* backButton() const {
        return buttonGlyph(confirmWithCircle ? ControllerButton::Cross : ControllerButton::Circle);
    }
};

// Missing files use defaults; malformed files are reported without overwriting them.
bool loadPreferences(const char* path, Preferences& preferences);
bool savePreferences(const char* path, const Preferences& preferences);
} // namespace pspapp
