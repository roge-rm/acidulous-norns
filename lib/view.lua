-- The norns screen: the parts list and a part's params page.

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
-- been over 80% in the last 2 seconds.
local function header(parts, left)
  screen.level(4)
  text(127, 6, string.format("%d%%", math.floor(parts.load + 0.5)), "right")
  if util.time() - parts.worst_at < 2 then
    screen.level(15)
    text(0, 6, string.format("overload %d%%", math.floor(parts.worst + 0.5)))
  elseif left then
    screen.level(8)
    text(0, 6, fit(left, 104))
  end
end

-- state: sel (part), k1 (held)
function view.main(parts, state)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local sel = parts.list[state.sel]
  header(parts, sel.machine and (sel.n .. " " .. sel.machine .. "  " .. (sel.patch or "")) or nil)
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
    text(8, y, part.channel > 0 and tostring(part.channel) or "-")
    screen.level(bright)
    local m = part.machine and parts.machine_info(part.machine)
    if on and state.k1 then screen.level(15) end
    text(20, y, m and m.short or "-")
    screen.level(on and state.k1 and 6 or bright)
    if part.machine then text(48, y, fit(part.patch or "", 66)) end
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

-- The part's first params. state: sel, param (selected row), first (top row)
view.PARAM_ROWS = 6

function view.params(parts, state, params_get)
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  local part = parts.list[state.sel]
  header(parts, part.n .. " " .. (part.machine or "none") .. "  " .. (part.patch or ""))
  local defs = part.machine and parts.defs[part.machine] or {}
  for r = 1, view.PARAM_ROWS do
    local j = state.first + r - 1
    local d = defs[j]
    if not d then break end
    local y = 8 + 9 * r
    local on = j == state.param
    if on then
      screen.level(3)
      screen.rect(0, y - 7, 128, 9)
      screen.fill()
    end
    screen.level(on and 15 or 6)
    text(1, y, fit(d.name, 70))
    text(127, y, parts.readout(d, params_get(j)), "right")
  end
  screen.update()
end

function view.starting()
  screen.clear()
  screen.level(8)
  text(64, 34, "starting", "center")
  screen.update()
end

return view
