#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <malloc.h>
#include <unistd.h>

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_sysparam.h>

#include "app.hpp"
#include "ui.hpp"
#include "playback_osd.hpp"
#include "storage_paths.h"
#include <psp_runtime.h>
#include <wili_log.h>
#ifdef BILI_DEVICE_PLAYBACK_TEST
#include "../tests/psp_playback_test.hpp"
#endif

PSP_MODULE_INFO("Wiliwili PSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
// The loader selects the profile before libc initializes this heap.
PSP_HEAP_SIZE_KB(BILI_CLIENT_HEAP_KB);

int client_storage_index = 0;
const char *client_storage_device = nullptr;

namespace {

volatile int exiting = 0;
// Set while psp_video_play owns the main thread. Its cleanup can wait on a
// stalled decoder/audio/ME worker, so a cooperative HOME exit would never run.
volatile int playbackActive = 0;
constexpr uint64_t kForcedExitWaitUs = 5000000;  // unmeasured first guess

void logMemory(const char* phase) {
    const auto heap = mallinfo();
    WLOG_I(WLC_MEM, "%s heap_used=%u heap_arena_free=%u system_free=%u system_max=%u",
           phase, static_cast<unsigned>(heap.uordblks), static_cast<unsigned>(heap.fordblks),
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
}

// True when the loader started no ME bridge; native-ME playback is then impossible.
bool noMe = false;

int exitCallback(int, int, void*) {
    WLOG_I(WLC_CLIENT, "home_exit callback_enter playback_active=%d", playbackActive);
    exiting = 1;
    transfer_cancel = 1;
    WLOG_I(WLC_CLIENT, "home_exit stop_begin");
    psp_video_stop();
    WLOG_I(WLC_CLIENT, "home_exit stop_end playback_active=%d", playbackActive);
    if (!playbackActive) return 0;
    // This callback runs on its own thread and survives a hung playback. Give
    // the cooperative teardown (which restores the Media Engine) a chance to
    // finish, then force the exit. The limit is unmeasured; see the log line
    // and docs/playback-controls.md.
    const uint64_t begin = sceKernelGetSystemTimeWide();
    while (playbackActive && sceKernelGetSystemTimeWide() - begin < kForcedExitWaitUs)
        sceKernelDelayThread(10000);
    const unsigned waitedMs = static_cast<unsigned>((sceKernelGetSystemTimeWide() - begin) / 1000);
    if (!playbackActive) {
        WLOG_I(WLC_CLIENT, "home_exit playback_teardown_ms=%u", waitedMs);
        return 0;
    }
    WLOG_E(WLC_CLIENT, "home_exit forced after %u ms; playback teardown did not finish", waitedMs);
    sceKernelExitGame();
    return 0;
}

int callbackThread(SceSize, void*) {
    const int id = sceKernelCreateCallback("wiliwili_exit", exitCallback, nullptr);
    sceKernelRegisterExitCallback(id);
    WLOG_I(WLC_CLIENT, "exit_callback registered id=%d thread=%d", id, sceKernelGetThreadId());
    while (!exiting) {
        sceKernelDelayThreadCB(2000000);
        if (playbackActive) psp_video_log_health();
    }
    return 0;
}

void networkDialog() {
    pspUtilityNetconfData data{};
    data.base.size = sizeof(data);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &data.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &data.base.buttonSwap);
    data.base.graphicsThread = 17;
    data.base.accessThread = 19;
    data.base.fontThread = 18;
    data.base.soundThread = 16;
    data.action = PSP_NETCONF_ACTION_CONNECTAP;
    if (sceUtilityNetconfInitStart(&data) < 0) return;

    bool shutdown = false;
    while (!exiting) {
        ui_dialog_frame();
        const int state = sceUtilityNetconfGetStatus();
        if (state == PSP_UTILITY_DIALOG_VISIBLE) {
            sceUtilityNetconfUpdate(1);
        } else if (state == PSP_UTILITY_DIALOG_QUIT && !shutdown) {
            sceUtilityNetconfShutdownStart();
            shutdown = true;
        } else if (state == PSP_UTILITY_DIALOG_NONE) {
            break;
        }
        ui_dialog_present();
    }
    network_remember_profile();
    ui_resume();
}

}  // namespace

int main(int argc, char** argv) {
    wili_log_init();
    const char* root = wili_storage_from_client(argc > 0 && argv ? argv[0] : nullptr);
    if (!root || chdir(root) < 0) {
        WLOG_E(WLC_CLIENT, "cannot open package root; start through EBOOT.PBP");
        sceKernelDelayThread(5000000);
        sceKernelExitGame();
        return 1;
    }
    client_storage_index = root == wili_storage_roots[0] ? 0 : root == wili_storage_roots[1] ? 1 : 2;
    client_storage_device = std::strncmp(root, "ms0:", 4) == 0 ? "ms0:"
                          : std::strncmp(root, "ef0:", 4) == 0 ? "ef0:" : "host0:";
    WLOG_I(WLC_CLIENT, "root=%s", root);
    WLOG_I(WLC_CLIENT, "heap_kib=%d default_rgb_frames=%d",
           BILI_CLIENT_HEAP_KB, pspapp::defaultPlaybackBufferFrames());
    // This thread formats log lines (HOME exit and the 2 s playback health samples): vsnprintf
    // plus the line buffer overflow the old 4 KiB stack, so give it room.
    const int callback = sceKernelCreateThread("wiliwili_callbacks", callbackThread, 0x11, 0x4000,
                                               PSP_THREAD_ATTR_USER, nullptr);
    if (callback >= 0) sceKernelStartThread(callback, 0, nullptr);
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    sceIoMkdir(CLIENT_PATH("/cache"), 0777);

    ui_init();
    logMemory("ui_init");
#ifdef BILI_DEVICE_PLAYBACK_TEST
    const int playbackTest = controlledPlaybackTest();
    logMemory("playback_test_complete");
    ui_resume();
    ui_startup_status("播放测试完成 · START 退出");
    for (;;) {
        SceCtrlData pad{};
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) break;
        sceKernelDelayThreadCB(10000);
    }
    ui_shutdown();
    sceKernelExitGame();
    return playbackTest;
#endif
    pspapp::App app;
    // No ME bridge is loaded under the emulator's sceMpeg or with the stream server on (see
    // bootstrap.c); then nothing here may call into it.
    noMe = BILI_PPSSPP_SCE_MPEG && psp_runtime_detect() == PSP_RUNTIME_PPSSPP;
    for (int i = 1; i < argc; ++i)
        if (argv && argv[i] && std::strcmp(argv[i], "--no-me") == 0) noMe = true;
    if (!noMe) {
        app.setActiveMeBackend(psp_video_me_backend());
        WLOG_I(WLC_CLIENT, "me_backend=%d", app.activeMeBackend());
    } else {
        WLOG_I(WLC_CLIENT, "me_backend=none (sceMpeg owns the Media Engine)");
    }
    ui_startup_status("正在读取设置与本地缓存…");
    app.init();
    logMemory("app_init");
    app.setStatus("正在初始化网络…");
    ui_render(app);
#ifdef BILI_DEVICE_OSK_TEST
    app.setTab(pspapp::Tab::Search);
    ui_render(app);
    const std::string osk_text = ui_input_text("PSP OSK 测试", "");
    FILE* osk_result = std::fopen(CLIENT_PATH("/osk-result.txt"), "w");
    if (osk_result) {
        std::fprintf(osk_result, "%s\n", osk_text.c_str());
        std::fclose(osk_result);
    }
    ui_shutdown();
    sceKernelExitGame();
    return 0;
#endif

    const auto initError = pspapp::WifiManager{}.initialize();
    const int net = initError ? initError.rawCode : 0;
    logMemory("network_init");
    if (net < 0) {
        app.setStatus(pspapp::errorMessage(initError));
        pspapp::logError(CLIENT_PATH("/last-error.txt"), initError);
    }
    if (net >= 0 && !network_ready() && !network_has_profile()) {
        app.setStatus("请选择 Wi-Fi 连接 · " + std::string(app.preferences().backButton()) + " 返回离线首页");
        ui_render(app);
        networkDialog();
    }
    if (net >= 0) {
        app.refresh();
    }

    unsigned previous = 0;
    unsigned heldDirection = 0;
    int64_t repeatAt = 0;

    while (!exiting) {
        app.poll();

        if (app.playbackReady()) {
            ui_render(app);
            ui_prepare_playback();
            if (pspapp::defaultPlaybackBufferFrames() == 2) {
                logMemory("before_http_trim");
                network_trim_idle_http();
            }
            logMemory("before_playback");
            PspVideoOptions options{};
            options.path = app.playback().path;
            options.log_root = CLIENT_ROOT;
            options.source = bili_playback_source(&app.playback());
            options.audio_source = bili_playback_audio_source(&app.playback());
            // A server stream restarts at the requested time on the server, so its own
            // clock starts at zero and the player adds the offset for display.
            const BiliPlayback& prepared = app.playback();
            options.start_ms = prepared.restart_seek ? 0 : app.playbackStartMs();
            options.restart_seek = prepared.restart_seek;
            options.timeline_offset_ms = prepared.timeline_offset_ms;
            options.timeline_duration_ms = prepared.timeline_duration_ms;
            options.rgb_buffer_frames = app.preferences().playbackBufferFrames;
            options.rgb_pixel_format = pspapp::playbackUsesRgb565(app.preferences().playbackPixelFormat)
                ? PSP_VIDEO_PIXEL_FORMAT_RGB565 : PSP_VIDEO_PIXEL_FORMAT_RGBA8888;
            options.cancel_button = app.preferences().confirmWithCircle
                ? PSP_VIDEO_CANCEL_CROSS : PSP_VIDEO_CANCEL_CIRCLE;
            // wili-server streams are decoded by the firmware's sceMpeg and by nothing else:
            // there is deliberately no fallback to the ME decoder for them.
            options.backend = prepared.restart_seek ? PSP_VIDEO_BACKEND_SCE_MPEG
                            : BILI_PPSSPP_SCE_MPEG && psp_runtime_detect() == PSP_RUNTIME_PPSSPP
                                  ? PSP_VIDEO_BACKEND_SCE_MPEG
                                  : PSP_VIDEO_BACKEND_NATIVE_ME;
            if (options.backend == PSP_VIDEO_BACKEND_NATIVE_ME && noMe) {
                // Live rooms and other direct playback need the ME decoder, which is off while the
                // stream server is on. Fail cleanly instead of calling into an absent bridge.
                ui_resume();
                app.finishPlayback(-1, 0, 0, 0, PSP_VIDEO_STAGE_SETUP, 0);
                app.setStatus(pspapp::text(pspapp::TextId::stream_err_no_me, app.preferences().locale));
                previous = ~0u;
                continue;
            }
            auto osd = std::make_unique<pspapp::PlaybackOsd>(app.playbackTitle(), app.preferences());
            osd->enableNetwork(app.playbackCard().bvid, app.playbackCard().cid, app.playbackCard().aid);
            options.presenter = osd->presenter();
            options.start_paused = app.playbackPaused();
            auto settings = app.playerSettings();
            settings.quality = app.playback().quality;
            settings.quality_count = app.playback().quality_count;
            for (int i = 0; i < settings.quality_count; ++i) settings.qualities[i] = app.playback().qualities[i];
            options.settings = &settings;
            PspVideoResult result{};
            playbackActive = 1;
            const int error = psp_video_play(&options, &result);
            playbackActive = 0;
            logMemory("after_playback");
            ui_resume();
            osd.reset();
            app.setPlayerSettings(result.settings);
            if (result.quality_requested && !exiting && error >= 0)
                app.seekPlayback(result.position_ms, result.seek_paused != 0, result.quality_requested);
            else if (result.seek_requested && !exiting && error >= 0)
                app.seekPlayback(result.seek_ms, result.seek_paused != 0);
            else app.finishPlayback(error, result.frames, result.eof, result.cancelled, result.error_stage, result.position_ms);
            previous = ~0u;
            continue;
        }

        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~previous;
        const unsigned direction = pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
        const int64_t now = sceKernelGetSystemTimeWide();
        if (direction != heldDirection) {
            heldDirection = direction;
            repeatAt = now + (app.preferences().slowRepeat ? 500000 : 350000);
        } else if (direction && now >= repeatAt) {
            // Repeat navigation only; held confirm/back must never activate twice.
            const bool adjustingSetting = app.screen() == pspapp::Screen::Settings &&
                app.settingSelection() < static_cast<size_t>(pspapp::SettingItem::Wifi);
            // Settings values and comment like/replies must not fire repeatedly while held.
            pressed |= (adjustingSetting || app.commentPage()) ? direction & (PSP_CTRL_UP | PSP_CTRL_DOWN) : direction;
            repeatAt = now + (app.preferences().slowRepeat ? 180000 : 90000);
        }
        previous = pad.Buttons;
        const unsigned confirmButton = app.preferences().confirmWithCircle ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS;
        const unsigned backButton = app.preferences().confirmWithCircle ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;

        if (app.utilityPage() && !app.busy()) {
            if (pressed & PSP_CTRL_START) break;
            if (pressed & backButton) app.back();
            else if (pressed & confirmButton) {
                if (app.screen() == pspapp::Screen::Settings &&
                    app.settingSelection() == static_cast<size_t>(pspapp::SettingItem::Wifi)) {
                    networkDialog();
                    app.setStatus(network_ready() ? "Wi-Fi 已连接" : "已返回 · 未连接 Wi-Fi");
                    previous = ~0u;
                } else app.activate();
            } else if ((pressed & PSP_CTRL_TRIANGLE) && app.screen() == pspapp::Screen::Storage)
                app.updateStorage();
            else if (pressed & PSP_CTRL_UP) {
                if (app.screen() == pspapp::Screen::LicenseText) ui_scroll_document(app, -1);
                else app.moveSelection(0, -1);
            } else if (pressed & PSP_CTRL_DOWN) {
                if (app.screen() == pspapp::Screen::LicenseText) ui_scroll_document(app, 1);
                else app.moveSelection(0, 1);
            } else if (pressed & PSP_CTRL_LEFT) app.moveSelection(-1, 0);
            else if (pressed & PSP_CTRL_RIGHT) app.moveSelection(1, 0);
        } else if (app.screen() == pspapp::Screen::Login) {
            if (pressed & PSP_CTRL_START)
                break;
            if (pressed & backButton)
                app.back();
            else if (pressed & PSP_CTRL_TRIANGLE)
                app.refreshLogin();
            else if (!app.busy()) {
                if (pressed & confirmButton)
                    app.refreshLogin();
                else if ((pressed & PSP_CTRL_SQUARE) &&
                         (app.auth().state == pspapp::AuthState::SignedIn ||
                          app.auth().state == pspapp::AuthState::Restored))
                    app.logout();
                else if (pressed & PSP_CTRL_SELECT) {
                    networkDialog();
                    app.refreshLogin();
                    previous = ~0u;
                }
            }
        } else if (app.tileHubVisible()) {
            // Tile home: the d-pad moves the focus ring and confirm opens a tile. Moving works
            // while a background load is running; input is ignored during the launch animation.
            if (pressed & PSP_CTRL_START) break;
            if (app.tileLaunching()) {
            } else if (pressed & PSP_CTRL_SELECT) {
                networkDialog();
                app.refresh();
                previous = ~0u;
            } else if (pressed & confirmButton) app.tileActivate();
            else if (pressed & PSP_CTRL_UP) app.tileMove(0, -1);
            else if (pressed & PSP_CTRL_DOWN) app.tileMove(0, 1);
            else if (pressed & PSP_CTRL_LEFT) app.tileMove(-1, 0);
            else if (pressed & PSP_CTRL_RIGHT) app.tileMove(1, 0);
        } else if (app.busy()) {
            if (pressed & backButton) app.back();
            else if (pressed & PSP_CTRL_RTRIGGER) {
                app.shoulder(1);
            }
            if (pressed & PSP_CTRL_START) break;
        } else {
            if (pressed & PSP_CTRL_START) break;
            if (app.playbackFailed() && (pressed & (confirmButton | PSP_CTRL_TRIANGLE))) app.retry();
            else if (pressed & PSP_CTRL_LTRIGGER) app.shoulder(-1);
            else if (pressed & PSP_CTRL_RTRIGGER) app.shoulder(1);
            else if (app.commentPage()) {
                // Comments and reply threads: up/down move between comments, confirm expands one
                // in place, triangle opens the action card, left/right are like and reply list.
                if (app.commentMenuOpen()) {
                    if (pressed & (backButton | PSP_CTRL_TRIANGLE)) app.commentMenuClose();
                    else if (pressed & confirmButton) app.commentMenuActivate();
                    else if (pressed & PSP_CTRL_LEFT) app.commentMenuMove(-1);
                    else if (pressed & PSP_CTRL_RIGHT) app.commentMenuMove(1);
                } else if (pressed & backButton) app.back();
                else if (pressed & confirmButton) app.commentToggle();
                else if (pressed & PSP_CTRL_TRIANGLE) app.commentOpenMenu();
                else if (pressed & PSP_CTRL_SQUARE) app.beginComment();
                else if (pressed & PSP_CTRL_LEFT) app.commentQuickLike();
                else if (pressed & PSP_CTRL_RIGHT) app.commentQuickReplies();
                else if (pressed & PSP_CTRL_UP) ui_comment_move(app, -1);
                else if (pressed & PSP_CTRL_DOWN) ui_comment_move(app, 1);
            } else if (app.screen() == pspapp::Screen::Browse) {
                if (pressed & backButton) app.back();
                else if (pressed & confirmButton) app.activate();
                else if (pressed & PSP_CTRL_TRIANGLE) app.browseRefresh();
                else if (pressed & PSP_CTRL_SQUARE) app.communityShortcut();
                else if (pressed & PSP_CTRL_LEFT) app.browseMove(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.browseMove(1, 0);
                else if (pressed & PSP_CTRL_UP) ui_scroll_browse(app, -1);
                else if (pressed & PSP_CTRL_DOWN) ui_scroll_browse(app, 1);
            } else if (app.screen() == pspapp::Screen::Detail) {
                if (pressed & backButton) app.back();
                else if (pressed & confirmButton) app.requestPlayback();
                else if (pressed & PSP_CTRL_SQUARE) app.toggleLater();
                else if (pressed & PSP_CTRL_TRIANGLE) {
                    if (!app.detailReady()) app.retry();
                    else app.openBrowse({pspapp::BrowseKind::Space, app.detail().card.mid});
                } else if (pressed & PSP_CTRL_UP) app.moveSelection(0, -1);
                else if (pressed & PSP_CTRL_DOWN) app.moveSelection(0, 1);
                else if (pressed & PSP_CTRL_LEFT) app.moveSelection(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.moveSelection(1, 0);
            } else if (app.screen() == pspapp::Screen::Mine) {
                if (pressed & PSP_CTRL_SQUARE) app.communityShortcut();
                else if (pressed & backButton) app.back();
                else if (pressed & confirmButton) app.activate();
                else if (pressed & PSP_CTRL_UP) app.moveSelection(0, -1);
                else if (pressed & PSP_CTRL_DOWN) app.moveSelection(0, 1);
                else if (pressed & PSP_CTRL_LEFT) app.moveSelection(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.moveSelection(1, 0);
            } else {
                if (pressed & confirmButton) {
                    if (app.searchInputSelected()) {
                        const std::string keyword = ui_input_text("搜索 Bilibili", app.query());
                        if (!keyword.empty()) app.search(keyword);
                        previous = ~0u;
                    } else {
                        app.activate();
                    }
                } else if (pressed & backButton) {
                    app.back();
                } else if (pressed & PSP_CTRL_SQUARE) {
                    const std::string keyword = ui_input_text("搜索 Bilibili", app.query());
                    if (!keyword.empty()) app.search(keyword);
                    previous = ~0u;
                } else if (pressed & PSP_CTRL_TRIANGLE) {
                    if (app.tab() == pspapp::Tab::Search && app.searchView() == pspapp::SearchView::History)
                        app.removeSearchHistory();
                    else app.refresh();
                } else if (pressed & PSP_CTRL_SELECT) {
                    networkDialog();
                    app.refresh();
                    previous = ~0u;
                } else if (pressed & PSP_CTRL_LEFT) {
                    app.moveSelection(-1, 0);
                } else if (pressed & PSP_CTRL_RIGHT) {
                    app.moveSelection(1, 0);
                } else if (pressed & PSP_CTRL_UP) {
                    app.moveSelection(0, -1);
                } else if (pressed & PSP_CTRL_DOWN) {
                    app.moveSelection(0, 1);
                }
            }
        }
        if (app.settingsInput() != pspapp::App::SettingsInput::None && !app.busy()) {
            bool confirmed = false;
            const std::string text = ui_input_text(app.settingsInputTitle(), app.settingsInputInitial(), &confirmed);
            if (confirmed) app.submitSettingsInput(text);
            else app.cancelSettingsInput();
            previous = ~0u;
        }
        if (app.communityInputPending() && !app.busy()) {
            bool confirmed = false;
            const std::string text = ui_input_text("撰写内容", app.communityDraft(), &confirmed);
            if (confirmed) app.submitCommunityInput(text);
            else app.cancelCommunityInput();
            previous = ~0u;
        }
        ui_render(app);
    }

    transfer_cancel = 1;
    WLOG_I(WLC_CLIENT, "shutdown app_begin");
    app.shutdown();
    WLOG_I(WLC_CLIENT, "shutdown network_begin");
    network_close();
    WLOG_I(WLC_CLIENT, "shutdown ui_begin");
    ui_shutdown();
    logMemory("shutdown");
    wili_log_sink_shutdown(); // writes pending lines and stops the writer thread
    wili_log_shutdown();
    sceKernelExitGame();
    return 0;
}
