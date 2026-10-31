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
    value.playbackBufferFrames = 3;
    assert(!pspapp::savePreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"playback_buffer_frames":3})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"playback_buffer_frames":"8"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"me_backend":"unknown"})";
    assert(!pspapp::loadPreferences(path, value));
    std::ofstream(path) << R"({"version":1,"dark_theme":false,"body_size":14,"show_covers":true,"slow_repeat":false,"me_backend":1})";
    assert(!pspapp::loadPreferences(path, value));
    std::remove(path);
    std::puts("Preference migration, round trips and invalid buffers: passed");
}
