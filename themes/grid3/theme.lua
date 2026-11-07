-- Three columns of poster-like cards, scrolling down. The smallest useful layout theme.
return {
  name = "Three columns",
  layout = function(ctx)
    local v = ctx.view
    return {
      items = Layout.grid(ctx.count, { cols = 3, x = 10, y = v.y + 6, w = 150, h = 92, gx = 5, gy = 5 }),
      card = "tile",
      anchor = 100,
      focus = { kind = "ring" },
    }
  end,
}
