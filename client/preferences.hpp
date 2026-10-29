#pragma once
#include <cstddef>
#include "memory_budget.hpp"

namespace pspapp {
enum class SettingItem : std::size_t {
    DarkTheme,
    BodySize,
    ShowCovers,
    SlowRepeat,
    PlaybackBufferFrames,
    ConfirmWithCircle,
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

    const char* confirmButton() const { return confirmWithCircle ? "○" : "×"; }
    const char* backButton() const { return confirmWithCircle ? "×" : "○"; }
};

// Missing files use defaults; malformed files are reported without overwriting them.
bool loadPreferences(const char* path, Preferences& preferences);
bool savePreferences(const char* path, const Preferences& preferences);
} // namespace pspapp
