-- The norns screen: the parts list and a part's macro page, and the
-- OMX-27's 128x32 screen.

local view = {}

local function text(x, y, s, align)
  screen.move(x, y)
  if align == "right" then screen.text_right(s)
  elseif align == "center" then screen.text_center(s)
  else screen.text(s) end
end

-- s cut to fit in w pixels.
local function fit(s, w)
  s = s or ""
  if screen.text_extents(s) <= w then return s end
  while #s > 1 and screen.text_extents(s .. ".") > w do s = s:sub(1, -2) end
  return s .. "."
end

-- 0-1 on a 48 dB scale, for a linear peak.
local function meter(peak)
  if peak <= 0 then return 0 end
  return util.clamp(1 + (20 * math.log(peak, 10)) / 48, 0, 1)
end

-- Load in the top right, and a warning on the left while the worst block has
-- been over 80% in the last 2 seconds, or `note` if there is one.
local function header(parts, left, note)
  screen.level(4)
  text(127, 6, string.format("%d%%", math.floor(parts.load + 0.5)), "right")
  if note then
    screen.level(15)
    text(0, 6, fit(note, 104))
  elseif util.time() - parts.worst_at < 2 then
    screen.level(15)
    text(0, 6, string.format("overload %d%%", math.floor(parts.worst + 0.5)))
  elseif left then
    screen.level(8)
    text(0, 6, fit(left, 104))
  end
end

-- state: sel (part), k1 (held). note: a line for the header, or nil
function view.main(parts, state, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local sel = parts.list[state.sel]
  local files = parts.material(state.sel)
  header(parts, sel.machine and (sel.n .. " " .. sel.machine .. "  " .. (files or sel.patch or "")) or nil, note)
  for i, part in ipairs(parts.list) do
    local y = 7 + 7 * i
    local on = i == state.sel
    if on then
      screen.level(3)
      screen.rect(0, y - 6, 128, 7)
      screen.fill()
    end
    local bright = part.machine and (on and 15 or 6) or (on and 8 or 2)
    screen.level(bright)
    text(1, y, tostring(i))
    screen.level(math.max(1, bright - 4))
    text(8, y, parts.mpe == i and "mpe" or part.channel > 0 and tostring(part.channel) or "-")
    screen.level(bright)
    local m = part.machine and parts.machine_info(part.machine)
    if on and state.k1 then screen.level(15) end
    text(20, y, m and m.short or "-")
    screen.level(on and state.k1 and 6 or bright)
    if part.machine then
      if parts.mpe == i and state.mpe_missing then
        text(48, y, "mpe: no device")
      else
        text(48, y, fit(part.patch or "", 66))
      end
    end
    -- a dot while a note is held or just played, and the part's level
    if part.held > 0 or util.time() - part.last_note < 0.15 then
      screen.level(15)
      screen.rect(116, y - 4, 2, 2)
      screen.fill()
    end
    local w = math.floor(meter(part.level) * 7 + 0.5)
    if w > 0 then
      screen.level(on and 15 or 8)
      screen.rect(120, y - 5, w, 4)
      screen.fill()
    end
  end
  screen.update()
end

-- The part's 5 macros, a row each: label, bar of the 0-1 value, readout.
-- state: sel, macro (selected row). list: from macros.resolve
-- Labels get a column wide enough for any 6-letter label.
local LABEL_W, BAR_X, BAR_W = 36, 38, 44
function view.macros(parts, state, list, note)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local part = parts.list[state.sel]
  local files = parts.material(state.sel)
  header(parts, files and (part.n .. " " .. (part.patch or "") .. "  " .. files)
    or (part.n .. " " .. (part.machine or "none") .. "  " .. (part.patch or "")), note)
  for k, mac in ipairs(list) do
    local y = 7 + 11 * k
    local on = k == state.macro
    if on then
      screen.level(3)
      screen.rect(0, y - 8, 128, 11)
      screen.fill()
    end
    local p = mac.slot and params:lookup_param(parts.id(state.sel, "s" .. mac.slot))
    screen.level(p and (on and 15 or 6) or 2)
    text(1, y, fit(mac.label, LABEL_W))
    if p then
      local v = p:get()
      screen.level(on and 6 or 2)
      screen.rect(BAR_X, y - 4, BAR_W, 3)
      screen.fill()
      screen.level(on and 15 or 8)
      local w = math.floor(v * BAR_W + 0.5)
      if w > 0 then
        screen.rect(BAR_X, y - 4, w, 3)
        screen.fill()
      end
      screen.level(on and 15 or 6)
      text(127, y, fit(parts.readout(mac.def, v), 127 - BAR_X - BAR_W - 3), "right")
    end
  end
  screen.update()
end

-- The OMX-27's screen, at the top left of the norns screen, without
-- screen.update: pixels at level 8 or more are lit on the OMX.
-- Part, machine and octave on top and the patch below, or for a second after
-- a pot is turned, its macro: label, readout and a bar. While the pot is
-- waiting to pick up the value, a tick shows where the pot is.
-- list: from macros.resolve. omx: the surface (octave, showing()).
function view.omx(parts, state, list, omx)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  screen.level(15)
  local part = parts.list[state.sel]
  local f = omx.showing()
  local mac = f and list[f.k]
  local p = mac and mac.slot and params:lookup_param(parts.id(state.sel, "s" .. mac.slot))
  if p then
    local v = p:get()
    text(0, 7, mac.label)
    text(127, 7, fit(parts.readout(mac.def, v), 80), "right")
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

-- msg is wrapped over two lines if it's too wide.
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
