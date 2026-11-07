-- One row of large cards scrolling sideways; the focused card grows and the others dim.
return {
  name = "Strip",
  layout = function(ctx)
    local v = ctx.view
    return {
      items = Layout.strip(ctx.count, { x = 24, y = v.y + 38, w = 168, h = 100, gap = 14 }),
      axis = "x",
      card = "tile",
      anchor = 150,
      focus = { kind = "scale", scale = 1.2, dy = -4, dim = true },
    }
  end,
}
