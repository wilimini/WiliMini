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
    ConfirmWithCircle,
    Language,
    Wifi,
    CoverCache,
    Help,
    About,
    Count
};

struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
    int playbackBufferFrames = defaultPlaybackBufferFrames();
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
