#pragma once

namespace pspapp {
struct Preferences {
    bool darkTheme = false;
    int bodySize = 14;
    bool showCovers = true;
    bool slowRepeat = false;
};

// Missing files use defaults; malformed files are reported without overwriting them.
bool loadPreferences(const char* path, Preferences& preferences);
bool savePreferences(const char* path, const Preferences& preferences);
} // namespace pspapp
