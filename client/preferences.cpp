#include "preferences.hpp"
#include <cerrno>
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

namespace pspapp {
const char* meBackendCode(MeBackend backend) {
    return backend == MeBackend::CustomCore ? "custom-core" : "mist";
}

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
        loaded.playbackBufferFrames = root.value("playback_buffer_frames", defaultPlaybackBufferFrames());
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
    if (preferences.locale != Locale::ZhHans && preferences.locale != Locale::EnUs) return false;
    const std::string text = nlohmann::json({{"version", 1},
        {"dark_theme", preferences.darkTheme}, {"body_size", preferences.bodySize},
        {"show_covers", preferences.showCovers}, {"slow_repeat", preferences.slowRepeat},
        {"playback_buffer_frames", preferences.playbackBufferFrames},
        {"me_backend", meBackendCode(preferences.meBackend)},
        {"confirm_with_circle", preferences.confirmWithCircle},
        {"confirm_button", preferences.confirmWithCircle ? "circle" : "cross"},
        {"locale", localeCode(preferences.locale)}}).dump();
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
