#include "playback_osd.hpp"
#include "bitmap_font.hpp"
#include "services/network.hpp"
#include "bilibili.h"
#include "assets/player_icons.generated.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <pspdisplay.h>
#include <malloc.h>
#include <pspdmac.h>
#include <pspgu.h>
#include <pspkernel.h>

namespace pspapp {
namespace {
constexpr unsigned frameBytes = 512 * 272 * 4;
void* vram(unsigned offset) { return reinterpret_cast<void*>(0x44000000u + offset); }
void timestamp(char* value, size_t size, int64_t ms) {
    const auto seconds = std::max<int64_t>(0, ms) / 1000;
    if (seconds >= 3600)
        std::snprintf(value, size, "%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else std::snprintf(value, size, "%02lld:%02lld", seconds / 60, seconds % 60);
}
}
PlaybackOsd::PlaybackOsd(const std::string& title, const Preferences& preferences, const std::vector<DanmakuComment>& comments, const std::vector<SubtitleTrack>& subtitles, bool initializeDisplay) : preferences_(preferences), title_(title) {
    const bool en = preferences.locale == Locale::EnUs;
    seeking_ = en ? "Seek preview" : "跳转预览";
    buffering_ = en ? "Seeking..." : "正在定位…";
    menuLabels_[0] = en ? "Quality" : "画质";
    menuLabels_[1] = en ? "Danmaku" : "弹幕";
    menuLabels_[2] = en ? "Density" : "密度";
    menuLabels_[3] = en ? "Opacity" : "不透明度";
    menuLabels_[4] = en ? "Subtitles" : "字幕";
    display_ = en ? "Show" : "显示";
    noSubtitles_ = en ? "Unavailable" : "暂无可用字幕";
    density_[0] = en ? "Low" : "稀疏";
    density_[1] = en ? "Medium" : "适中";
    density_[2] = en ? "High" : "密集";
    on_ = en ? "On" : "开"; off_ = en ? "Off" : "关";
    for (unsigned i = 0; i < 4; ++i) {
        unsigned u = i * 20;
        glyphs_[count_++] = {0xf000 + i, static_cast<unsigned short>(u), 0, 20, 20, 20};
        for (unsigned y = 0; y < 20; ++y)
            std::memcpy(atlas_ + y * 512 + u, player_icons::alpha[i] + y * 20, 20);
    }
    qualityNames_[0] = en ? "360P" : "360P 流畅";
    qualityNames_[1] = en ? "480P" : "480P 清晰";
    qualityNames_[2] = en ? "720P" : "720P 高清";
    for (const auto& label : qualityNames_) cache(label);
    cache("P");
    cache(display_); cache(noSubtitles_); cache(on_); cache(off_); cache(buffering_);
    for (const auto& label : menuLabels_) cache(label);
    for (const auto& label : density_) cache(label);
    cache(seeking_);
    cache("0123456789:/- ?p%..."); cache(title_);
    overlayMessages_[1] = en ? "Danmaku unavailable" : "弹幕加载失败";
    overlayMessages_[2] = en ? "Subtitles unavailable" : "字幕加载失败";
    overlayMessages_[3] = en ? "Loading subtitles..." : "正在加载字幕…";
    overlayMessages_[4] = en ? "Subtitles loaded" : "字幕已加载";
    startupLabels_[0] = en ? "Connecting video..." : "正在连接视频源…";
    startupLabels_[1] = en ? "Preparing audio..." : "正在准备音频…";
    startupLabels_[2] = en ? "Buffering first frame..." : "正在缓冲首帧…";
    for (const auto& label : overlayMessages_) cache(label);
    for (const auto& label : startupLabels_) cache(label);
    for (unsigned i = 0; i < 256; ++i) palette_[i] = (i << 24) | 0x00ffffff;
    cacheComments(comments);
    cacheSubtitles(subtitles);
    for (unsigned i = 0; i < 16; ++i) commentPalette_[i] = (i * 17u << 24) | 0xffffff;
    sceKernelDcacheWritebackRange(commentPalette_, sizeof(commentPalette_));
    sceKernelDcacheWritebackRange(atlas_, sizeof(atlas_));
    sceKernelDcacheWritebackRange(palette_, sizeof(palette_));
    // Third VRAM page holds only video, so hiding controls or redrawing a
    // paused frame never leaves stale text. No extra RGB allocation in RAM.
    if (initializeDisplay) std::memset(vram(2 * frameBytes), 0, frameBytes);
    presenter_ = {this, [](void* opaque, const uint32_t* pixels, const PspVideoStatus* status) {
        return static_cast<PlaybackOsd*>(opaque)->draw(pixels, *status);
    }, [](void* opaque, PspVideoSettings* settings) {
        static_cast<PlaybackOsd*>(opaque)->updateSettings(*settings);
    }};
}
PlaybackOsd::~PlaybackOsd() {
    overlayStop_ = 1;
    if (overlayThread_ >= 0) {
        if (overlayStarted_) sceKernelWaitThreadEnd(overlayThread_, nullptr);
        sceKernelDeleteThread(overlayThread_);
    }
    delete pending_;
    if (overlayLock_ >= 0) sceKernelDeleteSema(overlayLock_);
}
void PlaybackOsd::enableNetwork(const std::string& bvid, uint64_t cid, uint64_t aid, HttpClient::Transport transport) {
    if (!cid || overlayThread_ >= 0) return;
    bvid_ = bvid; cid_ = cid; aid_ = aid; overlayTransport_ = std::move(transport);
    overlayLock_ = sceKernelCreateSema("overlay_state", 0, 1, 1, nullptr);
    if (overlayLock_ < 0) return;
    overlayThread_ = sceKernelCreateThread("playback_overlays", overlayEntry, 0x28,
                                         224 * 1024, PSP_THREAD_ATTR_USER, nullptr);
}
int PlaybackOsd::overlayEntry(SceSize, void* arg) {
    auto* self = *static_cast<PlaybackOsd**>(arg);
    try { self->loadOverlays(); }
    catch (...) {
        sceKernelWaitSema(self->overlayLock_, 1, nullptr);
        self->overlayMessage_ = 2;
        self->metadataCount_ = 0;
        sceKernelSignalSema(self->overlayLock_, 1);
    }
    return 0;
}
bool PlaybackOsd::publish(PlaybackOsd* prepared, bool comments) {
    while (!overlayStop_) {
        sceKernelWaitSema(overlayLock_, 1, nullptr);
        if (!pending_) {
            pending_ = prepared; pendingComments_ = comments;
            sceKernelSignalSema(overlayLock_, 1);
            return true;
        }
        sceKernelSignalSema(overlayLock_, 1);
        sceKernelDelayThread(10000);
    }
    delete prepared;
    return false;
}
void PlaybackOsd::receiveOverlays() {
    if (overlayLock_ < 0 || sceKernelPollSema(overlayLock_, 1) < 0) return;
    auto* prepared = pending_;
    const bool comments = pendingComments_;
    pending_ = nullptr;
    if (overlayMessage_) {
        shownMessage_ = overlayMessage_;
        messageUntil_ = sceKernelGetSystemTimeWide() + 5000000;
        overlayMessage_ = 0;
    }
    sceKernelSignalSema(overlayLock_, 1);
    if (!prepared) return;
    // Previous GU list is synchronized before this function can run again.
    // All allocation, rasterization and font reads happened on the worker.
    if (comments) comments_.swap(prepared->comments_);
    else {
        subtitles_.swap(prepared->subtitles_);
        subtitleGlyphs_.swap(prepared->subtitleGlyphs_);
        std::memcpy(atlas_, prepared->atlas_, sizeof(atlas_));
        std::memcpy(glyphs_, prepared->glyphs_, sizeof(glyphs_));
        count_ = prepared->count_;
        subtitleTrack_ = subtitleCue_ = -1;
        sceKernelDcacheWritebackRange(atlas_, sizeof(atlas_));
        sceKernelWaitSema(overlayLock_, 1, nullptr);
        metadataCount_ = static_cast<int>(subtitles_.size());
        sceKernelSignalSema(overlayLock_, 1);
    }
    delete prepared;
}
void PlaybackOsd::updateSettings(PspVideoSettings& settings) {
    if (overlayLock_ < 0) return;
    sceKernelWaitSema(overlayLock_, 1, nullptr);
    if (metadataCount_ >= 0) {
        settings.subtitle_count = metadataCount_;
        if (settings.subtitle_track > metadataCount_) settings.subtitle_track = 0;
    }
    requestedTrack_ = settings.subtitle_track;
    sceKernelSignalSema(overlayLock_, 1);
}
void PlaybackOsd::loadOverlays() {
    HttpClient::Transport deviceTransport = [this](const std::string& url, ErrorOperation op) {
        BiliHttpResponse response{};
        HttpResponse out;
        bili_http_get_response_cancel(url.c_str(), &response, 512 * 1024, &overlayStop_);
        if (response.text) { out.body = response.text; bili_http_free(response.text); }
        if (response.failure) out.error = {ErrorDomain::Network,
            overlayStop_ ? ErrorKind::Cancelled : ErrorKind::HttpFailure, op, response.transport_code, response.status};
        return out;
    };
    HttpClient http(overlayTransport_ ? overlayTransport_ : deviceTransport);
    auto message = [this](int code) {
        sceKernelWaitSema(overlayLock_, 1, nullptr);
        overlayMessage_ = code;
        sceKernelSignalSema(overlayLock_, 1);
    };
    // This thread starts only after the first frame has reached the presenter.
    bool failed = false;
    auto tracks = subtitleTracks(bvid_, cid_, aid_, http, failed);
    if (overlayStop_) return;
    if (failed) message(2);
    auto* metadata = new PlaybackOsd(title_, preferences_, {}, tracks, false);
    if (!publish(metadata, false)) return;
    auto response = http.get("https://api.bilibili.com/x/v1/dm/list.so?oid=" + std::to_string(cid_), ErrorOperation::Request, false);
    if (overlayStop_) return;
    if (response) {
        auto comments = parseDanmaku(response.value.body);
        auto* prepared = new PlaybackOsd(title_, preferences_, comments, {}, false);
        if (!publish(prepared, true)) return;
    } else message(1);
    int loaded = 0;
    while (!overlayStop_) {
        sceKernelWaitSema(overlayLock_, 1, nullptr);
        const int selected = requestedTrack_;
        sceKernelSignalSema(overlayLock_, 1);
        if (selected > 0 && selected <= static_cast<int>(tracks.size()) && selected != loaded) {
            message(3);
            auto result = http.get(tracks[selected - 1].url, ErrorOperation::Request, false);
            if (overlayStop_) return;
            for (auto& track : tracks) track.cues.clear();
            if (result) tracks[selected - 1].cues = parseSubtitles(result.value.body);
            failed = !result || tracks[selected - 1].cues.empty();
            loaded = selected;
            auto* prepared = new PlaybackOsd(title_, preferences_, {}, tracks, false);
            failed = failed || prepared->subtitles_[selected - 1].cues.empty();
            if (!publish(prepared, false)) return;
            message(failed ? 2 : 4);
        } else {
            if (selected == 0) loaded = 0; // Off/on permits retry of a failed track.
            sceKernelDelayThread(50000);
        }
    }
}

void PlaybackOsd::cacheSubtitles(const std::vector<SubtitleTrack>& tracks) {
    std::set<uint32_t> points{'?'};
    for (const auto& track : tracks) {
        auto next = points;
        for (const auto& cue : track.cues) {
            for (size_t p = 0; p < cue.text.size() && next.size() <= 4096;)
                next.insert(bitmapNext(cue.text, p));
            if (next.size() > 4096) break;
        }
        if (subtitles_.size() == 3) break;
        if (next.size() > 4096) {
            subtitles_.push_back({track.label, {}, track.url});
            cache(track.label);
            continue;
        }
        points = std::move(next);
        subtitles_.push_back(track);
        cache(track.label);
    }
    subtitleGlyphs_.reserve(points.size());
    for (auto cp : points) {
        if (const auto* glyph = bitmapGlyph(cp, 16)) subtitleGlyphs_.push_back(*glyph);
    }
}
void PlaybackOsd::drawSubtitles(const PspVideoStatus& status) {
    const int track = status.settings.subtitle_track - 1;
    if (track < 0 || track >= static_cast<int>(subtitles_.size()) || status.buffering || !haveFrame_) return;
    const auto& cues = subtitles_[track].cues;
    const int cue = subtitleAt(cues, status.position_ms);
    if (cue < 0) return;
    const bool narrow = status.menu_open >= 2;
    if (track != subtitleTrack_ || cue != subtitleCue_ || narrow != subtitleNarrow_) {
        subtitleNarrow_ = narrow;
        subtitleTrack_ = track; subtitleCue_ = cue;
        std::memset(subtitlePixels_, 0, sizeof(subtitlePixels_));
        // Preloaded glyph copies keep disk/font-cache access out of presentation.
        const BitmapGlyph* lines[2][64]{};
        int counts[2]{}, widths[2]{}, row = 0;
        const auto& value = cues[cue].text;
        for (size_t p = 0; p < value.size();) {
            auto cp = bitmapNext(value, p);
            if (cp == '\n') { if (++row == 2) break; continue; }
            if (cp < 32) continue;
            auto glyph = std::lower_bound(subtitleGlyphs_.begin(), subtitleGlyphs_.end(), cp,
                [](const BitmapGlyph& g, uint32_t c) { return g.codepoint < c; });
            if (glyph == subtitleGlyphs_.end() || glyph->codepoint != cp) continue;
            const int advance = std::max(1, int(glyph->advance) - 1);
            if (widths[row] + advance > (narrow ? 240 : 432) || counts[row] == 64) { if (++row == 2) break; }
            lines[row][counts[row]++] = &*glyph;
            widths[row] += advance;
        }
        for (int row = 0; row < 2; ++row) {
            int x = ((narrow ? 264 : 480) - widths[row]) / 2;
            for (int i = 0; i < counts[row]; ++i) {
                const auto& g = *lines[row][i];
                for (int yy = 0; yy < g.height; ++yy) for (int xx = 0; xx < g.width; ++xx) {
                    const int from = yy * g.width + xx;
                    const unsigned a = g.alpha[from / 2] >> (from % 2 * 4) & 15;
                    const int to = (row * 22 + yy) * 512 + x + xx;
                    auto& pixel = subtitlePixels_[to / 2];
                    pixel |= a << (to % 2 * 4);
                }
                x += std::max(1, int(g.advance) - 1);
            }
        }
        sceKernelDcacheWritebackRange(subtitlePixels_, sizeof(subtitlePixels_));
    }
    const int y = status.controls_visible || status.seeking || status.menu_open ? 166 : 218;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 15, 0);
    sceGuClutLoad(2, commentPalette_);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, 512, 64, 512, subtitlePixels_);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    auto sprite = [&](int x, int top, unsigned color) {
        struct V { unsigned short u, v; short x, y, z; };
        auto* v = static_cast<V*>(sceGuGetMemory(2 * sizeof(V)));
        v[0] = {0, 0, static_cast<short>(x), static_cast<short>(top), 0};
        v[1] = {480, 44, static_cast<short>(x + 480), static_cast<short>(top + 44), 0};
        sceGuColor(color);
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
    };
    sprite(-1, y, 0xff000000); sprite(1, y, 0xff000000);
    sprite(0, y - 1, 0xff000000); sprite(0, y + 1, 0xff000000);
    sprite(0, y, 0xffffffff);
    sceGuClutMode(GU_PSM_8888, 0, 255, 0);
    sceGuClutLoad(32, palette_);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, 512, 256, 512, atlas_);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
}

void PlaybackOsd::cacheComments(const std::vector<DanmakuComment>& source) {
    // Fixed playback memory budget; distribute retained comments over the
    // source timeline instead of exhausting the budget at the beginning.
    const size_t count = std::min<size_t>(96, source.size());
    size_t used = 0;
    DanmakuLanes layouts[3];
    comments_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& input = source[i * source.size() / count];
        Comment comment;
        comment.timeMs = input.timeMs; comment.mode = input.mode; comment.color = input.color;
        comment.width = std::min(448, bitmapMeasure(input.text, 16) + 4);
        comment.textureWidth = 32;
        while (comment.textureWidth < comment.width) comment.textureWidth *= 2;
        const size_t bytes = comment.textureWidth * 32 / 2;
        if (used + bytes > 768 * 1024) continue;
        bool visible = false;
        for (int d = 0; d < 3; ++d) {
            comment.lanes[d] = layouts[d].place(input.timeMs, comment.width, input.mode, 2 + d * 2);
            visible |= comment.lanes[d] >= 0;
        }
        if (!visible) continue;
        comment.pixels.reset(static_cast<unsigned char*>(memalign(16, bytes)));
        if (!comment.pixels) break;
        std::memset(comment.pixels.get(), 0, bytes);
        int x = 2;
        for (size_t at = 0; at < input.text.size();) {
            const auto* glyph = bitmapGlyph(bitmapNext(input.text, at), 16);
            if (!glyph) continue;
            if (x + glyph->advance > comment.width - 2) break;
            for (int yy = 0; yy < glyph->height && yy + 4 < 32; ++yy)
                for (int xx = 0; xx < glyph->width && x + xx < comment.width; ++xx) {
                    const int from = yy * glyph->width + xx;
                    unsigned alpha = (glyph->alpha[from / 2] >> ((from % 2) * 4)) & 15;
                    const int to = (yy + 4) * comment.textureWidth + x + xx;
                    unsigned char& packed = comment.pixels.get()[to / 2];
                    const int shift = (to % 2) * 4;
                    alpha = std::max(alpha, unsigned(packed >> shift & 15));
                    packed = (packed & ~(15 << shift)) | (alpha << shift);
                }
            x += glyph->advance;
        }
        sceKernelDcacheWritebackRange(comment.pixels.get(), bytes);
        used += bytes;
        comments_.push_back(std::move(comment));
    }
}
void PlaybackOsd::commentSprite(int x, int y, int width, unsigned color) {
    struct Vertex { unsigned short u, v; short x, y, z; };
    auto* vertices = static_cast<Vertex*>(sceGuGetMemory(2 * sizeof(Vertex)));
    vertices[0] = {0, 0, static_cast<short>(x), static_cast<short>(y), 0};
    vertices[1] = {static_cast<unsigned short>(width), 32, static_cast<short>(x + width), static_cast<short>(y + 32), 0};
    sceGuColor(color);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, vertices);
}
void PlaybackOsd::drawComments(const PspVideoStatus& status) {
    if (!status.settings.danmaku_enabled || status.buffering || !haveFrame_) return;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 15, 0);
    sceGuClutLoad(2, commentPalette_);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuScissor(0, 36, 480, status.settings.subtitle_track > 0 &&
        (status.controls_visible || status.seeking || status.buffering) ? 160 : 190);
    const int density = std::clamp(status.settings.danmaku_density, 0, 2);
    const unsigned alpha = std::clamp(status.settings.danmaku_opacity, 0, 100) * 255u / 100u << 24;
    for (const auto& comment : comments_) {
        const int lane = comment.lanes[density];
        const int64_t elapsed = status.position_ms - comment.timeMs;
        const int64_t duration = comment.mode == 1 ? (480 + comment.width) * 1000 / 60 : 4000;
        if (lane < 0 || elapsed < 0 || elapsed >= duration) continue;
        const int x = comment.mode == 1 ? 480 - static_cast<int>(elapsed * 60 / 1000) : (480 - comment.width) / 2;
        const int y = 36 + lane * 24;
        sceGuTexImage(0, comment.textureWidth, 32, comment.textureWidth, comment.pixels.get());
        commentSprite(x - 1, y, comment.width, alpha);
        commentSprite(x + 1, y, comment.width, alpha);
        commentSprite(x, y - 1, comment.width, alpha);
        commentSprite(x, y + 1, comment.width, alpha);
        const unsigned rgb = comment.color;
        commentSprite(x, y, comment.width, alpha | (rgb >> 16) | (rgb & 0xff00) | ((rgb & 255) << 16));
    }
    sceGuScissor(0, 0, 480, 272);
}
void PlaybackOsd::cache(const std::string& value) {
    for (size_t pos = 0; pos < value.size();) {
        uint32_t cp = bitmapNext(value, pos);
        bool exists = false;
        for (unsigned i = 0; i < count_; ++i) if (glyphs_[i].cp == cp) exists = true;
        if (exists || count_ == 300) continue;
        const auto* glyph = bitmapGlyph(cp, 16);
        if (!glyph || glyph->width > 20 || glyph->height > 20) continue;
        unsigned u = count_ % 25 * 20, v = count_ / 25 * 20;
        glyphs_[count_++] = {cp, static_cast<unsigned short>(u), static_cast<unsigned short>(v),
                            glyph->width, glyph->height, glyph->advance};
        for (unsigned y = 0; y < glyph->height; ++y)
            for (unsigned x = 0; x < glyph->width; ++x) {
                unsigned pixel = y * glyph->width + x;
                atlas_[(v + y) * 512 + u + x] = ((glyph->alpha[pixel / 2] >> ((pixel % 2) * 4)) & 15) * 17;
            }
    }
}
void PlaybackOsd::rectangle(int x, int y, int w, int h, unsigned color) {
    struct Vertex { short x, y, z; };
    auto* vertices = static_cast<Vertex*>(sceGuGetMemory(2 * sizeof(Vertex)));
    vertices[0] = {static_cast<short>(x), static_cast<short>(y), 0};
    vertices[1] = {static_cast<short>(x + w), static_cast<short>(y + h), 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(color);
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, vertices);
}
void PlaybackOsd::text(int x, int y, std::string_view value, unsigned color, int right, int size) {
    struct Vertex { unsigned short u, v; short x, y, z; };
    sceGuEnable(GU_TEXTURE_2D);
    sceGuColor(color);
    for (size_t pos = 0; pos < value.size();) {
        const uint32_t cp = bitmapNext(value, pos);
        const Glyph* glyph = nullptr;
        for (unsigned i = 0; i < count_; ++i) if (glyphs_[i].cp == cp) { glyph = &glyphs_[i]; break; }
        if (!glyph) continue;
        const int advance = std::max(1, (glyph->advance * size + 8) / 16 - (size < 16 ? 1 : 0));
        if (x + advance > right) break;
        auto* vertices = static_cast<Vertex*>(sceGuGetMemory(2 * sizeof(Vertex)));
        vertices[0] = {glyph->u, glyph->v, static_cast<short>(x), static_cast<short>(y), 0};
        vertices[1] = {static_cast<unsigned short>(glyph->u + glyph->width), static_cast<unsigned short>(glyph->v + glyph->height),
                       static_cast<short>(x + (glyph->width * size + 8) / 16), static_cast<short>(y + (glyph->height * size + 8) / 16), 0};
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, vertices);
        x += advance;
    }
}
void PlaybackOsd::icon(int x, int y, unsigned id) {
    const char code[] = {char(0xef), char(0x80), char(0x80 + id), 0};
    text(x, y, code, 0xffffffff);
}
void PlaybackOsd::gradient(int y, int height, unsigned topAlpha, unsigned bottomAlpha) {
    struct Vertex { unsigned color; short x, y, z; };
    auto* v = static_cast<Vertex*>(sceGuGetMemory(4 * sizeof(Vertex)));
    v[0] = {topAlpha << 24, 0, static_cast<short>(y), 0};
    v[1] = {topAlpha << 24, 480, static_cast<short>(y), 0};
    v[2] = {bottomAlpha << 24, 0, static_cast<short>(y + height), 0};
    v[3] = {bottomAlpha << 24, 480, static_cast<short>(y + height), 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, nullptr, v);
}
int PlaybackOsd::draw(const uint32_t* pixels, const PspVideoStatus& status) {
    const uint64_t now = sceKernelGetSystemTimeWide();
    receiveOverlays();
    if (!pixels && now - lastDraw_ < 33333) return 0;
    if (pixels) {
        // The previous GU list has completed before this clean page is reused.
        int result = pixels == vram(2 * frameBytes) ? 0 : sceDmacMemcpy(vram(2 * frameBytes), pixels, frameBytes);
        if (result < 0) return result;
        haveFrame_ = true;
        if (overlayThread_ >= 0 && !overlayStarted_) {
            PlaybackOsd* self = this;
            overlayStarted_ = sceKernelStartThread(overlayThread_, sizeof(self), &self) >= 0;
            if (!overlayStarted_) { sceKernelDeleteThread(overlayThread_); overlayThread_ = -1; }
        }
    }
    lastDraw_ = now;
    sceGuStart(GU_DIRECT, commands_);
    sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void*>(page_ * frameBytes), 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuCopyImage(GU_PSM_8888, 0, 0, 480, 272, 512, vram(2 * frameBytes),
                   0, 0, 512, vram(page_ * frameBytes));
    sceGuTexSync();
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    drawComments(status);
    sceGuClutMode(GU_PSM_8888, 0, 255, 0);
    sceGuClutLoad(32, palette_);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, 512, 256, 512, atlas_);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    constexpr unsigned white = 0xffffffff, muted = 0xffc8c3bf, pink = 0xff9972fb;
    const bool visible = status.controls_visible || status.seeking || status.buffering || !haveFrame_;
    if (visible) {
        gradient(0, 62, 175, 0);
        gradient(166, 106, 0, 225);
        text(16, 12, title_, white, 464, 14);
        const int64_t position = status.seeking ? status.preview_ms : status.position_ms;
        char elapsed[32], duration[32], times[72];
        timestamp(elapsed, sizeof(elapsed), position);
        if (status.duration_ms > 0) timestamp(duration, sizeof(duration), status.duration_ms);
        else std::snprintf(duration, sizeof(duration), "--:--");
        std::snprintf(times, sizeof(times), "%s / %s", elapsed, duration);
        rectangle(16, 220, 448, 2, 0xff777777);
        if (status.duration_ms > 0) {
            int progress = static_cast<int>(std::clamp<int64_t>(position, 0, status.duration_ms) * 448 / status.duration_ms);
            rectangle(16, 220, progress, 2, pink);
            rectangle(14 + progress, 217, 5, 8, pink);
        }
        icon(16, 236, status.paused ? 0 : 1);
        text(46, 239, times, white, 235, 12);
        const int categories[] = {1, 4, 0};
        for (int i = 0; i < 3; ++i) {
            const int x = 276 + i * 64;
            const int category = categories[i];
            const bool focus = status.menu_open && (status.menu_row == category || (category == 1 && (status.menu_row == 2 || status.menu_row == 3)));
            const char* quality = status.settings.quality == 64 ? "720P" : status.settings.quality == 32 ? "480P" : "360P";
            text(x, 239, category == 0 ? std::string_view(quality) : std::string_view(menuLabels_[category]), focus ? pink : white, x + 62, 12);
            if (focus) rectangle(x, 260, 28, 2, pink);
        }
        if (status.seeking || status.buffering) {
            rectangle(158, 113, 164, 46, 0x99000000);
            text(174, 117, status.buffering ? buffering_ : seeking_, white, 314, 12);
            text(174, 137, times, white, 314, 12);
        }
    }
    if (!haveFrame_) text(16, 191, startupLabels_[status.startup_stage == PSP_VIDEO_STAGE_AUDIO_INIT ? 1 :
        status.startup_stage == PSP_VIDEO_STAGE_PLAY ? 2 : 0], white, 464, 14);
    if (shownMessage_ && now < messageUntil_) text(16, 39, overlayMessages_[shownMessage_], muted, 464, 12);
    drawSubtitles(status);
    if (status.menu_open >= 2) {
        // Flat translucent list, with no rounded card or opaque selection tile.
        rectangle(274, 58, 190, 154, 0xc0000000);
        const bool danmakuMenu = status.menu_open == 2 && status.menu_row >= 1 && status.menu_row <= 3;
        text(286, 66, menuLabels_[danmakuMenu ? 1 : status.menu_row], white, 448, 14);
        rectangle(286, 91, 166, 1, 0x55ffffff);
        const int row = status.menu_row;
        const int count = danmakuMenu ? 3 : row == 0 ? status.settings.quality_count : row == 1 ? 2 : row == 4 ? static_cast<int>(subtitles_.size()) + 1 : 3;
        for (int i = 0; i < count; ++i) {
            bool selected;
            std::string_view label;
            char opacity[12];
            if (danmakuMenu) {
                selected = row == i + 1;
                label = i == 0 ? std::string_view(display_) : std::string_view(menuLabels_[i + 1]);
                const auto value = i == 0 ? std::string_view(status.settings.danmaku_enabled ? on_ : off_) :
                    i == 1 ? std::string_view(density_[status.settings.danmaku_density]) : std::string_view();
                if (i < 2) text(388, 100 + i * 25, value, muted, 439, 12);
                else {
                    std::snprintf(opacity, sizeof(opacity), "%d%%", status.settings.danmaku_opacity);
                    text(388, 100 + i * 25, opacity, muted, 439, 12);
                }
            } else if (row == 0) {
                // Display highest quality first; input still cycles through available streams.
                const int quality = status.settings.qualities[count - i - 1];
                selected = quality == status.quality_preview;
                label = qualityNames_[quality == 64 ? 2 : quality == 32 ? 1 : 0];
            } else if (row == 1) {
                selected = status.settings.danmaku_enabled == (i == 0);
                label = i == 0 ? on_ : off_;
            } else if (row == 2) {
                selected = status.menu_value == i;
                label = density_[i];
            } else if (row == 4) {
                selected = status.menu_value == i;
                label = i == 0 ? std::string_view(off_) : std::string_view(subtitles_[i - 1].label);
            } else {
                const int value = 50 + 25 * i;
                selected = status.menu_value == i;
                std::snprintf(opacity, sizeof(opacity), "%d%%", value);
                label = opacity;
            }
            text(286, 100 + i * 25, label, selected ? pink : muted, danmakuMenu ? 382 : 436, 13);
            if (selected) rectangle(446, 107 + i * 25, 4, 4, pink);
        }
        if (row == 4 && subtitles_.empty()) text(286, 132, overlayThread_ >= 0 && metadataCount_ < 0 ? overlayMessages_[3] : noSubtitles_, muted, 448, 12);
    }
    sceGuFinish();
    sceGuSync(0, 0);
    // Only flip after DMA + GE completion, and never draw into scanout.
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(vram(page_ * frameBytes), 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    page_ ^= 1;
    return 0;
}
}
