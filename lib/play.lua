-- The play page: the selected part's machine, patch and 5 macros.

local draw = include("lib/draw")
local text, fit, meter, header = draw.text, draw.fit, draw.meter, draw.header
local fill_rect, frame = draw.fill_rect, draw.frame
local squeeze, fit_value, knob = draw.squeeze, draw.fit_value, draw.knob
local motifs = include("lib/motifs")

local play = {}

-- The motif box, top right.
local MOTIF = {x = 84, y = 0, w = 44, h = 26}
local COL_W = 25.6
-- Labels are at most LABEL_W wide and values VALUE_W, and narrower where
-- two neighbours would come closer than GAP px. Spaces in a label go so a
-- gap inside it isn't read as a column gap.
local LABEL_W, VALUE_W, GAP = 25, 23, 3

local machines = include("lib/machines")

-- A look is the part of the page that can change per group: the frame, the
-- machine name's font and how a macro is drawn in its column. Each macro
-- widget fits in 13 px either side of cx and from cy - 6 to cy + 6; v is
-- 0-1, lv the widget's bright level (15 focused, 8 not), zero the 0-1 place
-- of a bipolar param's centre or nil.

-- The lit span of a 0-1 value: from 0, or from the centre for a bipolar one.
local function span(v, zero)
  if zero then return math.min(zero, v), math.max(zero, v) end
  return 0, v
end

-- A rule under the patch line, with marks every `every` px, `h` tall.
local function rule(lv, every, h, mark)
  fill_rect(0, 27, 128, 1, lv)
  if every then
    for x = 0, 127, every do fill_rect(x, 28, 1, h, mark or lv) end
  end
end

local looks = {}

-- synths: a panel with screws, knobs.
looks.synths = {face = 33, sizes = {16}, y = 13}
function looks.synths.frame(m)
  frame(m.x, m.y, m.w, m.h, 4)
  for _, x in ipairs({m.x + 2, m.x + m.w - 3}) do
    fill_rect(x, m.y + 2, 1, 1, 6)
    fill_rect(x, m.y + m.h - 3, 1, 1, 6)
  end
  fill_rect(0, 27, 82, 1, 3)
end
function looks.synths.macro(v, cx, cy, lv, zero)
  knob(cx, cy, 6, v, 3, lv, zero ~= nil, zero or 0)
end

-- drums: a thick box, step marks and pads that flash on notes.
looks.drums = {face = 8, sizes = {16, 14, 12}, y = 14}
function looks.drums.frame(m)
  frame(m.x, m.y, m.w, m.h, 5)
  frame(m.x + 1, m.y + 1, m.w - 2, m.h - 2, 5)
  for k = 0, 15 do fill_rect(k * 8 + 1, 27, 6, 1, k % 4 == 0 and 5 or 2) end
end
-- flash is 0-1, from the part's energy and held notes.
function looks.drums.macro(v, cx, cy, lv, zero, flash)
  local x, y, s = cx - 6, cy - 6, 13
  local hit = flash > 0.5
  if flash > 0.1 then fill_rect(x, y, s, s, math.floor(1 + 4 * flash + 0.5)) end
  frame(x, y, s, s, math.floor(3 + 12 * flash + 0.5))
  local a, b = span(v, zero)
  local top = y + s - 1 - math.floor(b * (s - 2) + 0.5)
  local bot = y + s - 1 - math.floor(a * (s - 2) + 0.5)
  if bot > top then fill_rect(x + 2, top, s - 4, bot - top, hit and 15 or lv) end
  if zero then fill_rect(x - 2, y + s - 1 - math.floor(zero * (s - 2) + 0.5), 2, 1, 6) end
end

-- keys: a keyboard strip, drawbars pulled up from the bottom.
looks.keys = {face = 22, sizes = {14, 12}, y = 13}
local BLACK = {[1] = true, [2] = true, [4] = true, [5] = true, [6] = true}
function looks.keys.frame(m)
  frame(m.x, m.y, m.w, m.h, 3)
  fill_rect(m.x, m.y + m.h - 3, m.w, 2, 3)
  fill_rect(0, 27, 128, 1, 3)
  for k = 0, 17 do
    if BLACK[k % 7] then fill_rect(k * 7 + 5, 28, 3, 2, 3) end
  end
end
function looks.keys.macro(v, cx, cy, lv, zero)
  local x, top, bot = cx - 2, cy - 6, cy + 6
  frame(x, top, 5, bot - top + 1, 3)
  local a, b = span(v, zero)
  local h = bot - top - 1
  local y1 = bot - math.floor(b * h + 0.5)
  local y0 = bot - math.floor(a * h + 0.5)
  if y0 > y1 then fill_rect(x + 1, y1, 3, y0 - y1, lv) end
  -- the drawbar's handle at the value
  local y = bot - math.floor(v * h + 0.5)
  fill_rect(cx - 4, math.min(y, bot - 1), 9, 2, lv)
  if zero then fill_rect(cx + 4, bot - math.floor(zero * h + 0.5), 2, 1, 6) end
end

-- strings: a neck either side of the box, strings with a marker.
looks.strings = {face = 10, sizes = {16, 14, 12}, y = 13}
function looks.strings.frame(m)
  fill_rect(m.x, m.y, 1, m.h, 5)
  fill_rect(m.x + m.w - 1, m.y, 1, m.h, 5)
  for y = m.y + 6, m.y + m.h - 1, 7 do fill_rect(m.x + 1, y, m.w - 2, 1, 2) end
  fill_rect(0, 27, 82, 1, 2)
end
function looks.strings.macro(v, cx, cy, lv, zero)
  local top, bot = cy - 6, cy + 6
  fill_rect(cx - 2, top, 5, 1, 4)
  fill_rect(cx - 2, bot, 5, 1, 4)
  local h = bot - top - 2
  local a, b = span(v, zero)
  local y1 = bot - 1 - math.floor(b * h + 0.5)
  local y0 = bot - 1 - math.floor(a * h + 0.5)
  fill_rect(cx, top + 1, 1, bot - top - 1, 3)
  fill_rect(cx, y1, 1, y0 - y1 + 1, lv)
  local y = bot - 1 - math.floor(v * h + 0.5)
  fill_rect(cx - 2, y - 1, 5, 3, lv)
  if zero then fill_rect(cx - 4, bot - 1 - math.floor(zero * h + 0.5), 2, 1, 6) end
end

-- winds: a rounded box, a breath line, horizontal bars.
looks.winds = {face = 4, sizes = {16, 14, 12}, y = 14}
function looks.winds.frame(m)
  fill_rect(m.x + 2, m.y, m.w - 4, 1, 4)
  fill_rect(m.x + 2, m.y + m.h - 1, m.w - 4, 1, 4)
  fill_rect(m.x, m.y + 2, 1, m.h - 4, 4)
  fill_rect(m.x + m.w - 1, m.y + 2, 1, m.h - 4, 4)
  for _, c in ipairs({{1, 1}, {m.w - 2, 1}, {1, m.h - 2}, {m.w - 2, m.h - 2}}) do
    fill_rect(m.x + c[1], m.y + c[2], 1, 1, 4)
  end
  screen.level(3)
  for x = 0, 127, 2 do
    screen.rect(x, 28 + math.floor(math.sin(x / 6) + 0.5), 1, 1)
  end
  screen.fill()
end
function looks.winds.macro(v, cx, cy, lv, zero)
  local x, w = cx - 10, 21
  local y = cy - 2
  fill_rect(x + 1, y, w - 2, 1, 3)
  fill_rect(x + 1, y + 4, w - 2, 1, 3)
  fill_rect(x, y + 1, 1, 3, 3)
  fill_rect(x + w - 1, y + 1, 1, 3, 3)
  local a, b = span(v, zero)
  local x0 = x + 1 + math.floor(a * (w - 2) + 0.5)
  local x1 = x + 1 + math.floor(b * (w - 2) + 0.5)
  if x1 > x0 then fill_rect(x0, y + 1, x1 - x0, 3, lv) end
  -- puffs trailing off the end
  if not zero and v > 0 then
    for k = 1, 2 do
      local px = x1 + 2 * k
      if px < x + w - 1 then fill_rect(px, y + 2, 1, 1, k == 1 and 6 or 3) end
    end
  end
  if zero then fill_rect(x + 1 + math.floor(zero * (w - 2) + 0.5), y - 2, 1, 2, 6) end
end

-- nature: no box, a line of hills under it, mounds.
looks.nature = {face = 16, sizes = {14, 12}, y = 13}
function looks.nature.frame(m)
  screen.level(4)
  for x = m.x, m.x + m.w - 1 do
    local h = math.floor(1.5 + 1.5 * math.sin(x / 5) + math.sin(x / 2.3) * 0.6)
    screen.rect(x, m.y + m.h - 1 - h, 1, 1)
  end
  screen.fill()
  screen.level(2)
  for x = 0, 127, 3 do screen.rect(x, 27 + (x % 9 == 0 and 0 or 1), 1, 1) end
  screen.fill()
end
function looks.nature.macro(v, cx, cy, lv, zero)
  local base = zero and cy or cy + 5
  fill_rect(cx - 10, base, 21, 1, 3)
  local hmax = zero and 5 or 11
  local amt = zero and (v - zero) / math.max(zero, 1 - zero) or v
  local hh = math.floor(math.abs(amt) * hmax + 0.5)
  if hh == 0 then return end
  local dir = amt >= 0 and -1 or 1
  local half = 3 + math.floor(math.abs(amt) * 6 + 0.5)
  screen.level(lv)
  for dx = -half, half do
    local f = 1 - (dx / (half + 0.5)) ^ 2
    local h = math.max(1, math.floor(hh * f + 0.5))
    screen.rect(cx + dx, dir < 0 and base - h or base + 1, 1, h)
  end
  screen.fill()
end

-- samples: brackets round the box, a timeline, waveform bars.
looks.samples = {face = 49, sizes = {16}, y = 13}
local WAVE = {2, 4, 6, 3, 5, 6, 4, 2, 5, 3, 1}
function looks.samples.frame(m)
  for _, x in ipairs({m.x, m.x + m.w - 1}) do fill_rect(x, m.y, 1, m.h, 5) end
  for _, x in ipairs({m.x + 1, m.x + m.w - 3}) do
    fill_rect(x, m.y, 2, 1, 5)
    fill_rect(x, m.y + m.h - 1, 2, 1, 5)
  end
  fill_rect(0, 27, 128, 1, 2)
  for x = 0, 127, 8 do fill_rect(x, 28, 1, x % 32 == 0 and 2 or 1, 4) end
end
function looks.samples.macro(v, cx, cy, lv, zero)
  local n = #WAVE
  local x0 = cx - (n - 1)
  local a, b = span(v, zero)
  for k = 1, n do
    local f = (k - 0.5) / n
    local h = WAVE[k]
    fill_rect(x0 + 2 * (k - 1), cy - h, 1, 2 * h + 1, (f >= a and f <= b) and lv or 2)
  end
  local px = x0 + math.floor(v * (2 * n - 2) + 0.5)
  if zero then fill_rect(x0 + math.floor(zero * (2 * n - 2) + 0.5), cy + 7, 1, 1, 6) end
  fill_rect(px, cy - 7, 1, 1, lv)
end

-- beyond: dotted box and rule, rings of dots.
looks.beyond = {face = 19, sizes = {14, 12}, y = 13}
function looks.beyond.frame(m)
  screen.level(5)
  for x = m.x, m.x + m.w - 1, 2 do
    screen.rect(x, m.y, 1, 1)
    screen.rect(x, m.y + m.h - 1, 1, 1)
  end
  for y = m.y, m.y + m.h - 1, 2 do
    screen.rect(m.x, y, 1, 1)
    screen.rect(m.x + m.w - 1, y, 1, 1)
  end
  screen.fill()
  screen.level(3)
  for x = 0, 127, 4 do screen.rect(x, 27, 1, 1) end
  screen.fill()
end
function looks.beyond.macro(v, cx, cy, lv, zero)
  local n = 12
  local a, b = span(v, zero)
  for k = 0, n - 1 do
    local t = math.rad(k * 360 / n)
    local sx, sy = math.sin(t), -math.cos(t)
    local x, y = math.floor(cx + 6 * sx + 0.5), math.floor(cy + 6 * sy + 0.5)
    -- dot k covers the value from k/n, wrapped so a bipolar centre sits at the top
    local f = k / n
    if zero then f = (f + zero) % 1 end
    local on = (zero or v > 0.01) and f >= a - 0.001 and f <= b + 0.001
    if zero and v == zero then on = false end
    -- a lit dot is a short dash pointing out from the centre
    fill_rect(x, y, 1, 1, on and lv or 3)
    if on then fill_rect(math.floor(cx + 4.6 * sx + 0.5), math.floor(cy + 4.6 * sy + 0.5), 1, 1, lv) end
  end
  -- the centre dot, lit past half
  fill_rect(cx, cy, 1, 1, v > 0.5 and lv or 3)
end

-- Machine -> look.
local by_machine = {}
for _, g in ipairs(machines.groups) do
  for _, name in ipairs(g.machines) do by_machine[name] = looks[g.name] end
end

local function look(machine)
  return by_machine[machine] or looks.synths
end

-- Strings for the columns centred at xs, each at most cap wide and narrow
-- enough that neighbours keep GAP px apart; get(k, w) gives column k's
-- string in w px.
local function columns(xs, cap, get)
  local out, ws = {}, {}
  for k = 1, #xs do
    out[k] = get(k, cap)
    ws[k] = screen.text_extents(out[k])
  end
  for k = 1, #xs - 1 do
    local room = 2 * (xs[k + 1] - xs[k] - GAP)
    if ws[k] + ws[k + 1] > room then
      -- the wider one gives way first, then the other if still needed
      local a = ws[k] >= ws[k + 1] and k or k + 1
      local b = a == k and k + 1 or k
      out[a] = get(a, room - ws[b])
      ws[a] = screen.text_extents(out[a])
      if ws[a] + ws[b] > room then
        out[b] = get(b, room - ws[a])
        ws[b] = screen.text_extents(out[b])
      end
    end
  end
  return out
end

-- Sets the style's font at the largest size the name fits in w pixels at,
-- or the plain font at 8, and returns the size.
local function big_name(style, name, w)
  screen.font_face(style.face)
  for _, size in ipairs(style.sizes) do
    screen.font_size(size)
    if screen.text_extents(name) <= w then return size end
  end
  screen.font_face(1)
  screen.font_size(8)
  return 8
end

-- Load as 4 pips in the bottom right, only when busy: they light at 50,
-- 65, 80 and 90%, brighter as more light, and all blink while header()
-- shows the overload warning.
local PIPS = {50, 65, 80, 90}
local function pips(parts)
  local now = util.time()
  local over = now - parts.worst_at < 2
  if parts.load < PIPS[1] and not over then return end
  local lit = 0
  for k, at in ipairs(PIPS) do if parts.load >= at then lit = k end end
  if over then lit = math.floor(now * 4) % 2 == 0 and 4 or 0 end
  for k = 1, 4 do
    fill_rect(116 + 3 * (k - 1), 59, 2, 4, k <= lit and (over and 15 or 6 + 3 * lit) or 2)
  end
end

-- The part number, then the MPE mark or keys, as the parts page marks them.
local function status(parts, state, keys, note)
  local y = 62
  local n = state.sel
  pips(parts)
  local shown = header(parts, tostring(n), note, "", y)
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
  style.frame(MOTIF)
  local motif = motifs[part.machine]
  if motif then
    -- inside the frame, cleared of the look's own marks
    local x, y, w, h = MOTIF.x + 2, MOTIF.y + 2, MOTIF.w - 4, MOTIF.h - 4
    fill_rect(x, y, w, h, 0)
    motif(x, y, w, h, util.time() % 1000, part.energy or 0, part.held)
  end
  -- the machine, and the patch with its place in the list
  local name = part.machine:lower()
  local size = big_name(style, name, MOTIF.x - 6)
  screen.level(15)
  text(2, size == 8 and 11 or style.y, fit(name, MOTIF.x - 6))
  screen.font_face(1)
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
  local flash = math.max(part.energy or 0, part.held > 0 and 0.6 or 0)
  local xs, vs = {}, {}
  for k, mac in ipairs(macs) do
    xs[k] = math.floor((k - 0.5) * COL_W + 0.5)
    vs[k] = mac.slot and params:get(parts.id(i, "s" .. mac.slot)) or nil
  end
  local labels = columns(xs, LABEL_W, function(k, w)
    return squeeze((macs[k].label:gsub(" ", "")), w)
  end)
  local values = columns(xs, VALUE_W, function(k, w)
    return vs[k] and fit_value(parts, macs[k].def, vs[k], w) or ""
  end)
  for k, mac in ipairs(macs) do
    local cx = xs[k]
    local sel = ps.focus == k
    screen.level(sel and 15 or 6)
    text(cx, 35, labels[k], "center")
    if mac.slot then
      local v = vs[k]
      local d = mac.def
      local zero = d.min < 0 and d.max > 0 and d.curve ~= "step" and parts.unmap(d, 0) or nil
      style.macro(v, cx, 44, part.dim and 2 or sel and 15 or 8, zero, flash)
      screen.level(part.dim and 2 or sel and 15 or 5)
      text(cx, 56, values[k], "center")
    else
      screen.level(sel and 15 or 5)
      text(cx, 47, "-", "center")
    end
  end
  screen.update()
end

return play
