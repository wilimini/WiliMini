// Settings pages and which rows each engine shows.
#include <cassert>
#include <cstdio>
#include <set>
#include "settings_layout.hpp"

using namespace pspapp;

static bool has(const std::vector<SettingItem>& rows, SettingItem item) {
    for (SettingItem row : rows) if (row == item) return true;
    return false;
}

int main() {
    // Every setting is reachable from some page, and none shows twice on one page.
    std::set<std::size_t> reachable;
    for (SettingsPage page : {SettingsPage::Main, SettingsPage::Engine, SettingsPage::Interface,
                              SettingsPage::Input, SettingsPage::Diagnostics}) {
        for (Engine engine : {Engine::Device, Engine::Stream}) {
            const auto rows = settingsRows(page, engine);
            assert(!rows.empty());
            std::set<std::size_t> seen;
            for (SettingItem row : rows) {
                assert(row != SettingItem::Count);
                assert(seen.insert(static_cast<std::size_t>(row)).second);
                reachable.insert(static_cast<std::size_t>(row));
            }
        }
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(SettingItem::Count); ++i) assert(reachable.count(i));
    assert(settingsRows(SettingsPage::ServerSetup, Engine::Stream).empty());

    // The top level is short and only opens things; every page it opens has rows.
    const auto main = settingsRows(SettingsPage::Main, Engine::Device);
    assert(main.size() <= settingsVisibleRows);
    for (SettingItem row : main) {
        assert(!settingAdjustable(row));
        const SettingsPage target = settingsPageFor(row);
        assert(target == SettingsPage::Main || !settingsRows(target, Engine::Device).empty());
    }
    assert(settingsPageFor(SettingItem::GoEngine) == SettingsPage::Engine);
    assert(settingsPageFor(SettingItem::Wifi) == SettingsPage::Main);

    // The engine page leads with the engine switch and then shows only that engine's rows.
    const auto device = settingsRows(SettingsPage::Engine, Engine::Device);
    const auto stream = settingsRows(SettingsPage::Engine, Engine::Stream);
    assert(device.front() == SettingItem::Engine && stream.front() == SettingItem::Engine);
    assert(has(device, SettingItem::MeBackend) && has(device, SettingItem::PlaybackBufferFrames));
    assert(!has(device, SettingItem::StreamSetup) && !has(device, SettingItem::StreamPreset) &&
           !has(device, SettingItem::StreamFps) && !has(device, SettingItem::StreamProfile) &&
           !has(device, SettingItem::StreamTest));
    assert(has(stream, SettingItem::StreamSetup) && has(stream, SettingItem::StreamTest) &&
           has(stream, SettingItem::StreamFps) && has(stream, SettingItem::StreamPreset) &&
           has(stream, SettingItem::StreamProfile));
    // Nothing that only the ME decoder reads is offered while streaming.
    assert(!has(stream, SettingItem::MeBackend) && !has(stream, SettingItem::PlaybackBufferFrames));
    assert(has(device, SettingItem::PlaybackPixelFormat) && has(stream, SettingItem::PlaybackPixelFormat));

    // Rows that act rather than cycle are not adjusted by left/right.
    assert(!settingAdjustable(SettingItem::StreamSetup) && !settingAdjustable(SettingItem::StreamTest));
    assert(settingAdjustable(SettingItem::Engine) && settingAdjustable(SettingItem::StreamPreset) &&
           settingAdjustable(SettingItem::DarkTheme));

    // The engine follows the stream switch, so old settings files keep working.
    Preferences preferences;
    assert(preferences.engine() == Engine::Device);
    preferences.streamServer = true;
    assert(preferences.engine() == Engine::Stream);

    std::puts("settings layout: passed");
    return 0;
}
