#pragma once
#include <cstddef>
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

struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
    int playbackBufferFrames = defaultPlaybackBufferFrames();
    MeBackend meBackend = MeBackend::CustomCore;
    bool confirmWithCircle = false;
    Locale locale = Locale::ZhHans;

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
