-- A dark palette, eased scrolling and a flash on every focus change, on a list of wide rows.
return {
  name = "Neon",
  shell = "hub", pages = "metro", dark = true,
  colors = {
    background = 0x0b0e17, panel = 0x141a2b, raised = 0x1b2340,
    text = 0xe8f1ff, dim = 0x7f8fb8, line = 0x25304f,
    accent = 0xff3d9a, ring = 0x38e8ff,
  },
  motion = { kind = "ease", ms = 180 },
  layout = function(ctx)
    local v = ctx.view
    if ctx.screen == "hub" then return nil end   -- the tile home keeps its built-in layout
    return {
      items = Layout.list(ctx.count, { x = 8, y = v.y + 4, w = 464, h = 40, gap = 4 }),
      card = "row",
      anchor = 90,
      focus = { kind = "ring", dx = 4 },
    }
  end,
  on = {
    focus = function(ev) return { pulse = true, flash = 0x38e8ff, flash_ms = 140 } end,
    open = function(ev) return { flash = 0xff3d9a, flash_ms = 220 } end,
  },
}
