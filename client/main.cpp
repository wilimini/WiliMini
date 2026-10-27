#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
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
#ifdef BILI_DEVICE_PLAYBACK_TEST
#include "../tests/psp_playback_test.hpp"
#endif

PSP_MODULE_INFO("Wiliwili PSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(24 * 1024);

namespace {

volatile int exiting = 0;

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

#ifdef BILI_DEVICE_SMOKE
void smokeLog(const char* format, ...) {
    FILE* file = std::fopen(CLIENT_ROOT "/smoke-stage.txt", "a");
    if (!file) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    std::fclose(file);
}

void writeSmokeResult(const pspapp::App& app, int error, unsigned frames, int eof,
                      int cancelled, bool streaming, const std::string& path) {
    FILE* file = std::fopen(CLIENT_ROOT "/smoke-cpp.txt", "w");
    if (!file) return;
    std::fprintf(file,
                 "error=%d frames=%u eof=%d cancelled=%d streaming=%d\n"
                 "free_kb=%u max_kb=%u comments=%zu comments_end=%d\n"
                 "status=%s\nnetwork_error=%s\npath=%s\n",
                 error, frames, eof, cancelled, streaming ? 1 : 0,
                 sceKernelTotalFreeMemSize() / 1024, sceKernelMaxFreeMemSize() / 1024,
                 app.comments().size(), app.commentsEnd() ? 1 : 0,
                 app.status().c_str(), network_error, path.c_str());
    std::fclose(file);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef BILI_DEVICE_SMOKE
    smokeLog("main");
#endif
    const int callback = sceKernelCreateThread("wiliwili_callbacks", callbackThread, 0x11, 0x1000,
                                               PSP_THREAD_ATTR_USER, nullptr);
    if (callback >= 0) sceKernelStartThread(callback, 0, nullptr);
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    sceIoMkdir(CLIENT_ROOT, 0777);
    sceIoMkdir(CLIENT_ROOT "/cache", 0777);
    chdir(CLIENT_ROOT);

    ui_init();
#ifdef BILI_DEVICE_PLAYBACK_TEST
    const int playbackTest = controlledPlaybackTest();
    ui_shutdown();
    sceKernelExitGame();
    return playbackTest;
#endif
#ifdef BILI_UI_REVIEW
    // Offline fixture build: no networking, credentials, or actual playback.
    int reviewPage = 0, reviewFocus = 0;
    unsigned reviewPlayerFlags = 0;
    int reviewPlayerSeconds = 198;
    int reviewPlayerControl = 0, reviewPlayerQuality = 0, reviewPlayerDanmaku = 0;
    int reviewPlayerPart = 1, reviewPlayerAspect = 0;
    uint64_t reviewPlayerTick = sceKernelGetSystemTimeWide();
    uint64_t reviewOsdDeadline = 0;
    unsigned previousButtons = 0;
    FILE* reviewAutoFile = std::fopen("review-auto.txt", "r");
    const bool reviewAuto = reviewAutoFile != nullptr;
    if (reviewAutoFile) std::fclose(reviewAutoFile);
    const auto reviewStart = sceKernelGetSystemTimeWide();
    while (!exiting) {
        SceCtrlData input{};
        sceCtrlPeekBufferPositive(&input, 1);
        unsigned pressed = input.Buttons & ~previousButtons;
        previousButtons = input.Buttons;
        const uint64_t reviewNow = sceKernelGetSystemTimeWide();
        if (reviewPage == 7 && !(reviewPlayerFlags & 2u)) {
            reviewPlayerSeconds += static_cast<int>((reviewNow - reviewPlayerTick) / 1000000u);
            if (reviewPlayerSeconds > 760) reviewPlayerSeconds = 760;
        }
        reviewPlayerTick = reviewNow;
        if (reviewPage == 7 && (reviewPlayerFlags & 1u) && !(reviewPlayerFlags & 2u) &&
            !(reviewPlayerFlags & 4u) && reviewOsdDeadline && reviewNow >= reviewOsdDeadline)
            reviewPlayerFlags &= ~1u;
        if (reviewAuto) reviewPage = ((reviewNow - reviewStart) / 2000000) % 14;
        else {
            if (pressed & PSP_CTRL_RTRIGGER) reviewPage = (reviewPage + 1) % 14;
            if (pressed & PSP_CTRL_LTRIGGER) reviewPage = (reviewPage + 13) % 14;
            if (reviewPage == 0) {
                reviewFocus %= 4;
                if ((pressed & PSP_CTRL_RIGHT) && reviewFocus % 2 == 0) ++reviewFocus;
                if ((pressed & PSP_CTRL_LEFT) && reviewFocus % 2 == 1) --reviewFocus;
                if ((pressed & PSP_CTRL_DOWN) && reviewFocus < 2) reviewFocus += 2;
                if ((pressed & PSP_CTRL_UP) && reviewFocus >= 2) reviewFocus -= 2;
            } else {
                if (pressed & (PSP_CTRL_RIGHT | PSP_CTRL_DOWN)) ++reviewFocus;
                if (pressed & (PSP_CTRL_LEFT | PSP_CTRL_UP)) reviewFocus = reviewFocus > 0 ? reviewFocus - 1 : 0;
            }
            if (reviewPage == 7) {
                if (pressed & PSP_CTRL_TRIANGLE) {
                    reviewPlayerFlags ^= 1u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if ((pressed & PSP_CTRL_CROSS) && reviewPlayerControl == 0) {
                    reviewPlayerFlags ^= 2u;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_START) {
                    reviewPlayerFlags ^= 2u;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_LEFT) {
                    reviewPlayerControl = (reviewPlayerControl + 5) % 6;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_RIGHT) {
                    reviewPlayerControl = (reviewPlayerControl + 1) % 6;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_UP) {
                    if (reviewPlayerControl == 1) reviewPlayerSeconds = reviewPlayerSeconds < 750 ? reviewPlayerSeconds + 10 : 760;
                    else if (reviewPlayerControl == 2) reviewPlayerQuality = (reviewPlayerQuality + 1) % 3;
                    else if (reviewPlayerControl == 3) reviewPlayerDanmaku = (reviewPlayerDanmaku + 1) % 2;
                    else if (reviewPlayerControl == 4) reviewPlayerPart = reviewPlayerPart == 1 ? 2 : 1;
                    else if (reviewPlayerControl == 5) reviewPlayerAspect = (reviewPlayerAspect + 1) % 3;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_DOWN) {
                    if (reviewPlayerControl == 1) reviewPlayerSeconds = reviewPlayerSeconds > 10 ? reviewPlayerSeconds - 10 : 0;
                    else if (reviewPlayerControl == 2) reviewPlayerQuality = (reviewPlayerQuality + 2) % 3;
                    else if (reviewPlayerControl == 3) reviewPlayerDanmaku = (reviewPlayerDanmaku + 1) % 2;
                    else if (reviewPlayerControl == 4) reviewPlayerPart = reviewPlayerPart == 1 ? 2 : 1;
                    else if (reviewPlayerControl == 5) reviewPlayerAspect = (reviewPlayerAspect + 2) % 3;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_SQUARE) {
                    reviewPlayerControl = 5;
                    reviewPlayerFlags = (reviewPlayerFlags | 1u) & ~8u;
                    reviewOsdDeadline = reviewNow + 3000000u;
                }
                if (pressed & PSP_CTRL_CIRCLE) {
                    if (reviewPlayerFlags & 1u) reviewPlayerFlags &= ~1u;
                    else reviewPage = 6;
                }
                if (reviewPlayerControl == 3 && (pressed & PSP_CTRL_CROSS)) reviewPlayerDanmaku ^= 1;
            } else {
                if (pressed & PSP_CTRL_CROSS) reviewPage = reviewPage == 0 ? 6 : reviewPage == 6 ? 7 : (reviewPage + 1) % 14;
                if (pressed & PSP_CTRL_CIRCLE) reviewPage = 0;
            }
        }
        if (reviewPage != 7) reviewPlayerFlags = 0;
        ui_render_review(reviewPage, reviewFocus, reviewPlayerFlags, reviewPlayerSeconds,
                         reviewPlayerControl, reviewPlayerQuality, reviewPlayerDanmaku,
                         reviewPlayerPart, reviewPlayerAspect);
    }
    ui_shutdown();
    sceKernelExitGame();
    return 0;
#endif
    pspapp::App app;
    app.init();
    ui_render(app);
#ifdef BILI_DEVICE_SMOKE
    smokeLog("init cards=%zu", app.cards().size());
#endif

#ifdef BILI_DEVICE_OSK_TEST
    app.setTab(pspapp::Tab::Search);
    ui_render(app);
    const std::string osk_text = ui_input_text("PSP OSK 测试", "");
    FILE* osk_result = std::fopen(CLIENT_ROOT "/osk-result.txt", "w");
    if (osk_result) {
        std::fprintf(osk_result, "%s\n", osk_text.c_str());
        std::fclose(osk_result);
    }
    ui_shutdown();
    sceKernelExitGame();
    return 0;
#endif

#ifdef BILI_DEVICE_OFFLINE_UI
    const int net = -1;
    app.setStatus("离线缓存 · PPSSPP UI 预览");
#else
    const auto initError = pspapp::WifiManager{}.initialize();
    const int net = initError ? initError.rawCode : 0;
#endif
#ifdef BILI_DEVICE_SMOKE
    smokeLog("network init=%d ready=%d profile=%d", net, network_ready(), network_has_profile());
#endif
#ifndef BILI_DEVICE_OFFLINE_UI
    if (net < 0) {
        app.setStatus(pspapp::errorMessage(initError));
        pspapp::logError(CLIENT_ROOT "/last-error.txt", initError);
    }
    if (net >= 0 && !network_ready() && !network_has_profile()) {
        app.setStatus("请选择 Wi-Fi 连接 · ○ 返回离线首页");
        ui_render(app);
        networkDialog();
    }
#endif
    if (net >= 0) {
#ifdef BILI_DEVICE_SMOKE_SEARCH
        app.search("wiliwili");
#elif defined(BILI_DEVICE_SMOKE_HOTS)
        app.setTab(pspapp::Tab::Hots);
#else
        app.refresh();
#endif
    }
#ifdef BILI_DEVICE_SMOKE
    smokeLog("refresh requested cards=%zu", app.cards().size());
#endif

    unsigned previous = 0;
    unsigned heldDirection = 0;
    int64_t repeatAt = 0;
#ifdef BILI_DEVICE_SMOKE
    int smokeStep = 0;
    bool smokeCommentsMore = false;
#ifdef BILI_DEVICE_SMOKE_NEXT_PAGE
    size_t smokeInitialCards = 0;
#endif
    const int64_t smokeDeadline = sceKernelGetSystemTimeWide() + 180000000LL;
    uint64_t smokeLastStage = 0;
    app.setStatus("SMOKE · 等待首页");
#endif

    while (!exiting) {
        app.poll();

#ifdef BILI_DEVICE_SMOKE
        const uint64_t smokeNow = (uint64_t)sceKernelGetSystemTimeWide();
        if (smokeNow - smokeLastStage >= 1000000u) {
            smokeLastStage = smokeNow;
            smokeLog("loop step=%d busy=%d screen=%d cards=%zu ready=%d status=%s",
                     smokeStep, app.busy() ? 1 : 0, static_cast<int>(app.screen()), app.cards().size(),
                     app.playbackReady() ? 1 : 0, app.status().c_str());
        }
        if (app.screen() == pspapp::Screen::Home && !app.busy() && !app.cards().empty() && smokeStep == 0) {
#ifdef BILI_DEVICE_SMOKE_NEXT_PAGE
            if (!smokeInitialCards) {
                smokeInitialCards = app.cards().size();
                while (app.selection() + 3 < app.cards().size()) app.moveSelection(0, 1);
                app.moveSelection(0, 1);
            } else if (app.cards().size() > smokeInitialCards) {
                smokeStep = 1;
#ifdef BILI_DEVICE_SMOKE_INDEX
                size_t target = smokeInitialCards + BILI_DEVICE_SMOKE_INDEX;
                if (target >= app.cards().size()) target = app.cards().size() - 1;
                while (app.selection() / 2 < target / 2) app.moveSelection(0, 1);
                if (app.selection() % 2 != target % 2)
                    app.moveSelection(target % 2 ? 1 : -1, 0);
#endif
                app.activate();
            }
#else
            smokeStep = 1;
#ifdef BILI_DEVICE_SMOKE_INDEX
            size_t target = BILI_DEVICE_SMOKE_INDEX;
            if (target >= app.cards().size()) target = app.cards().size() - 1;
            while (app.selection() / 2 < target / 2) app.moveSelection(0, 1);
            if (app.selection() % 2 != target % 2)
                app.moveSelection(target % 2 ? 1 : -1, 0);
#endif
            app.activate();
#endif
        } else if (app.screen() == pspapp::Screen::Detail && !app.busy() && smokeStep == 1) {
            smokeStep = 2;
            app.requestComments();
        } else if (!app.busy() && smokeStep == 2) {
            if (app.screen() == pspapp::Screen::Comments && !smokeCommentsMore && !app.commentsEnd() &&
                !app.comments().empty()) {
                smokeCommentsMore = true;
                app.loadMoreComments();
            } else {
                smokeStep = 3;
                if (app.screen() == pspapp::Screen::Comments) app.back();
            }
        } else if (app.screen() == pspapp::Screen::Detail && !app.busy() && smokeStep == 3) {
            smokeStep = 4;
            app.requestPlayback();
        }
        if (smokeStep == 4 && !app.busy() && !app.playbackReady()) {
            writeSmokeResult(app, -3, 0, 0, 0, false, {});
            break;
        }
        if (smokeStep < 4 && sceKernelGetSystemTimeWide() > smokeDeadline) {
            writeSmokeResult(app, -2, 0, 0, 0, false, {});
            break;
        }
#endif

        if (app.playbackReady()) {
            ui_render(app);
#ifdef BILI_DEVICE_SMOKE
            const bool streaming = app.playback().streaming != 0;
            const std::string playback_path = app.playback().path;
#endif
            PspVideoOptions options{};
            options.path = app.playback().path;
            options.log_root = CLIENT_ROOT;
            options.source = bili_playback_source(&app.playback());
            options.audio_source = bili_playback_audio_source(&app.playback());
            options.start_ms = app.playbackStartMs();
#ifdef BILI_DEVICE_SMOKE
            options.sample = 1;
#ifdef BILI_DEVICE_SMOKE_FULL
            options.sample = 0;
#endif
#endif
            PspVideoResult result{};
            const int error = psp_video_play(&options, &result);
            ui_resume();
            app.finishPlayback(error, result.frames, result.eof, result.cancelled, result.error_stage, result.position_ms);
#ifdef BILI_DEVICE_SMOKE
            smokeLog("playback returned error=%d frames=%u", error, result.frames);
            smokeLog("writing result");
            writeSmokeResult(app, error, result.frames, result.eof, result.cancelled,
                             streaming, playback_path);
            smokeLog("result written");
            break;
#endif
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
            const bool adjustingSetting = app.screen() == pspapp::Screen::Settings && app.settingSelection() < 4;
            pressed |= adjustingSetting ? direction & (PSP_CTRL_UP | PSP_CTRL_DOWN) : direction;
            repeatAt = now + (app.preferences().slowRepeat ? 180000 : 90000);
        }
        previous = pad.Buttons;

        if (app.utilityPage() && !app.busy()) {
            if (pressed & PSP_CTRL_START) break;
            if (pressed & PSP_CTRL_CIRCLE) app.back();
            else if (pressed & PSP_CTRL_CROSS) {
                if (app.screen() == pspapp::Screen::Settings && app.settingSelection() == 4) {
#ifdef BILI_DEVICE_OFFLINE_UI
                    app.setStatus("离线 UI 验证：请在正常构建中打开系统 Wi-Fi");
#else
                    networkDialog();
                    app.setStatus(network_ready() ? "Wi-Fi 已连接" : "已返回 · 未连接 Wi-Fi");
                    previous = ~0u;
#endif
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
            if (pressed & PSP_CTRL_CIRCLE) transfer_cancel = 1;
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
#ifdef BILI_DEVICE_OFFLINE_UI
        // State evidence for deterministic controller replays, excluded from normal builds.
        static std::string lastUiState;
        char uiState[768];
        std::snprintf(
            uiState, sizeof(uiState),
            "{\"screen\":%d,\"selection\":%zu,\"part\":%zu,\"focus\":%d,\"comments\":%zu,"
            "\"scroll\":%d,\"end\":%d,\"later\":%zu,\"busy\":%d,\"tab\":%d,\"search_view\":%d,"
            "\"topics\":%zu,\"cards\":%zu,\"search_error\":%d,\"auth_state\":%d,\"auth_error\":%d,"
            "\"setting\":%zu,\"dark\":%d,\"body_size\":%d,\"show_covers\":%d,\"slow_repeat\":%d,"
            "\"cover_files\":%zu,\"clearing_covers\":%d,\"cover_generation\":%u,\"document_scroll\":%d}",
            static_cast<int>(app.screen()), app.selection(), app.detailPart(),
            static_cast<int>(app.focusArea()), app.comments().size(), app.commentScroll(),
            app.commentsEnd(), app.later().size(), app.busy(), static_cast<int>(app.tab()),
            static_cast<int>(app.searchView()), app.searchTopics().size(), app.cards().size(),
            !app.searchError().empty(), static_cast<int>(app.auth().state), bool(app.authError()),
            app.settingSelection(), app.preferences().darkTheme, app.preferences().bodySize,
            app.preferences().showCovers, app.preferences().slowRepeat, app.coverFiles(),
            app.clearingCovers(), app.coverGeneration(), app.documentScroll());
        auto traceState = nlohmann::json::parse(uiState);
        if (app.hasBrowse()) {
            traceState["browse_kind"] = static_cast<int>(app.browsePage().request.kind);
            traceState["browse_items"] = app.browsePage().items.size();
            traceState["browse_selection"] = app.browseSelection();
            traceState["browse_page"] = app.browsePage().request.page;
            traceState["browse_more"] = app.browsePage().hasMore;
            traceState["browse_error"] = bool(app.browseError());
        }
        const std::string traceRecord = traceState.dump();
        if (lastUiState != traceRecord) {
            if (FILE* trace = std::fopen(CLIENT_ROOT "/ui-trace.jsonl", "a")) {
                std::fprintf(trace, "%s\n", traceRecord.c_str());
                std::fclose(trace);
            }
            lastUiState = traceRecord;
        }
#endif
        ui_render(app);
    }

    transfer_cancel = 1;
    app.shutdown();
    network_close();
    ui_shutdown();
    sceKernelExitGame();
    return 0;
}
