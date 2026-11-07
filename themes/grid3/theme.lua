-- Three columns of poster-like cards, scrolling down. The smallest useful layout theme.
return {
  name = "Three columns",
  shell = "sidebar",   -- keeps the left icon column; ctx.view.x/w exclude it
  layout = function(ctx)
    local v = ctx.view
    return {
      items = Layout.grid(ctx.count, { cols = 3, x = v.x + 4, y = v.y + 6, w = (v.w - 8 - 10) // 3, h = 92, gx = 5, gy = 5 }),
      card = "tile",
      anchor = 100,
      focus = { kind = "ring" },
    }
  end,
}
