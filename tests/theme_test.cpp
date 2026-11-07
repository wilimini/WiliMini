#include "theme.hpp"
#include <cassert>
#include <cstdio>

using namespace pspapp;

static LayoutRequest request(const char* screen, size_t count, size_t selected = 0) {
    LayoutRequest r;
    r.screen = screen;
    r.count = count;
    r.selected = selected;
    return r;
}

int main() {
    // The grid helper reproduces the built-in grid.
    {
        Theme t;
        assert(t.loadSource(R"(return { name = "grid", layout = function(ctx)
            return { items = Layout.grid(ctx.count, { y = 0 }), card = "tile", anchor = 100 } end })", "grid"));
        assert(t.loaded() && t.name() == "grid");
        Layout out;
        assert(t.layout(request("feed", 7), out));
        const Layout builtin = gridLayout(7, 2);
        assert(out.size() == 7 && out.themed && out.card == CardStyle::Tile && out.anchor == 100 && !out.horizontal);
        for (size_t i = 0; i < 7; ++i) {
            assert(out.items[i].x == builtin.items[i].x && out.items[i].y == builtin.items[i].y);
            assert(out.items[i].w == builtin.items[i].w && out.items[i].h == builtin.items[i].h);
        }
        assert(layoutScrollTarget(out, 5, 0) == layoutScrollTarget(builtin, 5, kFeedGridScrollAnchor));
    }
    // ctx is 1-based for selected, screens are passed, nil leaves the screen alone.
    {
        Theme t;
        assert(t.loadSource(R"(return { layout = function(ctx)
            if ctx.screen ~= "hub" then return nil end
            local r = Layout.strip(ctx.count, { w = 100 + ctx.selected })
            return { items = r, axis = "x", focus = { kind = "scale", scale = 1.2, dy = -4, dim = true } } end })", "t"));
        Layout out;
        assert(!t.layout(request("feed", 3), out) && t.error().empty());
        assert(t.layout(request("hub", 3, 1), out));
        assert(out.horizontal && out.items[0].w == 102 && out.focus.kind == FocusFx::Scale);
        assert(out.focus.scalePercent == 120 && out.focus.offsetY == -4 && out.focus.dimOthers);
        assert(layoutScrollTarget(out, 2, 10) == out.items[2].x - 10);
    }
    // Validation: too few items, non-numbers, wrong types, clamping.
    {
        Theme t;
        assert(t.loadSource("return { layout = function(ctx) return { items = { {0,0,10,10} } } end }", "few"));
        Layout out;
        assert(!t.layout(request("feed", 3), out) && !t.error().empty());
        assert(t.loadSource("return { layout = function(ctx) return { items = { {0,0,'a',10} } } end }", "nan"));
        assert(!t.layout(request("feed", 1), out));
        assert(t.loadSource("return { layout = function(ctx) return 5 end }", "num"));
        assert(!t.layout(request("feed", 1), out));
        assert(t.loadSource("return { layout = function(ctx) return { items = { {x=-99999,y=1e9,w=0,h=-5} } } end }", "clamp"));
        assert(t.layout(request("feed", 1), out));
        assert(out.items[0].x == -4000 && out.items[0].y == 4000 && out.items[0].w == 1 && out.items[0].h == 1);
        assert(!t.layout(request("feed", Theme::kMaxItems + 1), out));
    }
    // Sandbox: no io/os/load, runaway loops and memory bombs are stopped, errors do not stick.
    {
        Theme t;
        assert(t.loadSource(R"(return { layout = function(ctx)
            local n = 0
            if io or os or package or debug or load or dofile or require or print or collectgarbage then n = 1 end
            if string.dump then n = n + 1 end
            return { items = { {n,0,10,10} } } end })", "sandbox"));
        Layout out;
        assert(t.layout(request("feed", 1), out) && out.items[0].x == 0);
        assert(t.loadSource("return { layout = function() while true do end end }", "loop"));
        assert(!t.layout(request("feed", 1), out) && t.error().find("budget") != std::string::npos);
        assert(t.loadSource("return { layout = function() local s = ('x'):rep(100000000) return s end }", "bomb"));
        assert(!t.layout(request("feed", 1), out));
        assert(t.loadSource("return { layout = function() local t = {} for i = 1, 1e8 do t[i] = i end end }", "bomb2"));
        assert(!t.layout(request("feed", 1), out));
        assert(t.loadSource("return { layout = function() error('boom') end }", "err"));
        assert(!t.layout(request("feed", 1), out) && t.error().find("boom") != std::string::npos);
        // A load-time infinite loop and bad scripts fail cleanly and leave no theme loaded.
        assert(!t.loadSource("while true do end", "spin") && !t.loaded());
        assert(!t.loadSource("return 5", "notable") && !t.loaded());
        assert(!t.loadSource("return {", "syntax") && !t.loaded() && !t.error().empty());
        assert(!t.loadSource("\x1bLua", "bytecode") && !t.loaded());
        assert(!t.layout(request("feed", 1), out));
    }
    // Colours, motion and events.
    {
        Theme t;
        const unsigned before = t.generation();
        assert(t.loadSource(R"(return {
            colors = { background = 0x101820, text = {255, 240, 10}, accent = "bad" },
            motion = { kind = "ease", ms = 5000, stiffness = 1 },
            on = { focus = function(ev) if ev.index == 2 then return { pulse = true, flash = 0xff0000 } end end } })", "colors"));
        assert(t.generation() > before);
        assert(t.colors().background.set && t.colors().background.rgb == 0x101820);
        assert(t.colors().text.set && t.colors().text.rgb == 0xfff00a);
        assert(!t.colors().accent.set && !t.colors().line.set && !t.error().empty());
        assert(t.motion().kind == ThemeMotion::Ease && t.motion().durationMs == 2000 && t.motion().stiffness == 20.f);
        ThemeReaction r;
        assert(!t.event("focus", "feed", 0, r));
        assert(t.event("focus", "feed", 1, r) && r.pulse && r.flashRgb == 0xff0000);
        assert(!t.event("open", "feed", 1, r));
        t.unload();
        assert(!t.loaded() && !t.event("focus", "feed", 1, r));
    }
    std::puts("theme test passed");
    return 0;
}
