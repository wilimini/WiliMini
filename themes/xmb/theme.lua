-- A PSP "cross media bar": a row of category icons across the top, the focused category's items in
-- a column under it, the focused item big and the rest fading away. Meant for the classic home style
-- (it replaces the page header and has no tile home). Left/Right change category, Up/Down move in it.
-- Settings, Feed and Inbox are categories too: focus them and press confirm to open the page.
return {
  name = "Cross media bar",
  shell = "bar", pages = "classic", dark = true,   -- the theme draws its own category bar
  colors = {
    background = 0x14308a, panel = 0x1a3f9e, raised = 0x24509e,
    text = 0xffffff, dim = 0xb4c8ec, line = 0x3a64b8,
    accent = 0xffffff, ring = 0xffffff,
  },
  background = { top = 0x0b1445, bottom = 0x2b74c8, waves = true },
  motion = { kind = "ease", ms = 200 },

  layout = function(ctx)
    if ctx.screen == "hub" then return nil end
    local ROW, PITCH = 48, 52
    return {
      items = Layout.list(ctx.count, { x = 74, y = 124, w = 380, h = ROW, gap = PITCH - ROW }),
      tabs = true,          -- Left/Right switch category; the host draws the category bar
      card = "xmb",
      anchor = 124,         -- the focused item rests at this row
      clip_top = 100,        -- just under the bar
      dim_step = 60,        -- items fade with distance from the focus
      focus = { kind = "none" },
    }
  end,

  on = {
    focus = function(ev) return { pulse = true } end,
    open = function(ev) return { flash = 0xffffff, flash_ms = 180 } end,
  },
}
