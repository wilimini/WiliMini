#pragma once

// Theme scripting. A theme is a Lua file that returns a table; the host calls its functions to
// get layouts, a palette, animation settings and event reactions. Scripts never draw: they return
// plain data (rectangles, names, numbers) that the host validates, clamps and draws. A script that
// fails, runs too long or returns bad data is ignored for that call and the built-in look is used.
// No PSP dependencies, so it can be checked on the host (tests/theme_test.cpp).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "layout.hpp"

namespace pspapp {

struct LayoutRequest {
    const char* screen = "feed";  // "feed", "search", "mine" or "hub"
    size_t count = 0;
    size_t selected = 0;          // 0-based
    int style = 0;                // 0 classic, 1 tiles, 2 metro
    bool dark = false;
    int viewX = 0, viewY = 49, viewW = 480, viewH = 202;  // the area the content may use
    std::vector<std::string> ids; // optional names of the items (the hub tiles)
};

struct ThemeColors {
    // Each colour is 0xRRGGBB when set. A theme only overrides what it names.
    struct Slot { bool set = false; uint32_t rgb = 0; };
    Slot background, panel, raised, text, dim, line, accent, ring;
};

// What a theme needs from the app around it. `shell` is the navigation frame the theme lives in:
// "sidebar" (left icon column and category row), "hub" (tile home with a pivot header on pages) or
// "bar" (the theme draws the category bar itself). `pages` is the look of the pages the theme does not
// lay out (detail, comments, Settings): "classic" or "metro". `dark` selects the dark base colours.
struct ThemeMeta {
    std::string shell = "sidebar", pages = "classic";
    bool dark = false;
};

// A vertical gradient behind everything, optionally with slow translucent waves.
struct ThemeBackground {
    bool set = false;
    uint32_t top = 0, bottom = 0;  // 0xRRGGBB
    bool waves = false;
};

// How the scrolling content follows the focus.
struct ThemeMotion {
    enum Kind { Spring, Ease, Snap } kind = Spring;
    float stiffness = 190.f, damping = 24.f;  // Spring
    int durationMs = 220;                     // Ease
};

// What a script can ask the host to do when an event fires. All of it is data.
struct ThemeReaction {
    bool pulse = false;         // briefly enlarge the focus
    int flashRgb = -1;          // flash the focused item with this colour (0xRRGGBB), -1 for none
    int flashMs = 160;
    int sound = 0;              // reserved
};

class Theme {
public:
    Theme();
    ~Theme();
    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;

    // Replaces the loaded theme. Returns false (and error()) if the source does not run or does
    // not return a table. `name` is only used in messages.
    bool loadSource(const std::string& source, const std::string& name);
    bool loadFile(const std::string& path);
    void unload();
    bool loaded() const;
    const std::string& name() const;
    const std::string& error() const;   // last failure, empty when none
    // Bumps on every load/unload so caches built from this theme can be dropped.
    unsigned generation() const;

    // True if the script defines a layout function for the screen.
    bool hasLayout(const char* screen) const;
    // Runs the script's layout function. False means "use the built-in layout".
    bool layout(const LayoutRequest& request, Layout& out);

    const ThemeColors& colors() const;
    const ThemeMotion& motion() const;
    const ThemeBackground& background() const;
    const ThemeMeta& meta() const;
    // Runs the handler for an event ("focus", "open", "back", "page"); `index` is 0-based.
    bool event(const char* name, const char* screen, size_t index, ThemeReaction& out);

    // Limits (public for tests).
    static constexpr size_t kMemoryLimit = 384 * 1024;
    static constexpr long kInstructionBudget = 400000;
    static constexpr size_t kMaxItems = 512;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pspapp
