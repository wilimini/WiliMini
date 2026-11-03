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
        case SettingItem::DarkTheme: next.darkTheme = !next.darkTheme; break;
        case SettingItem::HomeStyle:
            next.homeStyle = next.homeStyle == HomeStyle::Classic ? HomeStyle::Tiles
                           : next.homeStyle == HomeStyle::Tiles ? HomeStyle::Metro : HomeStyle::Classic;
            break;
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
        case SettingItem::StreamServer: next.streamServer = !next.streamServer; break;
        case SettingItem::StreamPreset:
            next.streamPreset = next.streamPreset == StreamPreset::Performance ? StreamPreset::Balanced
                              : next.streamPreset == StreamPreset::Balanced ? StreamPreset::Quality
                                                                            : StreamPreset::Performance;
            break;
        case SettingItem::StreamProfile:
            next.streamProfile = next.streamProfile == StreamProfile::Auto ? StreamProfile::Cavlc
                               : next.streamProfile == StreamProfile::Cavlc ? StreamProfile::Cabac
                                                                            : StreamProfile::Auto;
            break;
        // Search / address / token / test only act on Confirm; see activateUtility().
        case SettingItem::StreamFind:
        case SettingItem::StreamAddress:
        case SettingItem::StreamToken:
        case SettingItem::StreamTest:
            return;
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
    preferences_ = next;
    const auto changed = static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection);
    if (changed == SettingItem::HomeStyle) {
        // Settings sits above the root, so the hub shows once this page is popped.
        hubOpen_ = preferences_.tileHome();
        hubSelection_ = 0;
    }
    if (changed == SettingItem::LogLevel || changed == SettingItem::LogFiles) applyLogPreferences(preferences_);
    if (changed == SettingItem::StreamServer) applyServerConfig();
    const auto selected = static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection);
    status_ = selected == SettingItem::PlaybackBufferFrames ? text(TextId::settings_saved_buffer, preferences_.locale)
            : selected == SettingItem::MeBackend && activeMeBackend_ != static_cast<int>(preferences_.meBackend)
                ? text(TextId::settings_saved_me_backend, preferences_.locale)
            : text(TextId::settings_saved, preferences_.locale);
    // The loader decides at start-up whether to load the ME bridge, so this needs a restart.
    if (selected == SettingItem::StreamServer)
        status_ = text(TextId::stream_status_restart, preferences_.locale);
}

bool App::streamTarget(StreamTarget& target, long long startMs) const {
    if (!serverStreaming()) return false;
    target.server = preferences_.serverConfig();
    target.preset = static_cast<WiliStreamPreset>(preferences_.streamPreset);
    target.profile = static_cast<WiliStreamProfile>(preferences_.streamProfile);
    target.startMs = startMs > 0 ? startMs : 0;
    return true;
}

// Called after the server settings change: the account is the PC's in server mode and
// the PSP's own session file otherwise, so reload whichever applies.
void App::applyServerConfig() {
    const WiliServer server = preferences_.serverConfig();
    bili_server_configure(&server);
    sessions_.clearEphemeral();
    clearAccountPages();
    auto restored = authService_.restore();
    if (restored) {
        authView_ = std::move(restored.value);
        sessionCheckPending_ = authView_.state == AuthState::Restored;
    } else {
        authView_ = {};
        sessionCheckPending_ = false;
        authError_ = restored.error;
    }
}

void App::startServerFind() {
    if (busy()) return;
    startJob(Job::ServerFind);
    status_ = text(TextId::stream_searching, preferences_.locale);
}

void App::startServerTest() {
    if (busy()) return;
    jobServer_ = preferences_.serverConfig();
    jobServer_.enabled = 1; // test even while the switch is off
    if (!wili_server_ready(&jobServer_)) {
        status_ = text(TextId::stream_status_incomplete, preferences_.locale);
        return;
    }
    startJob(Job::ServerTest);
    status_ = text(TextId::stream_testing, preferences_.locale);
}

// Worker thread: GET /wili/v1/hello and describe the answer.
void App::runServerTest() {
    const Locale locale = preferences_.locale;
    stagedServerMessage_.clear();
    char url[256];
    if (wili_server_hello_url(&jobServer_, url, sizeof(url)) != 0) {
        stagedServerMessage_ = text(TextId::stream_status_incomplete, locale);
        jobResult_ = 0;
        return;
    }
    BiliHttpResponse response;
    const int rc = bili_http_get_response(url, &response, 8192);
    if (transfer_cancel) {
        bili_http_free(response.text);
        jobResult_ = -1;
        return;
    }
    jobResult_ = 0;
    if (rc < 0) {
        bili_http_free(response.text);
        stagedServerMessage_ = text(response.status == 401 ? TextId::stream_test_token
                                  : response.status == 429 ? TextId::stream_test_rate
                                  : response.status != 0 ? TextId::stream_test_garbled
                                                         : TextId::stream_test_unreachable, locale);
        return;
    }
    try {
        const auto reply = nlohmann::json::parse(response.text ? response.text : "");
        const int proto = reply.at("proto").get<int>();
        if (proto != WILI_PROTO) {
            stagedServerMessage_ = formatText(TextId::stream_test_proto, locale,
                {{"theirs", std::to_string(proto)}, {"ours", std::to_string(WILI_PROTO)}});
        } else {
            const auto& login = reply.at("login");
            const auto& ffmpeg = reply.at("ffmpeg");
            const std::string account = login.at("state").get<std::string>() == "signed_in"
                ? formatText(TextId::stream_test_signed_in, locale, {{"user", login.value("name", std::string("?"))}})
                : std::string(text(TextId::stream_test_signed_out, locale));
            const std::string codec = ffmpeg.at("ok").get<bool>()
                ? std::string(text(TextId::stream_test_ffmpeg_ok, locale))
                : formatText(TextId::stream_test_ffmpeg_bad, locale, {{"problem", ffmpeg.value("problem", std::string("?"))}});
            stagedServerMessage_ = formatText(TextId::stream_test_ok, locale,
                {{"name", reply.at("name").get<std::string>()}, {"account", account}, {"codec", codec}});
        }
    } catch (const std::exception&) {
        stagedServerMessage_ = text(TextId::stream_test_garbled, locale);
    }
    bili_http_free(response.text);
}

void App::commitServerFind() {
    const Locale locale = preferences_.locale;
    if (stagedFound_.empty()) {
        status_ = text(TextId::stream_none_found, locale);
        return;
    }
    // Searching again moves on to the next server found, so several can be tried.
    size_t pick = 0;
    for (size_t i = 0; i < stagedFound_.size(); ++i)
        if (preferences_.serverHost == stagedFound_[i].host && preferences_.serverPort == stagedFound_[i].port)
            pick = (i + 1) % stagedFound_.size();
    const WiliFoundServer& server = stagedFound_[pick];
    Preferences next = preferences_;
    next.serverHost = server.host;
    next.serverPort = server.port;
    if (!savePreferences(CLIENT_PATH("/settings.json"), next)) {
        status_ = text(TextId::settings_save_failed, locale);
        return;
    }
    preferences_ = next;
    applyServerConfig();
    const std::string address = std::string(server.host) + ":" + std::to_string(server.port);
    const std::string name = server.name[0] ? server.name : address;
    status_ = stagedFound_.size() > 1
        ? formatText(TextId::stream_found_more, locale,
            {{"count", std::to_string(stagedFound_.size())}, {"name", name}, {"address", address}})
        : formatText(TextId::stream_found, locale, {{"name", name}, {"address", address}});
}

std::string App::settingsInputTitle() const {
    return text(settingsInput_ == SettingsInput::Token ? TextId::stream_input_token : TextId::stream_input_address,
                preferences_.locale);
}

std::string App::settingsInputInitial() const {
    if (settingsInput_ == SettingsInput::Token) return preferences_.serverToken;
    if (preferences_.serverHost.empty()) return {};
    return preferences_.serverHost + (preferences_.serverPort == WILI_SERVER_PORT
                                          ? std::string() : ":" + std::to_string(preferences_.serverPort));
}

void App::submitSettingsInput(const std::string& raw) {
    const SettingsInput kind = settingsInput_;
    settingsInput_ = SettingsInput::None;
    std::string value = raw;
    const auto first = value.find_first_not_of(' ');
    value = first == std::string::npos ? std::string() : value.substr(first, value.find_last_not_of(' ') - first + 1);
    Preferences next = preferences_;
    if (kind == SettingsInput::Address) {
        if (value.empty()) {
            next.serverHost.clear();
            next.serverPort = WILI_SERVER_PORT;
        } else {
            char host[WILI_SERVER_HOST_MAX];
            int port = 0;
            if (wili_server_parse_address(value.c_str(), host, sizeof(host), &port) != 0) {
                status_ = text(TextId::stream_bad_address, preferences_.locale);
                return;
            }
            next.serverHost = host;
            next.serverPort = port;
        }
    } else if (kind == SettingsInput::Token) {
        if (!value.empty() && !wili_server_valid_token(value.c_str())) {
            status_ = text(TextId::stream_bad_token, preferences_.locale);
            return;
        }
        next.serverToken = value;
    } else return;
    if (!savePreferences(CLIENT_PATH("/settings.json"), next)) {
        status_ = text(TextId::settings_save_failed, preferences_.locale);
        return;
    }
    preferences_ = next;
    applyServerConfig();
    status_ = text(preferences_.streamServer && !preferences_.serverReady() ? TextId::stream_status_incomplete
                                                                           : TextId::settings_saved, preferences_.locale);
}

std::string App::streamErrorMessage(const AppError& error) const {
    const std::string& code = error.detail;
    TextId id = TextId::stream_err_upstream;
    if (code == "unreachable") id = TextId::stream_err_unreachable;
    else if (code == "unauthorized") id = TextId::stream_err_token;
    else if (code == "rate_limited") id = TextId::stream_test_rate;
    else if (code == "login_required") id = TextId::stream_err_login;
    else if (code == "no_stream") id = TextId::stream_err_no_stream;
    else if (code == "busy") id = TextId::stream_err_busy;
    else if (code == "ffmpeg_missing") id = TextId::stream_err_ffmpeg;
    else if (code == "bad_request") id = TextId::stream_err_bad_request;
    return text(id, preferences_.locale);
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
        if (navigation_.get<SettingsRoute>().selection < static_cast<size_t>(SettingItem::Wifi)) {
            switch (static_cast<SettingItem>(navigation_.get<SettingsRoute>().selection)) {
                case SettingItem::StreamFind: startServerFind(); break;
                case SettingItem::StreamTest: startServerTest(); break;
                case SettingItem::StreamAddress: settingsInput_ = SettingsInput::Address; break;
                case SettingItem::StreamToken: settingsInput_ = SettingsInput::Token; break;
                default: changeSetting(); break;
            }
        } else if (navigation_.get<SettingsRoute>().selection == static_cast<size_t>(SettingItem::CoverCache)) {
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
