-- A PSP "cross media bar" feel: a bar of big tiles on the home screen, and on every list the
-- focused row steps out to the right while the others dim. Deep blue palette, eased scrolling.
local ORDER = { recommend = 1, hots = 2, search = 3, discover = 4, dynamic = 5, inbox = 6, mine = 7, settings = 8 }

return {
  name = "Cross media bar",
  colors = {
    background = 0x0c1f4a, panel = 0x14316b, raised = 0x1c3d82,
    text = 0xf2f6ff, dim = 0x93aedd, line = 0x2c4f9c,
    accent = 0x7fd3ff, ring = 0xffffff,
  },
  motion = { kind = "ease", ms = 200 },

  layout = function(ctx)
    local v = ctx.view
    if ctx.screen == "hub" then
      local items = {}
      for i = 1, ctx.count do
        items[i] = { 24 + ((ORDER[ctx.ids[i]] or i) - 1) * 108, 86, 88, 88 }
      end
      return { items = items, axis = "x", focus = { kind = "ring" } }
    end
    -- feed, search results, history and uploader lists
    return {
      items = Layout.list(ctx.count, { x = 18, y = v.y + 6, w = 420, h = 42, gap = 6 }),
      card = "row",
      anchor = 84,
      focus = { kind = "scale", scale = 1.04, dx = 14, dim = true },
    }
  end,

  on = {
    focus = function(ev) return { pulse = true } end,
    open = function(ev) return { flash = 0xffffff, flash_ms = 200 } end,
  },
}
