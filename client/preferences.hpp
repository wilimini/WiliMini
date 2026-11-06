#pragma once
#include <cstddef>
#include <cstdio>
#include <string>
#include "memory_budget.hpp"
#include "localization.hpp"
#include "stream_server.h"

namespace pspapp {
// Settings is a short list of pages, not one long list. Every row has one id; which ids a page shows
// (and, on the engine page, which engine's rows) is decided in settings_layout.hpp.
enum class SettingsPage : int { Main, Engine, Interface, Input, Diagnostics, ServerSetup };

// How WiliMini plays video. On-device decodes on this PSP's Media Engine and talks to Bilibili itself;
// PC stream asks wili-server on a PC to fetch and transcode, and decodes with the firmware's sceMpeg.
enum class Engine : int { Device, Stream };

enum class SettingItem : std::size_t {
    // Main page: entries that open a page or screen.
    GoEngine,
    GoInterface,
    GoInput,
    Wifi,
    CoverCache,
    GoDiagnostics,
    Help,
    About,
    // Playback engine page. Which of these show depends on the engine.
    Engine,
    MeBackend,
    PlaybackBufferFrames,
    PlaybackPixelFormat,
    StreamSetup,
    StreamTest,
    StreamFps,
    StreamPreset,
    StreamProfile,
    // Interface page.
    DarkTheme,
    HomeStyle,
    BodySize,
    ShowCovers,
    SlowRepeat,
    Language,
    // Input page.
    ConfirmWithCircle,
    InputMethod,
    // Diagnostics page.
    LogLevel,
    LogFiles,
    Count
};
inline constexpr std::size_t settingsVisibleRows = 11;

inline Engine engineOf(bool streamServer) { return streamServer ? Engine::Stream : Engine::Device; }

// Values match ME_BACKEND_* in decoder/me/me_bridge.h and client/me_backend_select.h.
// The bootstrap loads the matching bridge PRX, so a change applies on next launch.
// SceMpeg is not an ME bridge at all: the firmware's sceMpeg owns the Media Engine, so the loader
// starts no bridge and the ME decoder is unavailable until the setting is changed back and
// WiliMini is restarted. The stream server needs it.
enum class MeBackend : int { Mist = 0, CustomCore = 1, SceMpeg = 2 };
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

// Stream server (wili-server on a PC). The picture is always 480x272; what varies is which
// Bilibili tier the PC downloads (streamQuality, a WILI_QN_* id), the frame-rate cap and the
// bitrate (StreamPreset, kept under the settings key "stream_preset"). Values match
// WiliStreamPreset / WiliStreamProfile.
enum class StreamPreset : int { Performance = 0, Balanced = 1, Quality = 2 };
enum class StreamProfile : int { Auto = 0, Cavlc = 1, Cabac = 2 };
// Auto follows the source (up to 60 fps); Max30 caps it, which halves the 60 fps tiers.
enum class StreamFps : int { Auto = 0, Max30 = 30 };
const char* streamPresetCode(StreamPreset preset);
const char* streamProfileCode(StreamProfile profile);
const char* streamFpsCode(StreamFps fps);

struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
    int playbackBufferFrames = defaultPlaybackBufferFrames();
    PlaybackPixelFormat playbackPixelFormat = PlaybackPixelFormat::Auto;
    MeBackend meBackend = MeBackend::CustomCore;
    // What turning the stream server off goes back to (never SceMpeg).
    MeBackend meBackendBeforeStream = MeBackend::CustomCore;
    bool confirmWithCircle = false;
    Locale locale = Locale::ZhHans;
    LogVerbosity logLevel = LogVerbosity::Info;
    // Off by default: no log text is written to the Memory Stick unless this is on.
    bool saveLogFiles = false;
    // Chosen in Settings; old files default to Classic.
    HomeStyle homeStyle = HomeStyle::Classic;
    InputMethod inputMethod = InputMethod::Pinyin;
    // Off by default: the PSP talks to Bilibili directly.
    bool streamServer = false;
    std::string serverHost;
    int serverPort = WILI_SERVER_PORT;
    std::string serverToken; // stored in plain text on the memory stick, like the Wi-Fi profile
    // The tier last picked in the player's quality menu: the first one the next stream asks for. It is
    // not a settings row, because which tiers exist differs from video to video.
    int streamQuality = WILI_QN_DEFAULT;
    StreamFps streamFps = StreamFps::Auto;
    StreamPreset streamPreset = StreamPreset::Balanced;
    StreamProfile streamProfile = StreamProfile::Auto;
    WiliServer serverConfig() const {
        WiliServer server{};
        server.enabled = streamServer ? 1 : 0;
        std::snprintf(server.host, sizeof(server.host), "%s", serverHost.c_str());
        server.port = serverPort;
        std::snprintf(server.token, sizeof(server.token), "%s", serverToken.c_str());
        return server;
    }
    bool serverReady() const { WiliServer s = serverConfig(); return wili_server_ready(&s) != 0; }
    // Derived, not stored: the engine is "PC stream" exactly when the stream server is switched on.
    Engine engine() const { return engineOf(streamServer); }
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
