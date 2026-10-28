#pragma once

// Diagnostic entry point used only by BILI_DEVICE_PLAYBACK_TEST builds.
#include <nlohmann/json.hpp>

namespace {
struct PlaybackTestSource {
    FILE* file = nullptr;
    SceUID lock = -1;
    volatile bool cancelled = false;
    PspVideoSource source{};

    bool open(const std::string& path, bool live = false) {
        file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        const long bytes = std::ftell(file);
        lock = sceKernelCreateSema("test_source", 0, 1, 1, nullptr);
        if (lock < 0 || bytes <= 0) return false;
        source.opaque = this;
        source.size = live ? -1 : bytes;
        source.read_at = [](void* opaque, int64_t offset, void* buffer, int length) {
            auto& input = *static_cast<PlaybackTestSource*>(opaque);
            sceKernelWaitSema(input.lock, 1, nullptr);
            int result = -1;
            if (!input.cancelled && offset >= 0 && offset <= 0x7fffffff &&
                !std::fseek(input.file, static_cast<long>(offset), SEEK_SET)) {
                result = static_cast<int>(std::fread(buffer, 1, length, input.file));
                if (std::ferror(input.file)) result = -1;
            }
            sceKernelSignalSema(input.lock, 1);
            return result;
        };
        source.cancel = [](void* opaque) {
            static_cast<PlaybackTestSource*>(opaque)->cancelled = true;
        };
        return true;
    }
    ~PlaybackTestSource() {
        if (file) std::fclose(file);
        if (lock >= 0) sceKernelDeleteSema(lock);
    }
};

int controlledPlaybackTest() {
    using nlohmann::json;
    json output;
    int code = -1;
    try {
        FILE* file = std::fopen(CLIENT_ROOT "/playback-test.json", "rb");
        if (!file) return -1;
        char text[4096]{};
        const size_t size = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        const auto config = json::parse(text, text + size);
        const std::string video = std::string(CLIENT_ROOT) + "/" + config.at("video").get<std::string>();
        const std::string mode = config.value("mode", "file");
        const int repeats = config.value("repeat", 1);
        if (repeats < 1 || repeats > 8) return -1;
        output["plays"] = json::array();
        for (int i = 0; i < repeats; ++i) {
            PlaybackTestSource videoInput, audioInput;
            PspVideoOptions options{};
            options.path = video.c_str();
            options.log_root = CLIENT_ROOT;
            options.start_ms = config.value("start_ms", 0);
            options.sample = config.value("sample", false);
            options.rgb_buffer_frames = config.value("buffer_frames", 8);
            if (config.contains("buffers"))
                options.rgb_buffer_frames = config.at("buffers").at(i).get<int>();
            if (mode != "file") {
                if (!videoInput.open(video, mode == "live")) break;
                options.source = &videoInput.source;
            }
            if (mode == "split") {
                if (!audioInput.open(std::string(CLIENT_ROOT) + "/" + config.at("audio").get<std::string>())) break;
                options.audio_source = &audioInput.source;
            }
            const auto before = mallinfo();
            PspVideoResult result{};
            code = psp_video_play(&options, &result);
            const auto after = mallinfo();
            output["plays"].push_back({{"code", code}, {"frames", result.frames},
                {"buffer_frames", options.rgb_buffer_frames}, {"eof", result.eof},
                {"heap_before", before.uordblks}, {"heap_after", after.uordblks},
                {"system_free", sceKernelTotalFreeMemSize()}});
            output["code"] = code;
            output["frames"] = result.frames;
            output["eof"] = result.eof;
            output["cancelled"] = result.cancelled;
            output["position_ms"] = result.position_ms;
            output["error_stage"] = psp_video_stage_name(result.error_stage);
            std::printf("PLAYBACK_TEST play=%d buffer_frames=%d code=%d frames=%u eof=%u heap_before=%u heap_after=%u\n",
                        i + 1, options.rgb_buffer_frames, code, result.frames, result.eof,
                        static_cast<unsigned>(before.uordblks), static_cast<unsigned>(after.uordblks));
            if (code < 0 || result.cancelled) break;
        }
    } catch (...) { output["error"] = "Invalid playback test configuration"; }
    FILE* file = std::fopen(CLIENT_ROOT "/playback-test-result.json", "wb");
    if (file) {
        const std::string text = output.dump(2);
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
    return code;
}
} // namespace
