-- The norns screen: the parts list and a part's cards, and the
-- OMX-27's 128x32 screen.

local view = {}

local draw = include("lib/draw")
local text, fit, meter, header = draw.text, draw.fit, draw.meter, draw.header
local fill_rect, frame = draw.fill_rect, draw.frame

-- The part's machine in lowercase, or its short name if that's too wide.
local function machine_name(parts, part, w)
  local name = part.machine:lower()
  if screen.text_extents(name) <= w then return name end
  local m = parts.machine_info(part.machine)
  return (m and m.short or name):lower()
end

local function title(parts, i)
  local part = parts.list[i]
  if not part.machine then return nil end
  local files = parts.material(i)
  return part.n .. " " .. part.machine:lower() .. "  " .. (files or part.patch or ""):lower()
end

-- The 8 parts a page shows: the first, 1 or 9, and "1-8" or "9-16" for
-- the header, or nil with 8 parts.
local function shown(parts, state)
  local first = state.sel > 8 and 9 or 1
  if parts.active <= 8 then return first, nil end
  return first, first == 1 and "1-8" or "9-16"
end

-- The header's right side: the 8 shown, then `right` or the load.
local function with_bank(parts, bank, right)
  if not bank then return right end
  return bank .. "  " .. (right or string.format("%d%%", math.floor(parts.load + 0.5)))
end

-- state: sel (part), k1 (held). note: a line for the header, or nil
-- A row per part: a 2 px bar, bright while the part sounds, its number,
-- machine and patch, and its level on a dim track.
function view.main(parts, state, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local first, bank = shown(parts, state)
  header(parts, title(parts, state.sel), note, with_bank(parts, bank))
  -- two-digit numbers push the machine right
  local mx, mw = first > 1 and 13 or 10, first > 1 and 28 or 31
  for k = 1, 8 do
    local i = first + k - 1
    local part = parts.list[i]
    local y = 13 + 7 * (k - 1)
    local on = i == state.sel
    if on then fill_rect(0, y - 6, 128, 7, 3) end
    local b = part.machine and (on and 15 or 7) or (on and 8 or 2)
    local sounding = part.machine and (part.held > 0 or meter(part.level) > 0.1)
    fill_rect(0, y - 5, 2, 5, sounding and 15 or (part.machine and 6 or 1))
    screen.level(b)
    text(first > 1 and 3 or 4, y, tostring(i))
    if part.machine then
      if on and state.k1 then screen.level(15) end
      text(mx, y, machine_name(parts, part, mw))
      screen.level(on and state.k1 and 6 or math.max(1, b - 3))
      if parts.mpe == i and state.mpe_missing then
        text(44, y, "mpe: no device")
      elseif state.mpe_mark == i then
        text(44, y, fit((part.patch or ""):lower(), 60))
        -- the mpe mark: three bars of different heights, one per finger
        for k, h in ipairs({2, 4, 3}) do fill_rect(105 + 2 * k, y - h, 1, h, b) end
      else
        text(44, y, fit((part.patch or ""):lower(), 68))
      end
    else
      text(mx, y, "-")
    end
    fill_rect(114, y - 4, 12, 3, 1)
    local w = math.floor(meter(part.level) * 12 + 0.5)
    if w > 0 then fill_rect(114, y - 4, w, 3, on and 15 or 8) end
  end
  screen.update()
end

-- ---- the card page ------------------------------------------------------------

local cut, squeeze, short_value, knob = draw.cut, draw.squeeze, draw.short_value, draw.knob

local KNOB_W = 24
local CARD_W = 116

-- How wide a control is on the card.
local function control_w(ctl)
  if (ctl.kind == "switch" and ctl.opts and #ctl.opts <= 3) or ctl.kind == "onoff" then
    local w = 0
    for _, o in ipairs(ctl.kind == "onoff" and {ctl.label} or ctl.opts) do
      w = math.max(w, screen.text_extents(o))
    end
    return math.max(13, w + 4) + 4
  end
  return KNOB_W
end

-- A switch of up to 3 positions as the app's split square, or an on/off
-- square lit when on.
local function draw_switch(parts, ctl, x, w, v, sel)
  local fw = w - 4
  local fx = x + 2
  frame(fx, 28, fw, 30, sel and 12 or 4)
  if ctl.kind == "onoff" then
    local lit = v >= 0.5
    if lit then fill_rect(fx + 1, 29, fw - 2, 28, sel and 15 or 10) end
    screen.level(lit and 0 or (sel and 8 or 5))
    text(fx + fw // 2, 45, ctl.label, "center")
    return
  end
  local n = #ctl.opts
  local idx = math.floor(v * (n - 1) + 0.5) + 1
  local hc = 28 // n
  for k, o in ipairs(ctl.opts) do
    local top = 29 + (k - 1) * hc
    if k == idx then fill_rect(fx + 1, top, fw - 2, hc, sel and 15 or 10) end
    screen.level(k == idx and 0 or (sel and 8 or 5))
    text(fx + fw // 2, top + (hc - 5) // 2 + 5, o, "center")
  end
end

-- ps: the part's card state {card, knob, first, tab_first, pad}.
-- cards: from panel.cards. pads: from panel.pads, or nil.
function view.card(parts, state, cards, ps, pads, panel, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  header(parts, title(parts, state.sel), note)
  local part = parts.list[state.sel]
  if not part.machine or #cards == 0 then
    screen.level(4)
    text(64, 36, part.machine and "no controls" or "no machine", "center")
    screen.update()
    return
  end
  -- the card tabs, scrolled so the current one shows
  local widths = {}
  for k, c in ipairs(cards) do widths[k] = screen.text_extents(c.title) end
  if ps.card < ps.tab_first then ps.tab_first = ps.card end
  local function span(a, b)
    local w = 0
    for k = a, b do w = w + widths[k] + (k > a and 6 or 0) end
    return w
  end
  while ps.tab_first < ps.card and span(ps.tab_first, ps.card) > 128 do ps.tab_first = ps.tab_first + 1 end
  local x = 0
  for k = ps.tab_first, #cards do
    if x >= 128 then break end
    local on = k == ps.card
    screen.level(on and 15 or 3)
    text(x, 13, cards[k].title)
    if on then fill_rect(x, 15, widths[k], 1, 15) end
    x = x + widths[k] + 6
  end
  -- the card
  local card = cards[ps.card]
  frame(0, 18, CARD_W, 46, 4)
  fill_rect(0, 18, CARD_W, 7, 4)
  screen.level(0)
  text(3, 24, card.title)
  local list = card.controls
  if ps.knob < ps.first then ps.first = ps.knob end
  local function cw(a, b)
    local w = 0
    for k = a, b do w = w + control_w(list[k]) end
    return w
  end
  while ps.first < ps.knob and cw(ps.first, ps.knob) > CARD_W - 3 do ps.first = ps.first + 1 end
  x = 2
  for k = ps.first, #list do
    local ctl = list[k]
    local w = control_w(ctl)
    if x + w > CARD_W - 1 then break end
    local sel = k == ps.knob
    if ctl.kind == "pad" then
      local n = pads and #pads or 1
      local at = 1
      for j, p in ipairs(pads or {}) do if p == ps.pad then at = j end end
      local cx = x + 12
      screen.level(sel and 15 or 6)
      text(cx, 32, "pad", "center")
      knob(cx, 43, 7, n > 1 and (at - 1) / (n - 1) or 0, 3, sel and 15 or 8)
      screen.level(sel and 15 or 5)
      text(cx, 60, tostring(at), "center")
    else
      local v
      if ctl.mod then
        v = parts.mod_value(state.sel, ctl)
      elseif ctl.fx then
        v = parts.fx_value(state.sel, ctl)
      else
        local p = params:lookup_param(parts.id(state.sel, "s" .. ctl.slot))
        v = p and p:get() or 0
      end
      if w ~= KNOB_W then
        draw_switch(parts, ctl, x, w, v, sel)
      else
        local cx = x + 12
        local d = ctl.def
        screen.level(sel and 15 or 6)
        text(cx, 32, ctl.cut and cut(ctl.label, 24) or squeeze(ctl.label, 24), "center")
        local bipolar = d.min < 0 and d.max > 0 and d.curve ~= "step"
        local dim = parts.list[state.sel].dim and not ctl.fx and not ctl.mod
        knob(cx, 43, 7, v, 3, dim and 2 or sel and 15 or 8, bipolar, bipolar and parts.unmap(d, 0) or 0)
        screen.level(dim and 2 or sel and 15 or 5)
        text(cx, 60, squeeze(panel.option(ctl, v) or short_value(parts, d, v), 26), "center")
      end
    end
    x = x + w
  end
  -- the next card's edge
  if ps.card < #cards then
    frame(120, 18, 12, 46, 2)
    fill_rect(120, 18, 8, 7, 2)
  end
  screen.update()
end

-- ---- the machine picker and patch list ------------------------------------------

-- 7x7 icons for the group tabs, after the app's GroupIcons.kt.
local ICONS = {
  synths = {"0000000", "0110000", "1001000", "1000100", "0000100", "0000011", "0000000"},
  drums = {"0111110", "1000001", "0111110", "0100010", "0100010", "0011100", "0000000"},
  keys = {"1111111", "1010101", "1010101", "1010101", "1111111", "1111111", "0000000"},
  strings = {"0100010", "0100010", "0100010", "0100010", "0100010", "0100010", "0100010"},
  winds = {"0000000", "1111110", "1000011", "1111110", "0000000", "0000000", "0000000"},
  nature = {"0001000", "0011100", "0111110", "1111111", "0001000", "0001000", "0000000"},
  samples = {"0001000", "0101000", "0101010", "1111111", "0101010", "0101000", "0001000"},
  beyond = {"0011100", "0100010", "1001001", "1011101", "1001001", "0100010", "0011100"},
}

local function icon(x, y, name, lv)
  local rows = ICONS[name]
  if not rows then return end
  for r, row in ipairs(rows) do
    for c = 1, #row do
      if row:sub(c, c) == "1" then fill_rect(x + c - 1, y + r - 1, 1, 1, lv) end
    end
  end
end

-- A scrollbar on a track at x, from y for h pixels.
local function scrollbar(x, y, h, top, shown, count)
  fill_rect(x + 1, y, 1, h, 2)
  local bar = count > shown and math.max(3, math.floor(h * shown / count + 0.5)) or h
  local at = count > shown and math.floor((h - bar) * (top - 1) / (count - shown) + 0.5) or 0
  fill_rect(x, y + at, 3, bar, 9)
end

-- s: from lists.screen. Group tabs as icons, then a row per machine with
-- its short line; the part's machine is brighter.
function view.machines(parts, s)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local tab = s.tabs[s.tab]
  screen.level(4)
  text(127, 5, tab.name, "right")
  screen.level(8)
  text(0, 5, fit("machine for part " .. s.part, 126 - screen.text_extents(tab.name) - 4))
  for k, t in ipairs(s.tabs) do
    local x = 1 + (k - 1) * 16
    local on = k == s.tab
    if on then fill_rect(x - 1, 8, 15, 11, 4) end
    icon(x + 3, 10, t.icon, on and 15 or 5)
  end
  fill_rect(0, 20, 128, 1, 2)
  local shown = 5
  local scrolls = #s.rows > shown
  local right = scrolls and 123 or 128
  local col = 44
  for _, row in ipairs(s.rows) do col = math.max(col, screen.text_extents(row.name:lower()) + 6) end
  for k = s.top, math.min(#s.rows, s.top + shown - 1) do
    local row = s.rows[k]
    local y = 28 + (k - s.top) * 8
    local on = k == s.cursor
    if on then fill_rect(0, y - 6, right, 8, 3) end
    screen.level(on and 15 or (row.name == s.current and 12 or 7))
    text(2, y, row.name:lower())
    screen.level(on and 8 or 3)
    text(col, y, fit(row.about, right - col - 1))
  end
  if scrolls then scrollbar(124, 22, 41, s.top, shown, #s.rows) end
  screen.update()
end

-- s: from lists.screen. Family tabs, scrolled so the current one shows, then
-- a row per patch with "k3" on the cursor's; the part's patch is brighter.
function view.patches(parts, s)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  screen.level(4)
  text(127, 5, s.count, "right")
  local w_count = screen.text_extents(s.count) + 4
  screen.level(8)
  text(0, 5, fit(s.part .. " " .. s.machine:lower() .. "  patch", 127 - w_count))
  -- the tabs from first, up to 120 px, with < and > where more are
  local widths = {}
  for k, t in ipairs(s.tabs) do widths[k] = screen.text_extents(t.name) end
  local function span(a, b)
    local w = a > 1 and 6 or 0
    for k = a, b do w = w + widths[k] + 5 end
    return w
  end
  local first = 1
  while first < s.tab and span(first, s.tab) > 120 do first = first + 1 end
  local x = 0
  if first > 1 then
    screen.level(8)
    text(0, 13, "<")
    x = 6
  end
  for k = first, #s.tabs do
    local w = widths[k]
    if x + w > 120 then
      screen.level(8)
      text(127, 13, ">", "right")
      break
    end
    if k == s.tab then
      fill_rect(x, 7, w + 3, 8, 15)
      screen.level(0)
    else
      screen.level(5)
    end
    text(x + 1, 13, s.tabs[k].name)
    x = x + w + 5
  end
  fill_rect(0, 15, 128, 1, 15)
  if s.confirm then
    screen.level(15)
    text(64, 34, fit(s.confirm:lower(), 124), "center")
    screen.level(6)
    text(64, 50, "k2 no   k3 yes", "center")
    screen.update()
    return
  end
  local shown = 6
  for k = s.top, math.min(#s.rows, s.top + shown - 1) do
    local row = s.rows[k]
    local y = 23 + (k - s.top) * 7
    local on = k == s.cursor
    if on then fill_rect(0, y - 6, 120, 7, 3) end
    screen.level(on and 15 or (row.current and 12 or 7))
    text(2, y, fit(row.name:lower(), on and 90 or 116))
    if on then
      screen.level(6)
      text(118, y, s.hint or "k3", "right")
    end
  end
  scrollbar(124, 17, 46, s.top, shown, #s.rows)
  screen.update()
end

-- ---- the mixer, sends and scope ------------------------------------------------

-- The fader's place for a volume in dB, 0-1.
local function fader(db) return util.clamp((db + 60) / 63, 0, 1) end

-- The strips' columns: 8 parts and the out strip, the selected one lit.
-- out: the out strip is selected.
local function strip_frame(parts, state, out)
  local first = shown(parts, state)
  for k = 1, 8 do
    local i = first + k - 1
    local x = 1 + (k - 1) * 13
    local sel = not out and i == state.sel
    if sel then fill_rect(x - 1, 7, 13, 57, 3) end
    screen.level(sel and 15 or (parts.list[i].machine and 7 or 2))
    text(x + 5, 13, tostring(i), "center")
  end
  if out then fill_rect(105, 7, 23, 57, 3) end
  fill_rect(104, 7, 1, 57, 2)
end

-- The header's left side: the part and what the page sets, or "out". The
-- machine's name goes if it doesn't fit in w.
local function strip_title(parts, state, what, w)
  local tail = what and "  " .. what or ""
  if state.out then return "out" .. tail end
  local part = parts.list[state.sel]
  if not part.machine then return part.n .. " -" .. tail end
  local t = part.n .. " " .. part.machine:lower() .. tail
  if not w or screen.text_extents(t) <= w then return t end
  local m = parts.machine_info(part.machine)
  t = part.n .. " " .. (m and m.short or part.machine):lower() .. tail
  if screen.text_extents(t) <= w then return t end
  return part.n .. tail
end

local function db_text(db) return string.format("%.1f db", db) end

-- Each part's meter and fader, pan below, and m or s when it's muted or
-- soloed. The out strip has the master's meter and fader, and lim lit while
-- the limiter is at its ceiling.
function view.mixer(parts, state, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local i = state.sel
  local first, bank = shown(parts, state)
  local lufs = parts.lufs and parts.lufs > -60 and string.format("%d lufs", math.floor(parts.lufs + 0.5)) or ""
  local right = bank and lufs ~= "" and (bank .. "  " .. lufs) or bank or lufs
  local w = 123 - screen.text_extents(right)
  local left
  if state.out then
    left = strip_title(parts, state, db_text(parts.master_db()), w)
  else
    left = strip_title(parts, state, db_text(params:get(parts.id(i, "volume"))) .. "  " ..
      parts.pan_text(params:get(parts.id(i, "pan"))), w)
  end
  header(parts, left, note, right)
  strip_frame(parts, state, state.out)
  local top, bot = 15, 54
  local h = bot - top
  for c = 1, 8 do
    local k = first + c - 1
    local x = 1 + (c - 1) * 13
    local sel = not state.out and k == i
    local part = parts.list[k]
    fill_rect(x + 1, top, 2, h, 1)
    local m = math.floor(meter(part.level) * h + 0.5)
    if m > 0 then fill_rect(x + 1, bot - m, 2, m, sel and 15 or 9) end
    fill_rect(x + 7, top, 1, h, 4)
    local fy = bot - math.floor(fader(params:get(parts.id(k, "volume"))) * h + 0.5)
    fill_rect(x + 5, fy - 1, 5, 3, sel and 15 or 9)
    fill_rect(x + 2, 57, 9, 1, 2)
    local pan = x + 6 + math.floor(params:get(parts.id(k, "pan")) * 4 + 0.5)
    fill_rect(pan, 56, 1, 2, sel and 15 or 9)
    screen.level(15)
    -- s reaches the baseline, so 63 keeps it on screen. Placed on a whole
    -- pixel, as centring an even width blurs it.
    local mark = params:get(parts.id(k, "mute")) == 2 and "m" or (params:get(parts.id(k, "solo")) == 2 and "s")
    if mark then text(x + 6 - screen.text_extents(mark) // 2, 63, mark) end
  end
  -- out
  local x = 106
  screen.level(state.out and 15 or 10)
  text(116, 13, "out", "center")
  fill_rect(x + 1, top, 7, h, 1)
  local m = math.floor(meter(parts.master_level or 0) * h + 0.5)
  if m > 0 then fill_rect(x + 1, bot - m, 7, m, 12) end
  fill_rect(x + 14, top, 1, h, 4)
  local fy = bot - math.floor(fader(parts.master_db()) * h + 0.5)
  fill_rect(x + 12, fy - 1, 5, 3, state.out and 15 or 12)
  screen.level((parts.master_peak or 0) >= 0.93 and 15 or 4)
  text(116, 62, "lim", "center")
  screen.update()
end

-- A rev and a dly knob per part, and the reverb's size and the delay's
-- feedback on the out strip.
function view.sends(parts, state, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local i = state.sel
  local right
  if state.out then
    right = string.format("size %d  fdbk %d", math.floor(params:get("rev_size") * 100 + 0.5),
      math.floor(params:get("dly_feedback") * 100 + 0.5))
  else
    right = string.format("rev %d  dly %d", math.floor(params:get(parts.id(i, "rev")) * 100 + 0.5),
      math.floor(params:get(parts.id(i, "dly")) * 100 + 0.5))
  end
  local first, bank = shown(parts, state)
  right = with_bank(parts, bank, right)
  header(parts, strip_title(parts, state, nil, 123 - screen.text_extents(right)), note, right)
  strip_frame(parts, state, state.out)
  for c = 1, 8 do
    local k = first + c - 1
    local x = 1 + (c - 1) * 13
    local sel = not state.out and k == i
    knob(x + 5, 28, 5, params:get(parts.id(k, "rev")), 3, sel and 15 or 8)
    knob(x + 5, 50, 5, params:get(parts.id(k, "dly")), 3, sel and 15 or 8)
  end
  screen.level(state.out and 15 or 6)
  text(116, 21, "rev", "center")
  knob(116, 30, 6, parts.send_param_01("rev_size"), 3, state.out and 15 or 10)
  screen.level(state.out and 15 or 6)
  text(116, 46, "dly", "center")
  knob(116, 55, 6, parts.send_param_01("dly_feedback"), 3, state.out and 15 or 10)
  screen.update()
end

-- sc: {part, mods = {{slot, type}...}, at, trace}. The patch's modules as
-- boxes in a chain, the scope's module framed, and its output below.
function view.scope(parts, sc)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local part = parts.list[sc.part]
  header(parts, part.n .. " " .. (part.machine or "-"):lower() .. "  " .. (part.patch or ""):lower(), nil, "scope")
  local mods = sc.mods or {}
  local widths = {}
  for k, m in ipairs(mods) do widths[k] = screen.text_extents(m.type) + 4 end
  -- from the first box that lets the framed one show
  local first = 1
  local function span(a, b)
    local w = 0
    for k = a, b do w = w + widths[k] + (k < b and 4 or 0) end
    return w
  end
  while sc.at and first < sc.at and span(first, sc.at) > 128 do first = first + 1 end
  local x = 0
  for k = first, #mods do
    if x >= 128 then break end
    local on = k == sc.at
    local w = widths[k]
    frame(x, 9, w, 9, on and 15 or 4)
    screen.level(on and 15 or 6)
    text(x + 2, 15, mods[k].type)
    if k < #mods then fill_rect(x + w, 13, 4, 1, 4) end
    x = x + w + 4
  end
  if #mods == 0 then
    screen.level(4)
    text(0, 15, sc.mods and "no modules" or "")
  end
  frame(0, 22, 128, 42, 2)
  fill_rect(1, 43, 126, 1, 2)
  local trace = sc.trace
  if trace and #trace > 1 then
    screen.level(15)
    local prev
    for k = 1, math.min(126, #trace) do
      local y = util.clamp(math.floor(43 - trace[k] * 19 + 0.5), 23, 62)
      local a, b = y, y
      if prev then a, b = math.min(prev, y), math.max(prev, y) end
      screen.rect(k, a, 1, b - a + 1)
      screen.fill()
      prev = y
    end
  end
  if sc.at and mods[sc.at] then
    screen.level(5)
    text(126, 28, mods[sc.at].type .. " out", "right")
  end
  screen.update()
end

-- The OMX-27's screen, at the top left of the norns screen, without
-- screen.update: pixels at level 8 or more are lit on the OMX.
-- Part, machine and octave on top and the patch below, or for a second after
-- a pot is turned, its macro: label, readout and a bar. While the pot is
-- waiting to pick up the value, a tick shows where the pot is. A modifier
-- change shows instead for a moment, or while AUX + key 3 or 4 is held.
-- list: from macros.resolve. omx: the surface (octave, showing(),
-- message(), mod_readout()).
function view.omx(parts, state, list, omx)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  screen.level(15)
  local part = parts.list[state.sel]
  local top, big = omx.message()
  if top then
    text(0, 7, top)
    screen.font_size(16)
    if screen.text_extents(big) > 127 then screen.font_size(8) end
    text(0, 28, fit(big, 127))
    screen.font_size(8)
    return
  end
  local f = omx.showing()
  local label, v, readout
  if f and f.mod then
    label, v, readout = omx.mod_readout(f)
  else
    local mac = f and list[f.k]
    local p = mac and mac.slot and params:lookup_param(parts.id(state.sel, "s" .. mac.slot))
    if p then label, v, readout = mac.label, p:get(), parts.readout(mac.def, p:get()) end
  end
  if label then
    text(0, 7, label)
    text(127, 7, fit(readout, 80), "right")
    screen.rect(0.5, 12.5, 127, 9)
    screen.stroke()
    local w = math.floor(v * 125 + 0.5)
    if w > 0 then
      screen.rect(1, 13, w, 8)
      screen.fill()
    end
    if f.waiting then
      -- the pot's place, as a gap through the bar with marks above and below
      local x = 1 + math.floor(f.pos * 124 + 0.5)
      screen.level(0)
      screen.rect(x - 1, 13, 3, 8)
      screen.fill()
      screen.level(15)
      screen.rect(x, 10, 1, 14)
      screen.fill()
      text(64, 31, f.pos < v and "turn up to pick up" or "turn down to pick up", "center")
    end
    return
  end
  local oct = omx.octave
  text(0, 7, part.n .. " " .. (part.machine or "none"))
  text(127, 7, (part.channel > 0 and ("ch " .. part.channel) or "ch off") .. "  oct " .. (oct > 0 and "+" or "") .. oct, "right")
  screen.font_size(16)
  text(0, 28, fit(part.patch or "-", 127))
  screen.font_size(8)
end

-- s cut from the front to fit in w pixels, for paths.
local function fit_end(s, w)
  s = s or ""
  if screen.text_extents(s) <= w then return s end
  while #s > 1 and screen.text_extents(".." .. s) > w do s = s:sub(2) end
  return ".." .. s
end

-- The file picker. pick: from picker.screen(), {title, right, rows,
-- cursor, busy, short, info, cols, playhead}. With short, 4 rows and the
-- selected file's shape below: its loudest point per column, mirrored about
-- a centre line, brighter where the playhead has been.
function view.picker(pick)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local right_w = 0
  if pick.right then
    screen.level(4)
    text(127, 5, pick.right, "right")
    right_w = screen.text_extents(pick.right) + 4
  end
  screen.level(8)
  text(0, 5, fit_end(pick.title:lower(), 127 - right_w))
  if pick.busy then
    screen.level(15)
    text(64, 36, pick.busy:lower(), "center")
    screen.update()
    return
  end
  local rows = pick.rows
  local shown = pick.short and 4 or 8
  local top = util.clamp(pick.cursor - math.floor(shown / 2), 1, math.max(1, #rows - shown + 1))
  local scrolls = #rows > shown
  for k = top, math.min(#rows, top + shown - 1) do
    local y = 13 + 7 * (k - top)
    local on = k == pick.cursor
    if on then
      screen.level(3)
      screen.rect(0, y - 6, scrolls and 125 or 128, 7)
      screen.fill()
    end
    screen.level(on and 15 or 6)
    text(2, y, fit(rows[k]:lower(), scrolls and 121 or 124))
  end
  if scrolls then
    -- every list that scrolls shows where it is
    local h = 7 * shown
    local bar = math.max(3, math.floor(h * shown / #rows + 0.5))
    local y = 7 + math.floor((h - bar) * (top - 1) / (#rows - shown) + 0.5)
    screen.level(1)
    screen.rect(127, 7, 1, h)
    screen.fill()
    screen.level(6)
    screen.rect(127, y, 1, bar)
    screen.fill()
  end
  if pick.short then
    if pick.info then
      screen.level(4)
      text(127, 38, pick.info:lower(), "right")
    end
    if pick.cols then
      local mid = 52
      screen.level(2)
      screen.rect(0, mid, 128, 1)
      screen.fill()
      local head = pick.playhead and math.floor(pick.playhead * 127 + 0.5)
      for x = 0, 127 do
        local h = math.floor((pick.cols[x + 1] or 0) * 11 + 0.5)
        if h > 0 then
          screen.level(head and x < head and 12 or 6)
          screen.rect(x, mid - h, 1, 2 * h + 1)
          screen.fill()
        end
      end
      if head then
        screen.level(15)
        screen.rect(head, 40, 1, 24)
        screen.fill()
      end
    end
  end
  screen.update()
end

-- The song page. s: from song.screen(). The song's name, playing or
-- stopped with the scene, its pass and the tempo, then a row per scene: a
-- bar lit on the one playing, "next" on the queued one, the cursor's row
-- highlighted.
function view.song(parts, s, note)
  if s.kind == "list" then
    view.picker(s.pick)
    return
  end
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  header(parts, s.name or "song", s.note or note)
  if not s.name then
    screen.level(s.loading and 15 or 6)
    text(64, 34, s.loading and fit("loading " .. s.loading:lower(), 124) or "no song", "center")
    screen.level(4)
    if not s.loading then text(64, 48, "k2 songs", "center") end
    screen.update()
    return
  end
  local pos, scenes = s.pos, s.scenes
  local cur = scenes[pos.scene + 1]
  -- a triangle while playing, a square stopped
  if pos.playing then
    for k, h in ipairs({7, 5, 3, 1}) do fill_rect(k - 1, 14 - (7 - h) // 2 - h, 1, h, 15) end
  else
    fill_rect(0, 8, 5, 5, 8)
  end
  screen.level(pos.playing and 15 or 8)
  local line = cur and cur.name:lower() or ""
  if cur and pos.playing and cur.repeats > 1 then line = line .. "  " .. (pos.pass + 1) .. "/" .. cur.repeats end
  local bpm = pos.bpm and string.format("%d bpm", math.floor(pos.bpm + 0.5)) or ""
  text(8, 13, fit(line, 116 - screen.text_extents(bpm)))
  screen.level(6)
  text(127, 13, bpm, "right")
  fill_rect(0, 16, 128, 1, 2)
  local shown = 6
  local top = util.clamp(s.cursor - shown + 1, 1, math.max(1, #scenes - shown + 1))
  if s.cursor < top then top = s.cursor end
  local scrolls = #scenes > shown
  local right = scrolls and 123 or 128
  for k = top, math.min(#scenes, top + shown - 1) do
    local sc = scenes[k]
    local y = 24 + (k - top) * 7
    local on = k == s.cursor
    if on then fill_rect(0, y - 6, right, 7, 3) end
    local playing = k == pos.scene + 1
    fill_rect(0, y - 5, 2, 5, playing and (pos.playing and 15 or 8) or 1)
    screen.level(on and 15 or (playing and 12 or 6))
    local tail = k == pos.queued + 1 and "next" or (sc.repeats > 1 and "x" .. sc.repeats or "")
    text(5, y, fit(sc.name:lower(), right - 8 - screen.text_extents(tail)))
    screen.level(k == pos.queued + 1 and 15 or 3)
    text(right - 2, y, tail, "right")
  end
  if scrolls then scrollbar(124, 18, 45, top, shown, #scenes) end
  screen.update()
end

-- msg is wrapped over two lines if it's too wide.
-- The app's icon, 47 px wide, one grey level (hex) a pixel.
local SPLASH = {
  "17999999700000799999700000799999700000799999971",
  "79999999100000199999100000199999100000199999997",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999000000099999000000099999000000099999999",
  "99999999100000199999100000199999100000199999999",
  "99999999710001799999710001799999710001799999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "99999999999099999999999099999999999099999999999",
  "aefffffffffffffffffffffffffffffffffffffffffffea",
  "efffffffffffffffffffffffffffffffffffffffffffffe",
  "fffffffffffffffffffffffffffffffffffffffffffffff",
  "bfffffffffffffffffffffffffffffffffffffffffffffb",
  "2bfffffffffffffffffffffffffffffffffffffffffffb2",
  "006f10000000cff20000000000000009f40000000000000",
  "008f300000009ff0000000000000000af50000000000000",
  "00bf60000000aff0000000000000000cf70000000000000",
  "005a20000000cff2000000000000001ff90000000000000",
  "000000000000eff5000000000000002ffa0000000000000",
  "000000000002fff80000000000000009d40000000000000",
  "000000000005fffa0000000000000000000000000000000",
  "000000000006fffb0000000000000000000000000000000",
  "000000000002eff60000000000000000000000000000000",
  "00000000000015300000000000000000000000000000000"
}

-- The splash, its levels scaled by fade (0-1), with a status line under it.
function view.splash(fade, msg)
  screen.clear()
  local x0 = 64 - 23
  for y, row in ipairs(SPLASH) do
    local x = 1
    while x <= #row do
      local c = row:sub(x, x)
      local e = x
      while row:sub(e + 1, e + 1) == c do e = e + 1 end
      local lv = math.floor(tonumber(c, 16) * fade + 0.5)
      if lv > 0 then fill_rect(x0 + x - 1, 3 + y, e - x + 1, 1, lv) end
      x = e + 1
    end
  end
  if msg then
    screen.level(math.floor(4 * fade + 0.5))
    text(64, 60, msg, "center")
  end
  screen.update()
end

function view.starting(msg)
  screen.clear()
  screen.level(msg and 15 or 8)
  msg = msg or "starting"
  local cut = screen.text_extents(msg) > 120 and msg:sub(1, 24):match(".*() ") or nil
  if cut then
    text(64, 30, msg:sub(1, cut - 1), "center")
    text(64, 40, msg:sub(cut + 1), "center")
  else
    text(64, 34, msg, "center")
  end
  screen.update()
end

return view
