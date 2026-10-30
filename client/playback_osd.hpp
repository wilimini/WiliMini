#pragma once
#include "psp_video.h"
#include "preferences.hpp"
#include "services/danmaku.hpp"
#include "services/subtitles.hpp"
#include "bitmap_font.hpp"
#include <memory>
#include <cstdlib>
#include <string>
#include <string_view>

namespace pspapp {
class PlaybackOsd {
public:
    PlaybackOsd(const std::string& title, const Preferences& preferences, const std::vector<DanmakuComment>& comments = {}, const std::vector<SubtitleTrack>& subtitles = {});
    int subtitleCount() const { return static_cast<int>(subtitles_.size()); }
    const PspVideoPresenter* presenter() const { return &presenter_; }
private:
    struct Glyph { uint32_t cp; unsigned short u, v; unsigned char width, height, advance; };
    alignas(16) unsigned char atlas_[256 * 256]{};
    alignas(16) unsigned int palette_[256]{};
    alignas(16) unsigned int commands_[16384]{};
    Glyph glyphs_[144]{};
    struct Comment {
        int64_t timeMs = 0;
        unsigned color = 0xffffff;
        int mode = 1, width = 0, textureWidth = 0, lanes[3]{-1, -1, -1};
        std::unique_ptr<unsigned char, void(*)(void*)> pixels{nullptr, std::free};
    };
    std::vector<Comment> comments_;
    alignas(16) unsigned int commentPalette_[16]{};
    void cacheComments(const std::vector<DanmakuComment>& comments);
    void drawComments(const PspVideoStatus& status);
    void commentSprite(int x, int y, int width, unsigned color);
    std::vector<SubtitleTrack> subtitles_;
    std::vector<BitmapGlyph> subtitleGlyphs_;
    alignas(16) unsigned char subtitlePixels_[512 * 64 / 2]{};
    bool subtitleNarrow_ = false;
    int subtitleTrack_ = -1, subtitleCue_ = -1;
    void cacheSubtitles(const std::vector<SubtitleTrack>& tracks);
    void drawSubtitles(const PspVideoStatus& status);
    unsigned count_ = 0, page_ = 1;
    uint64_t lastDraw_ = 0;
    bool haveFrame_ = false;
    std::string title_, seeking_, buffering_, qualityNames_[3];
    std::string menuLabels_[5], density_[3], on_, off_, noSubtitles_;
    void icon(int x, int y, unsigned id);
    PspVideoPresenter presenter_{};
    void cache(const std::string& text);
    void text(int x, int y, std::string_view text, unsigned color, int right = 464, int size = 16);
    void rectangle(int x, int y, int w, int h, unsigned color);
    void gradient(int y, int height, unsigned topAlpha, unsigned bottomAlpha);
    int draw(const uint32_t* pixels, const PspVideoStatus& status);
};
}
