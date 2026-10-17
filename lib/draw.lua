-- Drawing helpers shared by the pages.

local draw = {}

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

-- Load in the top right, or `right` if given, and a warning on the left
-- while the worst block has been over 80% in the last 2 seconds, or `note`
-- if there is one. y is the baseline, 5 at the top. Returns true if it
-- drew `left`.
local function header(parts, left, note, right, y)
  y = y or 5
  screen.level(4)
  local w = right and 123 - screen.text_extents(right) or 104
  text(127, y, right or string.format("%d%%", math.floor(parts.load + 0.5)), "right")
  if note then
    screen.level(15)
    text(0, y, fit(note, w))
  elseif util.time() - parts.worst_at < 2 then
    screen.level(15)
    text(0, y, string.format("overload %d%%", math.floor(parts.worst + 0.5)))
  elseif left then
    screen.level(8)
    text(0, y, fit(left, w))
    return true
  end
end

local function fill_rect(x, y, w, h, lv)
  screen.level(lv)
  screen.rect(x, y, w, h)
  screen.fill()
end

-- A 1 px outline, drawn as rects so it lands on whole pixels.
local function frame(x, y, w, h, lv)
  fill_rect(x, y, w, 1, lv)
  fill_rect(x, y + h - 1, w, 1, lv)
  fill_rect(x, y, 1, h, lv)
  fill_rect(x + w - 1, y, 1, h, lv)
end

-- s cut to w pixels, without a dot.
local function cut(s, w)
  s = s or ""
  while #s > 1 and screen.text_extents(s) > w do s = s:sub(1, (utf8.offset(s, -1) or #s) - 1) end
  return s
end

-- A label in w pixels: vowels go from the end, keeping the first letter,
-- then it's cut.
local squeezed = {}
local function squeeze(s, w)
  local key = s .. "|" .. w
  if squeezed[key] then return squeezed[key] end
  local t = s
  local k = #t
  while screen.text_extents(t) > w and k > 1 do
    if t:sub(k, k):find("[aeiou]") and t:sub(k - 1, k - 1) ~= " " then t = t:sub(1, k - 1) .. t:sub(k + 1) end
    k = k - 1
  end
  t = cut(t, w)
  squeezed[key] = t
  return t
end

-- A value in few letters: 880, 1.2k, 70%, 210ms, +3st.
local function short_value(parts, def, v)
  local x = parts.map(def, v)
  local unit = (def.unit or ""):lower()
  local function num(n)
    local a = math.abs(n)
    local s
    if a >= 100 then s = string.format("%d", math.floor(n + 0.5))
    elseif a >= 10 then s = string.format("%.1f", n)
    else s = string.format("%.2f", n) end
    if s:find("%.") then s = s:gsub("0+$", ""):gsub("%.$", "") end
    if s == "-0" then s = "0" end
    return s
  end
  if unit == "" then
    if def.curve ~= "step" and def.min >= -2 and def.max <= 2 then
      return string.format("%d%%", math.floor(x * 100 + 0.5))
    end
    return num(x)
  elseif unit == "hz" then
    if x >= 1000 then return num(x / 1000) .. "k" end
    return num(x)
  elseif unit == "ms" then
    if x >= 1000 then return num(x / 1000) .. "s" end
    return num(x) .. "ms"
  elseif unit == "%" then
    return string.format("%d%%", math.floor(x + 0.5))
  elseif unit == "st" or unit == "ct" or unit == "db" then
    return (x > 0 and "+" or "") .. num(x) .. unit
  end
  return num(x) .. unit
end

-- Dots along a circle from angle a0 to a1 in degrees, 0 up and clockwise,
-- as the mockups draw them.
local function arc(cx, cy, r, a0, a1)
  local step = 2 / math.max(r, 1) * 30
  local a = a0
  while a <= a1 do
    local t = math.rad(a)
    screen.pixel(math.floor(cx + r * math.sin(t) + 0.5), math.floor(cy - r * math.cos(t) + 0.5))
    a = a + step
  end
  screen.fill()
end

-- A dial like the app's: a 270 degree track, the value arc and a pointer.
-- A param that goes either side of 0 lights from the top.
local function knob(cx, cy, r, v, lv_track, lv_val, bipolar, zero)
  screen.level(lv_track)
  arc(cx, cy, r, -135, 135)
  screen.level(lv_val)
  local a = -135 + 270 * v
  if bipolar then
    local z = -135 + 270 * zero
    arc(cx, cy, r, math.min(z, a), math.max(z, a))
  else
    arc(cx, cy, r, -135, a)
  end
  local t = math.rad(a)
  for k = 1, r - 1 do
    screen.pixel(math.floor(cx + k * math.sin(t) + 0.5), math.floor(cy - k * math.cos(t) + 0.5))
  end
  screen.fill()
end

draw.text, draw.fit, draw.meter, draw.header = text, fit, meter, header
draw.fill_rect, draw.frame = fill_rect, frame
draw.cut, draw.squeeze, draw.short_value, draw.knob = cut, squeeze, short_value, knob

return draw
