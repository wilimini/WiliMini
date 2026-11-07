#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace pspapp {

namespace {

constexpr int kHookCount = 1000;
constexpr int kCoordLimit = 30000;

// Helpers a theme can call. Plain Lua so scripts do not need to repeat the arithmetic.
const char kPrelude[] = R"LUA(
Layout = {}
local function opt(o, key, default) if o and o[key] ~= nil then return o[key] end return default end
-- Grid of count equal cells, left to right then down. Options: cols x y w h gx gy
function Layout.grid(count, o)
  local cols, x0, y0 = opt(o, "cols", 2), opt(o, "x", 12), opt(o, "y", 0)
  local w, h, gx, gy = opt(o, "w", 224), opt(o, "h", 98), opt(o, "gx", 4), opt(o, "gy", 4)
  local items = {}
  for i = 1, count do
    local c, r = (i - 1) % cols, (i - 1) // cols
    items[i] = { x0 + c * (w + gx), y0 + r * (h + gy), w, h }
  end
  return items
end
-- One cell per row. Options: x y w h gap
function Layout.list(count, o)
  local x0, y0, w, h, gap = opt(o, "x", 12), opt(o, "y", 0), opt(o, "w", 456), opt(o, "h", 34), opt(o, "gap", 2)
  local items = {}
  for i = 1, count do items[i] = { x0, y0 + (i - 1) * (h + gap), w, h } end
  return items
end
-- One row of cells scrolling sideways. Options: x y w h gap
function Layout.strip(count, o)
  local x0, y0, w, h, gap = opt(o, "x", 12), opt(o, "y", 0), opt(o, "w", 160), opt(o, "h", 120), opt(o, "gap", 8)
  local items = {}
  for i = 1, count do items[i] = { x0 + (i - 1) * (w + gap), y0, w, h } end
  return items
end
function Layout.clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end
)LUA";

struct Memory {
    size_t used = 0, limit = 0;
};

void* allocate(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    Memory* memory = static_cast<Memory*>(ud);
    if (!ptr) oldSize = 0;
    if (newSize == 0) {
        std::free(ptr);
        memory->used -= oldSize;
        return nullptr;
    }
    if (memory->used - oldSize + newSize > memory->limit) return nullptr;
    void* grown = std::realloc(ptr, newSize);
    if (grown) memory->used = memory->used - oldSize + newSize;
    return grown;
}

int clampInt(double value, int lo, int hi) {
    if (!(value == value)) return lo;  // NaN
    if (value < lo) return lo;
    if (value > hi) return hi;
    return static_cast<int>(std::floor(value));
}

bool readColor(lua_State* L, int index, ThemeColors::Slot& slot) {
    if (lua_isinteger(L, index)) {
        slot = {true, static_cast<uint32_t>(lua_tointeger(L, index)) & 0xffffffu};
        return true;
    }
    if (lua_istable(L, index)) {
        int channel[3];
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, index, i + 1);
            if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return false; }
            channel[i] = clampInt(lua_tonumber(L, -1), 0, 255);
            lua_pop(L, 1);
        }
        slot = {true, static_cast<uint32_t>(channel[0] << 16 | channel[1] << 8 | channel[2])};
        return true;
    }
    return false;
}

CardStyle cardFromName(const char* name) {
    if (!std::strcmp(name, "tile")) return CardStyle::Tile;
    if (!std::strcmp(name, "row")) return CardStyle::Row;
    if (!std::strcmp(name, "poster")) return CardStyle::Poster;
    if (!std::strcmp(name, "compact")) return CardStyle::Compact;
    return CardStyle::Default;
}

} // namespace

struct Theme::Impl {
    lua_State* L = nullptr;
    Memory memory;
    long budget = 0;
    int moduleRef = LUA_NOREF;
    std::string name, error;
    unsigned generation = 0;
    ThemeColors colors;
    ThemeMotion motion;

    ~Impl() { close(); }

    void close() {
        if (L) lua_close(L);
        L = nullptr;
        memory = Memory{};
        moduleRef = LUA_NOREF;
        name.clear();
        colors = ThemeColors{};
        motion = ThemeMotion{};
    }

    static void hook(lua_State* state, lua_Debug*) {
        Impl* self = *static_cast<Impl**>(lua_getextraspace(state));
        self->budget -= kHookCount;
        if (self->budget < 0) luaL_error(state, "instruction budget exceeded");
    }

    bool open() {
        close();
        memory.limit = kMemoryLimit;
        L = lua_newstate(allocate, &memory);
        if (!L) { error = "out of memory"; return false; }
        *static_cast<Impl**>(lua_getextraspace(L)) = this;
        // Only pure libraries: no io, os, package or debug, and no way to load other code.
        const luaL_Reg libs[] = {{"_G", luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                                 {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {nullptr, nullptr}};
        for (const luaL_Reg* lib = libs; lib->func; ++lib) {
            luaL_requiref(L, lib->name, lib->func, 1);
            lua_pop(L, 1);
        }
        static const char* const removed[] = {"dofile", "loadfile", "load", "loadstring", "require", "collectgarbage",
                                              "print", nullptr};
        for (const char* const* n = removed; *n; ++n) { lua_pushnil(L); lua_setglobal(L, *n); }
        lua_getglobal(L, "string");
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        lua_pop(L, 1);
        lua_sethook(L, hook, LUA_MASKCOUNT, kHookCount);
        return true;
    }

    // Runs the function and its nargs arguments on the stack with the instruction budget.
    bool call(int nargs, int nresults) {
        budget = kInstructionBudget;
        if (lua_pcall(L, nargs, nresults, 0) == LUA_OK) return true;
        const char* message = lua_tostring(L, -1);
        error = message ? message : "script error";
        lua_pop(L, 1);
        return false;
    }

    bool run(const std::string& source, const std::string& chunk, int nresults) {
        const std::string label = "=" + chunk;
        if (luaL_loadbufferx(L, source.data(), source.size(), label.c_str(), "t") != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            error = message ? message : "syntax error";
            lua_pop(L, 1);
            return false;
        }
        return call(0, nresults);
    }

    void readModule() {
        lua_rawgeti(L, LUA_REGISTRYINDEX, moduleRef);
        lua_getfield(L, -1, "name");
        if (lua_isstring(L, -1) && name.empty()) name = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "colors");
        if (lua_istable(L, -1)) {
            struct { const char* key; ThemeColors::Slot* slot; } map[] = {
                {"background", &colors.background}, {"panel", &colors.panel}, {"raised", &colors.raised}, {"text", &colors.text}, {"dim", &colors.dim},
                {"line", &colors.line}, {"accent", &colors.accent}, {"ring", &colors.ring}};
            for (auto& entry : map) {
                lua_getfield(L, -1, entry.key);
                if (!lua_isnil(L, -1) && !readColor(L, lua_gettop(L), *entry.slot)) {
                    error = std::string("colors.") + entry.key + " must be 0xRRGGBB or {r,g,b}";
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "motion");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "kind");
            if (lua_isstring(L, -1)) {
                const char* kind = lua_tostring(L, -1);
                if (!std::strcmp(kind, "ease")) motion.kind = ThemeMotion::Ease;
                else if (!std::strcmp(kind, "snap")) motion.kind = ThemeMotion::Snap;
                else motion.kind = ThemeMotion::Spring;
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "stiffness");
            if (lua_isnumber(L, -1)) motion.stiffness = static_cast<float>(clampInt(lua_tonumber(L, -1), 20, 1000));
            lua_pop(L, 1);
            lua_getfield(L, -1, "damping");
            if (lua_isnumber(L, -1)) motion.damping = static_cast<float>(clampInt(lua_tonumber(L, -1), 2, 100));
            lua_pop(L, 1);
            lua_getfield(L, -1, "ms");
            if (lua_isnumber(L, -1)) motion.durationMs = clampInt(lua_tonumber(L, -1), 30, 2000);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }

    bool getFunction(const char* key) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, moduleRef);
        lua_getfield(L, -1, key);
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) return true;
        lua_pop(L, 1);
        return false;
    }
};

Theme::Theme() : impl_(new Impl) {}
Theme::~Theme() = default;

bool Theme::loadSource(const std::string& source, const std::string& name) {
    Impl& s = *impl_;
    ++s.generation;
    s.error.clear();
    if (!s.open()) return false;
    if (!s.run(kPrelude, "prelude", 0)) { s.close(); return false; }
    if (!s.run(source, name, 1)) { s.close(); return false; }
    if (!lua_istable(s.L, -1)) {
        s.error = name + ": the script must return a table";
        s.close();
        return false;
    }
    s.moduleRef = luaL_ref(s.L, LUA_REGISTRYINDEX);
    s.readModule();
    if (s.name.empty()) s.name = name;
    return true;
}

bool Theme::loadFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        ++impl_->generation;
        impl_->error = "cannot open " + path;
        impl_->close();
        return false;
    }
    std::string source;
    char buffer[2048];
    size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        source.append(buffer, read);
        if (source.size() > 128 * 1024) break;
    }
    std::fclose(file);
    if (source.size() > 128 * 1024) {
        ++impl_->generation;
        impl_->error = "theme script is larger than 128 KB";
        impl_->close();
        return false;
    }
    return loadSource(source, path);
}

void Theme::unload() {
    ++impl_->generation;
    impl_->error.clear();
    impl_->close();
}

bool Theme::loaded() const { return impl_->L && impl_->moduleRef != LUA_NOREF; }
const std::string& Theme::name() const { return impl_->name; }
const std::string& Theme::error() const { return impl_->error; }
unsigned Theme::generation() const { return impl_->generation; }
const ThemeColors& Theme::colors() const { return impl_->colors; }
const ThemeMotion& Theme::motion() const { return impl_->motion; }

bool Theme::hasLayout(const char*) const {
    if (!loaded()) return false;
    const bool has = impl_->getFunction("layout");
    if (has) lua_pop(impl_->L, 1);
    return has;
}

bool Theme::layout(const LayoutRequest& request, Layout& out) {
    Impl& s = *impl_;
    if (!loaded() || request.count > kMaxItems) return false;
    lua_State* L = s.L;
    const int top = lua_gettop(L);
    if (!s.getFunction("layout")) return false;
    lua_createtable(L, 0, 10);
    lua_pushstring(L, request.screen); lua_setfield(L, -2, "screen");
    lua_pushinteger(L, static_cast<lua_Integer>(request.count)); lua_setfield(L, -2, "count");
    lua_pushinteger(L, static_cast<lua_Integer>(request.selected) + 1); lua_setfield(L, -2, "selected");
    static const char* const styles[] = {"classic", "tiles", "metro"};
    lua_pushstring(L, styles[std::max(0, std::min(2, request.style))]); lua_setfield(L, -2, "style");
    lua_pushboolean(L, request.dark); lua_setfield(L, -2, "dark");
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, request.viewX); lua_setfield(L, -2, "x");
    lua_pushinteger(L, request.viewY); lua_setfield(L, -2, "y");
    lua_pushinteger(L, request.viewW); lua_setfield(L, -2, "w");
    lua_pushinteger(L, request.viewH); lua_setfield(L, -2, "h");
    lua_setfield(L, -2, "view");
    if (!request.ids.empty()) {
        lua_createtable(L, static_cast<int>(request.ids.size()), 0);
        for (size_t i = 0; i < request.ids.size(); ++i) {
            lua_pushstring(L, request.ids[i].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
        }
        lua_setfield(L, -2, "ids");
    }
    if (!s.call(1, 1)) { lua_settop(L, top); return false; }
    if (lua_isnil(L, -1)) { lua_settop(L, top); return false; }   // the script leaves this screen alone
    const int result = lua_gettop(L);
    if (!lua_istable(L, result)) {
        s.error = "layout must return a table or nil";
        lua_settop(L, top);
        return false;
    }
    Layout layout;
    layout.themed = true;
    lua_getfield(L, result, "items");
    if (!lua_istable(L, -1)) {
        s.error = "layout result has no items table";
        lua_settop(L, top);
        return false;
    }
    layout.items.reserve(request.count);
    for (size_t i = 0; i < request.count; ++i) {
        lua_rawgeti(L, -1, static_cast<lua_Integer>(i) + 1);
        if (!lua_istable(L, -1)) {
            char message[96];
            std::snprintf(message, sizeof(message), "layout returned %zu items, need %zu", i, request.count);
            s.error = message;
            lua_settop(L, top);
            return false;
        }
        double value[4];
        static const char* const names[4] = {"x", "y", "w", "h"};
        for (int k = 0; k < 4; ++k) {
            lua_rawgeti(L, -1, k + 1);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_getfield(L, -1, names[k]); }
            if (!lua_isnumber(L, -1)) {
                char message[96];
                std::snprintf(message, sizeof(message), "item %zu: %s is not a number", i + 1, names[k]);
                s.error = message;
                lua_settop(L, top);
                return false;
            }
            value[k] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        // Far outside the range is a script bug, not something to clamp: clamping would stack items.
        if (!(std::fabs(value[0]) <= kCoordLimit && std::fabs(value[1]) <= kCoordLimit &&
              value[2] <= kCoordLimit && value[3] <= kCoordLimit)) {
            char message[96];
            std::snprintf(message, sizeof(message), "item %zu is out of range (limit %d)", i + 1, kCoordLimit);
            s.error = message;
            lua_settop(L, top);
            return false;
        }
        layout.items.push_back({clampInt(value[0], -kCoordLimit, kCoordLimit), clampInt(value[1], -kCoordLimit, kCoordLimit),
                                clampInt(value[2], 1, kCoordLimit), clampInt(value[3], 1, kCoordLimit)});
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // items
    lua_getfield(L, result, "card");
    if (lua_isstring(L, -1)) layout.card = cardFromName(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, result, "axis");
    if (lua_isstring(L, -1)) layout.horizontal = !std::strcmp(lua_tostring(L, -1), "x");
    lua_pop(L, 1);
    lua_getfield(L, result, "anchor");
    if (lua_isnumber(L, -1)) layout.anchor = clampInt(lua_tonumber(L, -1), 0, kCoordLimit);
    lua_pop(L, 1);
    lua_getfield(L, result, "focus");
    if (lua_istable(L, -1)) {
        const int focus = lua_gettop(L);
        lua_getfield(L, focus, "kind");
        if (lua_isstring(L, -1)) {
            const char* kind = lua_tostring(L, -1);
            layout.focus.kind = !std::strcmp(kind, "none") ? FocusFx::None : !std::strcmp(kind, "scale") ? FocusFx::Scale : FocusFx::Ring;
        }
        lua_pop(L, 1);
        lua_getfield(L, focus, "scale");
        if (lua_isnumber(L, -1)) layout.focus.scalePercent = clampInt(lua_tonumber(L, -1) * 100.0, 50, 250);
        lua_pop(L, 1);
        lua_getfield(L, focus, "dx");
        if (lua_isnumber(L, -1)) layout.focus.offsetX = clampInt(lua_tonumber(L, -1), -64, 64);
        lua_pop(L, 1);
        lua_getfield(L, focus, "dy");
        if (lua_isnumber(L, -1)) layout.focus.offsetY = clampInt(lua_tonumber(L, -1), -64, 64);
        lua_pop(L, 1);
        lua_getfield(L, focus, "dim");
        layout.focus.dimOthers = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_settop(L, top);
    s.error.clear();
    out = std::move(layout);
    return true;
}

bool Theme::event(const char* name, const char* screen, size_t index, ThemeReaction& out) {
    Impl& s = *impl_;
    if (!loaded()) return false;
    lua_State* L = s.L;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.moduleRef);
    lua_getfield(L, -1, "on");
    if (!lua_istable(L, -1)) { lua_settop(L, top); return false; }
    lua_getfield(L, -1, name);
    if (!lua_isfunction(L, -1)) { lua_settop(L, top); return false; }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, name); lua_setfield(L, -2, "name");
    lua_pushstring(L, screen); lua_setfield(L, -2, "screen");
    lua_pushinteger(L, static_cast<lua_Integer>(index) + 1); lua_setfield(L, -2, "index");
    if (!s.call(1, 1)) { lua_settop(L, top); return false; }
    ThemeReaction reaction;
    bool any = false;
    if (lua_istable(L, -1)) {
        const int table = lua_gettop(L);
        lua_getfield(L, table, "pulse");
        reaction.pulse = lua_toboolean(L, -1) != 0;
        any |= reaction.pulse;
        lua_pop(L, 1);
        lua_getfield(L, table, "flash");
        ThemeColors::Slot flash;
        if (!lua_isnil(L, -1) && readColor(L, lua_gettop(L), flash)) { reaction.flashRgb = static_cast<int>(flash.rgb); any = true; }
        lua_pop(L, 1);
        lua_getfield(L, table, "flash_ms");
        if (lua_isnumber(L, -1)) reaction.flashMs = clampInt(lua_tonumber(L, -1), 30, 1000);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    if (any) out = reaction;
    return any;
}

} // namespace pspapp
