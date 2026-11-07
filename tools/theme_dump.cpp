// Host tool for theme authors: runs a theme's layout function and prints the rectangles as JSON.
//   theme-dump THEME_DIR SCREEN COUNT [SELECTED] [STYLE]
// Used by tools/theme-preview.py. Screens: feed, search, mine, browse, hub.
#include "theme.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pspapp;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: theme-dump THEME_DIR SCREEN COUNT [SELECTED] [STYLE 0-2]\n");
        return 2;
    }
    Theme theme;
    if (!theme.loadFile(std::string(argv[1]) + "/theme.lua")) {
        std::fprintf(stderr, "error: %s\n", theme.error().c_str());
        return 1;
    }
    LayoutRequest request;
    request.screen = argv[2];
    request.count = static_cast<size_t>(std::atoi(argv[3]));
    request.selected = argc > 4 ? static_cast<size_t>(std::atoi(argv[4])) : 0;
    request.style = argc > 5 ? std::atoi(argv[5]) : 1;
    if (request.style) { request.viewY = 70; request.viewH = 181; } else { request.viewY = 53; request.viewH = 198; }
    if (std::string(argv[2]) == "hub") {
        static const char* const ids[] = {"recommend", "search", "discover", "dynamic", "inbox", "settings", "hots", "mine"};
        request.count = 8;
        for (const char* id : ids) request.ids.push_back(id);
    }
    Layout layout;
    const bool ok = theme.layout(request, layout);
    if (!ok && !theme.error().empty()) {
        std::fprintf(stderr, "error: %s\n", theme.error().c_str());
        return 1;
    }
    std::printf("{\"themed\":%s,\"horizontal\":%s,\"card\":%d,\"anchor\":%d,\"selected\":%zu,\"focus\":{\"kind\":%d,\"scale\":%d,\"dx\":%d,\"dy\":%d},\"items\":[",
                ok ? "true" : "false", layout.horizontal ? "true" : "false", static_cast<int>(layout.card), layout.anchor,
                request.selected, static_cast<int>(layout.focus.kind), layout.focus.scalePercent, layout.focus.offsetX,
                layout.focus.offsetY);
    for (size_t i = 0; i < layout.size(); ++i)
        std::printf("%s[%d,%d,%d,%d]", i ? "," : "", layout.items[i].x, layout.items[i].y, layout.items[i].w, layout.items[i].h);
    std::printf("]}\n");
    return 0;
}
