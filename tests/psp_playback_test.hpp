#pragma once

// Diagnostic entry point used only by BILI_DEVICE_PLAYBACK_TEST builds.
#include <nlohmann/json.hpp>
#include "playback_osd.hpp"
#include <memory>

namespace {
struct PlaybackTestPresentation {
    struct Sample { uint64_t time; PspVideoStatus status; } samples[512]{};
    unsigned count = 0;
    uint64_t last = 0, firstFrame = 0;
    const PspVideoPresenter* target = nullptr;
    PspVideoPresenter presenter{this, [](void* opaque, const uint32_t* pixels, const PspVideoStatus* status) {
        auto* self = static_cast<PlaybackTestPresentation*>(opaque);
        uint64_t now = sceKernelGetSystemTimeWide();
        if (pixels && !self->firstFrame) self->firstFrame = now;
        if (self->count < 512 && now - self->last >= 100000) {
            self->samples[self->count++] = {now, *status}; self->last = now;
        }
        return self->target->present(self->target->opaque, pixels, status);
    }, [](void* opaque, PspVideoSettings* settings) {
        auto* self = static_cast<PlaybackTestPresentation*>(opaque);
        if (self->target->update_settings) self->target->update_settings(self->target->opaque, settings);
    }};
};
struct PlaybackTestSource {
    FILE* file = nullptr;
    SceUID lock = -1;
    volatile bool cancelled = false, interrupted = false;
    PspVideoSource source{};

    bool open(const std::string& path, bool live = false) {
        file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        const long bytes = std::ftell(file);
        lock = sceKernelCreateSema("test_source", 0, 1, 1, nullptr);
        if (lock < 0 || bytes <= 0) return false;
        source.opaque = this;
        source.interrupt_reads = [](void* opaque, int interrupted) {
            static_cast<PlaybackTestSource*>(opaque)->interrupted = interrupted != 0;
        };
        source.size = live ? -1 : bytes;
        source.read_at = [](void* opaque, int64_t offset, void* buffer, int length) {
            auto& input = *static_cast<PlaybackTestSource*>(opaque);
            sceKernelWaitSema(input.lock, 1, nullptr);
            int result = -1;
            if (!input.cancelled && !input.interrupted && offset >= 0 && offset <= 0x7fffffff &&
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

#include "../benchmarks/bilibili/device_runner.hpp"

int controlledPlaybackTest() {
    if (FILE* benchmark = std::fopen(CLIENT_PATH("/benchmark.json"), "rb"))
        return bilibiliBenchmark(benchmark);
    using nlohmann::json;
    json output;
    int code = -1;
    try {
        FILE* file = std::fopen(CLIENT_PATH("/playback-test.json"), "rb");
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
        int64_t requestedStart = config.value("start_ms", 0);
        bool requestedPaused = config.value("start_paused", false);
        int seekCount = 0;
        for (int i = 0; i < repeats; ++i) {
            PlaybackTestSource videoInput, audioInput;
            BiliPlayback networkPlayback{};
            PspVideoOptions options{};
            options.path = video.c_str();
            options.log_root = CLIENT_ROOT;
            options.start_ms = requestedStart;
            options.start_paused = requestedPaused;
            options.sample = config.value("sample", false);
            options.rgb_buffer_frames = config.value("buffer_frames", pspapp::defaultPlaybackBufferFrames());
            if (config.contains("buffers"))
                options.rgb_buffer_frames = config.at("buffers").at(i).get<int>();
            if (mode == "range") {
                const int initialized = network_init();
                if (initialized < 0) { output["network_init"] = initialized; break; }
                const std::string videoUrl = config.at("video_url").get<std::string>();
                const std::string audioUrl = config.at("audio_url").get<std::string>();
                BiliVideo metadata{};
                BiliMediaUrl videoMedia{}, audioMedia{};
                videoMedia.url = videoUrl.c_str(); audioMedia.url = audioUrl.c_str();
                const int prepared = bili_prepare_range_playback(&metadata, &videoMedia, &audioMedia, &networkPlayback);
                if (prepared < 0) { output["network_prepare"] = prepared; output["network_error"] = network_error; break; }
                options.source = bili_playback_source(&networkPlayback);
                options.audio_source = bili_playback_audio_source(&networkPlayback);
            } else if (mode != "file") {
                if (!videoInput.open(video, mode == "live")) break;
                options.source = &videoInput.source;
            }
            if (mode == "split") {
                if (!audioInput.open(std::string(CLIENT_ROOT) + "/" + config.at("audio").get<std::string>())) break;
                options.audio_source = &audioInput.source;
            }
            PspVideoSettings settings{16, 3, {16, 32, 64}, 1, 1, 75, 0, 0};
            std::vector<std::pair<std::string, uint64_t>> overlayRequests;
            std::unique_ptr<pspapp::PlaybackOsd> osd;
            std::unique_ptr<PlaybackTestPresentation> presentation;
            if (config.value("controls", false)) {
                pspapp::Preferences prefs;
                prefs.locale = config.value("english", false) ? pspapp::Locale::EnUs : pspapp::Locale::ZhHans;
                prefs.confirmWithCircle = config.value("circle_confirm", false);
                options.cancel_button = prefs.confirmWithCircle ? PSP_VIDEO_CANCEL_CROSS : PSP_VIDEO_CANCEL_CIRCLE;
                std::vector<pspapp::DanmakuComment> comments;
                if (config.value("danmaku", false)) comments = pspapp::parseDanmaku(
                    "<i><d p=\"0,5,25,16777215,0\">PSP 固定弹幕测试</d>"
                    "<d p=\"0,1,25,16766720,0\">滚动弹幕 &amp; GE 叠加</d>"
                    "<d p=\"1,1,25,16777215,0\">同轨不追尾</d>"
                    "<d p=\"2,4,25,65535,0\">底部固定弹幕</d>"
                    "<d p=\"3,1,25,16777215,0\">暂停与跳转同步</d></i>");
                for (auto& comment : comments) comment.timeMs += config.value("comment_offset_ms", 0);
                std::vector<pspapp::SubtitleTrack> subtitles;
                if (config.value("subtitles", false)) subtitles = {
                    {"简体中文", {{0, 2000, "走慢一点，风景就在身边。"}, {3000, 6000, "暂停和跳转后，字幕跟随播放时间。"}}},
                    {"English", {{0, 2000, "Slow down. Enjoy the view."}, {3000, 6000, "Subtitles follow the playback clock."}}}};
                osd = std::make_unique<pspapp::PlaybackOsd>(config.value("title", "播放控件测试 / Playback controls"), prefs, comments, subtitles);
                settings.subtitle_count = osd->subtitleCount();
                settings.subtitle_track = config.value("subtitle_track", 0);
                if (config.value("async_overlays", false)) {
                    settings.subtitle_track = 1;
                    settings.subtitle_count = 2;
                    osd->enableNetwork("fixture", 1, 0, [&](const std::string& url, pspapp::ErrorOperation op) {
                        const bool metadata = url.find("/x/player/v2") != std::string::npos;
                        const bool comments = url.find("/dm/list.so") != std::string::npos;
                        overlayRequests.emplace_back(metadata ? "metadata" : comments ? "danmaku" : "selected_subtitle", sceKernelGetSystemTimeWide());
                        sceKernelDelayThread(1200000); // Deliberately slow optional resources.
                        pspapp::HttpResponse r;
                        if (config.value("overlay_failure", false)) {
                            r.error = {pspapp::ErrorDomain::Network, pspapp::ErrorKind::Timeout, op, 28};
                        } else if (metadata) r.body = R"({"code":0,"data":{"subtitle":{"subtitles":[{"lan_doc":"简体中文","subtitle_url":"https://fixture.invalid/zh.json"},{"lan_doc":"English","subtitle_url":"https://fixture.invalid/en.json"}]}}})";
                        else if (comments) r.body = "<i><d p=\"0,1,25,16777215,0\">后台弹幕测试</d></i>";
                        else r.body = R"({"body":[{"from":0,"to":8,"content":"字幕按需加载，不阻塞首帧。"}]})";
                        return r;
                    });
                }
                options.settings = &settings;
                presentation = std::make_unique<PlaybackTestPresentation>();
                presentation->target = osd->presenter();
                options.presenter = &presentation->presenter;
            }
            const auto before = mallinfo();
            PspVideoResult result{};
            code = psp_video_play(&options, &result);
            osd.reset(); // Join optional resource worker before inspecting requests.
            for (const auto& request : overlayRequests) output["overlay_requests"].push_back({{"kind", request.first}, {"time_us", request.second}});
            if (mode == "range") {
                const int networkResult = bili_finish_playback(&networkPlayback);
                output["network_cleanup"] = networkResult;
            }
            const auto after = mallinfo();
            if (presentation) {
                output["first_frame_us"] = presentation->firstFrame;
                auto samples = json::array();
                for (unsigned n = 0; n < presentation->count; ++n) {
                    const auto& item = presentation->samples[n];
                    samples.push_back({{"time_us", item.time}, {"position_ms", item.status.position_ms},
                        {"paused", item.status.paused}, {"visible", item.status.controls_visible},
                        {"seeking", item.status.seeking}, {"preview_ms", item.status.preview_ms},
                        {"menu", item.status.menu_open}, {"subtitle", item.status.settings.subtitle_track}, {"danmaku", item.status.settings.danmaku_enabled},
                        {"density", item.status.settings.danmaku_density}, {"opacity", item.status.settings.danmaku_opacity}});
                }
                output["control_samples"].push_back(std::move(samples));
            }
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
            output["seek_count"] = seekCount + result.seeks;
            output["last_seek_ms"] = result.seek_ms;
            output["quality_requested"] = result.quality_requested;
            if (result.seek_requested && seekCount < 4) {
                requestedStart = result.seek_ms;
                requestedPaused = result.seek_paused;
                output["seek_targets"].push_back(result.seek_ms);
                ++seekCount;
                --i;
                continue;
            }
            std::printf("PLAYBACK_TEST play=%d buffer_frames=%d code=%d frames=%u eof=%u heap_before=%u heap_after=%u\n",
                        i + 1, options.rgb_buffer_frames, code, result.frames, result.eof,
                        static_cast<unsigned>(before.uordblks), static_cast<unsigned>(after.uordblks));
            if (code < 0 || result.cancelled) break;
        }
    } catch (...) { output["error"] = "Invalid playback test configuration"; }
    FILE* file = std::fopen(CLIENT_PATH("/playback-test-result.json"), "wb");
    if (file) {
        const std::string text = output.dump(2);
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
    return code;
}
} // namespace
