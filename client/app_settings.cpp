#include "app.hpp"
#include "build_info.hpp"
#include "settings_pages.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <pspiofilemgr.h>
#include <wili_log.h>

namespace pspapp {
namespace {
std::string readSettingsText(const char* path, size_t limit) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::rewind(file);
    std::string text(length > 0 && static_cast<size_t>(length) <= limit ? length : 0, '\0');
    if (!text.empty() && std::fread(&text[0], 1, text.size(), file) != text.size()) text.clear();
    std::fclose(file);
    // Copyright files sometimes contain DOS newlines.
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}
bool coverEntry(const SceIoDirent& entry) {
    if (!FIO_S_ISREG(entry.d_stat.st_mode)) return false;
    const std::string name = entry.d_name;
    if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".jpg")) return false;
    for (size_t i = 0; i < name.size() - 4; ++i)
        if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_' && name[i] != '-') return false;
    return true;
}
} // namespace

void applyLogPreferences(const Preferences& preferences) {
    // Set the base level first so a channel override is not reset by it.
    wili_log_set_files(preferences.saveLogFiles);
    if (preferences.saveLogFiles) {
        // Whole-session log, kept as wili.log and wili.log.1 (about 256 KiB each at most).
        sceIoMkdir(CLIENT_PATH("/logs"), 0777);
        wili_log_sink_start(WILI_LOG_SINK_SESSION, CLIENT_PATH("/logs/wili.log"), 0, 256 * 1024);
    } else {
        wili_log_sink_stop(WILI_LOG_SINK_SESSION);
        wili_log_sink_stop(WILI_LOG_SINK_RUN);
    }
    wili_log_set_all(static_cast<WiliLogLevel>(preferences.logLevel));
    if (!preferences.logChannels.empty()) wili_log_configure(preferences.logChannels.c_str());
    WLOG_I(WLC_CLIENT, "log_level=%s channels=%s files=%s", logVerbosityCode(preferences.logLevel),
           preferences.logChannels.empty() ? "-" : preferences.logChannels.c_str(),
           preferences.saveLogFiles ? "on" : "off");
}

void App::openSettings() {
    if (busy()) return;
    navigation_.push<SettingsRoute>();
    navigation_.current().focus = FocusArea::Content;
    status_ = formatText(TextId::settings_status_open, preferences_.locale,
                         {{"confirm", preferences_.confirmButton()}});
}

void App::changeSetting() {
    if (screen() != Screen::Settings || navigation_.get<SettingsRoute>().selection >=
            static_cast<size_t>(SettingItem::Wifi)) return;
    if (defaultPlaybackBufferFrames() == 2 &&
        navigation_.get<SettingsRoute>().selection == static_cast<size_t>(SettingItem::PlaybackBufferFrames)) return;
    Preferences next = preferences_;
    switch (static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection)) {
        case SettingItem::Theme: {
            // Cycle through the built-in themes, then the folders under themes/.
            const auto choices = themeChoices();
            const std::string current = preferences_.theme.empty()
                ? std::string(builtinThemeId(preferences_.homeStyle, preferences_.darkTheme)) : preferences_.theme;
            size_t at = 0;
            for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == current) at = i + 1;
            next.theme = choices[at % choices.size()];
            break;
        }
        case SettingItem::BodySize: next.bodySize = next.bodySize == 14 ? 16 : 14; break;
        case SettingItem::ShowCovers: next.showCovers = !next.showCovers; break;
        case SettingItem::SlowRepeat: next.slowRepeat = !next.slowRepeat; break;
        case SettingItem::PlaybackBufferFrames: next.playbackBufferFrames = next.playbackBufferFrames == 2 ? 4 :
                    next.playbackBufferFrames == 4 ? 8 : 2; break;
        case SettingItem::PlaybackPixelFormat:
            next.playbackPixelFormat = next.playbackPixelFormat == PlaybackPixelFormat::Auto
                ? PlaybackPixelFormat::Rgb565
                : next.playbackPixelFormat == PlaybackPixelFormat::Rgb565
                    ? PlaybackPixelFormat::Rgba8888 : PlaybackPixelFormat::Auto;
            break;
        case SettingItem::MeBackend:
            next.meBackend = next.meBackend == MeBackend::Mist ? MeBackend::CustomCore : MeBackend::Mist;
            break;
        case SettingItem::InputMethod:
            next.inputMethod = next.inputMethod == InputMethod::Pinyin ? InputMethod::System : InputMethod::Pinyin;
            break;
        case SettingItem::LogLevel:
            next.logLevel = next.logLevel == LogVerbosity::Error ? LogVerbosity::Warn
                          : next.logLevel == LogVerbosity::Warn ? LogVerbosity::Info
                          : next.logLevel == LogVerbosity::Info ? LogVerbosity::Debug : LogVerbosity::Error;
            break;
        case SettingItem::LogFiles: next.saveLogFiles = !next.saveLogFiles; break;
        case SettingItem::ConfirmWithCircle: next.confirmWithCircle = !next.confirmWithCircle; break;
        case SettingItem::Language:
            next.locale = next.locale == Locale::ZhHans ? Locale::EnUs : Locale::ZhHans;
            break;
        default: return;
    }
    if (!savePreferences(CLIENT_PATH("/settings.json"), next)) {
        status_ = text(TextId::settings_save_failed, preferences_.locale);
        return;
    }
    const Preferences before = preferences_;
    preferences_ = next;
    const auto changed = static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection);
    if (changed == SettingItem::Theme) {
        loadTheme();
        HomeStyle builtinStyle;
        bool builtinDark;
        if (!theme_.loaded() && !parseBuiltinTheme(preferences_.theme, builtinStyle, builtinDark)) {
            // The script did not load: keep the previous theme and say why.
            status_ = std::string(text(TextId::settings_save_failed, preferences_.locale)) + " · " + theme_.error();
            preferences_ = before;
            loadTheme();
            return;
        }
        savePreferences(CLIENT_PATH("/settings.json"), preferences_);   // now with the resolved style
        // Settings sits above the root, so the hub shows once this page is popped.
        hubOpen_ = preferences_.tileHome();
        hubSelection_ = 0;
    }
    if (changed == SettingItem::LogLevel || changed == SettingItem::LogFiles) applyLogPreferences(preferences_);
    const auto selected = static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection);
    status_ = selected == SettingItem::PlaybackBufferFrames ? text(TextId::settings_saved_buffer, preferences_.locale)
            : selected == SettingItem::MeBackend && activeMeBackend_ != static_cast<int>(preferences_.meBackend)
                ? text(TextId::settings_saved_me_backend, preferences_.locale)
            : text(TextId::settings_saved, preferences_.locale);
}

void App::updateStorage() {
    coverFiles_ = 0;
    coverBytes_ = 0;
    const SceUID directory = sceIoDopen(CLIENT_PATH("/cache"));
    if (directory < 0) {
        status_ = "无法读取封面缓存";
        return;
    }
    SceIoDirent entry{};
    int result;
    while ((result = sceIoDread(directory, &entry)) > 0) {
        if (coverEntry(entry)) { ++coverFiles_; coverBytes_ += entry.d_stat.st_size; }
        std::memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(directory);
    if (result < 0) status_ = "缓存目录读取未完成";
}

std::string App::buildVersion() const {
    return pspapp::buildVersion;
}

// Called by App::activate for the utility pages, without changing list selection.
void App::activateUtility() {
    if (screen() == Screen::Settings) {
        if (navigation_.get<SettingsRoute>().selection < static_cast<size_t>(SettingItem::Wifi)) changeSetting();
        else if (navigation_.get<SettingsRoute>().selection == static_cast<size_t>(SettingItem::CoverCache)) {
            navigation_.push<StorageRoute>();
            updateStorage();
        } else if (navigation_.get<SettingsRoute>().selection == static_cast<size_t>(SettingItem::Help)) navigation_.push<HelpRoute>();
        else if (navigation_.get<SettingsRoute>().selection == static_cast<size_t>(SettingItem::About)) navigation_.push<AboutRoute>();
    } else if (screen() == Screen::About) {
        navigation_.push<LicensesRoute>();
    } else if (screen() == Screen::Licenses) {
        const std::string path = std::string(CLIENT_ROOT) + "/licenses/" + settingsLicenses[navigation_.get<LicensesRoute>().selection].file;
        auto text = readSettingsText(path.c_str(), 256 * 1024);
        if (text.empty()) {
            status_ = "许可文件缺失或无法读取";
            return;
        }
        LicenseTextRoute page;
        page.text = std::move(text);
        navigation_.push(std::move(page));
    } else if (screen() == Screen::Storage) {
        if (!navigation_.get<StorageRoute>().clearing) {
            updateStorage();
            if (coverFiles_) navigation_.get<StorageRoute>().clearing = true;
            else status_ = "没有可清理的封面缓存";
            return;
        }
        navigation_.get<StorageRoute>().clearing = false;
        const SceUID directory = sceIoDopen(CLIENT_PATH("/cache"));
        if (directory < 0) { status_ = "无法读取封面缓存"; return; }
        bool failed = false;
        SceIoDirent entry{};
        int result;
        while ((result = sceIoDread(directory, &entry)) > 0) {
            if (coverEntry(entry)) {
                const std::string path = std::string(CLIENT_ROOT) + "/cache/" + entry.d_name;
                if (sceIoRemove(path.c_str()) < 0) failed = true;
            }
            std::memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(directory);
        ++coverGeneration_;
        completedCoverKey_.clear();
        coverDirty_ = true;
        updateStorage();
        status_ = failed || result < 0 ? "部分缓存未能清理 · 可重试" : "封面缓存已清理";
    }
}
} // namespace pspapp
