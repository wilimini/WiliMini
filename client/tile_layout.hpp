#pragma once

// Geometry for the tile home screen. Pure arithmetic with no PSP dependencies so it can be
// checked on the host (tests/tile_layout_test.cpp). Pixel values assume the 480x272 screen.

#include <cstddef>

namespace pspapp {

enum class TileId { Recommend, Hots, Search, Discover, Dynamic, Inbox, Mine, Settings };

struct TileSpec {
    TileId id;
    int col, row, cols, rows;  // grid cells; the grid has kTileRows rows and grows to the right
};

constexpr int kTileUnit = 66;
constexpr int kTileGap = 4;
constexpr int kTileRows = 3;
constexpr int kTileOriginX = 10;   // left edge of column 0 when the panorama is not scrolled
constexpr int kTileOriginY = 50;   // top edge of row 0
constexpr int kTileMargin = 10;    // keep this much clear at the screen edges when scrolling
constexpr int kScreenWidth = 480;
constexpr int kTileLaunchMs = 240;  // length of the tile flip-out played before a tile opens

// Medium tiles span 2x2, wide tiles 4x2, small tiles 1x1.
constexpr TileSpec kTiles[] = {
    {TileId::Recommend, 0, 0, 2, 2},
    {TileId::Search,    0, 2, 1, 1},
    {TileId::Discover,  1, 2, 1, 1},
    {TileId::Dynamic,   2, 2, 1, 1},
    {TileId::Inbox,     3, 2, 1, 1},
    {TileId::Settings,  4, 2, 1, 1},
    {TileId::Hots,      2, 0, 4, 2},
    {TileId::Mine,      6, 0, 2, 2},
};
constexpr int kTileCount = static_cast<int>(sizeof(kTiles) / sizeof(kTiles[0]));

constexpr int tileX(const TileSpec& t) { return kTileOriginX + t.col * (kTileUnit + kTileGap); }
constexpr int tileY(const TileSpec& t) { return kTileOriginY + t.row * (kTileUnit + kTileGap); }
constexpr int tileWidth(const TileSpec& t) { return t.cols * kTileUnit + (t.cols - 1) * kTileGap; }
constexpr int tileHeight(const TileSpec& t) { return t.rows * kTileUnit + (t.rows - 1) * kTileGap; }

// Moves the focus ring in a direction (dx or dy is +/-1). Returns the new index, or `from`
// when nothing lies that way. A candidate must start beyond the far edge of the current tile;
// among those, one in line with the current tile wins, then the nearest.
inline int tileNeighbor(int from, int dx, int dy) {
    if (from < 0 || from >= kTileCount) return 0;
    const TileSpec& a = kTiles[from];
    const int aL = tileX(a), aR = aL + tileWidth(a), aT = tileY(a), aB = aT + tileHeight(a);
    int best = from, bestScore = 0x7fffffff;
    for (int i = 0; i < kTileCount; ++i) {
        if (i == from) continue;
        const TileSpec& b = kTiles[i];
        const int bL = tileX(b), bR = bL + tileWidth(b), bT = tileY(b), bB = bT + tileHeight(b);
        const int along = dx > 0 ? bL - aR : dx < 0 ? aL - bR : dy > 0 ? bT - aB : aT - bB;
        if (along < 0) continue;
        // A tile that overlaps the current one across the movement axis is directly in line.
        const bool inLine = dx ? (bT < aB && bB > aT) : (bL < aR && bR > aL);
        const int offset = dx ? (bT + bB) - (aT + aB) : (bL + bR) - (aL + aR);
        const int score = along * 2 + (inLine ? 0 : (offset < 0 ? -offset : offset) + 1000);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

// How far (in pixels) the panorama is shifted left so the selected tile is fully visible.
// `current` is the present shift; the result never scrolls further than needed.
inline int tilePan(int selected, int current) {
    if (selected < 0 || selected >= kTileCount) return 0;
    const TileSpec& t = kTiles[selected];
    const int left = tileX(t), right = left + tileWidth(t);
    if (right - current > kScreenWidth - kTileMargin) current = right - (kScreenWidth - kTileMargin);
    if (left - current < kTileMargin) current = left - kTileMargin;
    return current < 0 ? 0 : current;
}

} // namespace pspapp
