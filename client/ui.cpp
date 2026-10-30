#include "ui.hpp"
#include "bitmap_font.hpp"
#include "assets/wiliwili/sidebar.hpp"
#include "settings_pages.hpp"

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <pspgum.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_osk.h>

extern "C" {
#include <jpeglib.h>
}

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 272;
constexpr int kCoverWidth = 288;
constexpr int kCoverHeight = 162;
constexpr int kMaxCovers = 10;

constexpr uint32_t color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) | r;
}

uint32_t kBackground = color(242, 243, 245);
uint32_t kPanel = color(255, 255, 255);
uint32_t kPanelRaised = color(232, 235, 239);
uint32_t kLine = color(221, 224, 229);
uint32_t kText = color(43, 46, 53);
uint32_t kTextDim = color(117, 123, 134);
uint32_t kPink = color(204, 65, 113);
constexpr uint32_t kBlue = color(0, 174, 236);
constexpr uint32_t kBlack = color(0, 0, 0);
int bodySize = 14;
bool showCovers = true;

void applyPreferences(const pspapp::Preferences& settings) {
    bodySize = settings.bodySize;
    showCovers = settings.showCovers;
    kBackground = settings.darkTheme ? color(35, 37, 42) : color(242, 243, 245);
    kPanel = settings.darkTheme ? color(42, 44, 50) : color(255, 255, 255);
    kPanelRaised = settings.darkTheme ? color(48, 51, 58) : color(232, 235, 239);
    kLine = settings.darkTheme ? color(66, 69, 78) : color(221, 224, 229);
    kText = settings.darkTheme ? color(235, 237, 242) : color(43, 46, 53);
    kTextDim = settings.darkTheme ? color(163, 170, 184) : color(117, 123, 134);
    kPink = settings.darkTheme ? color(255, 128, 170) : color(204, 65, 113);
}

std::string uiText(const pspapp::App& app, pspapp::TextId id,
                   std::initializer_list<pspapp::TextArg> args = {}) {
    return pspapp::formatText(id, app.preferences().locale, args);
}

uint32_t commands[65536] __attribute__((aligned(16)));
void* draw_buffer = nullptr;

struct CoverSlot {
    std::string key;
    std::vector<uint32_t> pixels;
    int width = kCoverWidth, height = kCoverHeight;
};

CoverSlot covers[kMaxCovers];
size_t next_cover = 0;

struct JpegFailure {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void jpegFailure(j_common_ptr context) {
    auto* failure = reinterpret_cast<JpegFailure*>(context->err);
    longjmp(failure->jump, 1);
}

uint32_t* framebuffer() {
    return reinterpret_cast<uint32_t*>(0x44000000u + reinterpret_cast<uintptr_t>(draw_buffer));
}

void fillRect(int x, int y, int width, int height, uint32_t shade) {
    if (width <= 0 || height <= 0) return;
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(kWidth, x + width);
    const int bottom = std::min(kHeight, y + height);
    if (left >= right || top >= bottom) return;
    uint32_t* destination = framebuffer();
    for (int row = top; row < bottom; ++row) {
        uint32_t* line = destination + row * 512;
        std::fill(line + left, line + right, shade);
    }
}

void beginFrame() {
    sceGuStart(GU_DIRECT, commands);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuScissor(0, 0, kWidth, kHeight);
}

void presentFrame() {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    draw_buffer = sceGuSwapBuffers();
}

int fontSize(float scale) { return scale >= 0.8f ? 16 : scale >= 0.6f ? bodySize : 12; }

void blendPixel(int x, int y, uint32_t shade, unsigned alpha) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight || !alpha) return;
    uint32_t& destination = framebuffer()[y * 512 + x];
    uint32_t result = 0xff000000;
    for (int shift = 0; shift < 24; shift += 8)
        result |= (((((shade >> shift) & 255) * alpha +
                     ((destination >> shift) & 255) * (255 - alpha)) / 255) << shift);
    destination = result;
}

void drawText(int x, int y, float scale, uint32_t shade, std::string_view text, int maxWidth,
              bool centered = false, int clipTop = 0, int clipBottom = kHeight) {
    const int size = fontSize(scale);
    const int right = std::min(kWidth, x + maxWidth);
    if (centered) x += std::max(0, (maxWidth - bitmapMeasure(text, size)) / 2);
    for (size_t pos = 0; pos < text.size();) {
        const auto* glyph = bitmapGlyph(bitmapNext(text, pos), size);
        if (!glyph) continue;
        if (x + glyph->advance > right) break;
        for (int yy = 0; yy < glyph->height; ++yy) {
            if (y + yy < clipTop || y + yy >= clipBottom) continue;
            for (int xx = 0; xx < glyph->width && x + xx < right; ++xx) {
                const int pixel = yy * glyph->width + xx;
                blendPixel(x + xx, y + yy, shade, ((glyph->alpha[pixel / 2] >> ((pixel % 2) * 4)) & 15) * 17);
            }
        }
        x += glyph->advance;
    }
}

std::vector<std::string> textLines(std::string_view text, int size, int maxWidth) {
    std::vector<std::string> lines;
    int used = 0;
    std::string line;
    for (size_t pos = 0; pos < text.size();) {
        const size_t start = pos;
        const uint32_t cp = bitmapNext(text, pos);
        const int advance = bitmapAdvance(cp, size);
        if (cp == '\n' || used + advance > maxWidth) {
            lines.push_back(line);
            line.clear(); used = 0;
        }
        if (cp != '\n') { line.append(text, start, pos - start); used += advance; }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

int drawWrapped(int x, int y, float scale, uint32_t shade, std::string_view text, int maxWidth,
                int maxLines, int lineHeight, int clipTop = 0, int clipBottom = kHeight) {
    const auto lines = textLines(text, fontSize(scale), maxWidth);
    lineHeight = std::max(lineHeight, fontSize(scale) + 4);
    const int count = std::min(maxLines, static_cast<int>(lines.size()));
    for (int i = 0; i < count; ++i)
        drawText(x, y + i * lineHeight, scale, shade, lines[i], maxWidth, false, clipTop, clipBottom);
    return count;
}

std::string formatDuration(int seconds) {
    if (seconds < 0) seconds = 0;
    char text[24]{};
    if (seconds >= 3600)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    else
        std::snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
    return text;
}

std::string formatCount(uint64_t value) {
    char text[32]{};
    if (value >= 100000000ULL)
        std::snprintf(text, sizeof(text), "%.1f亿", static_cast<double>(value) / 100000000.0);
    else if (value >= 10000ULL)
        std::snprintf(text, sizeof(text), "%.1f万", static_cast<double>(value) / 10000.0);
    else
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    return text;
}

bool loadJpeg(const std::string& path, std::vector<uint32_t>& pixels, int* fitWidth = nullptr, int* fitHeight = nullptr,
              int outputWidth = kCoverWidth, int outputHeight = kCoverHeight) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    jpeg_decompress_struct decoder{};
    JpegFailure failure{};
    decoder.err = jpeg_std_error(&failure.base);
    failure.base.error_exit = jpegFailure;
    if (setjmp(failure.jump)) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE);
    if (decoder.image_width > 2048 || decoder.image_height > 2048) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        return false;
    }
    if (fitWidth && fitHeight) {
        outputWidth = 456;
        outputHeight = std::max(1, static_cast<int>(decoder.image_height * 456 / decoder.image_width));
        if (outputHeight > 215) {
            outputHeight = 215;
            outputWidth = std::max(1, static_cast<int>(decoder.image_width * 215 / decoder.image_height));
        }
        *fitWidth = outputWidth; *fitHeight = outputHeight;
    }
    while (decoder.image_width / (decoder.scale_denom * 2) >= static_cast<JDIMENSION>(outputWidth) && decoder.scale_denom < 8)
        decoder.scale_denom *= 2;
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    JSAMPARRAY row = (*decoder.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&decoder), JPOOL_IMAGE,
                                                  decoder.output_width * 3, 1);
    pixels.resize(static_cast<size_t>(outputWidth) * outputHeight);
    int destinationRow = 0;
    while (decoder.output_scanline < decoder.output_height) {
        const unsigned sourceRow = decoder.output_scanline;
        jpeg_read_scanlines(&decoder, row, 1);
        while (destinationRow < outputHeight &&
               static_cast<unsigned>(destinationRow * decoder.output_height / outputHeight) == sourceRow) {
            for (int x = 0; x < outputWidth; ++x) {
                const unsigned sourceX = static_cast<unsigned>(x * decoder.output_width / outputWidth);
                pixels[static_cast<size_t>(destinationRow) * outputWidth + x] =
                    color(row[0][sourceX * 3], row[0][sourceX * 3 + 1], row[0][sourceX * 3 + 2]);
            }
            ++destinationRow;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    std::fclose(file);
    return destinationRow == outputHeight;
}

CoverSlot* coverSlot(const std::string& bvid, int width = kCoverWidth, int height = kCoverHeight) {
    if (bvid.empty()) return nullptr;
    for (auto& slot : covers) {
        if (slot.key == bvid && slot.width == width && slot.height == height) return &slot;
    }
    CoverSlot& slot = covers[next_cover++ % kMaxCovers];
    slot.key = bvid;
    slot.width = width; slot.height = height;
    slot.pixels.clear();
    char path[256]{};
    std::snprintf(path, sizeof(path), "%s/cache/%s.jpg", CLIENT_ROOT, bvid.c_str());
    if (!loadJpeg(path, slot.pixels, nullptr, nullptr, width, height)) slot.pixels.clear();
    return &slot;
}

void drawCover(const std::string& bvid, int x, int y, int width, int height, bool detailed = false,
               int clipTop = 0, int clipBottom = kHeight) {
    CoverSlot* slot = showCovers ? coverSlot(bvid, width, height) : nullptr;
    if (!slot || slot->pixels.empty()) {
        const int top = std::max(y, clipTop);
        const int bottom = std::min(y + height, clipBottom);
        fillRect(x, top, width, bottom - top, kPanelRaised);
        drawText(x + 8, y + height / 2 - 7, detailed ? 0.72f : 0.55f, kTextDim, "bilibili", width - 16,
                 false, clipTop, clipBottom);
        return;
    }
    uint32_t* destination = framebuffer();
    for (int row = 0; row < height; ++row) {
        if (y + row < clipTop || y + row >= clipBottom) continue;
        const int sourceY = std::min(slot->height - 1, row * slot->height / height);
        const uint32_t* source = slot->pixels.data() + static_cast<size_t>(sourceY) * slot->width;
        uint32_t* line = destination + (y + row) * 512 + x;
        for (int column = 0; column < width; ++column) {
            const int sourceX = std::min(slot->width - 1, column * slot->width / width);
            line[column] = source[sourceX];
        }
    }
}

void drawHeader(const pspapp::App& app) {
    static const int iconY[] = {38, 71, 104, 191, 221};
    const bool navFocus = app.focusArea() == pspapp::FocusArea::Navigation;
    const int activeNav = app.utilityPage() ? 4 : navFocus ? app.navigationSelection() : app.tab() == pspapp::Tab::Mine ? 2 : 0;
    for (int icon = 0; icon < 5; ++icon) {
        for (int yy = 0; yy < 19; ++yy) for (int xx = 0; xx < 19; ++xx) {
            const unsigned pixel = sidebarPixels[icon][(yy * 22 / 19) * 22 + xx * 22 / 19];
            const unsigned alpha = ((pixel >> 12) & 15) * 17;
            blendPixel(6 + xx, iconY[icon] + yy,
                       app.preferences().darkTheme ? kTextDim : color(((pixel >> 8) & 15) * 17, ((pixel >> 4) & 15) * 17, (pixel & 15) * 17),
                       alpha);
        }
    }
    fillRect(0, iconY[activeNav] - 1, navFocus ? 3 : 2, 22, kPink);
    if (app.utilityPage()) {
        const char* title = pspapp::text(pspapp::TextId::page_settings_title, app.preferences().locale);
        switch (app.screen()) {
            case pspapp::Screen::Storage:
                title = pspapp::text(pspapp::TextId::page_storage_title, app.preferences().locale); break;
            case pspapp::Screen::Help:
                title = pspapp::text(pspapp::TextId::page_help_title, app.preferences().locale); break;
            case pspapp::Screen::About:
                title = pspapp::text(pspapp::TextId::page_about_title, app.preferences().locale); break;
            case pspapp::Screen::Licenses:
                title = pspapp::text(pspapp::TextId::page_licenses_title, app.preferences().locale); break;
            case pspapp::Screen::LicenseText:
                title = pspapp::text(pspapp::TextId::page_license_text_title, app.preferences().locale); break;
            default: break;
        }
        drawText(38, 3, 0.9f, kText, title, 390);
        fillRect(38, 26, 430, 1, kLine);
        return;
    }
    drawText(38, 3, 0.9f, kText, app.tab() == pspapp::Tab::Mine ? "我的" : app.tab() == pspapp::Tab::Search ? "搜索" : "首页", 290);
    fillRect(38, 26, 430, 1, kLine);
    if (app.tab() == pspapp::Tab::Search) {
        const char* meta = app.searchView() == pspapp::SearchView::History ? "历史" :
                           app.searchView() == pspapp::SearchView::Results ? "视频" : "热搜";
        drawText(430, 6, 0.5f, kTextDim, meta, 40);
        return;
    }
    if (app.tab() != pspapp::Tab::Mine) {
        const char* labels[] = {"推荐", "热门", "搜索", "发现"};
        for (int i = 0; i < 4; ++i) {
            drawText(39 + i * 56, 30, 0.5f, static_cast<int>(app.tab()) == i ? kPink : kTextDim,
                     labels[i], 48);
            if (app.focusArea() == pspapp::FocusArea::Categories && static_cast<int>(app.tab()) == i)
                fillRect(39 + i * 56, 46, 24, 2, kPink);
        }
    }
    char meta[32]{};
    if (app.tab() == pspapp::Tab::Mine)
        std::snprintf(meta, sizeof(meta), "LOCAL");
    else
        std::snprintf(meta, sizeof(meta), "P%d", app.page());
    drawText(430, 6, 0.5f, kTextDim, meta, 40);
}

void drawFooter(const pspapp::App& app) {
    static std::string lastStatus;
    static uint64_t statusUntil = 0;
    const uint64_t nowTicks = sceKernelGetSystemTimeWide();
    const char* confirm = app.preferences().confirmButton();
    const char* back = app.preferences().backButton();
    if (lastStatus != app.status()) {
        lastStatus = app.status();
        statusUntil = nowTicks + 2000000;
    }
    fillRect(0, 258, kWidth, 14, kBackground);
    std::string hint;
    if (app.utilityPage()) {
        if (app.screen() == pspapp::Screen::Settings)
            hint = uiText(app, pspapp::TextId::hint_settings, {{"confirm", confirm}, {"back", back}});
        else if (app.screen() == pspapp::Screen::Storage)
            hint = uiText(app, app.clearingCovers() ? pspapp::TextId::hint_storage_confirm
                                                    : pspapp::TextId::hint_storage,
                          {{"confirm", confirm}, {"back", back}});
        else if (app.screen() == pspapp::Screen::Licenses)
            hint = uiText(app, pspapp::TextId::hint_licenses, {{"confirm", confirm}, {"back", back}});
        else if (app.screen() == pspapp::Screen::LicenseText)
            hint = uiText(app, pspapp::TextId::hint_license_text, {{"back", back}});
        else if (app.screen() == pspapp::Screen::About)
            hint = uiText(app, pspapp::TextId::hint_about, {{"confirm", confirm}, {"back", back}});
        else hint = uiText(app, pspapp::TextId::hint_back_settings, {{"back", back}});
    } else if (app.screen() == pspapp::Screen::Login) {
        hint = uiText(app, pspapp::TextId::hint_login, {{"back", back}});
    } else if (app.busy()) {
        const char *progress = network_progress_message();
        hint = uiText(app, pspapp::TextId::hint_busy,
                      {{"progress", progress[0] ? progress : app.status()}, {"back", back}});
    } else if (app.focusArea() == pspapp::FocusArea::Navigation) {
        hint = uiText(app, pspapp::TextId::hint_navigation, {{"confirm", confirm}});
    } else if (app.focusArea() == pspapp::FocusArea::Categories) {
        hint = uiText(app, pspapp::TextId::hint_categories);
    } else if (app.screen() == pspapp::Screen::Detail && !app.detailReady()) {
        hint = uiText(app, pspapp::TextId::hint_detail_loading, {{"back", back}});
    } else if (app.screen() == pspapp::Screen::Detail) {
        hint = uiText(app, pspapp::TextId::hint_detail,
                      {{"confirm", confirm}, {"action", app.detailStartMs() > 0
                          ? pspapp::text(pspapp::TextId::value_resume, app.preferences().locale)
                          : pspapp::text(pspapp::TextId::value_play, app.preferences().locale)},
                       {"back", back}});
    } else if (app.screen() == pspapp::Screen::Browse) {
        const auto& page = app.browsePage();
        if (app.atTabRoot()) {
            hint = uiText(app, pspapp::TextId::hint_browse_root,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::Text) {
            hint = uiText(app, pspapp::TextId::hint_browse_text, {{"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::Image) {
            hint = uiText(app, pspapp::TextId::hint_browse_image, {{"back", back}});
        } else if (pspapp::isPgcPosterPage(page.request.kind)) {
            hint = uiText(app, pspapp::TextId::hint_browse_pgc,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::Season) {
            hint = uiText(app, pspapp::TextId::hint_browse_season,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::Space) {
            hint = uiText(app, page.followed ? pspapp::TextId::hint_browse_space_follow
                                             : pspapp::TextId::hint_browse_space_unfollow,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::DynamicDetail) {
            hint = uiText(app, page.followed ? pspapp::TextId::hint_browse_dynamic_unlike
                                             : pspapp::TextId::hint_browse_dynamic_like,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::ChooseCollection) {
            const bool removesCollection = !page.items.empty() &&
                app.browseSelection() < page.items.size() && page.items[app.browseSelection()].duration;
            hint = uiText(app, removesCollection ? pspapp::TextId::hint_browse_collection_remove
                                                 : pspapp::TextId::hint_browse_collection_add,
                          {{"confirm", confirm}, {"back", back}});
        } else if (page.request.kind == pspapp::BrowseKind::Conversation ||
                   page.request.kind == pspapp::BrowseKind::Comments ||
                   page.request.kind == pspapp::BrowseKind::Replies) {
            hint = uiText(app, pspapp::TextId::hint_browse_conversation,
                          {{"confirm", confirm}, {"back", back}});
        } else {
            hint = uiText(app, pspapp::TextId::hint_browse_default,
                          {{"confirm", confirm}, {"back", back}});
        }
    } else if (app.screen() == pspapp::Screen::Comments) {
        hint = uiText(app, pspapp::TextId::hint_comments, {{"confirm", confirm}});
    } else if (app.tab() == pspapp::Tab::Search) {
        if (app.searchView() == pspapp::SearchView::History) {
            hint = uiText(app, pspapp::TextId::hint_search_history,
                          {{"confirm", confirm}, {"back", back}});
        } else if (app.searchView() == pspapp::SearchView::Landing) {
            hint = uiText(app, pspapp::TextId::hint_search_landing,
                          {{"confirm", confirm}, {"back", back}});
        } else {
            hint = uiText(app, pspapp::TextId::hint_search_results,
                          {{"confirm", confirm}, {"back", back}});
        }
    } else if (app.tab() == pspapp::Tab::Mine) {
        hint = uiText(app, pspapp::TextId::hint_mine, {{"confirm", confirm}, {"back", back}});
    } else {
        hint = uiText(app, pspapp::TextId::hint_home, {{"confirm", confirm}});
    }
    if (app.screen() != pspapp::Screen::Login && !app.clearingCovers() && !app.busy() &&
        (nowTicks < statusUntil ||
         (app.playbackFailed() && app.screen() == pspapp::Screen::Detail)) &&
        !lastStatus.empty())
        hint = lastStatus;
    drawText(12, 254, 0.46f, kTextDim, hint, 456);
    if (app.busy() && !app.pollingLogin()) {
        const unsigned total = transfer_total_kb;
        const unsigned now = transfer_kb;
        int width = total ? static_cast<int>(static_cast<uint64_t>(now) * 460 / total) : 100;
        if (width > 460) width = 460;
        if (!total) width = static_cast<int>((sceKernelGetSystemTimeLow() / 12000) % 460);
        fillRect(10, 256, width, 2, kPink);
    }
}

void drawEmpty(const std::string& title, const std::string& subtitle) {
    drawText(40, 112, 0.83f, kText, title, 400, true);
    drawText(40, 143, 0.56f, kTextDim, subtitle, 400, true);
}

void drawCardGrid(const pspapp::App& app) {
    const auto& cards = app.cards();
    if (cards.empty()) {
        if (app.tab() == pspapp::Tab::Search) {
        drawEmpty(app.query().empty() ? "搜索 Bilibili" : "没有找到相关视频",
                      app.query().empty() ? "按 " + std::string(app.preferences().confirmButton()) + " 打开系统键盘，支持中文输入"
                                          : "按 " + std::string(app.preferences().confirmButton()) + " 修改关键词");
            if (!app.searchHistory().empty()) {
                drawText(62, 183, 0.45f, kTextDim, "最近搜索", 250);
                std::string history;
                for (size_t i = 0; i < app.searchHistory().size() && i < 4; ++i) {
                    if (!history.empty()) history += "  ·  ";
                    history += app.searchHistory()[i];
                }
                drawText(62, 198, 0.45f, kTextDim, history, 356);
            }
        } else {
            drawEmpty("正在准备首页", "△ 刷新 · SELECT 打开 PSP 网络设置");
        }
        return;
    }

    static float scroll = 0.0f;
    static float velocity = 0.0f;
    static uint64_t lastFrame = 0;
    const uint64_t now = sceKernelGetSystemTimeWide();
    const float dt = lastFrame ? std::min(0.033f, static_cast<float>(now - lastFrame) / 1000000.0f) : 0.016f;
    lastFrame = now;
    const size_t row = app.selection() / 2;
    const float target = row ? static_cast<float>(row * 162 - 42) : 0.0f;
    velocity += (190.0f * (target - scroll) - 24.0f * velocity) * dt;
    scroll += velocity * dt;
    if (std::fabs(target - scroll) < 0.4f && std::fabs(velocity) < 1.0f) {
        scroll = target;
        velocity = 0.0f;
    }

    constexpr int clipTop = 49;
    constexpr int clipBottom = 251;
    for (size_t index = 0; index < cards.size(); ++index) {
        const int x = 39 + static_cast<int>(index % 2) * 218;
        const int y = 53 + static_cast<int>(index / 2) * 162 - static_cast<int>(scroll + 0.5f);
        if (y >= clipBottom || y + 118 <= clipTop) continue;
        const bool selected = index == app.selection() && app.focusArea() == pspapp::FocusArea::Content;
        drawCover(cards[index].bvid, x, y, 210, 118, false, clipTop, clipBottom);
        if (selected) {
            const int markerTop = std::max(clipTop, y);
            const int markerBottom = std::min(clipBottom, y + 118);
            fillRect(x - 4, markerTop, 2, markerBottom - markerTop, kPink);
        }
        if (cards[index].duration > 0 && y + 101 >= clipTop && y + 101 < clipBottom) {
            const std::string duration = formatDuration(cards[index].duration);
            const int badgeWidth = static_cast<int>(duration.size()) * 6 + 8;
            fillRect(x + 210 - badgeWidth, y + 101, badgeWidth, 15, color(0, 0, 0));
            drawText(x + 210 - badgeWidth + 4, y + 101, 0.42f, color(255, 255, 255), duration, badgeWidth - 5,
                     false, clipTop, clipBottom);
        }
        drawText(x, y + 120, 0.7f, selected ? kPink : kText, cards[index].title, 210, false, clipTop, clipBottom);
        std::string meta = cards[index].author;
        if (cards[index].views > 0) meta += "  ·  " + formatCount(cards[index].views);
        drawText(x, y + 140, 0.51f, kTextDim, meta, 210, false, clipTop, clipBottom);
    }
}

void drawDetail(const pspapp::App& app) {
    const auto& detail = app.detail();
    drawText(12, 3, 0.9f, kText, "视频 / 详情", 350);
    fillRect(12, 26, 456, 1, kLine);

    drawCover(detail.card.bvid, 12, 35, 128, 72, true);
    drawWrapped(151, 32, 0.9f, kText, detail.card.title, 317, 2, 20);
    drawText(151, 75, 0.5f, kPink, "UP " + detail.card.author, 317);
    std::string stats = formatCount(detail.card.views) + " 播放  ·  " + formatCount(detail.danmaku) + " 弹幕";
    drawText(151, 92, 0.5f, kTextDim, stats, 317);

    drawText(12, 113, 0.5f, kPink, std::string(app.preferences().confirmButton()) + " 播放", 74);
    drawText(86, 113, 0.5f, kTextDim, "R 评论", 74);
    drawText(158, 113, 0.5f, app.currentIsLater() ? kPink : kTextDim,
             app.currentIsLater() ? "□ 已稍后" : "□ 稍后", 90);
    drawText(268, 113, 0.5f, kTextDim, formatCount(detail.like) + " 赞 · " + formatCount(detail.favorite) + " 收藏", 200);
    drawWrapped(12, 135, 0.7f, kText, detail.description.empty() ? "暂无简介" : detail.description,
                456, 2, 18);

    fillRect(12, 176, 456, 1, kLine);
    drawText(12, 183, 0.5f, kPink, "分 P  " + std::to_string(detail.pages.size()), 150);
    drawText(120, 183, 0.5f, kTextDim, "↑↓ 选择分集", 250);
    if (!detail.pages.empty()) {
        size_t first = app.detailPart() > 1 ? app.detailPart() - 1 : 0;
        if (detail.pages.size() > 1 && first + 2 > detail.pages.size()) first = detail.pages.size() - 2;
        for (size_t row = 0; row < 2 && first + row < detail.pages.size(); ++row) {
            const size_t index = first + row;
            const int y = 204 + static_cast<int>(row) * 24;
            const bool selected = index == app.detailPart();
            if (selected) fillRect(12, y + 2, 2, 13, kPink);
            drawText(20, y, 0.7f, selected ? kPink : kText,
                     "P" + std::to_string(index + 1) + "  " + detail.pages[index].title, 380);
            drawText(412, y + 2, 0.5f, kTextDim, formatDuration(detail.pages[index].duration), 56);
        }
    } else {
        drawText(12, 204, 0.7f, kText, detail.card.title, 456);
    }
}

void drawMine(const pspapp::App& app) {
    const auto &auth = app.auth();
    const bool selected = app.selection() == 0 && app.focusArea() == pspapp::FocusArea::Content;
    if (selected)
        fillRect(33, 32, 2, 34, kPink);
    const bool signedIn = auth.state == pspapp::AuthState::SignedIn;
    drawText(39, 31, 0.7f, selected ? kPink : kText,
             signedIn ? "账号 · " + (auth.name.empty() ? auth.mid : auth.name) : "账号 · 扫码登录",
             428);
    drawText(39, 52, 0.5f, kTextDim,
             pspapp::authStateMessage(auth.state, app.preferences().confirmWithCircle), 428);
    fillRect(39, 77, 428, 1, kLine);
    drawText(39, 83, 0.5f, kTextDim, "观看历史 / 稍后观看 · 本地记录", 428);
    const size_t total = app.history().size() + app.later().size();
    if (!total) {
        drawText(39, 124, 0.7f, kText, "还没有本地记录", 428);
        drawText(39, 150, 0.5f, kTextDim, "播放后保存历史；详情页按 □ 加入稍后观看。", 428);
        return;
    }
    const size_t selection = app.selection() ? app.selection() - 1 : 0;
    const size_t first = selection > 1 ? selection - 1 : 0;
    for (size_t row = 0; row < 2 && first + row < total; ++row) {
        const size_t index = first + row;
        const bool history = index < app.history().size();
        const auto &entry =
            history ? app.history()[index] : app.later()[index - app.history().size()];
        const int y = 104 + int(row) * 72;
        const bool active =
            index + 1 == app.selection() && app.focusArea() == pspapp::FocusArea::Content;
        if (active)
            fillRect(33, y, 2, 56, kPink);
        drawCover(entry.bvid, 39, y, 100, 56);
        drawWrapped(147, y, 0.7f, active ? kPink : kText, entry.title, 320, 2, 18);
        drawText(147, y + 40, 0.5f, kTextDim,
                 std::string(history ? "历史 · " : "稍后 · ") + entry.author + " · " +
                     formatDuration(entry.duration),
                 320);
    }
}

void drawLogin(const pspapp::App &app) {
    using pspapp::AuthState;
    const auto &auth = app.auth();
    drawText(20, 12, 0.83f, kText, "账号登录", 440);
    fillRect(20, 39, 440, 1, kLine);
    const bool signedIn = auth.state == AuthState::SignedIn || auth.state == AuthState::Restored;
    if (signedIn) {
        drawText(28, 68, 0.83f, kText, auth.name.empty() ? "Bilibili 账号" : auth.name, 424);
        drawText(28, 102, 0.7f, kTextDim, "UID " + auth.mid, 424);
        drawText(28, 136, 0.7f, kPink,
                 pspapp::authStateMessage(auth.state, app.preferences().confirmWithCircle), 424);
        drawText(28, 198, 0.5f, kTextDim, "观看历史与稍后观看仍保存在本机", 424);
        drawText(28, 224, 0.5f, kTextDim,
                 "□ 退出本机账号    △ 检查登录状态    " + std::string(app.preferences().backButton()) + " 返回", 424);
    } else {
        if (auth.qrSize > 0 && !auth.qr.empty() && auth.state != AuthState::QrExpired) {
            const int scale = std::min(3, 176 / (auth.qrSize + 8));
            const int width = (auth.qrSize + 8) * scale;
            const int x = 28 + (184 - width) / 2, y = 59 + (184 - width) / 2;
            fillRect(x, y, width, width, color(255, 255, 255));
            for (int row = 0; row < auth.qrSize; ++row)
                for (int col = 0; col < auth.qrSize; ++col)
                    if (auth.qr[row * auth.qrSize + col])
                        fillRect(x + (col + 4) * scale, y + (row + 4) * scale, scale, scale,
                                 kBlack);
        } else {
            drawWrapped(36, 112, 0.7f, kTextDim,
                        app.busy() ? "正在获取二维码…" : "△ 获取新的二维码", 168, 3, 22);
        }
        drawWrapped(234, 64, 0.7f, kText,
                    pspapp::authStateMessage(auth.state, app.preferences().confirmWithCircle), 218, 3, 23);
        if (!app.authError())
            drawWrapped(234, 158, 0.5f, kTextDim, "手机扫码并确认后，将保存登录状态。", 218, 3, 18);
        drawText(234, 224, 0.5f, kPink,
                 "△ 刷新    " + std::string(app.preferences().backButton()) + " 返回", 218);
    }
    if (app.authError())
        drawWrapped(signedIn ? 28 : 234, signedIn ? 158 : 141, 0.5f, kPink,
                    pspapp::errorMessage(app.authError()), 218, 3, 16);
}

void drawSearch(const pspapp::App& app) {
    using pspapp::SearchView;
    const bool contentFocus = app.focusArea() == pspapp::FocusArea::Content;
    auto mark = [&](size_t index, int y, int height) {
        const bool selected = contentFocus && app.selection() == index;
        if (selected) fillRect(33, y + 2, 2, height, kPink);
        return selected ? kPink : kText;
    };
    if (app.searchView() == SearchView::History) {
        drawText(39, 31, 0.5f, kTextDim, "最近搜索 · 仅保存在本机", 428);
        const auto& history = app.searchHistory();
        if (history.empty()) {
            drawText(39, 80, 0.7f, kText, "还没有搜索历史", 428);
            drawText(39, 105, 0.5f, kTextDim,
                     "□ 输入关键词，或 " + std::string(app.preferences().backButton()) + " 返回查看热搜", 428);
        }
        for (size_t i = 0; i < history.size() && i < 8; ++i) {
            const int y = 54 + static_cast<int>(i) * 24;
            drawText(39, y, 0.7f, mark(i, y, 14), history[i], 428);
        }
        return;
    }
    if (app.searchView() == SearchView::Landing) {
        drawText(39, 34, 0.7f, mark(0, 34, 15),
                 "搜索视频 · 按 " + std::string(app.preferences().confirmButton()) + " 输入关键词", 428);
        fillRect(39, 56, 428, 1, kLine);
        drawText(39, 64, 0.5f, kTextDim, "历史", 48);
        std::string preview;
        for (size_t i = 0; i < app.searchHistory().size() && i < 2; ++i) {
            if (!preview.empty()) preview += "  ·  ";
            preview += app.searchHistory()[i];
        }
        drawText(95, 63, 0.7f, mark(1, 63, 15),
                 preview.empty() ? "暂无记录 · " + std::string(app.preferences().confirmButton()) + " 查看" : preview, 372);
        drawText(39, 95, 0.5f, kPink, "热搜", 80);
        const auto& topics = app.searchTopics();
        if (topics.empty()) {
            drawText(39, 124, 0.7f, kTextDim, app.busy() ? "正在载入热搜…" : "热搜暂不可用", 428);
            drawText(39, 150, 0.5f, kTextDim, "□ 仍可输入关键词搜索", 428);
        }
        const size_t selected = app.selection() >= 2 ? app.selection() - 2 : 0;
        const size_t first = selected > 2 ? selected - 2 : 0;
        for (size_t row = 0; row < 3 && first + row < topics.size(); ++row) {
            const size_t i = first + row;
            const int y = 115 + static_cast<int>(row) * 43;
            const uint32_t shade = mark(i + 2, y, 30);
            char rank[12];
            std::snprintf(rank, sizeof(rank), "%02u", static_cast<unsigned>(i + 1));
            drawText(39, y, 0.7f, i < 3 ? kPink : kTextDim, rank, 25);
            drawWrapped(70, y, 0.7f, shade, topics[i].title, 397, 2, 18);
        }
        return;
    }
    drawText(39, 31, 0.5f, app.searchError().empty() ? kTextDim : kPink,
             app.searchError().empty() ? "关键词：" + app.query() : app.searchError() + " · △ 重试",
             345);
    drawText(390, 31, 0.5f, kTextDim, std::to_string(app.cards().size()) + " 条", 77);
    const auto& cards = app.cards();
    if (cards.empty()) {
        drawText(39, 91, 0.7f, kText, app.busy() ? "正在搜索…" :
                 app.searchError().empty() ? "没有找到相关视频" : "搜索未完成", 428);
        drawWrapped(39, 117, 0.5f, kTextDim,
                    app.searchError().empty() ? "□ 修改关键词，" + std::string(app.preferences().backButton()) + " 返回搜索" : app.searchError(),
                    428, 3, 16);
        return;
    }
    const size_t first = app.selection() > 2 ? app.selection() - 2 : 0;
    for (size_t row = 0; row < 3 && first + row < cards.size(); ++row) {
        const size_t i = first + row;
        const auto& card = cards[i];
        const int y = 56 + static_cast<int>(row) * 65;
        const uint32_t shade = mark(i, y, 54);
        drawCover(card.bvid, 39, y, 100, 56);
        drawWrapped(147, y, 0.7f, shade, card.title, 320, 2, 18);
        drawText(147, y + 40, 0.5f, kTextDim, card.author + " · " + formatCount(card.views) + " · " + formatDuration(card.duration), 320);
    }
}

void drawUtility(const pspapp::App& app) {
    using pspapp::Screen;
    const auto locale = app.preferences().locale;
    if (app.screen() == Screen::Settings) {
        drawText(39, 31, 0.5f, kTextDim, pspapp::text(pspapp::TextId::settings_header, locale), 428);
        const pspapp::TextId labels[] = {
            pspapp::TextId::settings_theme, pspapp::TextId::settings_body_size,
            pspapp::TextId::settings_covers, pspapp::TextId::settings_repeat,
            pspapp::TextId::settings_playback_buffer, pspapp::TextId::settings_confirm_button,
            pspapp::TextId::settings_language, pspapp::TextId::settings_wifi,
            pspapp::TextId::settings_cover_cache, pspapp::TextId::settings_help,
            pspapp::TextId::settings_about};
        const auto& preferences = app.preferences();
        const std::string values[] = {pspapp::text(preferences.darkTheme ? pspapp::TextId::value_dark
                                                                         : pspapp::TextId::value_light, locale),
            std::to_string(preferences.bodySize) + " px",
            pspapp::text(preferences.showCovers ? pspapp::TextId::value_on_demand
                                               : pspapp::TextId::value_off, locale),
            pspapp::text(preferences.slowRepeat ? pspapp::TextId::value_slow
                                                : pspapp::TextId::value_standard, locale),
            pspapp::formatText(pspapp::TextId::value_buffer, locale,
                {{"frames", std::to_string(preferences.playbackBufferFrames)},
                 {"memory", std::to_string(preferences.playbackBufferFrames * 544)}}),
            pspapp::formatText(pspapp::TextId::value_confirm, locale,
                {{"button", preferences.confirmButton()}}),
            pspapp::text(locale == pspapp::Locale::ZhHans ? pspapp::TextId::value_language_zh
                                                          : pspapp::TextId::value_language_en, locale),
            pspapp::text(pspapp::TextId::value_system_connect, locale),
            pspapp::text(pspapp::TextId::value_manage, locale),
            pspapp::text(pspapp::TextId::value_view, locale),
            pspapp::text(pspapp::TextId::value_view, locale)};
        for (size_t i = 0; i < static_cast<size_t>(pspapp::SettingItem::Count); ++i) {
            const int y = 49 + static_cast<int>(i) * 18;
            if (app.settingSelection() == i) fillRect(33, y + 3, 2, 16, kPink);
            drawText(39, y, 0.7f, kText, pspapp::text(labels[i], locale), 250);
            drawText(300, y, 0.7f, app.settingSelection() == i ? kPink : kTextDim, values[i], 168);
        }
    } else if (app.screen() == Screen::Storage) {
        drawText(39, 31, 0.5f, kTextDim, pspapp::text(pspapp::TextId::storage_title, locale), 428);
        drawText(39, 60, 0.7f, kText, pspapp::text(pspapp::TextId::storage_cover_count, locale), 260);
        drawText(330, 60, 0.7f, kText,
                 uiText(app, pspapp::TextId::storage_count_value,
                        {{"count", std::to_string(app.coverFiles())}}), 138);
        char size[32];
        std::snprintf(size, sizeof(size), "%.1f KiB", app.coverBytes() / 1024.0);
        drawText(39, 86, 0.7f, kText, pspapp::text(pspapp::TextId::storage_space, locale), 260);
        drawText(330, 86, 0.7f, kText, size, 138);
        fillRect(39, 118, 428, 1, kLine);
        drawWrapped(39, 135, 0.7f, kText, pspapp::text(pspapp::TextId::storage_download_info, locale), 428, 2, 18);
        drawText(39, 185, 0.5f, kTextDim,
                 pspapp::text(pspapp::TextId::storage_preserved_info, locale), 428);
        if (app.clearingCovers())
            drawText(39, 215, 0.7f, kPink, uiText(app, pspapp::TextId::storage_clear_confirm,
                     {{"confirm", app.preferences().confirmButton()},
                      {"back", app.preferences().backButton()}}), 428);
    } else if (app.screen() == Screen::Help) {
        const pspapp::TextId labels[] = {
            pspapp::TextId::help_label_general, pspapp::TextId::help_label_sidebar,
            pspapp::TextId::help_label_home, pspapp::TextId::help_label_search,
            pspapp::TextId::help_label_mine, pspapp::TextId::help_label_detail,
            pspapp::TextId::help_label_comments};
        const pspapp::TextId rows[] = {
            pspapp::TextId::help_general, pspapp::TextId::help_sidebar,
            pspapp::TextId::help_home, pspapp::TextId::help_search,
            pspapp::TextId::help_mine, pspapp::TextId::help_detail,
            pspapp::TextId::help_comments};
        const std::string text[] = {
            uiText(app, rows[0], {{"confirm", app.preferences().confirmButton()},
                                  {"back", app.preferences().backButton()}}),
            pspapp::text(rows[1], locale), pspapp::text(rows[2], locale), pspapp::text(rows[3], locale),
            uiText(app, rows[4], {{"confirm", app.preferences().confirmButton()}}),
            pspapp::text(rows[5], locale), pspapp::text(rows[6], locale)};
        for (int i = 0; i < 7; ++i) {
            const int y = 39 + i * 29;
            drawText(39, y + 2, 0.5f, kTextDim, pspapp::text(labels[i], locale), 68);
            drawText(112, y, 0.7f, kText, text[i], 356);
        }
    } else if (app.screen() == Screen::About) {
        drawText(39, 39, 0.9f, kText, "WiliMini", 428);
        drawText(39, 66, 0.5f, kTextDim, app.buildVersion(), 428);
        drawText(39, 99, 0.7f, kText, pspapp::text(pspapp::TextId::about_design, locale), 428);
        drawText(39, 127, 0.7f, kText, pspapp::text(pspapp::TextId::about_reference, locale), 428);
        drawText(39, 155, 0.5f, kTextDim, pspapp::text(pspapp::TextId::about_font, locale), 428);
        fillRect(39, 188, 428, 1, kLine);
        fillRect(33, 209, 2, 16, kPink);
        drawText(39, 206, 0.7f, kPink, uiText(app, pspapp::TextId::about_license,
                 {{"confirm", app.preferences().confirmButton()}}), 428);
    } else if (app.screen() == Screen::Licenses) {
        drawText(39, 31, 0.5f, kTextDim, pspapp::text(pspapp::TextId::licenses_title, locale), 428);
        for (size_t i = 0; i < pspapp::settingsLicenseCount; ++i) {
            const int y = 54 + static_cast<int>(i) * 23;
            const bool selected = app.licenseSelection() == i;
            if (selected) fillRect(33, y + 3, 2, 16, kPink);
            drawText(39, y, 0.7f, selected ? kPink : kText, pspapp::settingsLicenses[i].title, 428);
        }
    }
}

const std::vector<std::string>& licenseLines(const pspapp::App& app) {
    static std::string cachedText;
    static int cachedSize = 0;
    static std::vector<std::string> lines;
    if (cachedText != app.licenseText() || cachedSize != app.preferences().bodySize) {
        cachedText = app.licenseText();
        cachedSize = app.preferences().bodySize;
        lines = textLines(cachedText, cachedSize, 428);
    }
    return lines;
}

void drawLicenseText(const pspapp::App& app) {
    drawText(39, 31, 0.5f, kTextDim, pspapp::settingsLicenses[app.licenseSelection()].title, 428);
    const auto& lines = licenseLines(app);
    const int height = app.preferences().bodySize + 4;
    const int first = app.documentScroll() / height;
    for (int line = first; line < static_cast<int>(lines.size()); ++line) {
        const int y = 53 + line * height - app.documentScroll();
        if (y >= 251) break;
        drawText(39, y, 0.7f, kText, lines[line], 428, false, 53, 251);
    }
}

int commentsHeight(const pspapp::App& app) {
    int height = 0;
    for (const auto& item : app.comments())
        height += 29 + static_cast<int>(textLines(item.message, app.preferences().bodySize, 456).size()) * (app.preferences().bodySize + 4);
    return height;
}

void drawPgcPosters(const pspapp::App& app) {
    const auto& page = app.browsePage();
    drawText(12, 7, 0.9f, kPink, page.title, 360);
    fillRect(12, 31, 456, 1, kLine);
    const bool followed = page.request.kind == pspapp::BrowseKind::FollowBangumi ||
                          page.request.kind == pspapp::BrowseKind::FollowCinema;
    drawText(12, 38, 0.5f, kPink, followed ? "已追作品" : "推荐", 140);
    fillRect(12, 53, followed ? 48 : 24, 1, kPink);
    const size_t first = app.browseSelection() / 4 * 4;
    drawText(364, 38, 0.5f, kTextDim,
        std::to_string(std::min(first + 1, page.items.size())) + "–" +
        std::to_string(std::min(first + 4, page.items.size())) + " / " + std::to_string(page.items.size()), 104);
    if (first >= page.items.size()) {
        drawText(12, 105, 0.7f, kPink,
                 std::string(app.preferences().confirmButton()) + " 加载下一页", 456);
        return;
    }
    for (size_t i = first; i < std::min(first + 4, page.items.size()); ++i) {
        const auto& item = page.items[i];
        const int x = 12 + static_cast<int>(i - first) * 118;
        const bool selected = i == app.browseSelection();
        drawCover(pspapp::pgcCoverKey(item), x, 62, 100, 134);
        if (selected) {
            fillRect(x - 2, 60, 104, 2, kBlue);
            fillRect(x - 2, 196, 104, 2, kBlue);
            fillRect(x - 2, 60, 2, 138, kBlue);
            fillRect(x + 100, 60, 2, 138, kBlue);
        }
        drawWrapped(x, 203, 0.7f, selected ? kPink : kText, item.title, 102, 2, 16);
        drawText(x, 237, 0.5f, kTextDim, item.subtitle, 102);
    }
}

void drawPgcEpisodes(const pspapp::App& app) {
    const auto& page = app.browsePage();
    std::vector<size_t> episodes;
    for (size_t i = 0; i < page.items.size(); ++i)
        if (page.items[i].next.kind == pspapp::BrowseKind::Episode) episodes.push_back(i);
    const bool synopsis = app.browseSelection() < page.items.size() &&
                          page.items[app.browseSelection()].next.kind == pspapp::BrowseKind::Text;
    drawText(12, 6, 0.9f, kText, page.title, 370);
    drawText(408, 10, 0.5f, kPink, page.followed ? "已追番" : "+ 追番", 60);
    drawText(12, 27, 0.5f, kTextDim, page.subtitle.empty()
        ? "共" + std::to_string(episodes.size()) + "集" : page.subtitle, 456);
    fillRect(12, 43, 456, 1, kLine);
    drawText(12, 48, 0.5f, synopsis ? kTextDim : kPink, "分集", 36);
    drawText(60, 48, 0.5f, synopsis ? kPink : kTextDim, "简介", 36);
    fillRect(synopsis ? 60 : 12, 62, 24, 1, kPink);
    if (synopsis) {
        drawWrapped(12, 76, 0.7f, kText, page.description, 456, 8, bodySize + 4);
        return;
    }
    const auto selected = std::find(episodes.begin(), episodes.end(), app.browseSelection());
    const size_t ordinal = selected == episodes.end() ? 0 : static_cast<size_t>(selected - episodes.begin());
    const size_t first = ordinal / 3 * 3;
    drawText(370, 48, 0.5f, kTextDim,
        std::to_string(episodes.empty() ? 0 : first + 1) + "–" + std::to_string(std::min(first + 3, episodes.size())) +
        " / " + std::to_string(episodes.size()), 98);
    if (episodes.empty()) drawText(12, 92, 0.7f, kTextDim, "暂无分集", 456);
    for (size_t n = first; n < std::min(first + 3, episodes.size()); ++n) {
        const auto& item = page.items[episodes[n]];
        const int y = 70 + static_cast<int>(n - first) * 62;
        const bool active = episodes[n] == app.browseSelection();
        if (active) fillRect(8, y - 3, 464, 60, kPanelRaised);
        drawCover(pspapp::pgcCoverKey(item), 12, y, 96, 54);
        if (active) {
            fillRect(10, y - 2, 100, 2, kBlue);
            fillRect(10, y + 54, 100, 2, kBlue);
            fillRect(10, y - 2, 2, 58, kBlue);
            fillRect(108, y - 2, 2, 58, kBlue);
        }
        drawText(122, y + 7, 0.7f, active ? kPink : kText, item.title, 342);
        std::string status;
        if (item.completed) status = "已看完";
        else if (item.startMs > 0) status = "继续观看 · " + formatDuration(static_cast<int>(item.startMs / 1000)) + " / " + formatDuration(item.duration);
        else if (item.duration > 0) status = std::to_string(item.duration / 60) + "分钟";
        if (!item.subtitle.empty()) status += (status.empty() ? "" : " · ") + item.subtitle;
        drawText(122, y + 31, 0.5f, kTextDim, status, 342);
    }
}

void drawBrowse(const pspapp::App& app) {
    const auto& page = app.browsePage();
    if (!app.browseError() && !page.items.empty()) {
        if (pspapp::isPgcPosterPage(page.request.kind)) { drawPgcPosters(app); return; }
        if (page.request.kind == pspapp::BrowseKind::Season) { drawPgcEpisodes(app); return; }
    }
    drawText(12, 3, 0.9f, kText, page.title, 456);
    fillRect(12, 26, 456, 1, kLine);
    if (page.request.kind == pspapp::BrowseKind::Image) {
        static std::string imageKey;
        static std::vector<uint32_t> imagePixels;
        static int width=0, height=0;
        if(imageKey!=page.description) {
            imageKey=page.description;imagePixels.clear();
            loadJpeg(std::string(CLIENT_ROOT)+"/cache/"+imageKey+".jpg",imagePixels,&width,&height);
        }
        if(imagePixels.empty())drawText(12,90,0.7f,kTextDim,
            "图片加载失败 · " + std::string(app.preferences().backButton()) + " 返回动态",456);
        else {
            const int x=(480-width)/2,y=32+(215-height)/2;
            for(int row=0;row<height;++row)std::copy(imagePixels.begin()+row*width,imagePixels.begin()+(row+1)*width,framebuffer()+(y+row)*512+x);
        }
        return;
    }
    if (page.request.kind == pspapp::BrowseKind::Text) {
        const auto lines = textLines(page.description, app.preferences().bodySize, 456);
        const int height = app.preferences().bodySize + 4;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int y = 34 + static_cast<int>(i) * height - app.browseScroll();
            if (y >= 251) break;
            if (y + height > 32) drawText(12, y, 0.7f, kText, lines[i], 456, false, 32, 251);
        }
        return;
    }
    int top = 35;
    if (app.browseError()) {
        drawText(12, top, 0.5f, kPink, pspapp::errorMessage(app.browseError()) + " · △ 重试", 456);
        top += 20;
    } else if (!page.description.empty()) {
        drawWrapped(12, top, 0.5f, kTextDim, page.description, 456, 2, 16);
        top += 38;
    }
    if (page.items.empty()) {
        drawText(12, top + 27, 0.7f, kTextDim, app.busy() ? "正在载入…" :
            app.browseError() ? std::string(app.preferences().confirmButton()) + " 登录 / △ 重试" : "暂无内容", 456);
        return;
    }
    const size_t total = page.items.size() + (page.hasMore ? 1 : 0);
    const size_t first = app.browseSelection() > 1 ? app.browseSelection() - 1 : 0;
    for (size_t index = first; index < total; ++index) {
        const int y = top + static_cast<int>(index - first) * 68;
        if (y + 30 > 251) break;
        const bool selected = index == app.browseSelection();
        if (selected) fillRect(6, y + 2, 2, 50, kPink);
        if (index == page.items.size()) {
            drawText(12, y, 0.7f, selected ? kPink : kText, "加载下一页", 456);
            break;
        }
        const auto& item = page.items[index];
        drawWrapped(12, y, 0.7f, selected ? kPink : kText, item.title, 456, 1, 18);
        drawWrapped(12, y + 20, 0.5f, kTextDim,
            item.next.kind == pspapp::BrowseKind::LiveStream || item.text.empty()
                ? item.subtitle : item.text, 456, 2, 16);
        if (y + 62 < 251) fillRect(12, y + 62, 456, 1, kLine);
    }
}

void drawComments(const pspapp::App& app) {
    drawText(12, 3, 0.9f, kText, "视频 / 评论", 350);
    fillRect(12, 26, 456, 1, kLine);
    drawText(12, 30, 0.5f, kPink, "热门", 48);
    drawText(75, 30, 0.5f, kTextDim, app.detail().card.title, 390);
    const auto& comments = app.comments();
    if (comments.empty()) {
        drawEmpty("评论区为空", std::string(app.preferences().backButton()) + " 返回视频详情");
        return;
    }
    int y = 52 - app.commentScroll();
    const int lineHeight = app.preferences().bodySize + 4;
    size_t commentIndex = 0;
    for (const auto& item : comments) {
        const auto lines = textLines(item.message, app.preferences().bodySize, 456);
        const int height = 29 + static_cast<int>(lines.size()) * lineHeight;
        if (y + height > 49 && y < 251) {
            if (commentIndex == app.selectedComment()) fillRect(6, std::max(y, 49), 2, std::min(y + height, 251) - std::max(y, 49), kPink);
            drawText(12, y, 0.5f, item.uploader ? kPink : kTextDim,
                     std::string(item.top ? "置顶 · " : "") + item.author, 235, false, 49, 251);
            drawText(253, y, 0.5f, kTextDim, item.time, 135, false, 49, 251);
            drawText(400, y, 0.5f, kTextDim, formatCount(static_cast<uint64_t>(item.like)) + " 赞", 68, false, 49, 251);
            for (size_t line = 0; line < lines.size(); ++line) {
                const int lineY = y + 17 + static_cast<int>(line) * lineHeight;
                if (lineY + lineHeight <= 49 || lineY >= 251) continue;
                drawText(12, lineY, 0.7f, kText, lines[line], 456, false, 49, 251);
            }
            if (y + height - 5 >= 49 && y + height - 5 < 251)
                fillRect(12, y + height - 5, 456, 1, kLine);
        }
        y += height;
        ++commentIndex;
    }
}

}  // namespace

void ui_scroll_browse(pspapp::App& app, int direction) {
    if (app.browsePage().request.kind == pspapp::BrowseKind::Text) {
        const int height = app.preferences().bodySize + 4;
        app.setBrowseScrollLimit(std::max(0, static_cast<int>(textLines(app.browsePage().description, app.preferences().bodySize, 456).size()) * height - 215));
    }
    app.browseMove(0, direction);
}
void ui_select_comment(pspapp::App& app, int direction) {
    app.selectComment(direction);
    int offset = 0;
    for (size_t i = 0; i < app.selectedComment() && i < app.comments().size(); ++i)
        offset += 29 + static_cast<int>(textLines(app.comments()[i].message, app.preferences().bodySize, 456).size()) * (app.preferences().bodySize + 4);
    app.setCommentScrollLimit(std::max(0, commentsHeight(app) - 199));
    app.scrollComments(offset - app.commentScroll());
}
void ui_scroll_comments(pspapp::App& app, int direction) {
    app.setCommentScrollLimit(std::max(0, commentsHeight(app) - 199));
    app.scrollComments(direction * (app.preferences().bodySize + 4));
}

void ui_scroll_document(pspapp::App& app, int direction) {
    const int height = app.preferences().bodySize + 4;
    app.setDocumentScrollLimit(std::max(0, static_cast<int>(licenseLines(app).size()) * height - 198));
    app.moveSelection(0, direction);
}

void ui_init() {
    sceGuInit();
    sceGuStart(GU_DIRECT, commands);
    sceGuDepthBuffer(reinterpret_cast<void*>(2 * 512 * 272 * 4), 512);
    sceGuDrawBuffer(GU_PSM_8888, nullptr, 512);
    sceGuDispBuffer(480, 272, reinterpret_cast<void*>(512 * 272 * 4), 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    draw_buffer = nullptr;
    bitmapFontInit(CLIENT_PATH("/wqy.bmf"));
}

void ui_startup_status(const char *message) {
    beginFrame();
    fillRect(0, 0, kWidth, kHeight, kBackground);
    drawText(12, 12, 0.8f, kText, "WiliMini", 456);
    drawText(12, 254, 0.46f, kTextDim, message, 456);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    draw_buffer = sceGuSwapBuffers();
}

void ui_shutdown() {
    bitmapFontShutdown();
    sceGuTerm();
}

void ui_prepare_playback() {
    // Decoded covers are cheap to reload after playback and can otherwise keep
    // nearly 1.8 MiB alive while FFmpeg needs reference pictures and RGB frames.
    for (auto& cover : covers) {
        cover.key.clear();
        std::vector<uint32_t>().swap(cover.pixels);
    }
    next_cover = 0;
}

void ui_resume() {
    sceGuStart(GU_DIRECT, commands);
    sceGuDrawBuffer(GU_PSM_8888, draw_buffer, 512);
    sceGuDispBuffer(480, 272,
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(draw_buffer) ^ (512 * 272 * 4)), 512);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);
}

void ui_dialog_frame() {
    beginFrame();
    sceGuClearColor(kBackground);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void ui_dialog_present() {
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    draw_buffer = sceGuSwapBuffers();
}

void ui_render(const pspapp::App& app) {
    applyPreferences(app.preferences());
    static unsigned coverGeneration = 0;
    if (coverGeneration != app.coverGeneration()) {
        for (auto& cover : covers)
            if (app.completedCoverKey().empty() || cover.key == app.completedCoverKey())
                cover = CoverSlot{};
        coverGeneration = app.coverGeneration();
    }
    beginFrame();
    fillRect(0, 0, kWidth, kHeight, kBackground);
    if (app.screen() == pspapp::Screen::Browse && app.hasBrowse()) {
        drawBrowse(app);
    } else if (app.utilityPage()) {
        drawHeader(app);
        if (app.screen() == pspapp::Screen::LicenseText) drawLicenseText(app);
        else drawUtility(app);
    } else if (app.screen() == pspapp::Screen::Login) {
        drawLogin(app);
    } else if (app.screen() == pspapp::Screen::Detail) {
        drawDetail(app);
    } else if (app.screen() == pspapp::Screen::Comments) {
        drawComments(app);
    } else {
        drawHeader(app);
        if (app.screen() == pspapp::Screen::Mine || app.tab() == pspapp::Tab::Mine) drawMine(app);
        else if (app.tab() == pspapp::Tab::Search) drawSearch(app);
        else drawCardGrid(app);
    }
    drawFooter(app);
    presentFrame();
}

std::string ui_input_text(const std::string& title, const std::string& initial, bool* confirmed) {
    if (confirmed) *confirmed = false;
    SceUtilityOskParams params{};
    SceUtilityOskData data{};
    unsigned short description[128]{};
    unsigned short input[128]{};
    unsigned short output[256]{};
    auto utf8ToUtf16 = [](const std::string& text, unsigned short* destination, size_t capacity) {
        size_t output = 0;
        for (size_t index = 0; index < text.size() && output + 2 < capacity;) {
            const unsigned char first = static_cast<unsigned char>(text[index]);
            uint32_t codepoint = first;
            size_t bytes = 1;
            if ((first & 0xE0) == 0xC0) {
                codepoint = first & 0x1F;
                bytes = 2;
            } else if ((first & 0xF0) == 0xE0) {
                codepoint = first & 0x0F;
                bytes = 3;
            } else if ((first & 0xF8) == 0xF0) {
                codepoint = first & 0x07;
                bytes = 4;
            }
            if (index + bytes > text.size()) break;
            for (size_t continuation = 1; continuation < bytes; ++continuation)
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[index + continuation]) & 0x3F);
            if (codepoint <= 0xFFFF) {
                destination[output++] = static_cast<unsigned short>(codepoint);
            } else {
                codepoint -= 0x10000;
                destination[output++] = static_cast<unsigned short>(0xD800 + (codepoint >> 10));
                destination[output++] = static_cast<unsigned short>(0xDC00 + (codepoint & 0x3FF));
            }
            index += bytes;
        }
        destination[output] = 0;
    };
    utf8ToUtf16(title, description, sizeof(description) / sizeof(description[0]));
    utf8ToUtf16(initial, input, sizeof(input) / sizeof(input[0]));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.lines = 1;
    data.unk_24 = 1;
    data.desc = description;
    data.intext = input;
    data.outtextlength = sizeof(output) / sizeof(output[0]);
    data.outtext = output;
    data.outtextlimit = 96;

    params.base.size = sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

#ifdef BILI_DEVICE_OSK_TEST
    FILE* log = std::fopen(CLIENT_PATH("/osk.log"), "w");
#else
    FILE* log = nullptr;
#endif
    const int init = sceUtilityOskInitStart(&params);
    if (log) {
        std::fprintf(log, "init=%d size=%u desc=%s\n", init, params.base.size, title.c_str());
        std::fflush(log);
    }
    if (init < 0) {
        if (log) std::fclose(log);
        ui_resume();
        return initial;
    }

    bool shutdown = false;
    int previous_state = -1;
    unsigned visible_frames = 0;
    for (;;) {
        ui_dialog_frame();
        const int state = sceUtilityOskGetStatus();
        if (log && state != previous_state) {
            std::fprintf(log, "state=%d\n", state);
            std::fflush(log);
            previous_state = state;
        }
        if (state == PSP_UTILITY_DIALOG_VISIBLE) {
            const int update = sceUtilityOskUpdate(1);
            if (log && visible_frames == 0)
                std::fprintf(log, "update=%d\n", update);
            ++visible_frames;
        } else if (state == PSP_UTILITY_DIALOG_QUIT && !shutdown) {
            const int stop = sceUtilityOskShutdownStart();
            if (log) std::fprintf(log, "shutdown=%d\n", stop);
            shutdown = true;
        } else if (state == PSP_UTILITY_DIALOG_NONE) {
            break;
        }
        if (log) std::fflush(log);
        ui_dialog_present();
    }
    if (log) {
        std::fprintf(log, "result=%d visible_frames=%u\n", data.result, visible_frames);
        std::fclose(log);
    }
    ui_resume();
    if (data.result != PSP_UTILITY_OSK_RESULT_CHANGED) return initial;

    if (confirmed) *confirmed = true;
    std::string result;
    for (int index = 0; index < data.outtextlength && output[index]; ++index) {
        uint32_t codepoint = output[index];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && index + 1 < data.outtextlength) {
            const uint32_t low = output[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }
        if (codepoint < 0x80) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return result;
}
