-- The play page's machine motifs: motifs[name](x, y, w, h, t, energy, held)
-- draws a small picture of the machine in the box from x, y, w by h px.
-- t is seconds for slow idle motion, energy 0-1 from the part's level, held
-- the part's held notes. Each draws in a few grey levels with a handful of
-- rects per level, as the screen redraws 15 times a second.

local floor, sin, cos, exp, abs, min, max = math.floor, math.sin, math.cos, math.exp, math.abs, math.min, math.max
local pi = math.pi

local function rnd(v) return floor(v + 0.5) end

local function rect(x, y, w, h) screen.rect(x, y, w, h) end

local function lit(lv)
  screen.level(util.clamp(rnd(lv), 0, 15))
end

local function fill_rect(x, y, w, h, lv)
  lit(lv)
  screen.rect(x, y, w, h)
  screen.fill()
end

-- A line through the columns x0 to x1 at y = f(column), as runs at each y
-- joined by vertical steps. Adds to the path; the caller fills.
local function curve(x0, x1, f)
  local py = rnd(f(x0))
  local run = x0
  for cx = x0 + 1, x1 do
    local y = rnd(f(cx))
    if y ~= py then
      rect(run, py, cx - run, 1)
      if y > py + 1 then rect(cx, py + 1, 1, y - py - 1)
      elseif y < py - 1 then rect(cx, y + 1, 1, py - y - 1) end
      run, py = cx, y
    end
  end
  rect(run, py, x1 - run + 1, 1)
end

-- A line from x0, y0 to x1, y1, a rect per pixel. Adds to the path.
local function line(x0, y0, x1, y1)
  local n = max(abs(x1 - x0), abs(y1 - y0))
  for k = 0, n do
    local f = n == 0 and 0 or k / n
    rect(rnd(x0 + (x1 - x0) * f), rnd(y0 + (y1 - y0) * f), 1, 1)
  end
end

-- An ellipse outline as runs per row {dx, dy, w}, cached by radii.
local rings = {}
local function ring_runs(rx, ry)
  local key = rx * 100 + ry
  if rings[key] then return rings[key] end
  local on = {}
  local steps = floor(8 * max(rx, ry)) + 8
  for k = 0, steps - 1 do
    local a = 2 * pi * k / steps
    local dx, dy = rnd(rx * cos(a)), rnd(ry * sin(a))
    on[dy] = on[dy] or {}
    on[dy][dx] = true
  end
  local runs = {}
  for dy = -ry, ry do
    local row = on[dy]
    if row then
      local dx = -rx
      while dx <= rx do
        if row[dx] then
          local s = dx
          while row[dx + 1] do dx = dx + 1 end
          runs[#runs + 1] = {s, dy, dx - s + 1}
        end
        dx = dx + 1
      end
    end
  end
  rings[key] = runs
  return runs
end

local function ring(cx, cy, rx, ry)
  for _, r in ipairs(ring_runs(rx, ry or rx)) do rect(cx + r[1], cy + r[2], r[3], 1) end
end

-- A filled ellipse, a rect per row.
local function disc(cx, cy, rx, ry)
  for dy = -ry, ry do
    local hw = rnd(rx * math.sqrt(math.max(0, 1 - (dy / (ry + 0.5)) ^ 2)))
    rect(cx - hw, cy + dy, 2 * hw + 1, 1)
  end
end

-- A sprite from rows of characters, compiled to runs per level once.
-- levels maps a character to a grey level; others are blank.
local function sprite(rows, levels)
  local by = {}
  for y, row in ipairs(rows) do
    local x = 1
    while x <= #row do
      local c = row:sub(x, x)
      local lv = levels[c]
      if lv then
        local s = x
        while row:sub(x + 1, x + 1) == c do x = x + 1 end
        by[lv] = by[lv] or {}
        local t = by[lv]
        t[#t + 1] = {s - 1, y - 1, x - s + 1}
      end
      x = x + 1
    end
  end
  local out = {}
  for lv, runs in pairs(by) do out[#out + 1] = {lv = lv, runs = runs} end
  table.sort(out, function(a, b) return a.lv < b.lv end)
  return out
end

-- Draws a sprite at x, y; scale multiplies every level.
local function stamp(sp, x, y, scale)
  for _, g in ipairs(sp) do
    lit(g.lv * (scale or 1))
    for _, r in ipairs(g.runs) do rect(x + r[1], y + r[2], r[3], 1) end
    screen.fill()
  end
end

local M = {}

-- Reflux: a falling saw through a resonant filter; notes open it, so it
-- rings after each reset.
function M.Reflux(x, y, w, h, t, e, held)
  local mid = y + h / 2 - 0.5
  local amp = 6 + 2 * e
  local per = 20
  local shift = t * 4
  local k = 14 - 7 * e
  local om = 40 * e
  lit(2)
  rect(x, rnd(mid), w, 1)
  screen.fill()
  lit(8 + 7 * e)
  curve(x, x + w - 1, function(cx)
    local p = ((cx - x + shift) % per) / per
    local v = (2 * p - 1) + 2 * exp(-k * p) * cos(om * p)
    return mid - util.clamp(v, -1.4, 1.4) * amp
  end)
  screen.fill()
end

-- Hexbeat: 16 step pads in two rows, a step light running over them; the
-- programmed steps glow with the part's level.
local HEX = {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1}
function M.Hexbeat(x, y, w, h, t, e, held)
  local step = floor(t * 4) % 16
  for row = 0, 1 do
    local py = y + 2 + row * 10
    for col = 0, 7 do
      local n = row * 8 + col
      local px = x + col * 5
      local on = HEX[n + 1] == 1
      local lv = 2
      if on then lv = 4 + 9 * e end
      if n == step then lv = held > 0 and 15 or max(lv, 8) end
      fill_rect(px, py + 2, 4, 5, lv)
      -- the step's LED
      fill_rect(px + 1, py, 2, 1, on and 10 or (n == step and 6 or 1))
    end
  end
end

-- Trinity: three oscillators, sine, saw and square, slightly detuned so
-- they drift; louder with the level.
function M.Trinity(x, y, w, h, t, e, held)
  local amp = 1.6 + 1.4 * e
  local mids = {y + 3, y + 10.5, y + 18}
  local rates = {1.0, 1.13, 0.91}
  local shapes = {
    function(p) return sin(2 * pi * p) end,
    function(p) return 1 - 2 * (p % 1) end,
    function(p) return (p % 1) < 0.5 and 1 or -1 end,
  }
  for k = 1, 3 do
    local ph = t * rates[k] * 0.6
    lit((k == 1 and 10 or (k == 2 and 7 or 5)) + 5 * e)
    curve(x, x + w - 1, function(cx)
      return mids[k] - amp * shapes[k]((cx - x) / 13 + ph)
    end)
    screen.fill()
  end
end

-- Ratio: six FM operators, three modulators over three carriers, each with
-- a dot turning at its ratio; the links light with the level.
local RATIOS = {1, 2, 3.5, 1, 0.5, 7}
function M.Ratio(x, y, w, h, t, e, held)
  local cols = {x + 6, x + 19, x + 32}
  local rows = {y + 5, y + 16}
  local speed = 1 + 5 * e
  -- links, the third modulator also feeding the second carrier
  lit(3 + 10 * e)
  for c = 1, 3 do rect(cols[c], rows[1] + 4, 1, 4) end
  line(cols[3] - 3, rows[1] + 2, cols[2] + 3, rows[2] - 2)
  -- feedback on the first modulator
  rect(cols[1] - 5, rows[1] - 4, 1, 5)
  rect(cols[1] - 5, rows[1] - 4, 4, 1)
  rect(cols[1] - 5, rows[1], 2, 1)
  screen.fill()
  for k = 1, 6 do
    local c, r = cols[(k - 1) % 3 + 1], rows[k <= 3 and 1 or 2]
    lit(k <= 3 and 6 + 6 * e or 9 + 6 * e)
    ring(c, r, 3)
    screen.fill()
    local a = t * RATIOS[k] * speed
    fill_rect(c + rnd(1.4 * sin(a)), r - rnd(1.4 * cos(a)), 1, 1, 15)
  end
end

-- Formulate: a chip's pulse wave with its width sweeping, over a row of
-- block meters that jump with the level.
function M.Formulate(x, y, w, h, t, e, held)
  local duty = 0.5 + 0.35 * sin(t * 0.7)
  local per = 10
  lit(8 + 7 * e)
  curve(x, x + w - 1, function(cx)
    local p = ((cx - x + t * 4) % per) / per
    return p < duty and y + 1 or y + 6
  end)
  screen.fill()
  for c = 0, 7 do
    local wob = 0.5 + 0.5 * sin(t * (3 + c * 0.7) + c * 1.9)
    local n = 1 + floor((e * (0.55 + 0.45 * wob)) * 4.99)
    for b = 0, 4 do
      local on = b < n
      fill_rect(x + c * 5, y + 19 - b * 3, 4, 2, on and (b >= 3 and 15 or 6 + 2 * b) or 1)
    end
  end
end

-- Genesis: a kick drum's front head with lugs, the centre pushing out with
-- each hit and a glint going slowly round the rim.
function M.Genesis(x, y, w, h, t, e, held)
  local cx, cy = x + 20, y + 11
  e = max(e, held > 0 and 0.6 or 0)
  local jig = (e > 0.6 and floor(t * 30) % 2 == 0) and 1 or 0
  cx = cx + jig
  lit(9)
  ring(cx, cy, 10)
  screen.fill()
  lit(3 + 4 * e)
  ring(cx, cy, 8)
  screen.fill()
  -- lugs round the rim
  lit(12)
  for k = 0, 7 do
    local a = k * pi / 4 + pi / 8
    rect(cx + rnd(10 * cos(a)), cy + rnd(10 * sin(a)), 1, 1)
  end
  screen.fill()
  -- the head pushing out, then the logo
  if e > 0.05 then
    lit(2 + 8 * e)
    disc(cx, cy, rnd(2 + 4 * e), rnd(2 + 4 * e))
    screen.fill()
  end
  lit(10 + 5 * e)
  ring(cx, cy, 2)
  screen.fill()
  local a = t * 0.8
  fill_rect(cx + rnd(10 * cos(a)), cy + rnd(10 * sin(a)), 1, 1, 15)
end

-- Tine: five tines fixed at the left, ringing at their free ends; one at a
-- time hums a little when idle.
function M.Tine(x, y, w, h, t, e, held)
  fill_rect(x, y + 1, 3, h - 2, 6)
  local idle = floor(t / 2.5) % 5
  local since = (t % 2.5)
  for k = 0, 4 do
    local len = 34 - k * 5
    local ty = y + 2 + k * 4.5
    local a = 2.4 * e + (k == idle and 0.9 * exp(-since * 1.5) or 0)
    local om = 9 + k * 3
    local ph = sin(t * om)
    lit(7 + 8 * e)
    curve(x + 3, x + 3 + len, function(cx)
      local f = (cx - x - 3) / len
      return ty + a * f * f * ph
    end)
    screen.fill()
    -- the pickup beyond the tip
    fill_rect(x + 5 + len, rnd(ty), 2, 1, 3 + 6 * e)
  end
end

-- Palm: a conga, its skin lighting and rippling on notes with slap marks
-- over it, and a glint moving slowly round the rim when idle.
function M.Palm(x, y, w, h, t, e, held)
  local cx, top = x + 20, y + 5
  e = max(e, held > 0 and 0.6 or 0)
  -- the barrel body, widest a third of the way down, and its bands
  lit(6)
  local bot = y + h - 1
  for yy = top + 2, bot do
    local f = (yy - top) / (bot - top)
    local hw = rnd(9 + 2 * sin(pi * min(1, f * 1.4)) - 3 * f)
    rect(cx - hw, yy, 1, 1)
    rect(cx + hw, yy, 1, 1)
  end
  rect(cx - 6, bot, 13, 1)
  screen.fill()
  lit(4)
  rect(cx - 9, top + 3, 19, 1)
  rect(cx - 8, bot - 3, 17, 1)
  screen.fill()
  -- the skin, lit and rippling with the level
  if e > 0.05 then
    lit(1 + 6 * e)
    disc(cx, top, 8, 2)
    screen.fill()
    local p = (t * 3) % 1
    lit(6 + 9 * e)
    ring(cx, top, max(1, rnd(8 * p)), max(1, rnd(2 * p)))
    screen.fill()
    -- slap marks over the skin
    lit(15 * e)
    rect(cx - 12, top - 4, 2, 1)
    rect(cx - 10, top - 3, 1, 1)
    rect(cx + 11, top - 4, 2, 1)
    rect(cx + 10, top - 3, 1, 1)
    screen.fill()
  end
  lit(12)
  ring(cx, top, 9, 2)
  screen.fill()
  local a = t * 0.9
  fill_rect(cx + rnd(9 * cos(a)), top + rnd(2 * sin(a)), 1, 1, 15)
end

-- Tongue: a jaw harp, a frame and its reed, the reed's end shaking on notes.
function M.Tongue(x, y, w, h, t, e, held)
  local cx, cy = x + 9, y + 11
  lit(9)
  ring(cx, cy, 7, 7)
  line(cx + 6, cy - 4, x + 34, cy - 1)
  line(cx + 6, cy + 4, x + 34, cy + 1)
  screen.fill()
  -- the reed, from the ring's back through the frame and bent up at the end
  local a = 2.5 * e + 0.4 * max(0, sin(t * 0.9)) ^ 8
  local ph = sin(t * 40)
  local tip = 0
  lit(14)
  curve(cx - 7, x + 37, function(px)
    local f = (px - (cx - 7)) / (x + 37 - cx + 7)
    local d = a * f * f * ph
    tip = d
    return cy + d
  end)
  screen.fill()
  lit(14)
  rect(x + 38, rnd(cy + tip) - 5, 1, 5)
  rect(x + 37, rnd(cy + tip) - 6, 1, 1)
  screen.fill()
  -- the sound, faint lines either side of the reed
  if e > 0.15 then
    lit(2 + 6 * e)
    rect(x + 24, cy - 6, 6, 1)
    rect(x + 24, cy + 6, 6, 1)
    rect(x + 22, cy - 8, 4, 1)
    rect(x + 22, cy + 8, 4, 1)
    screen.fill()
  end
end

-- Timber: a woodwind with tone holes, fingers moving over them, and breath
-- leaving the bell on notes.
local FINGERS = {
  {1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 0}, {1, 1, 1, 1, 0, 0}, {1, 1, 1, 0, 0, 0},
  {1, 1, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {1, 1, 0, 1, 0, 0},
}
function M.Timber(x, y, w, h, t, e, held)
  local mid = y + 10
  -- the mouthpiece, the body and the bell
  lit(7)
  rect(x, mid - 1, 5, 2)
  rect(x + 5, mid - 2, 25, 5)
  screen.fill()
  lit(8)
  for k = 0, 4 do
    local hw = 2 + floor(k * k / 4)
    rect(x + 30 + k, mid - hw, 1, 2 * hw + 1)
  end
  screen.fill()
  -- rings on the body
  fill_rect(x + 5, mid - 2, 1, 5, 11)
  fill_rect(x + 17, mid - 2, 1, 5, 11)
  local rate = held > 0 and 6 or 0.7
  local f = FINGERS[floor(t * rate) % #FINGERS + 1]
  for k = 1, 6 do
    local hx = x + 7 + (k - 1) * 4 + (k > 3 and 1 or 0)
    fill_rect(hx, mid, 2, 1, 0)
    if f[k] == 1 then fill_rect(hx, mid - 4, 2, 3, 13) end
  end
  -- breath out of the bell
  if e > 0.05 then
    for k = 0, 2 do
      local p = (t * 1.5 + k / 3) % 1
      local px = x + 35 + rnd(4 * p)
      fill_rect(px, mid + rnd((k - 1) * (2 + 6 * p)), 1, 1, (15 - 12 * p) * e)
    end
  end
end

-- Aviary: a songbird on a branch, blinking now and then; its beak opens and
-- notes rise from it while one plays.
local BIRD = sprite({
  "........###....",
  ".......#####...",
  ".......#####...",
  "......######...",
  ".##..########..",
  "..###++++####..",
  "...##+++++###..",
  "....#++++###...",
  ".....#######...",
  "......#####....",
}, {["#"] = 12, ["+"] = 6})
function M.Aviary(x, y, w, h, t, e, held)
  local bx, by = x + 6, y + 6
  local bob = (e > 0.2 and floor(t * 8) % 2 == 0) and -1 or 0
  if e <= 0.2 and (t % 4) < 0.25 then bob = 1 end
  -- the branch and the feet
  lit(5)
  rect(x, y + 18, w, 1)
  rect(x + 30, y + 17, 6, 1)
  screen.fill()
  fill_rect(bx + 7, by + 10, 1, 2, 9)
  fill_rect(bx + 9, by + 10, 1, 2, 9)
  stamp(BIRD, bx, by + bob)
  local hy = by + bob
  -- the eye blinks every few seconds
  local blink = (t % 3.3) < 0.15
  fill_rect(bx + 10, hy + 2, 1, 1, blink and 12 or 0)
  -- the beak, open with the level
  local open = held > 0 and 2 or rnd(2 * e)
  lit(15)
  rect(bx + 13, hy + 2 - open, 2, 1)
  rect(bx + 13, hy + 3 + open, 2, 1)
  if open == 0 then rect(bx + 15, hy + 2, 1, 1) end
  screen.fill()
  -- notes rising from the beak
  if e > 0.1 then
    for k = 0, 2 do
      local p = (t * 0.9 + k / 3) % 1
      local nx = bx + 17 + rnd(p * 14)
      local ny = hy + 1 - rnd(p * 6) + rnd(sin(p * 9 + k))
      fill_rect(nx, ny, 2, 2, (15 - 12 * p) * e)
      fill_rect(nx + 1, ny - 3, 1, 3, (15 - 12 * p) * e)
    end
  end
end

-- Fret: six strings over frets, shaking with the level; idle, one at a time
-- quivers.
local FRETS = {0, 9, 17, 24, 30, 35}
function M.Fret(x, y, w, h, t, e, held)
  lit(3)
  for _, f in ipairs(FRETS) do rect(x + f, y, 1, h) end
  screen.fill()
  fill_rect(x, y, 2, h, 8)
  -- inlays
  lit(4)
  rect(x + 20, y + 10, 2, 2)
  rect(x + 32, y + 10, 2, 2)
  screen.fill()
  local idle = floor(t / 1.7) % 6
  for k = 0, 5 do
    local sy = y + 2 + k * 3.5
    local a = 1.3 * e * (0.7 + 0.3 * k / 5) + (k == idle and 0.6 * (1 - (t % 1.7) / 1.7) or 0)
    local ph = sin(t * (23 + k * 7) + k)
    lit(5 + k + 6 * e)
    curve(x + 2, x + w - 1, function(cx)
      return sy + a * ph * sin(pi * (cx - x - 2) / (w - 3))
    end)
    screen.fill()
  end
end

-- Chanter: a bagpipe, the bag swelling with the level, three drones on
-- top sounding and the chanter below.
function M.Chanter(x, y, w, h, t, e, held)
  local cx, cy = x + 13, y + 13
  local swell = 0.4 * sin(t * 1.1) + 1.2 * e
  lit(4 + 4 * e)
  disc(cx, cy, rnd(8 + swell), rnd(5 + swell * 0.6))
  screen.fill()
  lit(11)
  -- the blowpipe, up to the left
  line(cx - 6, cy - 4, x + 1, y + 3)
  -- the drones, fanning up to the right
  line(cx - 2, cy - 5, x + 18, y + 1)
  line(cx + 2, cy - 4, x + 27, y + 2)
  line(cx + 5, cy - 3, x + 36, y + 4)
  -- the chanter, down to the right
  line(cx + 6, cy + 3, x + 24, y + 21)
  screen.fill()
  -- drone tips, and sound off them with the level
  local tips = {{x + 18, y + 1}, {x + 27, y + 2}, {x + 36, y + 4}}
  for k, p in ipairs(tips) do
    fill_rect(p[1] - 1, p[2], 3, 1, 15)
    if e > 0.05 then
      local d = rnd(1 + 2 * ((t * 2 + k / 3) % 1))
      fill_rect(p[1] + d, p[2] - d, 1, 1, 15 * e)
    end
  end
  -- fingers on the chanter
  local f = floor(t * (held > 0 and 5 or 0.6)) % 3
  for k = 0, 2 do
    fill_rect(x + 25 + k * 3 - 3, y + 15 + k * 2, 2, 1, k == f and 15 or 5)
  end
end

-- Brazen: a horn's tube and valves into a flaring bell, the valves going
-- down on notes and the bell glowing and sounding with the level.
function M.Brazen(x, y, w, h, t, e, held)
  local mid = y + 10
  local bell = x + 32
  local function hw(cx)
    local f = (cx - x - 14) / (bell - x - 14)
    if f <= 0 then return 1 end
    return 1 + 8 * f ^ 2.5
  end
  -- the bell's mouth, lit with the level
  if e > 0.05 then
    lit(2 + 9 * e)
    for cx = bell - 4, bell do
      local s = rnd(hw(cx))
      rect(cx, mid - s + 1, 1, 2 * s - 1)
    end
    screen.fill()
  end
  lit(10)
  rect(x, mid - 1, 2, 3)
  curve(x + 2, bell, function(cx) return mid - hw(cx) end)
  curve(x + 2, bell, function(cx) return mid + hw(cx) end)
  rect(bell, mid - 9, 1, 19)
  -- the loop under the valves
  rect(x + 5, mid + 4, 1, 4)
  rect(x + 5, mid + 8, 16, 1)
  rect(x + 20, mid + 2, 1, 6)
  screen.fill()
  -- the valves
  local down = held > 0 and 1 or 0
  for k = 0, 2 do
    local vx = x + 7 + k * 4
    fill_rect(vx, mid - 5 + down, 2, 4 - down, 6)
    fill_rect(vx - 1, mid - 6 + down, 4, 1, 13)
  end
  -- sound off the bell
  if e > 0.05 then
    for k = 0, 1 do
      local p = (t * 2 + k / 2) % 1
      local sx = bell + 2 + rnd(4 * p)
      local sh = rnd(3 + 4 * p)
      fill_rect(sx, mid - sh, 1, 2 * sh + 1, (14 - 10 * p) * e)
    end
  end
end

-- Fathom: three rows of waves, rising and growing with the level.
function M.Fathom(x, y, w, h, t, e, held)
  local rise = 4 * e
  local rows = {
    {y + 7, 11, 0.7, 1, 4},
    {y + 12, 9, -1.0, 1.3, 8},
    {y + 17, 7, 1.4, 1.6, 13},
  }
  for k, r in ipairs(rows) do
    local base, len, speed, amp, lv = r[1] - rise * (k / 3), r[2], r[3], r[4] * (1 + e), r[5]
    lit(lv)
    curve(x, x + w - 1, function(cx)
      return base - amp * sin(2 * pi * ((cx - x) / len + t * speed * 0.3))
    end)
    screen.fill()
  end
  -- foam on the front row when it's high
  if e > 0.3 then
    local base, len, speed, amp = rows[3][1] - rise, rows[3][2], rows[3][3], rows[3][4] * (1 + e)
    lit(15)
    for cx = x, x + w - 1, 2 do
      local s = sin(2 * pi * ((cx - x) / len + t * speed * 0.3))
      if s > 0.85 then rect(cx, rnd(base - amp * s) - 1, 1, 1) end
    end
    screen.fill()
  end
  -- the sea bed
  fill_rect(x, y + h - 1, w, 1, 2)
end

return M
