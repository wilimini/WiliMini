#include "layout.hpp"
#include "tile_layout.hpp"
#include <algorithm>
#include <cassert>

using namespace pspapp;

// The row/column arithmetic the layout engine replaced.
static int oldGridMove(int value, int count, int columns, int dx, int dy) {
    if (dx < 0 && value % columns != 0) --value;
    if (dx > 0 && value % columns != columns - 1 && value + 1 < count) ++value;
    if (dy < 0 && value >= columns) value -= columns;
    if (dy > 0 && value + columns < count) value += columns;
    return value;
}

// The old Mine tile movement; index 0 is the account row.
static int oldMineMove(int at, int total, int dx, int dy) {
    const int last = total - 1;
    int target = at;
    if (at == 0) { if (dy > 0 || dx > 0) target = 1; }
    else if (dx < 0 && (at - 1) % 2 == 1) target = at - 1;
    else if (dx > 0 && (at - 1) % 2 == 0) target = at + 1 <= last ? at + 1 : at;
    else if (dy < 0) target = at - 1 < 2 ? 0 : at - 2;
    else if (dy > 0) target = at + 2 <= last ? at + 2 : at;
    return std::max(0, std::min(target, last));
}

int main() {
    const int moves[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    // Spatial movement in the built-in grid and list equals the old arithmetic for every count.
    for (int columns = 1; columns <= 2; ++columns) {
        for (int count = 1; count <= 13; ++count) {
            const Layout layout = columns == 1 ? listLayout(count) : gridLayout(count, 2);
            for (int from = 0; from < count; ++from)
                for (const auto& m : moves)
                    assert(static_cast<int>(layoutNeighbor(layout, from, m[0], m[1])) ==
                           oldGridMove(from, count, columns, m[0], m[1]));
        }
    }

    // Mine tiles: account row then a two-column grid. The old code also let the account row
    // step right into the first record, which the app keeps as a special case.
    for (int records = 0; records <= 11; ++records) {
        const Layout layout = accountGridLayout(records);
        const int total = records + 1;
        for (int from = 0; from < total; ++from)
            for (const auto& m : moves) {
                if (from == 0 && m[0] > 0) continue;
                assert(static_cast<int>(layoutNeighbor(layout, from, m[0], m[1])) ==
                       oldMineMove(from, total, m[0], m[1]));
            }
    }

    // Scroll targets equal the old per-screen formulas.
    for (int sel = 0; sel < 12; ++sel) {
        assert(layoutScrollTarget(listLayout(12), sel, kFeedRowScrollAnchor) == std::max(0, (sel - 2) * 36));
        assert(layoutScrollTarget(gridLayout(12, 2), sel, kFeedGridScrollAnchor) == std::max(0, sel / 2 * 102 - 100));
        assert(layoutScrollTarget(accountGridLayout(12), sel + 1, kFeedGridScrollAnchor) ==
               std::max(0, 54 + sel / 2 * 102 - 100));
    }

    // The first item whose cover is wanted: the row above the selected one in the grid, two
    // items above in the list.
    for (int sel = 0; sel < 12; ++sel) {
        const Layout list = listLayout(12), grid = gridLayout(12, 2);
        assert(static_cast<int>(layoutFirstVisible(list, layoutScrollTarget(list, sel, kFeedRowScrollAnchor))) ==
               (sel > 2 ? sel - 2 : 0));
        assert(static_cast<int>(layoutFirstVisible(grid, layoutScrollTarget(grid, sel, kFeedGridScrollAnchor))) ==
               (sel >= 2 ? (sel / 2 - 1) * 2 : 0));
    }

    // Irregular layouts (what a theme script would return): a hero card and a side column.
    Layout hero;
    hero.items = {{0, 0, 300, 200}, {304, 0, 150, 98}, {304, 102, 150, 98}, {0, 204, 454, 40}};
    assert(layoutNeighbor(hero, 0, 1, 0) == 1);          // right of the hero: the nearest card in line
    assert(layoutNeighbor(hero, 2, -1, 0) == 0);
    assert(layoutNeighbor(hero, 1, 0, 1) == 2);
    assert(layoutNeighbor(hero, 2, 0, 1) == 3);
    assert(layoutNeighbor(hero, 0, 0, 1) == 3);
    assert(layoutNeighbor(hero, 3, 0, -1) == 0);
    assert(layoutNeighbor(hero, 0, 0, -1) == 0);         // nothing above
    assert(layoutEdge(hero, 0, -1, 0));
    // Not in line: strict mode stays, fallback mode jumps to the nearest item that way.
    Layout stair;
    stair.items = {{0, 0, 100, 50}, {200, 100, 100, 50}};
    assert(layoutNeighbor(stair, 0, 1, 0) == 0);
    assert(layoutNeighbor(stair, 0, 1, 0, true) == 1);

    assert(layoutNeighbor(Layout{}, 0, 1, 0) == 0);

    // The tile home as a layout: movement (with the in-line preference) and panning equal the
    // original tile functions.
    {
        const Layout hub = builtinHubLayout();
        assert(hub.size() == static_cast<size_t>(kTileCount) && hub.horizontal);
        for (int from = 0; from < kTileCount; ++from)
            for (const auto& m : moves)
                assert(static_cast<int>(layoutNeighbor(hub, from, m[0], m[1], true)) == tileNeighbor(from, m[0], m[1]));
        for (int sel = 0; sel < kTileCount; ++sel)
            for (int current : {0, 37, 150, 400})
                assert(layoutReveal(hub, sel, current, kScreenWidth, kTileMargin) == tilePan(sel, current));
    }
    // A vertical strip pans along y.
    {
        Layout column;
        column.items = {{0, 0, 100, 100}, {0, 110, 100, 100}, {0, 220, 100, 100}};
        assert(layoutReveal(column, 2, 0, 258, 10) == 320 - (258 - 10));
        assert(layoutReveal(column, 0, 100, 258, 10) == 0);
    }
    return 0;
}
