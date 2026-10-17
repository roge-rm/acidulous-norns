-- The play page: the selected part's machine, patch and 5 macros.

local draw = include("lib/draw")
local text, fit, meter, header = draw.text, draw.fit, draw.meter, draw.header
local fill_rect, frame = draw.fill_rect, draw.frame
local cut, squeeze, short_value, knob = draw.cut, draw.squeeze, draw.short_value, draw.knob

local play = {}

-- The motif box, top right.
local MOTIF = {x = 84, y = 0, w = 44, h = 26}
local COL_W = 25.6

-- A look is the part of the page that can change per machine: the motif
-- and how a macro is drawn in its column. cx is the column's centre, cy
-- where the widget sits, v is 0-1.
local plain = {}

function plain.motif(x, y, w, h)
  frame(x, y, w, h, 3)
end

function plain.macro(parts, mac, v, cx, cy, sel)
  local d = mac.def
  local bipolar = d.min < 0 and d.max > 0 and d.curve ~= "step"
  knob(cx, cy, 6, v, 3, sel and 15 or 8, bipolar, bipolar and parts.unmap(d, 0) or 0)
end

-- The look for a machine.
local function look(machine)
  return plain
end

-- Sets the font to the largest size the name fits in w pixels at, or 8,
-- and returns it.
local function big_name(name, w)
  screen.font_face(1)
  for _, size in ipairs({16, 12}) do
    screen.font_size(size)
    if screen.text_extents(name) <= w then return size end
  end
  screen.font_size(8)
  return 8
end

-- The part number, then the MPE mark or keys, as the parts page marks them.
local function status(parts, state, keys, note)
  local y = 62
  local n = state.sel
  local shown = header(parts, tostring(n), note, nil, y)
  if not shown then return end
  local x = 8
  screen.level(8)
  if parts.mpe == n and state.mpe_missing then
    text(x, y, "mpe: no device")
    return
  elseif state.mpe_mark == n then
    for k, h in ipairs({2, 4, 3}) do fill_rect(x + 2 * (k - 1), y - h, 1, h, 8) end
    x = x + 9
  end
  if keys then text(x, y, "keys") end
end

-- parts, state: as for the other pages. macs: the part's macros. ps: its
-- play page state {focus}. keys: a keys port is connected.
function play.draw(parts, state, macs, ps, note, keys)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local i = state.sel
  local part = parts.list[i]
  status(parts, state, keys, note)
  if not part.machine then
    screen.level(4)
    screen.font_face(1)
    screen.font_size(8)
    text(64, 34, "no machine", "center")
    screen.update()
    return
  end
  local style = look(part.machine)
  style.motif(MOTIF.x, MOTIF.y, MOTIF.w, MOTIF.h)
  -- the machine, and the patch with its place in the list
  local name = part.machine:lower()
  local size = big_name(name, MOTIF.x - 4)
  screen.level(15)
  text(0, size == 16 and 14 or 11, fit(name, MOTIF.x - 4))
  screen.font_size(8)
  local total = #(parts.patches[part.machine] or {})
  local at = total > 0 and params:get(parts.id(i, "patch")) or 0
  local count = string.format("%d/%d", at, total)
  local cw = screen.text_extents(count)
  local on = ps.focus == 0
  if on then fill_rect(0, 17, MOTIF.x - 2, 9, 15) end
  screen.level(on and 0 or 6)
  text(1, 24, fit((part.patch or ""):lower(), MOTIF.x - 8 - cw))
  text(MOTIF.x - 4, 24, count, "right")
  -- the macros
  for k, mac in ipairs(macs) do
    local cx = math.floor((k - 0.5) * COL_W + 0.5)
    local sel = ps.focus == k
    screen.level(sel and 15 or 6)
    text(cx, 35, squeeze(mac.label, 25), "center")
    if mac.slot then
      local v = params:get(parts.id(i, "s" .. mac.slot))
      style.macro(parts, mac, v, cx, 44, sel)
      screen.level(sel and 15 or 5)
      text(cx, 56, squeeze(short_value(parts, mac.def, v), 25), "center")
    else
      screen.level(sel and 15 or 5)
      text(cx, 47, "-", "center")
    end
  end
  screen.update()
end

return play
