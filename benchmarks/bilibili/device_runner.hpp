#pragma once

// Included inside the diagnostic test namespace, after PlaybackTestSource.
// This entry is absent from normal client builds.
int bilibiliBenchmark(FILE* configFile) {
    using nlohmann::json;
    FILE* index = nullptr;
    int status = -1;
    try {
        char text[16384]{};
        const size_t bytes = std::fread(text, 1, sizeof(text) - 1, configFile);
        const bool complete = std::feof(configFile) && !std::ferror(configFile);
        std::fclose(configFile);
        configFile = nullptr;
        if (!complete) return -1;
        const auto config = json::parse(text, text + bytes);
        const int repeats = config.value("repeat", 3);
        const auto& cases = config.at("cases");
        if (repeats < 1 || repeats > 8 || cases.empty() || cases.size() > 20) return -1;
        const std::string root = std::string(CLIENT_ROOT) + "/benchmark-results";
        // Never mix a new run with old results. Stage to a fresh game directory.
        if (sceIoMkdir(root.c_str(), 0777) < 0) return -1;
        index = std::fopen((root + "/index.jsonl").c_str(), "wb");
        if (!index) return -1;
        status = 0;
        for (const auto& item : cases) {
            const std::string id = item.at("id").get<std::string>();
            if (id.empty() || id.size() > 40 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
                throw std::runtime_error("Invalid benchmark ID");
            const std::string caseRoot = root + "/" + id;
            if (sceIoMkdir(caseRoot.c_str(), 0777) < 0) throw std::runtime_error("Cannot create case directory");
            for (int repeat = 1; repeat <= repeats; ++repeat) {
                SceCtrlData pad{};
                sceCtrlPeekBufferPositive(&pad, 1);
                if (pad.Buttons & PSP_CTRL_START) { status = -1; break; }
                char pass[16];
                std::snprintf(pass, sizeof(pass), "/r%02d", repeat);
                const std::string relative = "benchmark-results/" + id + pass;
                const std::string logRoot = std::string(CLIENT_ROOT) + "/" + relative;
                if (logRoot.size() > 110 || sceIoMkdir(logRoot.c_str(), 0777) < 0)
                    throw std::runtime_error("Cannot create pass directory");
                PlaybackTestSource video, audio;
                const std::string videoPath = item.at("video").get<std::string>();
                const std::string audioPath = item.at("audio").get<std::string>();
                for (const auto* path : {&videoPath, &audioPath}) {
                    if (path->find("..") != std::string::npos || path->find(':') != std::string::npos ||
                        path->compare(0, 10, "benchmark/") != 0)
                        throw std::runtime_error("Invalid benchmark path");
                }
                if (!video.open(std::string(CLIENT_ROOT) + "/" + videoPath) ||
                    !audio.open(std::string(CLIENT_ROOT) + "/" + audioPath))
                    throw std::runtime_error("Cannot open benchmark media");
                PspVideoOptions options{};
                options.source = &video.source;
                options.audio_source = &audio.source;
                options.log_root = logRoot.c_str();
                options.rgb_buffer_frames = config.value("buffer_frames", 2);
                options.diagnostic_profile = config.value("profile", false);
                const auto before = mallinfo();
                PspVideoResult result{};
                std::printf("BENCH begin id=%s repeat=%d profile=%d\n", id.c_str(), repeat, options.diagnostic_profile);
                const uint64_t start = sceKernelGetSystemTimeWide();
                const int code = psp_video_play(&options, &result);
                const uint64_t wall = sceKernelGetSystemTimeWide() - start;
                const auto after = mallinfo();
                json record = {{"id", id}, {"repeat", repeat}, {"log_root", relative},
                    {"code", code}, {"frames", result.frames}, {"eof", result.eof},
                    {"cancelled", result.cancelled}, {"error_stage", psp_video_stage_name(result.error_stage)},
                    {"wall_us", wall}, {"position_ms", result.position_ms},
                    {"heap_before", before.uordblks}, {"heap_after", after.uordblks},
                    {"system_free", sceKernelTotalFreeMemSize()}};
                const std::string line = record.dump() + "\n";
                if (std::fwrite(line.data(), 1, line.size(), index) != line.size() || std::fflush(index))
                    throw std::runtime_error("Cannot save benchmark index");
                std::printf("BENCH end id=%s repeat=%d code=%d eof=%u frames=%u\n",
                            id.c_str(), repeat, code, result.eof, result.frames);
                // Stop immediately on error/cancel, especially poisoned ME state.
                if (code < 0 || result.cancelled || !result.eof) { status = -1; break; }
            }
            if (status) break;
        }
    } catch (const std::exception& error) {
        std::printf("BENCH failed: %s\n", error.what());
        status = -1;
    }
    if (configFile) std::fclose(configFile);
    if (index) std::fclose(index);
    std::printf("BENCH complete status=%d; START to exit\n", status);
    return status;
}
