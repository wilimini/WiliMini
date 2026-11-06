#pragma once
// What each Settings page shows. Pure data and logic (no PSP headers) so the host tests can check it;
// the UI and the input code both ask it instead of special-casing the engines.
#include <cstddef>
#include <vector>
#include "preferences.hpp"

namespace pspapp {

// The rows of a page, in order. The engine page shows only the rows that apply to the engine.
inline std::vector<SettingItem> settingsRows(SettingsPage page, Engine engine) {
    using I = SettingItem;
    switch (page) {
        case SettingsPage::Main:
            return {I::GoEngine, I::GoInterface, I::GoInput, I::Wifi, I::CoverCache, I::GoDiagnostics,
                    I::Help, I::About};
        case SettingsPage::Engine:
            if (engine == Engine::Stream)
                return {I::Engine, I::StreamSetup, I::StreamTest, I::StreamFps, I::StreamPreset,
                        I::StreamProfile, I::PlaybackPixelFormat};
            return {I::Engine, I::MeBackend, I::PlaybackBufferFrames, I::PlaybackPixelFormat};
        case SettingsPage::Interface:
            return {I::DarkTheme, I::HomeStyle, I::BodySize, I::ShowCovers, I::SlowRepeat, I::Language};
        case SettingsPage::Input:
            return {I::ConfirmWithCircle, I::InputMethod};
        case SettingsPage::Diagnostics:
            return {I::LogLevel, I::LogFiles};
        case SettingsPage::ServerSetup:
            return {}; // the setup wizard draws its own steps
    }
    return {};
}

// Left/right changes these in place; the others only act on confirm.
inline bool settingAdjustable(SettingItem item) {
    switch (item) {
        case SettingItem::GoEngine: case SettingItem::GoInterface: case SettingItem::GoInput:
        case SettingItem::GoDiagnostics: case SettingItem::Wifi: case SettingItem::CoverCache:
        case SettingItem::Help: case SettingItem::About: case SettingItem::StreamSetup:
        case SettingItem::StreamTest: case SettingItem::Count:
            return false;
        default:
            return true;
    }
}

// The page a "Go" row opens, or Main when the row opens nothing.
inline SettingsPage settingsPageFor(SettingItem item) {
    switch (item) {
        case SettingItem::GoEngine: return SettingsPage::Engine;
        case SettingItem::GoInterface: return SettingsPage::Interface;
        case SettingItem::GoInput: return SettingsPage::Input;
        case SettingItem::GoDiagnostics: return SettingsPage::Diagnostics;
        default: return SettingsPage::Main;
    }
}

// The step count of the server setup wizard: find, token, test, done.
inline constexpr int kServerWizardSteps = 4;

} // namespace pspapp
