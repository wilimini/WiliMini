#include "tile_layout.hpp"
#include <cassert>

using namespace pspapp;

static int indexOf(TileId id) {
    for (int i = 0; i < kTileCount; ++i) if (kTiles[i].id == id) return i;
    return -1;
}

int main() {
    // Every tile fits in the three rows and no two tiles overlap.
    for (int i = 0; i < kTileCount; ++i) {
        assert(kTiles[i].row >= 0 && kTiles[i].row + kTiles[i].rows <= kTileRows);
        for (int j = i + 1; j < kTileCount; ++j) {
            const TileSpec &a = kTiles[i], &b = kTiles[j];
            const bool apart = a.col + a.cols <= b.col || b.col + b.cols <= a.col ||
                               a.row + a.rows <= b.row || b.row + b.rows <= a.row;
            assert(apart);
        }
    }
    // The grid fills the screen height above the footer (y 258).
    assert(tileY(kTiles[indexOf(TileId::Search)]) + kTileUnit <= 258);

    const int rec = indexOf(TileId::Recommend), hot = indexOf(TileId::Hots);
    const int search = indexOf(TileId::Search), disc = indexOf(TileId::Discover);
    const int dyn = indexOf(TileId::Dynamic), mine = indexOf(TileId::Mine);
    assert(tileNeighbor(rec, 1, 0) == hot);
    assert(tileNeighbor(rec, 0, 1) == search);
    assert(tileNeighbor(search, 1, 0) == disc);
    assert(tileNeighbor(disc, 1, 0) == dyn);
    assert(tileNeighbor(search, 0, -1) == rec);
    assert(tileNeighbor(hot, -1, 0) == rec);
    assert(tileNeighbor(hot, 1, 0) == mine);
    // Nothing lies beyond the edges.
    assert(tileNeighbor(rec, -1, 0) == rec);
    assert(tileNeighbor(rec, 0, -1) == rec);
    assert(tileNeighbor(search, 0, 1) == search);
    assert(tileNeighbor(mine, 1, 0) == mine);

    // Every tile is reachable from the first one by d-pad moves.
    bool seen[kTileCount] = {};
    int queue[kTileCount], head = 0, tail = 0;
    queue[tail++] = 0; seen[0] = true;
    while (head < tail) {
        const int at = queue[head++];
        const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& step : d) {
            const int next = tileNeighbor(at, step[0], step[1]);
            if (!seen[next]) { seen[next] = true; queue[tail++] = next; }
        }
    }
    assert(tail == kTileCount);

    // Panning: the first screen needs no scroll, the far tiles scroll just enough.
    assert(tilePan(rec, 0) == 0 && tilePan(hot, 0) == 0);
    const TileSpec& m = kTiles[mine];
    assert(tilePan(mine, 0) == tileX(m) + tileWidth(m) - (kScreenWidth - kTileMargin));
    int pan = tilePan(mine, 0);
    assert(tilePan(hot, pan) == pan);                   // already fully visible, no change
    assert(tilePan(rec, pan) == 0);                     // moving back to the first tile scrolls back
    assert(tilePan(mine, pan) == pan);                   // already visible, no change
    assert(tilePan(-1, 50) == 0 && tilePan(kTileCount, 50) == 0);
    return 0;
}
