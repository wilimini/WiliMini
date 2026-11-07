#pragma once

// Layout engine for scrolling content pages. A layout is a list of item rectangles in content
// coordinates (origin at the top left of the scrolled area). Everything that depends on where
// items are, i.e. D-pad movement, the scroll target and which covers to fetch, is derived from
// those rectangles instead of from row/column arithmetic. The built-in providers below
// reproduce the hand-written grid and list layouts; a theme can later supply its own rectangles.
// Pure arithmetic with no PSP dependencies so it can be checked on the host
// (tests/layout_test.cpp).

#include <cstddef>
#include <vector>

namespace pspapp {

struct LayoutRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

// How an item is drawn. Default lets the page pick its own look (the built-in screens).
enum class CardStyle { Default, Tile, Row, Poster, Compact };

// What the focused item looks like. The ring is the original look.
struct FocusFx {
    enum Kind { Ring, None, Scale } kind = Ring;
    int scalePercent = 100;  // Scale: the focused item is drawn this much larger, around its centre
    int offsetX = 0, offsetY = 0;  // the focused item is nudged by this much
    bool dimOthers = false;  // draw the unfocused items darker
};

struct Layout {
    std::vector<LayoutRect> items;
    // Content scrolls along y (vertical lists) or x (horizontal strips).
    bool horizontal = false;
    // Distance between the view origin and the focused item; -1 keeps the built-in value.
    int anchor = -1;
    CardStyle card = CardStyle::Default;
    FocusFx focus;
    bool themed = false;  // produced by a theme script
    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
};

// Built-in layouts, matching the original screens.
constexpr int kFeedGridX = 12, kFeedGridW = 224, kFeedGridH = 98;
constexpr int kFeedGridPitchX = 228, kFeedGridPitchY = 102;
constexpr int kFeedGridScrollAnchor = 100;  // the selected row stays this far below the view top
constexpr int kFeedRowX = 12, kFeedRowW = 456, kFeedRowH = 34, kFeedRowPitch = 36;
constexpr int kFeedRowScrollAnchor = 2 * kFeedRowPitch;
constexpr int kAccountRowH = 50;

// `columns` tiles per row, left to right, top to bottom. `insetY` leaves room above the first row.
inline Layout gridLayout(size_t count, int columns, int insetY = 0) {
    Layout layout;
    layout.items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const int col = static_cast<int>(i % columns), row = static_cast<int>(i / columns);
        layout.items.push_back({kFeedGridX + col * kFeedGridPitchX, insetY + row * kFeedGridPitchY,
                                kFeedGridW, kFeedGridH});
    }
    return layout;
}

// One full-width row per item.
inline Layout listLayout(size_t count, int insetY = 0) {
    Layout layout;
    layout.items.reserve(count);
    for (size_t i = 0; i < count; ++i)
        layout.items.push_back({kFeedRowX, insetY + static_cast<int>(i) * kFeedRowPitch, kFeedRowW, kFeedRowH});
    return layout;
}

// The Mine page in the tile style: a wide account row, then the local records as a grid.
inline Layout accountGridLayout(size_t records) {
    Layout layout;
    layout.items.reserve(records + 1);
    layout.items.push_back({kFeedGridX, 0, 2 * kFeedGridPitchX - 4, kAccountRowH});
    const Layout grid = gridLayout(records, 2, kAccountRowH + 4);
    layout.items.insert(layout.items.end(), grid.items.begin(), grid.items.end());
    return layout;
}

// Moves the focus from item `from` in a direction (dx or dy is +/-1). Returns the new index, or
// `from` when nothing lies that way. A candidate must start beyond the far edge of the current
// item and overlap it across the movement axis (be in line); the nearest one wins and ties go to
// the lower index. With `fallback`, an item that is not in line is accepted when none is.
inline size_t layoutNeighbor(const Layout& layout, size_t from, int dx, int dy, bool fallback = false) {
    if (from >= layout.size()) return layout.empty() ? 0 : layout.size() - 1;
    const LayoutRect& a = layout.items[from];
    size_t best = from;
    long bestScore = 0x7fffffffL;
    for (size_t i = 0; i < layout.size(); ++i) {
        if (i == from) continue;
        const LayoutRect& b = layout.items[i];
        const int along = dx > 0 ? b.x - a.right() : dx < 0 ? a.x - b.right() : dy > 0 ? b.y - a.bottom() : a.y - b.bottom();
        if (along < 0) continue;
        const bool inLine = dx ? (b.y < a.bottom() && b.bottom() > a.y) : (b.x < a.right() && b.right() > a.x);
        if (!inLine && !fallback) continue;
        const int offset = dx ? (b.y * 2 + b.h) - (a.y * 2 + a.h) : (b.x * 2 + b.w) - (a.x * 2 + a.w);
        const long score = static_cast<long>(along) * 2 + (inLine ? 0 : (offset < 0 ? -offset : offset) + 1000);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

// True when moving in the direction would leave the layout (no neighbour that way).
inline bool layoutEdge(const Layout& layout, size_t from, int dx, int dy) {
    return layoutNeighbor(layout, from, dx, dy) == from;
}

// Scroll offset that keeps item `index` `anchor` pixels below the top of the view (never negative).
inline int layoutScrollTarget(const Layout& layout, size_t index, int anchor) {
    if (index >= layout.size()) return 0;
    if (layout.anchor >= 0) anchor = layout.anchor;
    const int target = (layout.horizontal ? layout.items[index].x : layout.items[index].y) - anchor;
    return target > 0 ? target : 0;
}

// First item that is at least partly below the top of a view scrolled to `scroll`.
inline size_t layoutFirstVisible(const Layout& layout, int scroll) {
    for (size_t i = 0; i < layout.size(); ++i)
        if ((layout.horizontal ? layout.items[i].right() : layout.items[i].bottom()) > scroll) return i;
    return layout.size();
}

// Scroll offset that brings item `index` fully into a view of `extent` pixels along the scroll axis,
// moving no further than needed and keeping `margin` pixels clear. `current` is the present offset.
inline int layoutReveal(const Layout& layout, size_t index, int current, int extent, int margin) {
    if (index >= layout.size()) return 0;
    const LayoutRect& r = layout.items[index];
    const int near = layout.horizontal ? r.x : r.y, far = layout.horizontal ? r.right() : r.bottom();
    if (far - current > extent - margin) current = far - (extent - margin);
    if (near - current < margin) current = near - margin;
    return current < 0 ? 0 : current;
}

} // namespace pspapp
