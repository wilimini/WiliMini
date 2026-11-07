-- One row of large cards scrolling sideways; the focused card grows and the others dim.
return {
  name = "Strip",
  layout = function(ctx)
    local v = ctx.view
    if ctx.screen == "hub" then
      -- The tile home becomes one row of equal tiles, placed by name (ctx.ids) rather than by index.
      local rank = { recommend = 1, hots = 2, search = 3, discover = 4, dynamic = 5, inbox = 6, mine = 7, settings = 8 }
      local items = {}
      for i = 1, ctx.count do items[i] = { 20 + ((rank[ctx.ids[i]] or i) - 1) * 130, 70, 120, 120 } end
      return { items = items, axis = "x" }
    end
    return {
      items = Layout.strip(ctx.count, { x = 24, y = v.y + 38, w = 168, h = 100, gap = 14 }),
      axis = "x",
      card = "tile",
      anchor = 150,
      focus = { kind = "scale", scale = 1.2, dy = -4, dim = true },
    }
  end,
}
