#include <cstdio>
#include <cstring>
#include <string>
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
#include "storage_paths.h"
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

void logMemory(const char* phase) {
    const auto heap = mallinfo();
    std::printf("MEM %s heap_used=%u heap_arena_free=%u system_free=%u system_max=%u\n",
                phase, static_cast<unsigned>(heap.uordblks), static_cast<unsigned>(heap.fordblks),
                sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
}

int exitCallback(int, int, void*) {
    exiting = 1;
    transfer_cancel = 1;
    psp_video_stop();
    return 0;
}

int callbackThread(SceSize, void*) {
    const int id = sceKernelCreateCallback("wiliwili_exit", exitCallback, nullptr);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
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
    const char* root = wili_storage_from_client(argc > 0 && argv ? argv[0] : nullptr);
    if (!root || chdir(root) < 0) {
        std::printf("CLIENT cannot open package root; start through EBOOT.PBP\n");
        sceKernelDelayThread(5000000);
        sceKernelExitGame();
        return 1;
    }
    client_storage_index = root == wili_storage_roots[0] ? 0 : root == wili_storage_roots[1] ? 1 : 2;
    client_storage_device = std::strncmp(root, "ms0:", 4) == 0 ? "ms0:"
                          : std::strncmp(root, "ef0:", 4) == 0 ? "ef0:" : "host0:";
    std::printf("CLIENT root=%s\n", root);
    std::printf("CLIENT heap_kib=%d default_rgb_frames=%d\n",
                BILI_CLIENT_HEAP_KB, pspapp::defaultPlaybackBufferFrames());
    const int callback = sceKernelCreateThread("wiliwili_callbacks", callbackThread, 0x11, 0x1000,
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
        app.setStatus("请选择 Wi-Fi 连接 · ○ 返回离线首页");
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
            logMemory("before_playback");
            PspVideoOptions options{};
            options.path = app.playback().path;
            options.log_root = CLIENT_ROOT;
            options.source = bili_playback_source(&app.playback());
            options.audio_source = bili_playback_audio_source(&app.playback());
            options.start_ms = app.playbackStartMs();
            options.rgb_buffer_frames = app.preferences().playbackBufferFrames;
            PspVideoResult result{};
            const int error = psp_video_play(&options, &result);
            logMemory("after_playback");
            ui_resume();
            app.finishPlayback(error, result.frames, result.eof, result.cancelled, result.error_stage, result.position_ms);
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
            const bool adjustingSetting = app.screen() == pspapp::Screen::Settings && app.settingSelection() < 5;
            pressed |= adjustingSetting ? direction & (PSP_CTRL_UP | PSP_CTRL_DOWN) : direction;
            repeatAt = now + (app.preferences().slowRepeat ? 180000 : 90000);
        }
        previous = pad.Buttons;

        if (app.utilityPage() && !app.busy()) {
            if (pressed & PSP_CTRL_START) break;
            if (pressed & PSP_CTRL_CIRCLE) app.back();
            else if (pressed & PSP_CTRL_CROSS) {
                if (app.screen() == pspapp::Screen::Settings && app.settingSelection() == 5) {
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
            if (pressed & PSP_CTRL_CIRCLE)
                app.back();
            else if (pressed & PSP_CTRL_TRIANGLE)
                app.refreshLogin();
            else if (!app.busy()) {
                if (pressed & PSP_CTRL_CROSS)
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
        } else if (app.busy()) {
            if (pressed & PSP_CTRL_CIRCLE) app.cancelRequest();
            else if (pressed & PSP_CTRL_RTRIGGER) {
                if (app.screen() == pspapp::Screen::Detail) app.requestComments();
                else if (app.screen() == pspapp::Screen::Comments) app.loadMoreComments();
            }
            if (pressed & PSP_CTRL_START) break;
        } else {
            if (pressed & PSP_CTRL_START) break;
            if (app.screen() == pspapp::Screen::Browse) {
                if (pressed & PSP_CTRL_CIRCLE) app.back();
                else if (pressed & PSP_CTRL_CROSS) app.activate();
                else if (pressed & PSP_CTRL_TRIANGLE) app.browseRefresh();
                else if (pressed & PSP_CTRL_RTRIGGER) app.browseMore();
                else if (pressed & PSP_CTRL_SQUARE) app.communityShortcut();
                else if (pressed & PSP_CTRL_LEFT) app.browseMove(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.browseMove(1, 0);
                else if (pressed & PSP_CTRL_UP) ui_scroll_browse(app, -1);
                else if (pressed & PSP_CTRL_DOWN) ui_scroll_browse(app, 1);
            } else if (app.screen() == pspapp::Screen::Detail) {
                if (pressed & PSP_CTRL_CIRCLE) app.back();
                else if (pressed & PSP_CTRL_CROSS) app.requestPlayback();
                else if (pressed & PSP_CTRL_RTRIGGER) app.requestComments();
                else if (pressed & PSP_CTRL_SQUARE) app.toggleLater();
                else if (pressed & PSP_CTRL_LTRIGGER) app.openVideoActions();
                else if (pressed & PSP_CTRL_TRIANGLE) app.openBrowse({pspapp::BrowseKind::Space, app.detail().card.mid});
                else if (pressed & PSP_CTRL_UP) app.moveSelection(0, -1);
                else if (pressed & PSP_CTRL_DOWN) app.moveSelection(0, 1);
                else if (pressed & PSP_CTRL_LEFT) app.moveSelection(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.moveSelection(1, 0);
            } else if (app.screen() == pspapp::Screen::Comments) {
                if (pressed & PSP_CTRL_CIRCLE) app.back();
                else if (pressed & PSP_CTRL_RTRIGGER) app.loadMoreComments();
                else if (pressed & PSP_CTRL_CROSS) app.commentActions();
                else if (pressed & PSP_CTRL_SQUARE) app.beginComment();
                else if (pressed & PSP_CTRL_LEFT) ui_select_comment(app, -1);
                else if (pressed & PSP_CTRL_RIGHT) ui_select_comment(app, 1);
                else if (pressed & PSP_CTRL_UP) ui_scroll_comments(app, -1);
                else if (pressed & PSP_CTRL_DOWN) ui_scroll_comments(app, 1);
            } else if (app.screen() == pspapp::Screen::Mine) {
                if (pressed & PSP_CTRL_SQUARE) app.communityShortcut();
                else if (pressed & PSP_CTRL_CIRCLE) app.back();
                else if (pressed & PSP_CTRL_CROSS) app.activate();
                else if (pressed & PSP_CTRL_UP) app.moveSelection(0, -1);
                else if (pressed & PSP_CTRL_DOWN) app.moveSelection(0, 1);
                else if (pressed & PSP_CTRL_LEFT) app.moveSelection(-1, 0);
                else if (pressed & PSP_CTRL_RIGHT) app.moveSelection(1, 0);
                else if (pressed & PSP_CTRL_LTRIGGER) app.switchTab(-1);
                else if (pressed & PSP_CTRL_RTRIGGER) app.switchTab(1);
            } else {
                if (pressed & PSP_CTRL_CROSS) {
                    if (app.searchInputSelected()) {
                        const std::string keyword = ui_input_text("搜索 Bilibili", app.query());
                        if (!keyword.empty()) app.search(keyword);
                        previous = ~0u;
                    } else {
                        app.activate();
                    }
                } else if (pressed & PSP_CTRL_CIRCLE) {
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
                } else if (pressed & PSP_CTRL_LTRIGGER) {
                    app.switchTab(-1);
                } else if (pressed & PSP_CTRL_RTRIGGER) {
                    app.switchTab(1);
                }
            }
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
    app.shutdown();
    network_close();
    ui_shutdown();
    logMemory("shutdown");
    sceKernelExitGame();
    return 0;
}
