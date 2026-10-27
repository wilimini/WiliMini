#pragma once
#include <cstddef>

namespace pspapp {
struct LicenseEntry { const char* title; const char* file; };
inline constexpr LicenseEntry settingsLicenses[] = {
    {"文泉驿微米黑 · 版权", "wqy.txt"},
    {"文泉驿微米黑 · Apache 2.0", "wqy-apache-2.0.txt"},
    {"Wiliwili · GPL", "wiliwili.txt"},
    {"Wiliwili 素材 · GPL", "wiliwili-assets.txt"},
    {"FFmpeg · LGPL", "ffmpeg.txt"},
    {"curl", "curl.txt"},
    {"二维码生成器 · MIT", "qrcodegen.txt"},
    {"ME bridge · MIST", "me-mist.txt"},
};
inline constexpr size_t settingsLicenseCount = sizeof(settingsLicenses) / sizeof(settingsLicenses[0]);
} // namespace pspapp
