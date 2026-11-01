#include "ui.hpp"
#include "bitmap_font.hpp"
#include "title_font.hpp"
#include "assets/wiliwili/sidebar.hpp"
#include "settings_pages.hpp"

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <pspgum.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
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
    if (settings.tileHome()) {
        // Metro palette: flat black or white, high-contrast text, one accent blue.
        kBackground = settings.darkTheme ? color(0, 0, 0) : color(255, 255, 255);
        kPanel = settings.darkTheme ? color(16, 16, 16) : color(255, 255, 255);
        kPanelRaised = settings.darkTheme ? color(36, 36, 38) : color(228, 230, 233);
        kLine = settings.darkTheme ? color(58, 58, 62) : color(206, 208, 212);
        kText = settings.darkTheme ? color(255, 255, 255) : color(24, 24, 28);
        kTextDim = settings.darkTheme ? color(150, 152, 158) : color(112, 114, 120);
        kPink = settings.darkTheme ? color(92, 150, 255) : color(0, 80, 239);
    }
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
               int clipTop = 0, int clipBottom = kHeight, uint32_t placeholder = 0, bool label = true) {
    CoverSlot* slot = showCovers ? coverSlot(bvid, width, height) : nullptr;
    if (!slot || slot->pixels.empty()) {
        const int top = std::max(y, clipTop);
        const int bottom = std::min(y + height, clipBottom);
        fillRect(x, top, width, bottom - top, placeholder ? placeholder : kPanelRaised);
        if (label)
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

void drawMetroHeader(const pspapp::App& app);
void drawTilesHeader(const pspapp::App& app);

void drawHeader(const pspapp::App& app) {
    static const int iconY[] = {38, 71, 104, 191, 221};
    const bool tile = app.preferences().tileHome();
    const bool navFocus = !tile && app.focusArea() == pspapp::FocusArea::Navigation;
    const int activeNav = app.utilityPage() ? 4 : navFocus ? app.navigationSelection() : app.tab() == pspapp::Tab::Mine ? 2 : 0;
    for (int icon = 0; !tile && icon < 5; ++icon) {
        for (int yy = 0; yy < 19; ++yy) for (int xx = 0; xx < 19; ++xx) {
            const unsigned pixel = sidebarPixels[icon][(yy * 22 / 19) * 22 + xx * 22 / 19];
            const unsigned alpha = ((pixel >> 12) & 15) * 17;
            blendPixel(6 + xx, iconY[icon] + yy,
                       app.preferences().darkTheme ? kTextDim : color(((pixel >> 8) & 15) * 17, ((pixel >> 4) & 15) * 17, (pixel & 15) * 17),
                       alpha);
        }
    }
    if (!tile) fillRect(0, iconY[activeNav] - 1, navFocus ? 3 : 2, 22, kPink);
    if (tile && !app.utilityPage()) {
        if (app.preferences().metro()) drawMetroHeader(app); else drawTilesHeader(app);
        return;
    }
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
        if (tile) { drawText(12, 4, 0.9f, kText, title, 440); return; }
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
    } else if (app.tileHubVisible()) {
        hint = uiText(app, pspapp::TextId::hint_tiles, {{"confirm", confirm}});
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
         app.playbackFailed()) &&
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

// ---- Tile home screen -------------------------------------------------------------------
// A Metro-style panorama. Layout and d-pad navigation come from tile_layout.hpp. Everything
// is drawn on the CPU into the framebuffer, so effects are cheap 2D tricks: a tile "flips"
// by squeezing its height about its centre line, and fades by blending with the background.
// Animation is time based (not frame based) so it looks the same if a frame is dropped.

// Blends d towards s; a is 0..256.
uint32_t shadeMix(uint32_t d, uint32_t s, unsigned a) {
    const uint32_t rb = ((((d & 0x00ff00ffu) * (256 - a)) + ((s & 0x00ff00ffu) * a)) >> 8) & 0x00ff00ffu;
    const uint32_t g = ((((d & 0x0000ff00u) * (256 - a)) + ((s & 0x0000ff00u) * a)) >> 8) & 0x0000ff00u;
    return 0xff000000u | rb | g;
}

// Fills a rectangle with `shade` at the given opacity (0..255), clipped to the screen.
void blendRect(int x, int y, int width, int height, uint32_t shade, unsigned alpha) {
    if (alpha >= 250) { fillRect(x, y, width, height, shade); return; }
    if (!alpha || width <= 0 || height <= 0) return;
    const int left = std::max(0, x), top = std::max(0, y);
    const int right = std::min(kWidth, x + width), bottom = std::min(kHeight, y + height);
    const unsigned a = alpha + (alpha >> 7);
    uint32_t* destination = framebuffer();
    for (int row = top; row < bottom; ++row) {
        uint32_t* line = destination + row * 512;
        for (int column = left; column < right; ++column) line[column] = shadeMix(line[column], shade, a);
    }
}

// Large page titles and the big numbers on live tiles, drawn from the 32 px title font. If the
// font file is missing, or a character is not in it, the text falls back to normal 16 px type.
void drawTitleText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {
    const int width = titleFontReady() ? titleMeasure(text) : -1;
    if (width < 0) { drawText(x, y + 10, 0.9f, shade, text, maxWidth); return; }
    const int right = std::min(kWidth, x + maxWidth);
    for (size_t pos = 0; pos < text.size();) {
        const TitleGlyph* glyph = titleGlyph(bitmapNext(text, pos));
        if (!glyph) continue;
        if (x + glyph->advance > right) break;
        for (int yy = 0; yy < glyph->height; ++yy)
            for (int xx = 0; xx < glyph->width && x + xx < right; ++xx) {
                const int pixel = yy * glyph->width + xx;
                const unsigned alpha = ((glyph->alpha[pixel / 2] >> ((pixel % 2) * 4)) & 15) * 17;
                blendPixel(x + xx, y + yy, shade, alpha);
            }
        x += glyph->advance;
    }
}

// Tiles style: 16 px text drawn at double size (32 px) for the big numbers on live tiles.
void drawBigText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {
    const int right = std::min(kWidth, x + maxWidth);
    for (size_t pos = 0; pos < text.size();) {
        const auto* glyph = bitmapGlyph(bitmapNext(text, pos), 16);
        if (!glyph) continue;
        if (x + glyph->advance * 2 > right) break;
        for (int yy = 0; yy < glyph->height; ++yy)
            for (int xx = 0; xx < glyph->width; ++xx) {
                const int pixel = yy * glyph->width + xx;
                const unsigned alpha = ((glyph->alpha[pixel / 2] >> ((pixel % 2) * 4)) & 15) * 17;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                    blendPixel(x + xx * 2 + dx, y + yy * 2 + dy, shade, alpha);
            }
        x += glyph->advance * 2;
    }
}

void drawIconGlyph(int icon, int x, int y, int size) {
    for (int yy = 0; yy < size; ++yy) for (int xx = 0; xx < size; ++xx) {
        const unsigned pixel = sidebarPixels[icon][(yy * 22 / size) * 22 + xx * 22 / size];
        blendPixel(x + xx, y + yy, color(255, 255, 255), ((pixel >> 12) & 15) * 17);
    }
}

// ---- Line icons: small vector shapes (32x32 grid) rendered with antialiasing and cached.
enum IconId { IconHome, IconFire, IconUser, IconSearch, IconClock, IconMail, IconGear, IconCompass, IconCount };
struct IconSeg { float x1, y1, x2, y2; };
struct IconCircle { float cx, cy, r; };
struct IconShape { std::vector<IconSeg> segs; std::vector<IconCircle> circles; };

void iconPoly(IconShape& shape, std::initializer_list<float> points, bool closed = false) {
    const float* p = points.begin();
    const size_t count = points.size() / 2;
    for (size_t i = 0; i + 1 < count; ++i) shape.segs.push_back({p[i * 2], p[i * 2 + 1], p[i * 2 + 2], p[i * 2 + 3]});
    if (closed && count > 2) shape.segs.push_back({p[count * 2 - 2], p[count * 2 - 1], p[0], p[1]});
}

IconShape buildIcon(int id) {
    IconShape s;
    switch (id) {
        case IconHome:
            iconPoly(s, {4, 15, 16, 4, 28, 15});
            iconPoly(s, {7, 13, 7, 28, 13, 28, 13, 19, 19, 19, 19, 28, 25, 28, 25, 13});
            break;
        case IconFire:
            iconPoly(s, {16, 3, 18, 8, 22, 11, 24, 16, 24, 19, 23, 23, 20, 26.5f, 16, 27.5f, 12, 26.5f, 9, 23, 8, 19,
                         9, 15, 11, 11, 13, 14, 15, 14, 16, 10, 16, 3});
            break;
        case IconUser:
            s.circles.push_back({16, 11, 6});
            iconPoly(s, {4, 29, 5, 24.5f, 8.5f, 21, 12, 19.5f, 16, 19, 20, 19.5f, 23.5f, 21, 27, 24.5f, 28, 29});
            break;
        case IconSearch:
            s.circles.push_back({13, 13, 8});
            iconPoly(s, {19, 19, 28, 28});
            break;
        case IconClock:
            s.circles.push_back({16, 16, 12});
            iconPoly(s, {16, 8, 16, 16, 22, 20});
            break;
        case IconMail:
            iconPoly(s, {3, 7, 29, 7, 29, 25, 3, 25}, true);
            iconPoly(s, {3, 8, 16, 18, 29, 8});
            break;
        case IconGear:
            s.circles.push_back({16, 16, 5});
            iconPoly(s, {16, 2, 16, 7}); iconPoly(s, {16, 25, 16, 30}); iconPoly(s, {2, 16, 7, 16});
            iconPoly(s, {25, 16, 30, 16}); iconPoly(s, {6, 6, 9.5f, 9.5f}); iconPoly(s, {22.5f, 22.5f, 26, 26});
            iconPoly(s, {26, 6, 22.5f, 9.5f}); iconPoly(s, {9.5f, 22.5f, 6, 26});
            break;
        case IconCompass:
            s.circles.push_back({16, 16, 12});
            iconPoly(s, {21, 11, 18, 18, 11, 21, 14, 14}, true);
            break;
    }
    return s;
}

float segmentDistance(const IconSeg& g, float px, float py) {
    const float dx = g.x2 - g.x1, dy = g.y2 - g.y1;
    const float length2 = dx * dx + dy * dy;
    float t = length2 > 0.f ? ((px - g.x1) * dx + (py - g.y1) * dy) / length2 : 0.f;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    const float cx = g.x1 + dx * t - px, cy = g.y1 + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

// Returns an alpha bitmap (size x size) for the icon; built on first use.
const std::vector<uint8_t>& iconBitmap(int id, int size) {
    // One cached bitmap per icon and size; only the sizes the UI uses are supported.
    static const int kSizes[] = {22, 26, 40};
    static std::vector<uint8_t> cache[IconCount][3];
    static const std::vector<uint8_t> none;
    int slot = -1;
    for (int k = 0; k < 3; ++k) if (kSizes[k] == size) slot = k;
    if (slot < 0) return none;
    std::vector<uint8_t>& bitmap = cache[id][slot];
    if (!bitmap.empty()) return bitmap;
    const IconShape shape = buildIcon(id);
    const float scale = 32.f / size, halfStroke = size > 30 ? 1.25f : 1.0f;
    bitmap.assign(static_cast<size_t>(size) * size, 0);
    for (int py = 0; py < size; ++py) for (int px = 0; px < size; ++px) {
        const float gx = (px + 0.5f) * scale, gy = (py + 0.5f) * scale;
        float nearest = 1e9f;
        for (const auto& seg : shape.segs) nearest = std::min(nearest, segmentDistance(seg, gx, gy));
        for (const auto& circle : shape.circles)
            nearest = std::min(nearest, std::fabs(std::sqrt((gx - circle.cx) * (gx - circle.cx) +
                                                           (gy - circle.cy) * (gy - circle.cy)) - circle.r));
        const float coverage = halfStroke + 0.5f - nearest / scale;
        bitmap[static_cast<size_t>(py) * size + px] = static_cast<uint8_t>(255.f * (coverage < 0.f ? 0.f : coverage > 1.f ? 1.f : coverage));
    }
    return bitmap;
}

void drawLineIcon(int id, int x, int y, int size, uint32_t shade) {
    if (id < 0 || id >= IconCount) return;
    const auto& bitmap = iconBitmap(id, size);
    if (bitmap.empty()) return;
    for (int yy = 0; yy < size; ++yy) for (int xx = 0; xx < size; ++xx)
        blendPixel(x + xx, y + yy, shade, bitmap[static_cast<size_t>(yy) * size + xx]);
}

// Top status strip: clock and battery on the right.
void drawStatusBar() {
    ScePspDateTime now{};
    sceRtcGetCurrentClockLocalTime(&now);
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%d:%02d", now.hour, now.minute);
    int percent = scePowerGetBatteryLifePercent();
    if (percent < 0 || percent > 100) percent = 100;
    drawText(446 - bitmapMeasure(clock, 12), 1, 0.42f, kTextDim, clock, 60);
    fillRect(452, 3, 16, 7, kTextDim);
    fillRect(453, 4, 14, 5, kBackground);
    fillRect(454, 5, std::max(1, percent * 12 / 100), 3, kTextDim);
    fillRect(468, 5, 2, 3, kTextDim);
}

float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
float easeOut(float t) { t = 1.f - clamp01(t); return 1.f - t * t * t; }
float easeIn(float t) { t = clamp01(t); return t * t; }

struct HubFx {
    bool wasVisible = false;
    int64_t enterMs = 0;        // when the tile screen last appeared
    bool ringReady = false;
    float ring[4]{};            // focus ring rectangle in panorama coordinates
    float pan = 0.f, panTarget = 0.f;
};
HubFx hubFx;

// Per-tile entrance (staggered left to right) and launch (the chosen tile leaves last).
struct TileMotion { float scale = 1.f; float opacity = 1.f; bool back = false; };

TileMotion tileMotion(const pspapp::App& app, int index, bool live, int64_t now) {
    using namespace pspapp;
    TileMotion motion;
    const TileSpec& tile = kTiles[index];
    const int launch = app.tileLaunchMs();
    if (launch >= 0) {
        const int distance = std::abs(tile.col - kTiles[app.tileSelection()].col);
        const bool chosen = index == app.tileSelection();
        const float delay = chosen ? 130.f : distance * 18.f;
        const float out = clamp01((launch - delay) / 220.f);
        motion.scale = 1.f - easeIn(out);
        motion.opacity = 1.f - out * 0.4f;
        if (chosen && launch < 130) motion.scale = 1.f - 0.07f * easeOut(launch / 130.f);  // press
        return motion;
    }
    const float in = clamp01((now - hubFx.enterMs - (tile.col * 55 + tile.row * 28)) / 420.f);
    if (in < 1.f) {
        motion.scale = std::max(0.02f, easeOut(in));
        motion.opacity = easeOut(in);
        return motion;
    }
    if (live) {
        // Live tile: flip to the back face for a few seconds, then back again.
        const int64_t period = 6200 + (index % 3) * 700;
        const int64_t t = now - hubFx.enterMs - 2400 - index * 900;
        if (t >= 0) {
            const int m = static_cast<int>(t % period);
            if (m < 330) { motion.scale = 1.f - easeIn(m / 330.f); }
            else if (m < 660) { motion.back = true; motion.scale = easeOut((m - 330) / 330.f); }
            else if (m < 3300) { motion.back = true; }
            else if (m < 3630) { motion.back = true; motion.scale = 1.f - easeIn((m - 3300) / 330.f); }
            else if (m < 3960) { motion.scale = easeOut((m - 3630) / 330.f); }
            motion.scale = std::max(motion.scale, 0.02f);
        }
    }
    return motion;
}

std::string lowered(std::string label) {
    for (char& c : label) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return label;
}

void drawHub(const pspapp::App& app, int64_t now) {
    using namespace pspapp;
    struct Look { TileId id; TextId label; int icon; uint32_t shade; };
    static const Look looks[] = {
        {TileId::Recommend, TextId::tile_recommend, IconHome, color(0, 80, 239)},
        {TileId::Hots,      TextId::tile_hots,      IconFire, color(216, 0, 115)},
        {TileId::Search,    TextId::tile_search,    IconSearch, color(100, 118, 135)},
        {TileId::Discover,  TextId::tile_discover,  IconCompass, color(162, 0, 37)},
        {TileId::Dynamic,   TextId::tile_dynamic,   IconUser, color(0, 171, 169)},
        {TileId::Inbox,     TextId::tile_inbox,     IconMail, color(0, 80, 239)},
        {TileId::Mine,      TextId::tile_mine,      IconClock, color(250, 104, 0)},
        {TileId::Settings,  TextId::tile_settings,  IconGear, color(100, 118, 135)},
    };
    const Locale locale = app.preferences().locale;
    const uint32_t white = color(255, 255, 255), softWhite = color(228, 232, 240);

    drawStatusBar();

    // Smooth scrolling toward the pan that keeps the selection on screen.
    hubFx.panTarget = static_cast<float>(tilePan(app.tileSelection(), static_cast<int>(hubFx.panTarget)));
    hubFx.pan += (hubFx.panTarget - hubFx.pan) * 0.22f;
    if (std::fabs(hubFx.panTarget - hubFx.pan) < 0.4f) hubFx.pan = hubFx.panTarget;
    const float pan = hubFx.pan;

    // Large light title: slides in on entrance and drifts slower than the tiles (parallax).
    const float titleIn = easeOut((now - hubFx.enterMs) / 450.f);
    drawTitleText(kTileOriginX - static_cast<int>(pan * 0.35f) - static_cast<int>((1.f - titleIn) * 36.f), 5,
                   shadeMix(kBackground, kText, static_cast<unsigned>(titleIn * 256.f)),
                   lowered(text(TextId::tile_title, locale)), 300);

    for (int i = 0; i < kTileCount; ++i) {
        const TileSpec& tile = kTiles[i];
        const Look* look = nullptr;
        for (const Look& candidate : looks) if (candidate.id == tile.id) look = &candidate;
        if (!look) continue;
        const int x = tileX(tile) - static_cast<int>(pan + 0.5f), y = tileY(tile), w = tileWidth(tile), h = tileHeight(tile);
        if (x + w + 4 < 0 || x - 4 >= kWidth) continue;

        std::string big, small;
        const bool live = app.tileLive(tile.id, big, small);
        const TileMotion motion = tileMotion(app, i, live, now);

        // Squeeze about the centre line (flip) and slightly in width (depth).
        const int drawH = std::max(1, static_cast<int>(h * motion.scale));
        const int drawW = static_cast<int>(w * (0.88f + 0.12f * motion.scale));
        const int drawX = x + (w - drawW) / 2, drawY = y + (h - drawH) / 2;
        const uint32_t face = motion.back ? shadeMix(look->shade, color(0, 0, 0), 56) : look->shade;
        blendRect(drawX, drawY, drawW, drawH, face, static_cast<unsigned>(motion.opacity * 255.f));
        if (motion.scale < 0.96f || motion.opacity < 0.96f) continue;  // content only on a flat face

        const char* label = text(look->label, locale);
        if (motion.back) {
            drawTitleText(x + 8, y + 2, white, big, w - 16);
            if (tile.cols == 1) continue;
            drawWrapped(x + 8, y + h - 32, 0.5f, softWhite, small, w - 16, 2, 16);
        } else if (tile.cols == 1) {
            drawLineIcon(look->icon, x + (w - 26) / 2, y + (h - 26) / 2, 26, white);
        } else {
            drawLineIcon(look->icon, x + 8, y + 8, 40, white);
            const std::string subtitle = app.tileSubtitle(tile.id);
            if (!subtitle.empty()) drawText(x + 8, y + h - 34, 0.5f, softWhite, subtitle, w - 16);
            drawText(x + 8, y + h - 18, 0.5f, white, label, w - 16);
        }
    }

    // Focus ring glides between tiles instead of jumping.
    const TileSpec& selected = kTiles[app.tileSelection()];
    const float target[4] = {static_cast<float>(tileX(selected)), static_cast<float>(tileY(selected)),
                             static_cast<float>(tileWidth(selected)), static_cast<float>(tileHeight(selected))};
    if (!hubFx.ringReady) { for (int k = 0; k < 4; ++k) hubFx.ring[k] = target[k]; hubFx.ringReady = true; }
    for (int k = 0; k < 4; ++k) {
        hubFx.ring[k] += (target[k] - hubFx.ring[k]) * 0.38f;
        if (std::fabs(target[k] - hubFx.ring[k]) < 0.5f) hubFx.ring[k] = target[k];
    }
    const int launch = app.tileLaunchMs();
    const bool entered = now - hubFx.enterMs > 700;
    if (entered && (launch < 0 || launch < 90)) {
        const int rx = static_cast<int>(hubFx.ring[0] - pan + 0.5f), ry = static_cast<int>(hubFx.ring[1] + 0.5f);
        const int rw = static_cast<int>(hubFx.ring[2] + 0.5f), rh = static_cast<int>(hubFx.ring[3] + 0.5f);
        fillRect(rx - 3, ry - 3, rw + 6, 2, kText);
        fillRect(rx - 3, ry + rh + 1, rw + 6, 2, kText);
        fillRect(rx - 3, ry - 3, 2, rh + 6, kText);
        fillRect(rx + rw + 1, ry - 3, 2, rh + 6, kText);
    }
}

void drawHubTiles(const pspapp::App& app, int64_t now) {
    using namespace pspapp;
    struct Look { TileId id; TextId label; int icon; uint32_t shade; };
    static const Look looks[] = {
        {TileId::Recommend, TextId::tile_recommend, 0, color(0, 80, 239)},
        {TileId::Hots,      TextId::tile_hots,     -1, color(216, 0, 115)},
        {TileId::Search,    TextId::tile_search,   -1, color(162, 0, 37)},
        {TileId::Discover,  TextId::tile_discover, -1, color(0, 171, 169)},
        {TileId::Dynamic,   TextId::tile_dynamic,   1, color(96, 169, 23)},
        {TileId::Inbox,     TextId::tile_inbox,     3, color(250, 104, 0)},
        {TileId::Mine,      TextId::tile_mine,      2, color(106, 0, 255)},
        {TileId::Settings,  TextId::tile_settings,  4, color(100, 118, 135)},
    };
    const Locale locale = app.preferences().locale;
    const uint32_t white = color(255, 255, 255), softWhite = color(228, 232, 240);

    // Smooth scrolling toward the pan that keeps the selection on screen.
    hubFx.panTarget = static_cast<float>(tilePan(app.tileSelection(), static_cast<int>(hubFx.panTarget)));
    hubFx.pan += (hubFx.panTarget - hubFx.pan) * 0.22f;
    if (std::fabs(hubFx.panTarget - hubFx.pan) < 0.4f) hubFx.pan = hubFx.panTarget;
    const float pan = hubFx.pan;

    // Title slides in on entrance and drifts slower than the tiles (parallax).
    const float titleIn = easeOut((now - hubFx.enterMs) / 450.f);
    drawText(kTileOriginX - static_cast<int>(pan * 0.35f) - static_cast<int>((1.f - titleIn) * 36.f), 6, 0.9f,
             shadeMix(kBackground, kText, static_cast<unsigned>(titleIn * 256.f)),
             text(TextId::tile_title, locale), 300);

    for (int i = 0; i < kTileCount; ++i) {
        const TileSpec& tile = kTiles[i];
        const Look* look = nullptr;
        for (const Look& candidate : looks) if (candidate.id == tile.id) look = &candidate;
        if (!look) continue;
        const int x = tileX(tile) - static_cast<int>(pan + 0.5f), y = tileY(tile), w = tileWidth(tile), h = tileHeight(tile);
        if (x + w + 4 < 0 || x - 4 >= kWidth) continue;

        std::string big, small;
        const bool live = app.tileLive(tile.id, big, small);
        const TileMotion motion = tileMotion(app, i, live, now);

        // Squeeze about the centre line (flip) and slightly in width (depth).
        const int drawH = std::max(1, static_cast<int>(h * motion.scale));
        const int drawW = static_cast<int>(w * (0.88f + 0.12f * motion.scale));
        const int drawX = x + (w - drawW) / 2, drawY = y + (h - drawH) / 2;
        const uint32_t face = motion.back ? shadeMix(look->shade, color(0, 0, 0), 56) : look->shade;
        blendRect(drawX, drawY, drawW, drawH, face, static_cast<unsigned>(motion.opacity * 255.f));
        if (motion.scale < 0.96f || motion.opacity < 0.96f) continue;  // content only on a flat face

        const char* label = text(look->label, locale);
        if (motion.back) {
            drawBigText(x + 8, y + 6, white, big, w - 16);
            if (tile.cols == 1) continue;
            drawWrapped(x + 8, y + h - 32, 0.5f, softWhite, small, w - 16, 2, 16);
        } else if (tile.cols == 1) {
            if (look->icon >= 0) {
                drawIconGlyph(look->icon, x + (w - 22) / 2, y + 10, 22);
                drawText(x + 2, y + h - 18, 0.5f, white, label, w - 4, true);
            } else {
                const bool fits = bitmapMeasure(label, 16) <= w - 4;
                drawText(x + 2, y + (h - (fits ? 16 : 12)) / 2, fits ? 0.9f : 0.5f, white, label, w - 4, true);
            }
        } else {
            if (look->icon >= 0) drawIconGlyph(look->icon, x + 8, y + 8, 44);
            drawText(x + 8, y + h - 24, 0.9f, white, label, w - 16);
            const std::string subtitle = app.tileSubtitle(tile.id);
            if (!subtitle.empty()) drawText(x + 8, y + h - 42, 0.5f, softWhite, subtitle, w - 16);
        }
    }

    // Focus ring glides between tiles instead of jumping.
    const TileSpec& selected = kTiles[app.tileSelection()];
    const float target[4] = {static_cast<float>(tileX(selected)), static_cast<float>(tileY(selected)),
                             static_cast<float>(tileWidth(selected)), static_cast<float>(tileHeight(selected))};
    if (!hubFx.ringReady) { for (int k = 0; k < 4; ++k) hubFx.ring[k] = target[k]; hubFx.ringReady = true; }
    for (int k = 0; k < 4; ++k) {
        hubFx.ring[k] += (target[k] - hubFx.ring[k]) * 0.38f;
        if (std::fabs(target[k] - hubFx.ring[k]) < 0.5f) hubFx.ring[k] = target[k];
    }
    const int launch = app.tileLaunchMs();
    const bool entered = now - hubFx.enterMs > 700;
    if (entered && (launch < 0 || launch < 90)) {
        const int rx = static_cast<int>(hubFx.ring[0] - pan + 0.5f), ry = static_cast<int>(hubFx.ring[1] + 0.5f);
        const int rw = static_cast<int>(hubFx.ring[2] + 0.5f), rh = static_cast<int>(hubFx.ring[3] + 0.5f);
        fillRect(rx - 3, ry - 3, rw + 6, 2, kText);
        fillRect(rx - 3, ry + rh + 1, rw + 6, 2, kText);
        fillRect(rx - 3, ry - 3, 2, rh + 6, kText);
        fillRect(rx + rw + 1, ry - 3, 2, rh + 6, kText);
    }
}

// ---- Tile pages ---------------------------------------------------------------------------
// With the tile theme on, every page shares the Metro look: flat palette, status bar, a large
// lowercase title with a pivot row, list rows with a small cover, a tinted selection that glides
// between rows, and a flip-in transition whenever the page changes. Navigation and data stay in
// App; this is drawing only.
constexpr int kListBottom = 252, kRowPitch = 36, kRowHeight = 34;
// Top of the Metro list area; pages without a pivot row (such as an uploader's videos) start higher.
struct ListBounds { int top = 66, y0 = 70; };
ListBounds listBounds;
const uint32_t kTilePalette[] = {color(0, 80, 239), color(216, 0, 115), color(96, 169, 23), color(250, 104, 0),
                                 color(0, 171, 169), color(162, 0, 37), color(106, 0, 255), color(100, 118, 135)};

struct PageFx {
    uint64_t key = ~0ull;
    int64_t enterMs = 0;
    float scroll = 0.f, velocity = 0.f;
    uint64_t lastUs = 0;
    bool glideReady = false;
    float glide[4]{};
    bool ringReady = false;  // Tiles style focus ring
    float ring[4]{};
};
PageFx pageFx;

struct TileItem {
    const std::string* bvid;
    const std::string* title;
    std::string meta;
    int duration;
};

// Spring-damped vertical scroll toward `target` pixels.
int updateScroll(float target) {
    const uint64_t now = sceKernelGetSystemTimeWide();
    const float dt = pageFx.lastUs ? std::min(0.033f, static_cast<float>(now - pageFx.lastUs) / 1000000.0f) : 0.016f;
    pageFx.lastUs = now;
    pageFx.velocity += (190.0f * (target - pageFx.scroll) - 24.0f * pageFx.velocity) * dt;
    pageFx.scroll += pageFx.velocity * dt;
    if (std::fabs(target - pageFx.scroll) < 0.4f && std::fabs(pageFx.velocity) < 1.0f) {
        pageFx.scroll = target;
        pageFx.velocity = 0.f;
    }
    return static_cast<int>(pageFx.scroll + 0.5f);
}

// Selected-row highlight: accent tint with an underline, as on the web design.
void drawSelection(int x, int y, int w, int h, int clipTop, int clipBottom) {
    const int top = std::max(y, clipTop), bottom = std::min(y + h, clipBottom);
    if (bottom <= top) return;
    blendRect(x, top, w, bottom - top, kPink, 56);
    if (y + h - 1 >= clipTop && y + h - 1 < clipBottom) fillRect(x, y + h - 1, w, 1, kPink);
}

// The highlight glides to its target instead of jumping.
void drawGlidingSelection(const float target[4]) {
    if (!pageFx.glideReady) { for (int k = 0; k < 4; ++k) pageFx.glide[k] = target[k]; pageFx.glideReady = true; }
    for (int k = 0; k < 4; ++k) {
        pageFx.glide[k] += (target[k] - pageFx.glide[k]) * 0.4f;
        if (std::fabs(target[k] - pageFx.glide[k]) < 0.5f) pageFx.glide[k] = target[k];
    }
    drawSelection(static_cast<int>(pageFx.glide[0] + 0.5f), static_cast<int>(pageFx.glide[1] + 0.5f),
                  static_cast<int>(pageFx.glide[2] + 0.5f), static_cast<int>(pageFx.glide[3] + 0.5f),
                  listBounds.top, kListBottom);
}

void drawRow(int y, const TileItem& item, size_t index) {
    const int top = std::max(y, listBounds.top), bottom = std::min(y + kRowHeight, kListBottom);
    if (bottom <= top) return;
    drawCover(*item.bvid, 12, y + 2, 54, 30, false, top, bottom, kTilePalette[index % 8], false);
    drawText(74, y + 2, 0.7f, kText, *item.title, 388, false, top, bottom);
    drawText(74, y + 19, 0.42f, kTextDim, item.meta, 388, false, top, bottom);
}

// Draws `items` as list rows starting `topInset` pixels below the list origin.
void drawRowList(const std::vector<TileItem>& items, int topInset, int scroll) {
    for (size_t i = 0; i < items.size(); ++i) {
        const int y = listBounds.y0 + topInset + static_cast<int>(i) * kRowPitch - scroll;
        if (y >= kListBottom || y + kRowHeight <= listBounds.top) continue;
        drawRow(y, items[i], i);
    }
}

void drawVideoGrid(const pspapp::App& app) {
    const auto& cards = app.cards();
    std::vector<TileItem> items;
    items.reserve(cards.size());
    for (const auto& card : cards) {
        std::string meta = card.author;
        if (card.views > 0) meta += "  ·  " + formatCount(card.views);
        if (card.duration > 0) meta += "  ·  " + formatDuration(card.duration);
        items.push_back({&card.bvid, &card.title, std::move(meta), card.duration});
    }
    const int selected = static_cast<int>(app.selection());
    const int scroll = updateScroll(std::max(0.f, static_cast<float>((selected - 2) * kRowPitch)));
    drawRowList(items, 0, scroll);
    if (app.focusArea() == pspapp::FocusArea::Content) {
        const float target[4] = {12.f, static_cast<float>(listBounds.y0 + selected * kRowPitch - scroll), 456.f,
                                 static_cast<float>(kRowHeight)};
        drawGlidingSelection(target);
    }
}

void drawMineTiles(const pspapp::App& app) {
    using namespace pspapp;
    const auto& auth = app.auth();
    const bool signedIn = auth.state == AuthState::SignedIn;
    const int selection = static_cast<int>(app.selection());
    const int scroll = updateScroll(std::max(0.f, static_cast<float>((selection - 2) * kRowPitch)));

    // Account row: accent square with a person icon, like a thumbnail.
    const int ay = listBounds.y0 - scroll;
    if (ay + kRowHeight > listBounds.top) {
        const int top = std::max(ay, listBounds.top), bottom = std::min(ay + kRowHeight, kListBottom);
        fillRect(12, top, 54, bottom - top, kTilePalette[0]);
        if (ay >= listBounds.top) drawLineIcon(IconUser, 12 + 16, ay + 6, 22, color(255, 255, 255));
        drawText(74, ay + 2, 0.7f, kText,
                 signedIn ? "账号 · " + (auth.name.empty() ? auth.mid : auth.name) : "账号 · 扫码登录",
                 388, false, top, bottom);
        drawText(74, ay + 19, 0.42f, kTextDim, authStateMessage(auth.state, app.preferences().confirmWithCircle),
                 388, false, top, bottom);
    }

    std::vector<TileItem> items;
    items.reserve(app.history().size() + app.later().size());
    for (const auto& entry : app.history())
        items.push_back({&entry.bvid, &entry.title, "历史 · " + entry.author + " · " + formatDuration(entry.duration),
                         entry.duration});
    for (const auto& entry : app.later())
        items.push_back({&entry.bvid, &entry.title, "稍后 · " + entry.author + " · " + formatDuration(entry.duration),
                         entry.duration});
    if (items.empty()) {
        drawText(12, listBounds.y0 + kRowPitch + 8, 0.7f, kText, "还没有本地记录", 440);
        drawText(12, listBounds.y0 + kRowPitch + 30, 0.5f, kTextDim, "播放后保存历史；详情页按 □ 加入稍后观看。", 440);
    }
    drawRowList(items, kRowPitch, scroll);
    if (app.focusArea() == FocusArea::Content) {
        const float target[4] = {12.f, static_cast<float>(listBounds.y0 + selection * kRowPitch - scroll), 456.f,
                                 static_cast<float>(kRowHeight)};
        drawGlidingSelection(target);
    }
}

constexpr int kGridTop = 49, kGridBottom = 251, kGridY0 = 53;
constexpr int kTileX = 12, kTileW = 224, kTileH = 98, kPitchX = 228, kPitchY = 102;
constexpr int kAccountH = 50;
void fillClipped(int x, int y, int w, int h, uint32_t shade) {
    const int top = std::max(y, kGridTop), bottom = std::min(y + h, kGridBottom);
    if (bottom > top) fillRect(x, top, w, bottom - top, shade);
}

// Focus ring that glides to its target rectangle.
void drawRing(const float target[4]) {
    if (!pageFx.ringReady) { for (int k = 0; k < 4; ++k) pageFx.ring[k] = target[k]; pageFx.ringReady = true; }
    for (int k = 0; k < 4; ++k) {
        pageFx.ring[k] += (target[k] - pageFx.ring[k]) * 0.4f;
        if (std::fabs(target[k] - pageFx.ring[k]) < 0.5f) pageFx.ring[k] = target[k];
    }
    const int x = static_cast<int>(pageFx.ring[0] + 0.5f), y = static_cast<int>(pageFx.ring[1] + 0.5f);
    const int w = static_cast<int>(pageFx.ring[2] + 0.5f), h = static_cast<int>(pageFx.ring[3] + 0.5f);
    fillClipped(x - 3, y - 3, w + 6, 2, kText);
    fillClipped(x - 3, y + h + 1, w + 6, 2, kText);
    fillClipped(x - 3, y - 3, 2, h + 6, kText);
    fillClipped(x + w + 1, y - 3, 2, h + 6, kText);
}

void drawVideoTile(int x, int y, const TileItem& item, size_t index) {
    const int top = std::max(y, kGridTop), bottom = std::min(y + kTileH, kGridBottom);
    if (bottom <= top) return;
    const uint32_t white = color(255, 255, 255);
    // The 16:9 cover is drawn taller than the tile and cropped by the clip rows.
    drawCover(*item.bvid, x, y - 10, kTileW, 118, false, top, bottom, kTilePalette[index % 8], false);
    if (item.duration > 0 && y + 4 >= kGridTop) {
        const std::string duration = formatDuration(item.duration);
        const int badge = static_cast<int>(duration.size()) * 6 + 8;
        blendRect(x + kTileW - badge - 4, y + 4, badge, 14, color(0, 0, 0), 170);
        drawText(x + kTileW - badge, y + 4, 0.42f, white, duration, badge, false, top, bottom);
    }
    const int stripTop = std::max(y + kTileH - 32, top);
    if (bottom > stripTop) blendRect(x, stripTop, kTileW, bottom - stripTop, color(0, 0, 0), 175);
    drawText(x + 6, y + kTileH - 31, 0.5f, white, *item.title, kTileW - 12, false, top, bottom);
    drawText(x + 6, y + kTileH - 16, 0.42f, color(200, 204, 214), item.meta, kTileW - 12, false, top, bottom);
}

// Draws `items` as a two-column tile grid starting `topInset` pixels below the grid origin.
void drawTileGrid(const std::vector<TileItem>& items, int selected, int topInset, int scroll) {
    for (size_t i = 0; i < items.size(); ++i) {
        const int x = kTileX + static_cast<int>(i % 2) * kPitchX;
        const int y = kGridY0 + topInset + static_cast<int>(i / 2) * kPitchY - scroll;
        if (y >= kGridBottom || y + kTileH <= kGridTop) continue;
        drawVideoTile(x, y, items[i], i);
    }
    if (selected >= 0 && static_cast<size_t>(selected) < items.size()) {
        const float target[4] = {static_cast<float>(kTileX + (selected % 2) * kPitchX),
                                 static_cast<float>(kGridY0 + topInset + (selected / 2) * kPitchY - scroll),
                                 static_cast<float>(kTileW), static_cast<float>(kTileH)};
        drawRing(target);
    }
}

void drawVideoGridTiles(const pspapp::App& app) {
    const auto& cards = app.cards();
    std::vector<TileItem> items;
    items.reserve(cards.size());
    for (const auto& card : cards) {
        std::string meta = card.author;
        if (card.views > 0) meta += "  ·  " + formatCount(card.views);
        items.push_back({&card.bvid, &card.title, std::move(meta), card.duration});
    }
    const bool focus = app.focusArea() == pspapp::FocusArea::Content;
    const int row = static_cast<int>(app.selection() / 2);
    const int scroll = updateScroll(std::max(0.f, static_cast<float>(row * kPitchY - 100)));
    drawTileGrid(items, focus ? static_cast<int>(app.selection()) : -1, 0, scroll);
}

void drawMineGrid(const pspapp::App& app) {
    using namespace pspapp;
    const auto& auth = app.auth();
    const bool signedIn = auth.state == AuthState::SignedIn;
    const size_t selection = app.selection();
    const bool focus = app.focusArea() == FocusArea::Content;
    constexpr int inset = kAccountH + 4;
    const int entryRow = selection ? static_cast<int>((selection - 1) / 2) : 0;
    const int scroll = updateScroll(selection ? std::max(0.f, static_cast<float>(inset + entryRow * kPitchY - 100)) : 0.f);

    // Account tile.
    const int ay = kGridY0 - scroll;
    const uint32_t white = color(255, 255, 255);
    if (ay + kAccountH > kGridTop) {
        const int top = std::max(ay, kGridTop), bottom = std::min(ay + kAccountH, kGridBottom);
        fillRect(kTileX, top, 2 * kPitchX - 4, bottom - top, kTilePalette[0]);
        drawText(kTileX + 10, ay + 7, 0.7f, white,
                 signedIn ? "账号 · " + (auth.name.empty() ? auth.mid : auth.name) : "账号 · 扫码登录",
                 2 * kPitchX - 24, false, top, bottom);
        drawText(kTileX + 10, ay + 29, 0.5f, color(220, 228, 250),
                 authStateMessage(auth.state, app.preferences().confirmWithCircle), 2 * kPitchX - 24, false, top, bottom);
    }

    std::vector<TileItem> items;
    items.reserve(app.history().size() + app.later().size());
    for (const auto& entry : app.history())
        items.push_back({&entry.bvid, &entry.title, "历史 · " + entry.author, entry.duration});
    for (const auto& entry : app.later())
        items.push_back({&entry.bvid, &entry.title, "稍后 · " + entry.author, entry.duration});
    if (items.empty()) {
        drawText(kTileX, kGridY0 + inset + 12, 0.7f, kText, "还没有本地记录", 440);
        drawText(kTileX, kGridY0 + inset + 38, 0.5f, kTextDim, "播放后保存历史；详情页按 □ 加入稍后观看。", 440);
    }
    drawTileGrid(items, focus && selection ? static_cast<int>(selection) - 1 : -1, inset, scroll);
    if (focus && selection == 0) {
        const float target[4] = {static_cast<float>(kTileX), static_cast<float>(ay),
                                 static_cast<float>(2 * kPitchX - 4), static_cast<float>(kAccountH)};
        drawRing(target);
    }
}

// One row of the Metro-style search page: coloured block (icon or rank), title, note.
void drawInfoRow(int y, uint32_t block, int icon, const char* rank, const std::string& title, const std::string& note) {
    const int top = std::max(y, listBounds.top), bottom = std::min(y + kRowHeight, kListBottom);
    if (bottom <= top) return;
    fillRect(12, top, 54, bottom - top, block);
    if (icon >= 0 && y >= listBounds.top && y + kRowHeight <= kListBottom)
        drawLineIcon(icon, 12 + 16, y + 6, 22, color(255, 255, 255));
    if (rank) drawText(12, y + 8, 0.7f, color(255, 255, 255), rank, 54, true, top, bottom);
    drawText(74, y + (note.empty() ? 9 : 2), 0.7f, kText, title, 388, false, top, bottom);
    if (!note.empty()) drawText(74, y + 19, 0.42f, kTextDim, note, 388, false, top, bottom);
}

// Search tab in the Metro style: the content starts below the title and pivot row.
void drawSearchMetro(const pspapp::App& app) {
    using namespace pspapp;
    const std::string confirm = app.preferences().confirmButton();
    const bool focus = app.focusArea() == FocusArea::Content;
    const int selection = static_cast<int>(app.selection());
    auto highlight = [&](int scroll, int inset) {
        if (!focus) return;
        const float target[4] = {12.f, static_cast<float>(listBounds.y0 + inset + selection * kRowPitch - scroll), 456.f,
                                 static_cast<float>(kRowHeight)};
        drawGlidingSelection(target);
    };
    if (app.searchView() == SearchView::History) {
        const auto& history = app.searchHistory();
        drawText(12, listBounds.y0 - 2, 0.42f, kTextDim, "最近搜索 · 仅保存在本机", 440);
        constexpr int inset = 18;
        const int scroll = updateScroll(std::max(0.f, static_cast<float>((selection - 2) * kRowPitch)));
        if (history.empty()) {
            drawText(12, listBounds.y0 + inset + 6, 0.7f, kText, "还没有搜索历史", 440);
            drawText(12, listBounds.y0 + inset + 30, 0.5f, kTextDim,
                     "□ 输入关键词，或 " + std::string(app.preferences().backButton()) + " 返回查看热搜", 440);
            return;
        }
        for (size_t i = 0; i < history.size() && i < 8; ++i)
            drawInfoRow(listBounds.y0 + inset + static_cast<int>(i) * kRowPitch - scroll, kTilePalette[3], IconClock, nullptr,
                        history[i], "");
        highlight(scroll, inset);
        return;
    }
    if (app.searchView() == SearchView::Landing) {
        const auto& topics = app.searchTopics();
        const int scroll = updateScroll(std::max(0.f, static_cast<float>((selection - 2) * kRowPitch)));
        std::string preview;
        for (size_t i = 0; i < app.searchHistory().size() && i < 2; ++i) {
            if (!preview.empty()) preview += "  ·  ";
            preview += app.searchHistory()[i];
        }
        drawInfoRow(listBounds.y0 - scroll, kTilePalette[7], IconSearch, nullptr, "搜索视频", "按 " + confirm + " 输入关键词");
        drawInfoRow(listBounds.y0 + kRowPitch - scroll, kTilePalette[3], IconClock, nullptr, "历史",
                    preview.empty() ? "暂无记录 · " + confirm + " 查看" : preview);
        if (topics.empty()) {
            const int y = listBounds.y0 + 2 * kRowPitch - scroll;
            drawText(12, y + 6, 0.7f, kTextDim, app.busy() ? "正在载入热搜…" : "热搜暂不可用", 440, false, listBounds.top, kListBottom);
            drawText(12, y + 28, 0.5f, kTextDim, "□ 仍可输入关键词搜索", 440, false, listBounds.top, kListBottom);
        }
        for (size_t i = 0; i < topics.size(); ++i) {
            char rank[16];
            std::snprintf(rank, sizeof(rank), "%02u", static_cast<unsigned>(i + 1));
            drawInfoRow(listBounds.y0 + static_cast<int>(i + 2) * kRowPitch - scroll, i < 3 ? kTilePalette[1] : kTilePalette[7],
                        -1, rank, topics[i].title, "");
        }
        highlight(scroll, 0);
        return;
    }
    // Results with nothing to list: the keyword line plus the reason.
    drawText(12, listBounds.y0 - 2, 0.5f, app.searchError().empty() ? kTextDim : kPink,
             app.searchError().empty() ? "关键词：" + app.query() : app.searchError() + " · △ 重试", 380);
    drawText(12, listBounds.y0 + 24, 0.7f, kText, app.busy() ? "正在搜索…" :
             app.searchError().empty() ? "没有找到相关视频" : "搜索未完成", 440);
    drawWrapped(12, listBounds.y0 + 50, 0.5f, kTextDim,
                app.searchError().empty() ? "□ 修改关键词，" + std::string(app.preferences().backButton()) + " 返回搜索"
                                          : app.searchError(), 440, 3, 16);
}

// ---- Video lists inside Browse pages (an uploader's videos, favourites, history, ...) ----------
// These use the same row or tile design as the home and search lists of each style.
std::string browseVideoMeta(const pspapp::BrowseItem& item) {
    std::string meta = item.subtitle;
    if (item.duration > 0) meta += (meta.empty() ? "" : "  ·  ") + formatDuration(item.duration);
    return meta;
}

// Classic: cover on the left, two-line title, author and length, like the search results.
void drawBrowseVideosClassic(const pspapp::App& app) {
    const auto& page = app.browsePage();
    drawText(12, 3, 0.9f, kText, page.title, 456);
    fillRect(12, 26, 456, 1, kLine);
    const size_t total = page.items.size() + (page.hasMore ? 1 : 0);
    const size_t first = app.browseSelection() > 1 ? app.browseSelection() - 1 : 0;
    for (size_t index = first; index < total; ++index) {
        const int y = 35 + static_cast<int>(index - first) * 65;
        if (y + 56 > 251) break;
        const bool selected = index == app.browseSelection();
        if (selected) fillRect(6, y + 2, 2, 52, kPink);
        if (index == page.items.size()) {
            drawText(12, y + 18, 0.7f, selected ? kPink : kText, "加载下一页", 456);
            break;
        }
        const auto& item = page.items[index];
        drawCover(item.bvid, 12, y, 100, 56);
        drawWrapped(120, y, 0.7f, selected ? kPink : kText, item.title, 348, 2, 18);
        drawText(120, y + 40, 0.5f, kTextDim, browseVideoMeta(item), 348);
    }
}

std::vector<TileItem> browseTileItems(const pspapp::BrowsePage& page, const std::string& empty, const std::string& more) {
    std::vector<TileItem> items;
    items.reserve(page.items.size() + 1);
    for (const auto& item : page.items) items.push_back({&item.bvid, &item.title, browseVideoMeta(item), item.duration});
    if (page.hasMore) items.push_back({&empty, &more, "", 0});
    return items;
}

// Tiles: the two-column cover grid of the home tab.
void drawBrowseVideosTiles(const pspapp::App& app) {
    const auto& page = app.browsePage();
    drawText(12, 3, 0.9f, kText, page.title, 456);
    fillRect(12, 26, 456, 1, kLine);
    static const std::string none, more = "加载下一页";
    const auto items = browseTileItems(page, none, more);
    const int selected = static_cast<int>(app.browseSelection());
    const int scroll = updateScroll(std::max(0.f, static_cast<float>(selected / 2 * kPitchY - 100)));
    drawTileGrid(items, selected, 0, scroll);
}

// Metro: a title and the list rows of the home tabs.
void drawBrowseVideosMetro(const pspapp::App& app) {
    const auto& page = app.browsePage();
    listBounds = {46, 50};
    drawStatusBar();
    drawTitleText(10, 5, kText, page.title, 400);
    static const std::string none, more = "加载下一页";
    const auto items = browseTileItems(page, none, more);
    const int selected = static_cast<int>(app.browseSelection());
    const int scroll = updateScroll(std::max(0.f, static_cast<float>((selected - 2) * kRowPitch)));
    drawRowList(items, 0, scroll);
    const float target[4] = {12.f, static_cast<float>(listBounds.y0 + selected * kRowPitch - scroll), 456.f,
                             static_cast<float>(kRowHeight)};
    drawGlidingSelection(target);
}

// Video detail as tiles: cover, title block, four action tiles and a strip of part tiles.
void drawDetailTiles(const pspapp::App& app) {
    using namespace pspapp;
    const auto& detail = app.detail();
    const uint32_t white = color(255, 255, 255), softWhite = color(228, 232, 240);
    drawText(12, 4, 0.9f, kText, "详情", 200);

    drawCover(detail.card.bvid, 12, 32, 224, 126, true, 0, kHeight, kTilePalette[4]);
    drawWrapped(244, 30, 0.7f, kText, detail.card.title, 224, 3, 18);
    drawText(244, 88, 0.5f, kPink, "UP " + detail.card.author, 224);
    drawText(244, 104, 0.5f, kTextDim,
             formatCount(detail.card.views) + " 播放 · " + formatCount(detail.danmaku) + " 弹幕", 224);
    drawWrapped(244, 124, 0.45f, kTextDim, detail.description.empty() ? "暂无简介" : detail.description,
                224, 2, 14);

    const std::string confirm = app.preferences().confirmButton();
    struct Action { uint32_t shade; std::string label; std::string note; };
    const Action actions[] = {
        {kTilePalette[0], confirm + (app.detailStartMs() > 0 ? " 继续" : " 播放"), formatCount(detail.like) + " 赞"},
        {kTilePalette[4], "R 评论", formatCount(detail.coin) + " 投币"},
        {app.currentIsLater() ? kTilePalette[2] : kTilePalette[3], app.currentIsLater() ? "□ 已稍后" : "□ 稍后",
         formatCount(detail.favorite) + " 收藏"},
        {kTilePalette[6], "△ UP主", detail.card.author}};
    for (int i = 0; i < 4; ++i) {
        const int x = 12 + i * 114;
        fillRect(x, 164, 110, 44, actions[i].shade);
        drawText(x + 8, 168, 0.7f, white, actions[i].label, 94);
        drawText(x + 8, 190, 0.45f, softWhite, actions[i].note, 94);
    }

    // Part tiles: three at a time, the selected one centred when possible.
    if (detail.pages.empty()) return;
    const size_t count = detail.pages.size();
    size_t first = app.detailPart() > 0 ? app.detailPart() - 1 : 0;
    if (count > 3 && first + 3 > count) first = count - 3;
    if (count <= 3) first = 0;
    for (size_t i = 0; i < 3 && first + i < count; ++i) {
        const size_t index = first + i;
        const int x = 12 + static_cast<int>(i) * 152;
        const bool selected = index == app.detailPart();
        fillRect(x, 212, 148, 38, selected ? kTilePalette[0] : kPanelRaised);
        drawText(x + 8, 215, 0.5f, selected ? white : kText,
                 "P" + std::to_string(index + 1) + "  " + detail.pages[index].title, 132);
        drawText(x + 8, 232, 0.42f, selected ? softWhite : kTextDim, formatDuration(detail.pages[index].duration), 132);
        if (selected) {
            fillRect(x - 3, 209, 154, 2, kText);
            fillRect(x - 3, 250, 154, 2, kText);
            fillRect(x - 3, 209, 2, 43, kText);
            fillRect(x + 149, 209, 2, 43, kText);
        }
    }
}

// Pivot header for the tab pages: the five sections in a row, the current one bright.
void drawTilesHeader(const pspapp::App& app) {
    using namespace pspapp;
    const Locale locale = app.preferences().locale;
    static const TextId labels[] = {TextId::tile_recommend, TextId::tile_hots, TextId::tile_search,
                                    TextId::tile_discover, TextId::tile_mine};
    static const Tab tabs[] = {Tab::Recommend, Tab::Hots, Tab::Search, Tab::Discover, Tab::Mine};
    const bool pivotFocus = app.focusArea() == FocusArea::Categories;
    int x = 12;
    for (int i = 0; i < 5; ++i) {
        std::string label = text(labels[i], locale);
        for (char& c : label) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool active = app.tab() == tabs[i];
        drawText(x, 4, 0.9f, active ? kText : kTextDim, label, 200);
        const int width = bitmapMeasure(label, 16);
        if (active) fillRect(x, 24, width, pivotFocus ? 3 : 1, kPink);
        x += width + 16;
    }
    if (app.tab() != Tab::Mine) {
        char meta[16]{};
        std::snprintf(meta, sizeof(meta), "P%d", app.page());
        drawText(436, 8, 0.5f, kTextDim, meta, 36);
    }
}

// Header for the tab pages: status bar, a large lowercase title and the pivot row of sections.
void drawMetroHeader(const pspapp::App& app) {
    using namespace pspapp;
    const Locale locale = app.preferences().locale;
    static const TextId labels[] = {TextId::tile_recommend, TextId::tile_hots, TextId::tile_search,
                                    TextId::tile_discover, TextId::tile_mine};
    static const Tab tabs[] = {Tab::Recommend, Tab::Hots, Tab::Search, Tab::Discover, Tab::Mine};
    drawStatusBar();
    for (int i = 0; i < 5; ++i)
        if (app.tab() == tabs[i]) drawTitleText(10, 5, kText, lowered(text(labels[i], locale)), 440);
    const bool pivotFocus = app.focusArea() == FocusArea::Categories;
    int x = 12;
    for (int i = 0; i < 5; ++i) {
        const std::string label = lowered(text(labels[i], locale));
        const bool active = app.tab() == tabs[i];
        drawText(x, 46, 0.9f, active ? kText : kTextDim, label, 200);
        const int width = bitmapMeasure(label, 16);
        if (active) fillRect(x, 63, width, pivotFocus ? 3 : 1, kPink);
        x += width + 16;
    }
}

// Flip-in transition for a page that just appeared: squeezes the finished frame toward the
// centre line and releases it. In place, working outward from the centre, so no copy is needed.
void squeezePage(int64_t now) {
    const float t = clamp01((now - pageFx.enterMs) / 340.f);
    if (t >= 1.f) return;
    const float f = std::max(0.04f, easeOut(t));
    constexpr int half = 129;  // rows 0..257 around the centre line
    uint32_t* destination = framebuffer();
    for (int d = 0; d < half; ++d) {
        const int source = static_cast<int>(d / f);
        const int rows[2][2] = {{half - 1 - d, half - 1 - source}, {half + d, half + source}};
        for (const auto& row : rows) {
            uint32_t* line = destination + row[0] * 512;
            if (source >= half) std::fill(line, line + kWidth, kBackground);
            else if (row[0] != row[1]) std::copy(destination + row[1] * 512, destination + row[1] * 512 + kWidth, line);
        }
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
        drawText(39, 31, 0.5f, kTextDim, pspapp::text(pspapp::TextId::settings_header, locale), 340);
        drawText(382, 31, 0.5f, kTextDim,
                 std::to_string(app.settingSelection() + 1) + " / " +
                     std::to_string(static_cast<size_t>(pspapp::SettingItem::Count)), 85);
        const pspapp::TextId labels[] = {
            pspapp::TextId::settings_theme, pspapp::TextId::settings_home_style,
            pspapp::TextId::settings_body_size,
            pspapp::TextId::settings_covers, pspapp::TextId::settings_repeat,
            pspapp::TextId::settings_playback_buffer, pspapp::TextId::settings_playback_pixel_format,
            pspapp::TextId::settings_me_backend, pspapp::TextId::settings_confirm_button,
            pspapp::TextId::settings_language, pspapp::TextId::settings_log_level,
            pspapp::TextId::settings_log_files, pspapp::TextId::settings_wifi,
            pspapp::TextId::settings_cover_cache, pspapp::TextId::settings_help,
            pspapp::TextId::settings_about};
        const auto& preferences = app.preferences();
        const std::string meBackend = pspapp::text(
            preferences.meBackend == pspapp::MeBackend::CustomCore ? pspapp::TextId::value_me_custom_core
                                                                   : pspapp::TextId::value_me_mist, locale);
        // A saved change only takes effect after the loader picks the other bridge PRX.
        const bool meRestart = app.activeMeBackend() >= 0 &&
            app.activeMeBackend() != static_cast<int>(preferences.meBackend);
        const std::string values[] = {pspapp::text(preferences.darkTheme ? pspapp::TextId::value_dark
                                                                         : pspapp::TextId::value_light, locale),
            pspapp::text(preferences.metro() ? pspapp::TextId::value_home_metro
                         : preferences.tileHome() ? pspapp::TextId::value_home_tiles
                                                  : pspapp::TextId::value_home_classic, locale),
            std::to_string(preferences.bodySize) + " px",
            pspapp::text(preferences.showCovers ? pspapp::TextId::value_on_demand
                                               : pspapp::TextId::value_off, locale),
            pspapp::text(preferences.slowRepeat ? pspapp::TextId::value_slow
                                                : pspapp::TextId::value_standard, locale),
            pspapp::formatText(pspapp::TextId::value_buffer, locale,
                {{"frames", std::to_string(preferences.playbackBufferFrames)},
                 {"memory", std::to_string(preferences.playbackBufferFrames *
                     (pspapp::playbackUsesRgb565(preferences.playbackPixelFormat) ? 272 : 544))}}),
            pspapp::text(preferences.playbackPixelFormat == pspapp::PlaybackPixelFormat::Auto
                    ? (pspapp::defaultPlaybackRgb565() ? pspapp::TextId::value_pixel_auto_565
                                                       : pspapp::TextId::value_pixel_auto_rgba8888)
                : preferences.playbackPixelFormat == pspapp::PlaybackPixelFormat::Rgb565
                    ? pspapp::TextId::value_pixel_565 : pspapp::TextId::value_pixel_rgba8888, locale),
            meRestart ? pspapp::formatText(pspapp::TextId::value_me_restart, locale, {{"backend", meBackend}})
                      : meBackend,
            pspapp::formatText(pspapp::TextId::value_confirm, locale,
                {{"button", preferences.confirmButton()}}),
            pspapp::text(locale == pspapp::Locale::ZhHans ? pspapp::TextId::value_language_zh
                                                          : pspapp::TextId::value_language_en, locale),
            preferences.logLevel == pspapp::LogVerbosity::Error ? "ERROR"
                : preferences.logLevel == pspapp::LogVerbosity::Warn ? "WARN"
                : preferences.logLevel == pspapp::LogVerbosity::Debug ? "DEBUG" : "INFO",
            pspapp::text(preferences.saveLogFiles ? pspapp::TextId::value_log_files_on : pspapp::TextId::value_off, locale),
            pspapp::text(pspapp::TextId::value_system_connect, locale),
            pspapp::text(pspapp::TextId::value_manage, locale),
            pspapp::text(pspapp::TextId::value_view, locale),
            pspapp::text(pspapp::TextId::value_view, locale)};
        const size_t end = std::min(static_cast<size_t>(pspapp::SettingItem::Count),
                                    app.settingsScroll() + pspapp::settingsVisibleRows);
        for (size_t i = app.settingsScroll(); i < end; ++i) {
            const int y = 49 + static_cast<int>(i - app.settingsScroll()) * 18;
            const bool selected = app.settingSelection() == i;
            if (preferences.tileHome()) {
                // Tile theme: the selected row becomes an accent tile.
                if (preferences.metro()) {
                    if (selected) drawSelection(33, y - 1, 436, 18, 0, kHeight);
                    drawText(39, y, 0.7f, kText, pspapp::text(labels[i], locale), 250);
                    drawText(300, y, 0.7f, selected ? kPink : kTextDim, values[i], 168);
                } else {
                    // Tiles style: the selected row becomes an accent tile.
                    if (selected) fillRect(33, y - 1, 436, 18, kTilePalette[0]);
                    drawText(39, y, 0.7f, selected ? color(255, 255, 255) : kText, pspapp::text(labels[i], locale), 250);
                    drawText(300, y, 0.7f, selected ? color(255, 255, 255) : kTextDim, values[i], 168);
                }
                continue;
            }
            if (selected) fillRect(33, y + 2, 2, 13, kPink);
            drawText(39, y, 0.7f, kText, pspapp::text(labels[i], locale), 250);
            drawText(300, y, 0.7f, selected ? kPink : kTextDim, values[i], 168);
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
        if (pspapp::isVideoListPage(page)) {
            if (app.preferences().metro()) drawBrowseVideosMetro(app);
            else if (app.preferences().tileHome()) drawBrowseVideosTiles(app);
            else drawBrowseVideosClassic(app);
            return;
        }
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
        const bool tile = app.preferences().tileHome();
        // Tile theme: each row is a tile; the selected one is filled with the accent colour.
        const int inset = tile ? 8 : 0, width = tile ? 440 : 456;
        const uint32_t white = color(255, 255, 255);
        const bool metro = app.preferences().metro();
        const uint32_t titleShade = tile ? (metro || !selected ? kText : white) : (selected ? kPink : kText);
        const uint32_t noteShade = tile && !metro && selected ? color(222, 230, 252) : kTextDim;
        if (tile && metro) { if (selected) drawSelection(12, y, 456, 62, 0, kHeight); }
        else if (tile) fillRect(12, y, 456, 62, selected ? kTilePalette[0] : kPanelRaised);
        else if (selected) fillRect(6, y + 2, 2, 50, kPink);
        if (index == page.items.size()) {
            drawText(12 + inset, y + (tile ? 20 : 0), 0.7f, titleShade, "加载下一页", width);
            break;
        }
        const auto& item = page.items[index];
        drawWrapped(12 + inset, y + (tile ? 5 : 0), 0.7f, titleShade, item.title, width, 1, 18);
        drawWrapped(12 + inset, y + (tile ? 25 : 20), 0.5f, noteShade,
            item.next.kind == pspapp::BrowseKind::LiveStream || item.text.empty()
                ? item.subtitle : item.text, width, 2, 16);
        if (!tile && y + 62 < 251) fillRect(12, y + 62, 456, 1, kLine);
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
    titleFontInit(CLIENT_PATH("/title.bmf"));
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
    titleFontShutdown();
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
    const int64_t nowMs = static_cast<int64_t>(sceKernelGetSystemTimeWide() / 1000);
    const bool hub = app.tileHubVisible();
    if (hub && !hubFx.wasVisible) hubFx.enterMs = nowMs;      // tiles flip in again
    hubFx.wasVisible = hub;
    const uint64_t pageKey = hub ? ~0ull : app.pageKey();
    if (pageKey != pageFx.key) {
        pageFx = PageFx{};
        pageFx.key = pageKey;
        pageFx.enterMs = nowMs;
    }
    const bool tile = app.preferences().tileHome();
    listBounds = ListBounds{};
    if (app.screen() == pspapp::Screen::Browse && app.hasBrowse()) {
        drawBrowse(app);
    } else if (app.utilityPage()) {
        drawHeader(app);
        if (app.screen() == pspapp::Screen::LicenseText) drawLicenseText(app);
        else drawUtility(app);
    } else if (app.screen() == pspapp::Screen::Login) {
        drawLogin(app);
    } else if (app.screen() == pspapp::Screen::Detail) {
        if (tile) drawDetailTiles(app); else drawDetail(app);
    } else if (app.screen() == pspapp::Screen::Comments) {
        drawComments(app);
    } else if (hub) {
        if (app.preferences().metro()) drawHub(app, nowMs); else drawHubTiles(app, nowMs);
    } else {
        drawHeader(app);
        if (app.screen() == pspapp::Screen::Mine || app.tab() == pspapp::Tab::Mine) {
            if (!tile) drawMine(app);
            else if (app.preferences().metro()) drawMineTiles(app);
            else drawMineGrid(app);
        } else if (app.tab() == pspapp::Tab::Search) {
            if (tile && app.searchView() == pspapp::SearchView::Results && !app.cards().empty()) {
                if (app.preferences().metro()) drawVideoGrid(app); else drawVideoGridTiles(app);
            } else if (app.preferences().metro()) drawSearchMetro(app);
            else drawSearch(app);
        } else if (tile && !app.cards().empty()) {
            if (app.preferences().metro()) drawVideoGrid(app); else drawVideoGridTiles(app);
        }
        else drawCardGrid(app);
    }
    if (!hub && app.preferences().tileHome()) squeezePage(nowMs);
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
