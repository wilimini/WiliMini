#include "app.hpp"
#include "settings_pages.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <pspiofilemgr.h>

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

void App::openSettings() {
    if (busy_) return;
    settingsReturnScreen_ = screen_;
    screen_ = Screen::Settings;
    focusArea_ = FocusArea::Content;
    clearingCovers_ = false;
    status_ = "设置 · ←→ 调整 · × 打开";
}

void App::changeSetting() {
    if (screen_ != Screen::Settings || settingSelection_ > 3) return;
    Preferences next = preferences_;
    switch (settingSelection_) {
        case 0: next.darkTheme = !next.darkTheme; break;
        case 1: next.bodySize = next.bodySize == 14 ? 16 : 14; break;
        case 2: next.showCovers = !next.showCovers; break;
        case 3: next.slowRepeat = !next.slowRepeat; break;
    }
    if (!savePreferences(CLIENT_ROOT "/settings.json", next)) {
        status_ = "设置保存失败 · 请检查记忆棒空间";
        return;
    }
    preferences_ = next;
    status_ = "已保存 · 立即生效";
}

void App::updateStorage() {
    coverFiles_ = 0;
    coverBytes_ = 0;
    const SceUID directory = sceIoDopen(CLIENT_ROOT "/cache");
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
    const auto text = readSettingsText(CLIENT_ROOT "/build.json", 1024 * 1024);
    try {
        return nlohmann::json::parse(text).value("build", "本地构建");
    } catch (...) { return "本地构建 · 无版本记录"; }
}

// Called by App::activate for the utility pages, without changing list selection.
void App::activateUtility() {
    if (screen_ == Screen::Settings) {
        if (settingSelection_ < 4) changeSetting();
        else if (settingSelection_ == 5) {
            screen_ = Screen::Storage;
            updateStorage();
        } else if (settingSelection_ == 6) screen_ = Screen::Help;
        else if (settingSelection_ == 7) screen_ = Screen::About;
    } else if (screen_ == Screen::About) {
        screen_ = Screen::Licenses;
    } else if (screen_ == Screen::Licenses) {
        const std::string path = std::string(CLIENT_ROOT) + "/licenses/" + settingsLicenses[licenseSelection_].file;
        licenseText_ = readSettingsText(path.c_str(), 256 * 1024);
        if (licenseText_.empty()) {
            status_ = "许可文件缺失或无法读取";
            return;
        }
        documentScroll_ = 0;
        documentScrollLimit_ = 0;
        screen_ = Screen::LicenseText;
    } else if (screen_ == Screen::Storage) {
        if (!clearingCovers_) {
            updateStorage();
            if (coverFiles_) clearingCovers_ = true;
            else status_ = "没有可清理的封面缓存";
            return;
        }
        clearingCovers_ = false;
        const SceUID directory = sceIoDopen(CLIENT_ROOT "/cache");
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
