#include "preferences.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>

int main(int argc, char** argv) {
    assert(argc == 3);
    const char* path = argv[1];
    const int defaultFrames = std::atoi(argv[2]);
    assert(defaultFrames == 2 || defaultFrames == 8);
    pspapp::Preferences value;
    // Existing version-1 settings must still load after adding playback buffers.
    std::ofstream(path) << R"({"version":1,"dark_theme":true,"body_size":16,"show_covers":false,"slow_repeat":true})";
    assert(pspapp::loadPreferences(path, value));
    assert(value.playbackBufferFrames == defaultFrames && value.darkTheme && value.bodySize == 16);
    assert(value.playbackPixelFormat == pspapp::PlaybackPixelFormat::Auto);
    assert(pspapp::playbackUsesRgb565(value.playbackPixelFormat) == (defaultFrames == 2));
    // Files saved before the ME backend setting existed use the Custom Core default.
    assert(value.meBackend == pspapp::MeBackend::CustomCore);
    for (auto backend : {pspapp::MeBackend::Mist, pspapp::MeBackend::CustomCore}) {
        value.meBackend = backend;
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences restored;
        assert(pspapp::loadPreferences(path, restored) && restored.meBackend == backend);
    }
    for (int frames : {2, 4, 8}) {
        value.playbackBufferFrames = frames;
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences restored;
        assert(pspapp::loadPreferences(path, restored));
        assert(restored.playbackBufferFrames == frames && restored.darkTheme);
    }
    for (auto format : {pspapp::PlaybackPixelFormat::Auto,
                        pspapp::PlaybackPixelFormat::Rgb565,
                        pspapp::PlaybackPixelFormat::Rgba8888}) {
        value.playbackPixelFormat = format;
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences restored;
        assert(pspapp::loadPreferences(path, restored));
        assert(restored.playbackPixelFormat == format);
    }
    value.playbackPixelFormat = static_cast<pspapp::PlaybackPixelFormat>(3);
    assert(!pspapp::savePreferences(path, value));
    value.playbackBufferFrames = 3;
    assert(!pspapp::savePreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"playback_buffer_frames":3})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"playback_buffer_frames":"8"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"me_backend":"unknown"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"playback_pixel_format":"unknown"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"me_backend":1})";
    assert(!pspapp::loadPreferences(path, value));
    // Files saved before the log settings existed keep the normal level and no overrides.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false})";
    assert(pspapp::loadPreferences(path, value));
    assert(value.logLevel == pspapp::LogVerbosity::Info && value.logChannels.empty());
    for (auto level : {pspapp::LogVerbosity::Error, pspapp::LogVerbosity::Warn,
                       pspapp::LogVerbosity::Info, pspapp::LogVerbosity::Debug}) {
        value.logLevel = level;
        value.logChannels = "net=debug, player=warn";
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences restored;
        assert(pspapp::loadPreferences(path, restored));
        assert(restored.logLevel == level && restored.logChannels == "net=debug, player=warn");
    }
    // Log files are off by default, round-trip when on, and must be a real boolean.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false})";
    assert(pspapp::loadPreferences(path, value) && !value.saveLogFiles);
    value.saveLogFiles = true;
    assert(pspapp::savePreferences(path, value));
    pspapp::Preferences filesRestored;
    assert(pspapp::loadPreferences(path, filesRestored) && filesRestored.saveLogFiles);
    value.saveLogFiles = false;
    assert(pspapp::savePreferences(path, value));
    assert(pspapp::loadPreferences(path, filesRestored) && !filesRestored.saveLogFiles);
    // The home style is Classic for files saved before it existed, round-trips, and is validated.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false})";
    assert(pspapp::loadPreferences(path, value) && value.homeStyle == pspapp::HomeStyle::Classic && !value.tileHome());
    for (auto style : {pspapp::HomeStyle::Tiles, pspapp::HomeStyle::Metro, pspapp::HomeStyle::Classic}) {
        value.homeStyle = style;
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences styleRestored;
        assert(pspapp::loadPreferences(path, styleRestored) && styleRestored.homeStyle == style);
        assert(styleRestored.tileHome() == (style != pspapp::HomeStyle::Classic));
        assert(styleRestored.metro() == (style == pspapp::HomeStyle::Metro));
    }
    // The input method defaults to Auto, round-trips and is validated.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false})";
    assert(pspapp::loadPreferences(path, value) && value.inputMethod == pspapp::InputMethod::Pinyin);
    for (auto method : {pspapp::InputMethod::System, pspapp::InputMethod::Pinyin}) {
        value.inputMethod = method;
        assert(pspapp::savePreferences(path, value));
        pspapp::Preferences methodRestored;
        assert(pspapp::loadPreferences(path, methodRestored) && methodRestored.inputMethod == method);
    }
    // An earlier build wrote "auto"; it loads as the built-in keyboard.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"input_method":"auto"})";
    assert(pspapp::loadPreferences(path, value) && value.inputMethod == pspapp::InputMethod::Pinyin);
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"input_method":"kana"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"input_method":1})";
    assert(!pspapp::loadPreferences(path, value));
    // The boolean written by the first tile build means the Tiles style.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"tile_home":true})";
    assert(pspapp::loadPreferences(path, value) && value.homeStyle == pspapp::HomeStyle::Tiles);
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"tile_home":1})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"home_style":"grid"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"home_style":2})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"log_files":"yes"})";
    assert(!pspapp::loadPreferences(path, value));
    // Channel overrides are plain words only; quotes, long text and unknown levels are rejected.
    value.logChannels = "net=\"me_backend\"";
    assert(!pspapp::savePreferences(path, value));
    value.logChannels = std::string(97, 'a');
    assert(!pspapp::savePreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"log_level":"loud"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"log_channels":"a;b"})";
    assert(!pspapp::loadPreferences(path, value));
    // Stream server: off and empty for old files, round-trips, and hand-edited text is validated.
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false})";
    assert(pspapp::loadPreferences(path, value));
    assert(!value.streamServer && value.serverHost.empty() && value.serverToken.empty() &&
           value.serverPort == 8787 && !value.serverReady());
    assert(value.streamPreset == pspapp::StreamPreset::Balanced && value.streamProfile == pspapp::StreamProfile::Auto);
    // Old files keep the previous behaviour: 480P source, frame rate left to the source.
    assert(value.streamQuality == WILI_QN_480P && value.streamFps == pspapp::StreamFps::Auto);
    value.streamServer = true;
    value.serverHost = "192.168.1.20";
    value.serverPort = 9000;
    value.serverToken = "k7m2q9xa";
    value.streamPreset = pspapp::StreamPreset::Quality;
    value.streamProfile = pspapp::StreamProfile::Cabac;
    value.streamQuality = WILI_QN_1080P60;
    value.streamFps = pspapp::StreamFps::Max30;
    assert(value.serverReady());
    assert(pspapp::savePreferences(path, value));
    pspapp::Preferences serverRestored;
    assert(pspapp::loadPreferences(path, serverRestored));
    assert(serverRestored.streamServer && serverRestored.serverHost == "192.168.1.20" &&
           serverRestored.serverPort == 9000 && serverRestored.serverToken == "k7m2q9xa" &&
           serverRestored.streamPreset == pspapp::StreamPreset::Quality &&
           serverRestored.streamProfile == pspapp::StreamProfile::Cabac &&
           serverRestored.streamQuality == WILI_QN_1080P60 && serverRestored.streamFps == pspapp::StreamFps::Max30);
    {
        const WiliServer config = serverRestored.serverConfig();
        assert(config.enabled == 1 && config.port == 9000 && std::string(config.host) == "192.168.1.20" &&
               std::string(config.token) == "k7m2q9xa");
    }
    // Enabled but incomplete never counts as ready, so nothing is rewritten.
    value.serverToken.clear();
    assert(!value.serverReady());
    value.serverToken = "k7m2q9xa";
    // Bad values are refused on save and on load.
    value.serverHost = "evil host/with spaces";
    assert(!pspapp::savePreferences(path, value));
    value.serverHost = "192.168.1.20:8787"; // the port belongs in server_port
    assert(!pspapp::savePreferences(path, value));
    value.serverHost = "192.168.1.20";
    value.serverToken = "bad token&k=1";
    assert(!pspapp::savePreferences(path, value));
    value.serverToken = "k7m2q9xa";
    value.serverPort = 0;
    assert(!pspapp::savePreferences(path, value));
    value.serverPort = 8787;
    value.streamQuality = 125; // not a tier the server offers
    assert(!pspapp::savePreferences(path, value));
    value.streamQuality = WILI_QN_480P;
    for (const char* bad : {R"("server_host":"a b")", R"("server_port":70000)", R"("server_token":"x")",
                            R"("server_token":"a&b=cdef")", R"("stream_preset":"ultra")",
                            R"("stream_profile":"high")", R"("stream_quality":125)", R"("stream_quality":"4K")",
                            R"("stream_fps":"120")", R"("stream_fps":60)", R"("stream_server":1)", R"("server_port":"80")"}) {
        std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,)"
                            << bad << "}";
        assert(!pspapp::loadPreferences(path, value));
    }
    std::remove(path);
    std::puts("Preference migration, round trips, invalid buffers and log settings: passed");
}
