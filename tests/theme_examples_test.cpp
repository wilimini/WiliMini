// Loads every theme under the given directory and checks what a player would run into:
// the script loads, its layouts are valid for many item counts, and every item can be reached
// from the first one with the D-pad.
#include "theme.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace pspapp;

static bool reachable(const Layout& layout) {
    std::vector<bool> seen(layout.size(), false);
    std::vector<size_t> todo{0};
    seen[0] = true;
    size_t count = 1;
    while (!todo.empty()) {
        const size_t at = todo.back();
        todo.pop_back();
        static const int moves[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& m : moves) {
            const size_t to = layoutNeighbor(layout, at, m[0], m[1]);
            if (!seen[to]) { seen[to] = true; ++count; todo.push_back(to); }
        }
    }
    return count == layout.size();
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path dir = argc > 1 ? argv[1] : "themes";
    int themes = 0, failures = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const fs::path script = entry.path() / "theme.lua";
        if (!fs::exists(script)) continue;
        ++themes;
        Theme theme;
        if (!theme.loadFile(script.string())) {
            std::printf("FAIL %s: %s\n", script.c_str(), theme.error().c_str());
            ++failures;
            continue;
        }
        for (const char* screen : {"feed", "search", "mine", "hub"}) {
            for (int style = 0; style < 3; ++style) {
                for (size_t count = 1; count <= 60; ++count) {
                    for (size_t selected : {size_t(0), count / 2, count - 1}) {
                        LayoutRequest request;
                        request.screen = screen;
                        request.count = count;
                        request.selected = selected;
                        request.style = style;
                        if (!std::string("hub").compare(screen))
                            for (size_t i = 0; i < count; ++i) request.ids.push_back("tile" + std::to_string(i));
                        Layout layout;
                        if (!theme.layout(request, layout)) {
                            if (!theme.error().empty()) {
                                std::printf("FAIL %s %s count=%zu: %s\n", script.c_str(), screen, count, theme.error().c_str());
                                ++failures;
                                count = 61;
                                break;
                            }
                            continue;   // the theme leaves this screen to the built-in layout
                        }
                        if (!reachable(layout)) {
                            std::printf("FAIL %s %s count=%zu: some items cannot be reached with the D-pad\n", script.c_str(), screen, count);
                            ++failures;
                            count = 61;
                            break;
                        }
                    }
                }
            }
        }
    }
    std::printf("%d themes checked, %d failures\n", themes, failures);
    return failures || !themes ? 1 : 0;
}
