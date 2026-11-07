# Themes

A theme is a Lua script that decides **where things go**, what colours the UI uses, how scrolling
moves and how the UI reacts to the D-pad. Scripts never draw: they return plain data (rectangles,
names, numbers) and the client validates, clamps and draws it. Anything wrong in a script
(error, endless loop, too much memory, bad data) is logged and that call falls back to the
built-in look, so a broken theme cannot lock the UI.

## Installing and choosing a theme

Put the theme in a folder next to the settings file:

```
ms0:/PSP/GAME/WiliMini/themes/<name>/theme.lua
```

Choose it in the game: **Settings → Theme** cycles through the built-in themes (Classic, Tiles and
Metro, each light and dark) and then every folder under `themes/`, and applies at once. The choice is
saved as `"theme"` in `settings.json` (a built-in id such as `metro-dark`, or the folder name: letters,
digits, `-`, `_`, up to 32 characters; the built-in names are reserved). If a script does not load, the
previous theme stays and the status line says why. Old settings files without `"theme"` keep working:
their home style and dark setting are the choice. The package ships the examples under `themes/`.

The built-in themes and script themes are the same kind of thing: a theme picks a **shell**, a page
look and (for a script) layouts, colours, motion and events. Home style is no longer a separate setting.

| Theme | Shows |
|---|---|
| `grid3` | three columns of tiles (the smallest useful layout theme) |
| `strip` | one row of big cards scrolling sideways, focus grows, others dim; the tile home as a row |
| `neon` | dark palette, eased scrolling, a flash on every focus change, wide rows |
| `xmb` | a "cross media bar" (`shell = "bar"`): gradient and waves background, a row of category icons, the focused category's items in a fading column |

## File format

The script returns a table. Every field is optional.

### `shell`, `pages`, `dark`

`shell` is the navigation frame the theme lives in, which decides how the user reaches Settings,
Mine, Feed and Inbox, and what area the layout gets (`ctx.view`):

| shell | Frame | `ctx.view` |
|---|---|---|
| `"sidebar"` (default) | the left icon column and the category row | starts right of the column (x 39) |
| `"hub"` | the tile home, and a pivot header on pages | full width, below the header |
| `"bar"` | the theme draws the category bar (set `tabs = true` in the layout) | full width |

`pages` (`"classic"` or `"metro"`) is the look of the pages the theme does not lay out (detail,
comments, Settings); `dark = true` selects the dark base colours. Together they pick the built-in
look underneath, so a theme never gets a frame it was not made for.

```lua
return {
  name = "My theme",
  colors = { background = 0x101820, text = {255, 255, 255}, accent = 0xff3d9a },
  motion = { kind = "ease", ms = 200 },
  layout = function(ctx) ... end,
  on = { focus = function(ev) ... end },
}
```

### `layout(ctx)`

Called when a page is entered, when its item count or the focus changes, and when the theme
loads; the result is cached, so a layout may depend on `ctx.selected`. Return `nil` to leave the
screen to the built-in layout.

`ctx` fields:

| Field | Meaning |
|---|---|
| `screen` | `"feed"` (home tabs), `"search"` (search results), `"mine"` (account and local records), `"browse"` (video lists such as dynamic feed, uploader videos), `"hub"` (the tile home) |
| `count` | number of items; the result must contain exactly this many rectangles |
| `selected` | focused item, **1-based** |
| `style` | `"classic"`, `"tiles"` or `"metro"` (the user's home style) |
| `dark` | the dark setting |
| `view` | `{x, y, w, h}`: the area below the page header that content may use |
| `ids` | on `"hub"` only: the name of each tile (`recommend`, `hots`, `search`, `discover`, `dynamic`, `inbox`, `mine`, `settings`) in item order. Place tiles by name, not by index |

The result:

| Field | Meaning |
|---|---|
| `items` | array of `{x, y, w, h}` (or `{x=,y=,w=,h=}`), one per item, in **screen** coordinates as seen at scroll 0. Fractions round down; `w`, `h` below 1 become 1; coordinates beyond ±30000 are an error |
| `axis` | `"y"` (default, scrolls down) or `"x"` (scrolls sideways) |
| `anchor` | how far from the view's top (or left) edge the focused item rests while scrolling; default 100 (y) or 12 (x) |
| `card` | `"tile"` (cover with title strip), `"row"` (small cover with two text lines), `"poster"` (cover with title only), `"compact"` (text only), `"xmb"` (small cover and text with no panel, fading with `dim_step`); default picks tile or row by height |
| `focus` | `{kind="ring"\|"scale"\|"none", scale=1.2, dx=, dy=, dim=true}` |

On `"hub"` the tiles keep their built-in faces; the layout moves and sizes them. A tile at most
70 px wide uses the small face.

Helpers (plain Lua in the sandbox): `Layout.grid(count, {cols, x, y, w, h, gx, gy})`,
`Layout.list(count, {x, y, w, h, gap})`, `Layout.strip(count, {x, y, w, h, gap})`,
`Layout.clamp(v, lo, hi)`. Each returns an array of rectangles for `items`.

D-pad movement follows the rectangles: from the focused item, the nearest item beyond its edge
that overlaps it across the movement axis (on the hub, the nearest item at all if none overlaps).
Every item must therefore be reachable by overlap; `tools/test-theme.sh` checks that for the
examples with 1 to 60 items.

### `colors`

`background`, `panel`, `raised`, `text`, `dim`, `line`, `accent`, `ring`. Each is `0xRRGGBB` or
`{r, g, b}`. Unnamed colours keep the built-in value for the user's light/dark and style setting.
`ring` is the focus outline (the text colour by default).

### `background`

`background = { top = 0x0b1445, bottom = 0x2b74c8, waves = true }` paints a vertical gradient behind the whole UI, with slow translucent waves if `waves` is set. Cards of style `"xmb"` have no panel of their own and sit directly on it.

### `motion`

How scrolling follows the focus. `kind = "spring"` (default; `stiffness` 20-1000, `damping`
2-100), `"ease"` (ease-out over `ms`, 30-2000) or `"snap"` (no animation).

### `on`

Event handlers, `function(ev) return reaction end`, where `ev = {name, screen, index}` (`index`
1-based; `screen` as in `layout`, plus `"browse"`). Events: `focus` (the focus moved), `open`
(confirm pressed on an item), `back`. A reaction is a table:

| Field | Effect |
|---|---|
| `pulse = true` | the focused item briefly grows 8 % and settles (160 ms) |
| `flash = 0xRRGGBB` / `{r,g,b}`, `flash_ms` | the focused item is tinted and fades (30-1000 ms) |

Return `nil` for no reaction. Reactions are data; a handler cannot change app state.

## Sandbox and limits

The script runs in its own Lua 5.4 state with only the base (without `load`, `dofile`,
`loadfile`, `require`, `collectgarbage`, `print`), `string` (without `dump`), `table`, `math`
and `utf8` libraries. There is no `io`, `os`, `package` or `debug`, and only text chunks load.

| Limit | Value |
|---|---|
| Lua memory | 384 KB per theme |
| Work per call | 400,000 VM instructions (about a few ms of Lua) |
| Script size | 128 KB |
| Items per layout | 512 (longer lists use the built-in layout) |

A failure is logged once per change as `theme layout <screen>: <message>` on the client log channel.

## Working on a theme

* `tools/test-theme.sh` builds Lua 5.4 for the host, runs the engine tests (sandbox, validation,
  limits) and loads every theme under `themes/`, checking its layouts for many item counts.
* `tools/theme-preview.py THEME SCREEN COUNT OUT.png --selected N --style 0|1|2` draws the
  rectangles of a layout, so a layout can be checked without a PSP or emulator.
* To see it in the emulator, add `--theme NAME` to `tools/ppsspp/seed-ui-fixture.py` (see
  `tools/README.md`).

Emulator and host results say nothing about the cost on a PSP-1000; the layout runs on the UI
thread at page entry and when the focus changes, so keep scripts short.
