#include "button_icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace pspapp {
namespace {
struct Pt { float x, y; };

constexpr int kSamples = 4;  // kSamples x kSamples sub-pixel samples per pixel
constexpr float kDim = 0.42f;  // alpha of the non-highlighted D-pad arms
constexpr float kPi = 3.14159265f;

float segmentDistance(Pt p, Pt a, Pt b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float ex = p.x - (a.x + t * dx), ey = p.y - (a.y + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

// A stroked outline: the points along `pts`, optionally closed.
struct Stroke {
    std::vector<Pt> pts;
    bool closed;
    float halfWidth;
    float distance(Pt p) const {
        float best = 1e9f;
        const size_t n = pts.size();
        for (size_t i = 0; i + 1 < n; ++i) best = std::min(best, segmentDistance(p, pts[i], pts[i + 1]));
        if (closed && n > 2) best = std::min(best, segmentDistance(p, pts[n - 1], pts[0]));
        return best;
    }
};

void arc(std::vector<Pt>& out, float cx, float cy, float r, float fromDeg, float toDeg, int steps) {
    for (int i = 0; i <= steps; ++i) {
        const float a = (fromDeg + (toDeg - fromDeg) * static_cast<float>(i) / static_cast<float>(steps)) * kPi / 180.f;
        out.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
}

struct Box { float x0, y0, x1, y1, alpha; };

// One icon is a set of coverage layers; a pixel takes the strongest layer.
struct Shape {
    std::vector<Stroke> strokes;
    std::vector<Box> boxes;
    bool circle = false; Pt circleCentre{}; float circleRadius = 0.f, circleHalf = 0.f;
    bool square = false; Pt squareCentre{}; float squareRadius = 0.f, squareHalf = 0.f;

    float coverageAt(Pt p) const {
        float best = 0.f;
        for (const Box& b : boxes)
            if (p.x >= b.x0 && p.x < b.x1 && p.y >= b.y0 && p.y < b.y1) best = std::max(best, b.alpha);
        for (const Stroke& s : strokes) if (s.distance(p) <= s.halfWidth) return 1.f;
        if (circle) {
            const float d = std::hypot(p.x - circleCentre.x, p.y - circleCentre.y);
            if (std::fabs(d - circleRadius) <= circleHalf) return 1.f;
        }
        if (square) {
            const float d = std::max(std::fabs(p.x - squareCentre.x), std::fabs(p.y - squareCentre.y));
            if (std::fabs(d - squareRadius) <= squareHalf) return 1.f;
        }
        return best;
    }
};

void rounded(std::vector<Pt>& out, float x0, float y0, float x1, float y1, float tl, float tr, float br, float bl) {
    arc(out, x0 + tl, y0 + tl, tl, 180, 270, 12);
    arc(out, x1 - tr, y0 + tr, tr, 270, 360, 12);
    arc(out, x1 - br, y1 - br, br, 0, 90, 12);
    arc(out, x0 + bl, y1 - bl, bl, 90, 180, 12);
}
} // namespace

bool renderButtonIcon(uint32_t cp, int size, uint8_t* alpha, const ButtonLabelFont* font) {
    if (!isButtonIcon(cp) || size < 8 || size > kButtonIconMaxSize || !alpha) return false;
    const char* label = buttonIconLabel(cp);
    if (label && (!font || !font->measure || !font->glyph)) return false;
    const int labelWidth = label ? buttonLabelWidth(font->measure(label, size)) : 0;
    const int w = buttonIconWidth(cp, size, labelWidth), h = buttonIconHeight(size);
    if (w > kButtonIconMaxWidth) return false;
    const float C = static_cast<float>(buttonIconCap(size)), cx = static_cast<float>(w) / 2.f, cy = buttonIconMid(size);
    const float hw = std::max(0.6f, static_cast<float>(size) * 0.055f);  // half the stroke width
    Shape shape;

    if (cp >= 0xE000 && cp <= 0xE003) {
        const float r = C / 2.f - hw;  // outer edge = half the cap height
        if (cp == 0xE000) {  // cross
            const float a = C * 0.42f;
            shape.strokes.push_back({{{cx - a, cy - a}, {cx + a, cy + a}}, false, hw});
            shape.strokes.push_back({{{cx - a, cy + a}, {cx + a, cy - a}}, false, hw});
        } else if (cp == 0xE001) {  // circle
            shape.circle = true; shape.circleCentre = {cx, cy}; shape.circleRadius = r; shape.circleHalf = hw;
        } else if (cp == 0xE002) {  // square
            shape.square = true; shape.squareCentre = {cx, cy}; shape.squareHalf = hw * 0.85f; shape.squareRadius = C * 0.42f - shape.squareHalf;
        } else {  // triangle: apex-up, its extent centred on the midline
            const float R = C * 0.6f, centre = cy + C * 0.15f;
            std::vector<Pt> pts;
            for (int a : {-90, 30, 150}) {
                const float rad = static_cast<float>(a) * kPi / 180.f;
                pts.push_back({cx + R * std::cos(rad), centre + R * std::sin(rad)});
            }
            shape.strokes.push_back({pts, true, hw});
        }
    } else if (cp >= 0xE010 && cp <= 0xE016) {  // D-pad: faint cross, highlighted arm tips
        const float arm = (C + 1.f) / 2.f, half = std::max(1.2f, C * 0.17f);
        shape.boxes.push_back({cx - half, cy - arm, cx + half, cy + arm, kDim});
        shape.boxes.push_back({cx - arm, cy - half, cx + arm, cy + half, kDim});
        // Only the outer tip of a pressed arm lights up; the centre square stays dim.
        const Box up{cx - half, cy - arm, cx + half, cy - half, 1.f}, down{cx - half, cy + half, cx + half, cy + arm, 1.f};
        const Box left{cx - arm, cy - half, cx - half, cy + half, 1.f}, right{cx + half, cy - half, cx + arm, cy + half, 1.f};
        switch (cp) {
        case 0xE010: shape.boxes.push_back(up); break;
        case 0xE011: shape.boxes.push_back(down); break;
        case 0xE012: shape.boxes.push_back(left); break;
        case 0xE013: shape.boxes.push_back(right); break;
        case 0xE014: shape.boxes.push_back(up); shape.boxes.push_back(down); break;
        case 0xE015: shape.boxes.push_back(left); shape.boxes.push_back(right); break;
        default: for (const Box& b : {up, down, left, right}) shape.boxes.push_back(b); break;
        }
    } else {  // L / R bumpers and START / SELECT pills around a body-font label
        const float ph = buttonLabelCap(size) + 4.f, m = hw + 0.1f;
        const float x0 = m, x1 = static_cast<float>(w) - m, y0 = cy - ph / 2.f + m, y1 = cy + ph / 2.f - m;
        std::vector<Pt> pts;
        if (cp == 0xE020 || cp == 0xE021) {
            const float big = (y1 - y0) * 0.7f, small = (y1 - y0) * 0.16f;
            rounded(pts, x0, y0, x1, y1, big, small, small, small);
            if (cp == 0xE021) for (Pt& p : pts) p.x = static_cast<float>(w) - p.x;
        } else {
            const float r = (y1 - y0) / 2.f;
            rounded(pts, x0, y0, x1, y1, r, r, r, r);
        }
        shape.strokes.push_back({pts, true, hw});
    }

    constexpr int kTotal = kSamples * kSamples;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0.f;
            for (int sy = 0; sy < kSamples; ++sy)
                for (int sx = 0; sx < kSamples; ++sx)
                    sum += shape.coverageAt({static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSamples,
                                             static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSamples});
            alpha[y * w + x] = static_cast<uint8_t>(std::lround(255.f * sum / kTotal));
        }
    if (label) {  // letters: the body font scaled by kButtonLabelPercent about the text midline
        constexpr float kScale = kButtonLabelPercent / 100.f;
        std::vector<uint8_t> cell(static_cast<size_t>(kButtonIconMaxSize + 2) * kButtonIconMaxHeight);
        float pen = static_cast<float>(w - labelWidth) / 2.f;
        for (const char* p = label; *p; ++p) {
            int advance = 0, gw = 0, gh = 0;
            if (!font->glyph(*p, size, &advance, &gw, &gh, cell.data()) || gw > kButtonIconMaxSize + 2 || gh > h) continue;
            for (int y = 0; y < h; ++y)
                for (int x = static_cast<int>(pen); x < w && x < static_cast<int>(pen + static_cast<float>(gw) * kScale) + 1; ++x) {
                    float sum = 0.f;
                    for (int sy = 0; sy < 3; ++sy)  // average a 3x3 patch of the source glyph
                        for (int sx = 0; sx < 3; ++sx) {
                            const float u = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 3.f - pen) / kScale;
                            const float v = cy + (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 3.f - cy) / kScale;
                            const int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
                            if (iu >= 0 && iu < gw && iv >= 0 && iv < gh) sum += cell[static_cast<size_t>(iv) * gw + iu];
                        }
                    uint8_t& out = alpha[y * w + x];
                    out = static_cast<uint8_t>(std::max<int>(out, static_cast<int>(sum / 9.f)));
                }
            pen += static_cast<float>(advance) * kScale;
        }
    }
    return true;
}
} // namespace pspapp
