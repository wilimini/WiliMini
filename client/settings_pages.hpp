#pragma once
#include <cstddef>
#include "localization.hpp"

namespace pspapp {
struct LicenseEntry { TextId title; const char* file; };
inline constexpr LicenseEntry settingsLicenses[] = {
    {TextId::license_wqy_copyright, "wqy.txt"},
    {TextId::license_wqy_apache, "wqy-apache-2.0.txt"},
    {TextId::license_source_han, "source-han-sans-ofl.txt"},
    {TextId::license_wiliwili, "wiliwili.txt"},
    {TextId::license_wiliwili_assets, "wiliwili-assets.txt"},
    {TextId::license_ffmpeg, "ffmpeg.txt"},
    {TextId::license_curl, "curl.txt"},
    {TextId::license_qrcodegen, "qrcodegen.txt"},
    {TextId::license_me_mist, "me-mist.txt"},
    {TextId::license_rime_ice, "rime-ice.txt"},
};
inline constexpr size_t settingsLicenseCount = sizeof(settingsLicenses) / sizeof(settingsLicenses[0]);
} // namespace pspapp
