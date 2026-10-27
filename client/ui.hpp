#pragma once

#include <string>

#include "app.hpp"

void ui_init();
void ui_shutdown();
void ui_resume();
void ui_dialog_frame();
void ui_dialog_present();
void ui_render(const pspapp::App& app);
void ui_select_comment(pspapp::App& app, int direction);
void ui_scroll_browse(pspapp::App& app, int direction);
void ui_scroll_comments(pspapp::App& app, int direction);
void ui_scroll_document(pspapp::App& app, int direction);
std::string ui_input_text(const std::string& title, const std::string& initial, bool* confirmed = nullptr);

#ifdef BILI_UI_REVIEW
void ui_render_review(int page, int focus, unsigned player_flags, int player_seconds,
                      int player_control, int player_quality, int player_danmaku,
                      int player_part, int player_aspect);
#endif
