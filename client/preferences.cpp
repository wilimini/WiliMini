#include "preferences.hpp"
#include <cerrno>
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

namespace pspapp {
bool playbackUsesRgb565(PlaybackPixelFormat format) {
    return format == PlaybackPixelFormat::Rgb565 ||
           (format == PlaybackPixelFormat::Auto && defaultPlaybackRgb565());
}

const char* meBackendCode(MeBackend backend) {
    return backend == MeBackend::CustomCore ? "custom-core" : "mist";
}

const char* homeStyleCode(HomeStyle style) {
    return style == HomeStyle::Metro ? "metro" : style == HomeStyle::Tiles ? "tiles" : "classic";
}

const char* inputMethodCode(InputMethod method) {
    return method == InputMethod::System ? "system" : "pinyin";
}

const char* logVerbosityCode(LogVerbosity level) {
    switch (level) {
        case LogVerbosity::Error: return "error";
        case LogVerbosity::Warn: return "warn";
        case LogVerbosity::Debug: return "debug";
        default: return "info";
    }
}

namespace {
constexpr std::size_t kMaxLogChannels = 96;
// Only what wili_log_configure() understands, so a typo cannot smuggle odd text into logs.
bool validLogChannels(const std::string& spec) {
    if (spec.size() > kMaxLogChannels) return false;
    for (const char c : spec)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '=' || c == ',' || c == ' '))
            return false;
    return true;
}
} // namespace

bool loadPreferences(const char* path, Preferences& preferences) {
    preferences = {};
    FILE* file = std::fopen(path, "rb");
    if (!file && errno == ENOENT) {
        // Recover a PSP replacement interrupted after moving the old file aside.
        std::rename((std::string(path) + ".bak").c_str(), path);
        file = std::fopen(path, "rb");
    }
    if (!file) return errno == ENOENT;
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::rewind(file);
    std::string text(length > 0 && length <= 4096 ? length : 0, '\0');
    const bool read = !text.empty() && std::fread(&text[0], 1, text.size(), file) == text.size();
    std::fclose(file);
    if (!read) return false;
    try {
        const auto root = nlohmann::json::parse(text);
        if (root.at("version").get<int>() != 1) return false;
        Preferences loaded;
        loaded.darkTheme = root.at("dark_theme").get<bool>();
        loaded.bodySize = root.at("body_size").get<int>();
        loaded.showCovers = root.at("show_covers").get<bool>();
        loaded.slowRepeat = root.at("slow_repeat").get<bool>();
        loaded.musicRepeat = root.value("music_repeat", 0);
        if (loaded.musicRepeat < 0 || loaded.musicRepeat > 2) loaded.musicRepeat = 0;
        loaded.playbackBufferFrames = root.value("playback_buffer_frames", defaultPlaybackBufferFrames());
        const std::string pixelFormat = root.value("playback_pixel_format", std::string("auto"));
        if (pixelFormat == "auto") loaded.playbackPixelFormat = PlaybackPixelFormat::Auto;
        else if (pixelFormat == "rgb565") loaded.playbackPixelFormat = PlaybackPixelFormat::Rgb565;
        else if (pixelFormat == "rgba8888") loaded.playbackPixelFormat = PlaybackPixelFormat::Rgba8888;
        else return false;
        const std::string meBackend = root.value("me_backend", std::string("custom-core"));
        if (meBackend == "mist") loaded.meBackend = MeBackend::Mist;
        else if (meBackend == "custom-core") loaded.meBackend = MeBackend::CustomCore;
        else return false;
        if (!root.contains("confirm_button")) {
            loaded.confirmWithCircle = root.value("confirm_with_circle", false);
        } else {
            const std::string button = root.at("confirm_button").get<std::string>();
            if (button == "circle") loaded.confirmWithCircle = true;
            else if (button == "cross") loaded.confirmWithCircle = false;
            else return false;
        }
        const std::string locale = root.value("locale", std::string("zh-Hans"));
        if (locale == "zh-Hans") loaded.locale = Locale::ZhHans;
        else if (locale == "en-US") loaded.locale = Locale::EnUs;
        else return false;
        const std::string logLevel = root.value("log_level", std::string("info"));
        if (logLevel == "error") loaded.logLevel = LogVerbosity::Error;
        else if (logLevel == "warn") loaded.logLevel = LogVerbosity::Warn;
        else if (logLevel == "info") loaded.logLevel = LogVerbosity::Info;
        else if (logLevel == "debug") loaded.logLevel = LogVerbosity::Debug;
        else return false;
        // Unknown or invalid values keep the default instead of rejecting the whole file.
        const auto channel = root.find("update_channel");
        if (channel != root.end() && channel->is_string()) {
            if (channel->get<std::string>() == "codeberg") loaded.updateChannel = 1;
            else if (channel->get<std::string>() == "github") loaded.updateChannel = 0;
        }
        const auto files = root.find("log_files");
        if (files != root.end()) {
            if (!files->is_boolean()) return false;
            loaded.saveLogFiles = files->get<bool>();
        }
        // Files from the first tile build used a boolean; it maps to the Tiles style.
        const auto legacy = root.find("tile_home");
        if (legacy != root.end()) {
            if (!legacy->is_boolean()) return false;
            loaded.homeStyle = legacy->get<bool>() ? HomeStyle::Tiles : HomeStyle::Classic;
        }
        const auto style = root.find("home_style");
        if (style != root.end()) {
            if (!style->is_string()) return false;
            const std::string value = style->get<std::string>();
            if (value == "classic") loaded.homeStyle = HomeStyle::Classic;
            else if (value == "tiles") loaded.homeStyle = HomeStyle::Tiles;
            else if (value == "metro") loaded.homeStyle = HomeStyle::Metro;
            else return false;
        }
        const auto input = root.find("input_method");
        if (input != root.end()) {
            if (!input->is_string()) return false;
            const std::string value = input->get<std::string>();
            // "auto" was written by an earlier build; it now means the built-in keyboard.
            if (value == "auto" || value == "pinyin") loaded.inputMethod = InputMethod::Pinyin;
            else if (value == "system") loaded.inputMethod = InputMethod::System;
            else return false;
        }
        loaded.logChannels = root.value("log_channels", std::string());
        if (!validLogChannels(loaded.logChannels)) return false;
        if (loaded.bodySize != 14 && loaded.bodySize != 16) return false;
        if (loaded.playbackBufferFrames != 2 && loaded.playbackBufferFrames != 4 &&
            loaded.playbackBufferFrames != 8) return false;
        preferences = loaded;
        return true;
    } catch (...) {
        return false;
    }
}

bool savePreferences(const char* path, const Preferences& preferences) {
    if (preferences.bodySize != 14 && preferences.bodySize != 16) return false;
    if (preferences.playbackBufferFrames != 2 && preferences.playbackBufferFrames != 4 &&
        preferences.playbackBufferFrames != 8) return false;
    if (preferences.playbackPixelFormat != PlaybackPixelFormat::Auto &&
        preferences.playbackPixelFormat != PlaybackPixelFormat::Rgb565 &&
        preferences.playbackPixelFormat != PlaybackPixelFormat::Rgba8888) return false;
    if (preferences.locale != Locale::ZhHans && preferences.locale != Locale::EnUs) return false;
    if (!validLogChannels(preferences.logChannels)) return false;
    const std::string text = nlohmann::json({{"version", 1},
        {"dark_theme", preferences.darkTheme}, {"body_size", preferences.bodySize},
        {"show_covers", preferences.showCovers}, {"slow_repeat", preferences.slowRepeat},
        {"music_repeat", preferences.musicRepeat},
        {"playback_buffer_frames", preferences.playbackBufferFrames},
        {"playback_pixel_format", preferences.playbackPixelFormat == PlaybackPixelFormat::Auto ? "auto" :
             preferences.playbackPixelFormat == PlaybackPixelFormat::Rgb565 ? "rgb565" : "rgba8888"},
        {"me_backend", meBackendCode(preferences.meBackend)},
        {"confirm_with_circle", preferences.confirmWithCircle},
        {"confirm_button", preferences.confirmWithCircle ? "circle" : "cross"},
        {"locale", localeCode(preferences.locale)},
        {"log_level", logVerbosityCode(preferences.logLevel)},
        {"log_files", preferences.saveLogFiles},
        {"update_channel", preferences.updateChannel == 1 ? "codeberg" : "github"},
        {"home_style", homeStyleCode(preferences.homeStyle)},
        {"input_method", inputMethodCode(preferences.inputMethod)},
        {"log_channels", preferences.logChannels}}).dump();
    const std::string temporary = std::string(path) + ".part";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fflush(file)) written = false;
    if (std::fclose(file)) written = false;
    if (!written) {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path)) {
        // PSP memory sticks cannot rename over an existing file.
        const std::string backup = std::string(path) + ".bak";
        std::remove(backup.c_str());
        if (std::rename(path, backup.c_str())) {
            std::remove(temporary.c_str());
            return false;
        }
        if (std::rename(temporary.c_str(), path)) {
            std::rename(backup.c_str(), path);
            std::remove(temporary.c_str());
            return false;
        }
        std::remove(backup.c_str());
    }
    return true;
}
} // namespace pspapp
