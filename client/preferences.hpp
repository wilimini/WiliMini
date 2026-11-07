#pragma once
#include <cstddef>
#include <string>
#include "memory_budget.hpp"
#include "localization.hpp"

namespace pspapp {
enum class SettingItem : std::size_t {
    DarkTheme,
    HomeStyle,
    BodySize,
    ShowCovers,
    SlowRepeat,
    PlaybackBufferFrames,
    PlaybackPixelFormat,
    MeBackend,
    ConfirmWithCircle,
    Language,
    InputMethod,
    LogLevel,
    LogFiles,
    Wifi,
    CoverCache,
    Help,
    Update,
    About,
    Count
};
inline constexpr std::size_t settingsVisibleRows = 11;

// Values match ME_BACKEND_* in decoder/me/me_bridge.h and client/me_backend_select.h.
// The bootstrap loads the matching bridge PRX, so a change applies on next launch.
enum class MeBackend : int { Mist = 0, CustomCore = 1 };
enum class PlaybackPixelFormat : int { Auto = 0, Rgb565 = 1, Rgba8888 = 2 };
bool playbackUsesRgb565(PlaybackPixelFormat format);
const char* meBackendCode(MeBackend backend);

// Values match WiliLogLevel in platform/include/wili_log.h. Verbose is compiled out of
// production builds, so it is not offered. Applied immediately, no restart needed.
enum class LogVerbosity : int { Error = 1, Warn = 2, Info = 3, Debug = 4 };
const char* logVerbosityCode(LogVerbosity level);

// Home screen style. Tiles and Metro both use the tile home; Tiles shows cover grids on
// the sub-pages, Metro follows the web design with large titles and list rows.
enum class HomeStyle : int { Classic = 0, Tiles = 1, Metro = 2 };
const char* homeStyleCode(HomeStyle style);

// Text entry: the built-in pinyin keyboard (default) or the PSP system keyboard.
enum class InputMethod : int { Pinyin = 0, System = 1 };
const char* inputMethodCode(InputMethod method);

struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
    int playbackBufferFrames = defaultPlaybackBufferFrames();
    PlaybackPixelFormat playbackPixelFormat = PlaybackPixelFormat::Auto;
    MeBackend meBackend = MeBackend::CustomCore;
    bool confirmWithCircle = false;
    Locale locale = Locale::ZhHans;
    LogVerbosity logLevel = LogVerbosity::Info;
    // Off by default: no log text is written to the Memory Stick unless this is on.
    bool saveLogFiles = false;
    // Chosen in Settings; old files default to Classic.
    HomeStyle homeStyle = HomeStyle::Classic;
    InputMethod inputMethod = InputMethod::Pinyin;
    bool tileHome() const { return homeStyle != HomeStyle::Classic; }
    bool metro() const { return homeStyle == HomeStyle::Metro; }
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
