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

-- Chanter: a bagpipe, the bag under the arm swelling with the level, three
-- long drones over the shoulder sounding off their tips, the blowpipe up to
-- the mouth and the chanter down with fingers on its holes.
local DRONES = {
  -- start x, y, end x, y, level
  {7, 9, 29, 0, 11},
  {13, 11, 34, 3, 13},
  {19, 13, 36, 8, 15},
}
function M.Chanter(x, y, w, h, t, e, held)
  local swell = 0.3 * sin(t * 1.1) + 1.4 * e
  local buzz = (e > 0.1 and floor(t * 24) % 2 == 0) and 1 or 0
  -- the drones, 2 px tubes with bands and flared tips
  for k, d in ipairs(DRONES) do
    local x0, y0, x1, y1 = x + d[1], y + d[2], x + d[3], y + d[4]
    lit(d[5] - 6)
    line(x0, y0 + 1, x1, y1 + 1)
    screen.fill()
    lit(d[5])
    line(x0, y0, x1, y1)
    screen.fill()
    lit(d[5] - 2)
    for _, f in ipairs({0.45, 0.75}) do
      local bx, by = rnd(x0 + (x1 - x0) * f), rnd(y0 + (y1 - y0) * f)
      rect(bx, by - 1, 2, 4)
    end
    screen.fill()
    local ty = y1 - (k == 3 and buzz or 0)
    fill_rect(x1 + 1, ty - 1, 1, 4, d[5])
    fill_rect(x1 + 2, ty - 2, 1, 6, d[5] - 3)
    if e > 0.05 then
      local p = (t * 2 + k / 3) % 1
      local dx = x1 + 4 + rnd(2 * p)
      if dx <= x + w - 1 then fill_rect(dx, ty + rnd(2 * p) - 1, 1, 1 + rnd(2 * p), (15 - 9 * p) * e) end
    end
  end
  -- the bag, tilted under the arm
  local cx, cy = x + 14, y + 15
  local rx, ry = rnd(9 + swell), rnd(4.4 + 0.6 * swell)
  for dy = -ry, ry do
    local hw = rnd(rx * math.sqrt(max(0, 1 - (dy / (ry + 0.5)) ^ 2)))
    local sx = cx - hw + rnd(-dy * 0.6)
    fill_rect(sx, cy + dy, 2 * hw + 1, 1, 4 + 4 * e)
    fill_rect(sx, cy + dy, 1, 1, 9)
    fill_rect(sx + 2 * hw, cy + dy, 1, 1, 9)
  end
  -- its cover's stripes
  lit(2 + 3 * e)
  rect(cx - 4, cy - 1, 9, 1)
  rect(cx - 5, cy + 2, 9, 1)
  screen.fill()
  -- the blowpipe up to the mouth
  lit(11)
  line(x + 5, y + 11, x + 2, y + 2)
  screen.fill()
  fill_rect(x + 1, y + 1, 3, 1, 14)
  -- the chanter, down from the bag's front, with its holes and fingers
  local f = floor(t * (held > 0 and 6 or 0.5)) % 3
  for k = 0, 6 do
    local px, py = x + 4 - rnd(k * 0.35), y + 15 + k
    if py <= y + h - 1 then
      local hole = k == 1 or k == 3 or k == 5
      fill_rect(px, py, 2, 1, hole and ((k - 1) / 2 == f and 15 or 1) or 10)
    end
  end
  fill_rect(x + 1, y + h - 1, 4, 1, 12)
end

-- Brazen: a trumpet, the leadpipe from the mouthpiece into three piston
-- valves, the main slide looped back and the bell flaring with a bright
-- rim. Valves go down on notes, the bell glows and sounds with the level.
local function tube(x0, x1, ty)
  fill_rect(x0, ty, x1 - x0 + 1, 1, 13)
  fill_rect(x0, ty + 1, x1 - x0 + 1, 1, 7)
end
function M.Brazen(x, y, w, h, t, e, held)
  local mid = y + 8
  local bell = x + 35
  local function hw(cx)
    local f = (cx - x - 24) / (bell - x - 24)
    if f <= 0 then return 1 end
    return 1 + 7 * f ^ 2.4
  end
  -- the bell's mouth, dark inside and glowing with the level
  lit(1 + 10 * e)
  for cx = bell - 6, bell - 1 do
    local s = rnd(hw(cx))
    if s > 1 then rect(cx, mid - s + 2, 1, 2 * s - 3) end
  end
  screen.fill()
  -- the bell's sides, the top one shining
  lit(14)
  curve(x + 24, bell, function(cx) return mid - hw(cx) + 1 end)
  screen.fill()
  lit(8)
  curve(x + 24, bell, function(cx) return mid + hw(cx) end)
  screen.fill()
  -- the rim
  fill_rect(bell, mid - 7, 1, 16, 15)
  fill_rect(bell + 1, mid - 6, 1, 14, 9)
  -- the bell pipe from the valves, and the main slide looped back under
  tube(x + 7, x + 24, mid)
  lit(10)
  rect(x + 5, mid + 1, 2, 1)
  rect(x + 4, mid + 2, 1, 5)
  rect(x + 5, mid + 7, 2, 1)
  screen.fill()
  tube(x + 7, x + 23, mid + 7)
  fill_rect(x + 23, mid + 3, 1, 4, 8)
  -- the leadpipe from the mouthpiece
  fill_rect(x, mid + 2, 1, 4, 14)
  fill_rect(x + 1, mid + 3, 2, 2, 11)
  tube(x + 3, x + 11, mid + 3)
  -- the valves, caps and buttons, pressed while a note is held
  local down = held > 0 and 2 or 0
  for k = 0, 2 do
    local vx = x + 12 + k * 4
    fill_rect(vx, mid - 1, 3, 11, 8)
    fill_rect(vx, mid - 1, 1, 11, 12)
    fill_rect(vx - 1, mid - 2, 5, 1, 13)
    fill_rect(vx - 1, mid + 10, 5, 1, 10)
    fill_rect(vx + 1, mid - 5 + down, 1, 3 - down, 6)
    fill_rect(vx, mid - 6 + down, 3, 1, 15)
  end
  -- the valve slides under
  lit(6)
  rect(x + 13, mid + 11, 1, 2)
  rect(x + 13, mid + 13, 9, 1)
  rect(x + 21, mid + 11, 1, 2)
  screen.fill()
  -- sound off the bell
  if e > 0.05 then
    for k = 0, 1 do
      local p = (t * 2 + k / 2) % 1
      local sx = bell + 3 + rnd(1.5 * p)
      local sh = rnd(3 + 4 * p)
      fill_rect(sx, mid - sh + 1, 1, 2 * sh - 1, (14 - 10 * p) * e)
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

-- Seconds since the last note on and how many there have been, per motif,
-- from held going up.
local onsets = {}
local function since_on(key, held, t)
  local s = onsets[key]
  if not s then
    s = {held = 0, at = -100, n = 0}
    onsets[key] = s
  end
  if held > s.held then s.at, s.n = t, s.n + 1 end
  s.held = held
  local d = t - s.at
  if d < 0 then d = 100 end
  return d, s.n
end

-- A cheap repeatable noise, 0-1, from a whole number.
local function hash(n)
  local v = sin(n * 12.9898) * 43758.5453
  return v - floor(v)
end

-- Resonance: a bell hung from a beam, swinging a little, struck by a
-- mallet on notes with rings spreading out from it.
local BELL = {2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 7}
function M.Resonance(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local cx, top = x + 11, y + 3
  local since = since_on("Resonance", held, t)
  local sway = rnd(0.6 * sin(t * 1.3) + 1.2 * e * sin(t * 9))
  fill_rect(x, y, 22, 1, 5)
  fill_rect(cx, y + 1, 1, 2, 7)
  -- the rings, round the bell's mouth, the newest brightest
  if e > 0.03 then
    for k = 0, 2 do
      local p = (t * 0.8 + k / 3) % 1
      lit((14 - 12 * p) * e)
      local r = 4 + rnd(p * 22)
      for _, run in ipairs(ring_runs(r, max(1, rnd(r * 0.45)))) do
        local s, e2 = run[1], run[1] + run[3] - 1
        if e2 > 6 and run[2] < 6 then
          s = max(s, 7)
          local px = cx + 3 + s
          local wd = min(e2 - s + 1, x + w - px)
          if wd > 0 then rect(px, top + 10 + run[2], wd, 1) end
        end
      end
      screen.fill()
    end
  end
  -- the bell, shaded across and bright at the lip
  for k, hw in ipairs(BELL) do
    local yy = top + k - 1
    local bx = cx + sway - hw
    fill_rect(bx, yy, 2 * hw + 1, 1, 6 + 5 * e)
    fill_rect(bx + 1, yy, 1, 1, 14)
    fill_rect(bx + 2 * hw, yy, 1, 1, 3)
  end
  fill_rect(cx + sway - 8, top + #BELL, 17, 1, 15)
  -- the clapper
  fill_rect(cx + sway - rnd(sway * 0.5), top + #BELL + 1, 2, 2, 9)
  -- the mallet, back and coming in on a note
  local d = min(1, since * 4)
  local mx = x + 30 - rnd(8 * (1 - d) * (1 - d))
  if held > 0 and since > 0.25 then mx = x + 30 end
  fill_rect(mx, top + 8, 4, 3, 12)
  fill_rect(mx + 4, top + 9, 7, 1, 7)
end

-- Draw: a concertina, its bellows opening and closing between two ends
-- with buttons; slow idle breaths, faster and wider when played, the
-- buttons lit by notes.
function M.Draw(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local rate = held > 0 and 2.2 or 0.5
  local open = 0.5 + 0.5 * sin(t * rate)
  local gap = rnd(10 + (6 + 8 * e) * open)
  local left = x + rnd((w - 12 - gap) / 2)
  local right = left + 6 + gap
  -- the ends, with a strap and buttons
  for _, ex in ipairs({left, right}) do
    fill_rect(ex, y + 1, 6, h - 2, 8)
    fill_rect(ex, y + 1, 6, 1, 13)
    fill_rect(ex + 1, y + 3, 4, h - 6, 3)
    for k = 0, 2 do
      local on = held > 0 and (floor(t * 4) + k) % 3 == 0
      fill_rect(ex + 2, y + 5 + k * 5, 2, 2, on and 15 or 10)
    end
  end
  -- the folds, a zigzag top and bottom with ridges between
  local folds = 4
  local fw = gap / folds
  lit(6 + 6 * e)
  for k = 0, folds - 1 do
    local f0 = left + 6 + rnd(k * fw)
    local f1 = left + 6 + rnd((k + 1) * fw)
    local fm = rnd((f0 + f1) / 2)
    line(f0, y + 3, fm, y + 1)
    line(fm, y + 1, f1, y + 3)
    line(f0, y + h - 4, fm, y + h - 2)
    line(fm, y + h - 2, f1, y + h - 4)
    rect(fm, y + 2, 1, h - 4)
  end
  screen.fill()
  lit(3 + 3 * e)
  for k = 0, folds do
    rect(left + 6 + rnd(k * fw), y + 4, 1, h - 8)
  end
  screen.fill()
end

-- Sympath: a sitar, the gourd and long neck with arched frets, its played
-- strings shaking and the sympathetic strings under them shimmering.
function M.Sympath(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  -- the gourd and the second one under the neck
  lit(4 + 2 * e)
  disc(x + 7, y + 13, 7, 7)
  screen.fill()
  lit(10)
  ring(x + 7, y + 13, 7, 7)
  screen.fill()
  lit(6)
  ring(x + 33, y + 16, 3, 3)
  screen.fill()
  -- the neck, the head's pegs and the frets
  fill_rect(x + 12, y + 8, 27, 5, 2)
  lit(9)
  for k = 0, 3 do rect(x + 30 + k * 2, y + 6 - (k % 2), 1, 2) end
  for k = 0, 2 do rect(x + 28 + k * 4, y + 13, 1, 2) end
  screen.fill()
  lit(7)
  for k = 0, 6 do
    local fx = x + 14 + k * 3
    rect(fx, y + 8, 1, 1)
    rect(fx - 1, y + 9, 1, 1)
    rect(fx + 1, y + 9, 1, 1)
  end
  screen.fill()
  -- the bridge
  fill_rect(x + 5, y + 11, 4, 1, 12)
  -- the played strings
  local idle = 0.4 * max(0, sin(t * 0.8)) ^ 6
  for k = 0, 1 do
    local sy = y + 9 + k
    local a = 1.2 * e + idle
    local ph = sin(t * (31 + 9 * k) + k)
    lit(11 + 4 * e - 2 * k)
    curve(x + 7, x + 38, function(cx)
      return sy - 1 + a * ph * sin(pi * (cx - x - 7) / 31) + (cx < x + 12 and 1 or 0)
    end)
    screen.fill()
  end
  -- the sympathetic strings, glinting along their length with the level
  lit(4)
  rect(x + 8, y + 12, 30, 1)
  screen.fill()
  local n = floor(t * 12)
  for k = 0, 5 do
    local v = hash(n * 7 + k)
    if v < 0.25 + 0.75 * e then
      fill_rect(x + 9 + floor(hash(n * 13 + k) * 28), y + 12, 2, 1, 6 + 9 * e)
    end
  end
end

-- Filament: a bow drawn back and forth over four strings and a bridge,
-- the string under it shaking while a note plays.
function M.Filament(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local strings = {x + 13, x + 18, x + 23, x + 28}
  local bowed = held > 0 and 3 or 2
  -- the bow's place, slow idle, longer strokes when played
  local rate = held > 0 and 1.6 or 0.4
  local bx = x + rnd(5 + (3 + 2 * e) * sin(t * rate))
  -- the strings over the bridge
  for k, sx in ipairs(strings) do
    local a = (k == bowed) and (0.4 + 1.2 * e) or 0.25 * e
    local ph = sin(t * (37 + k * 5))
    lit(k == bowed and 9 + 6 * e or 7)
    for yy = y, y + h - 5 do
      local f = (yy - y) / (h - 5)
      rect(sx + rnd(a * ph * sin(pi * f)), yy, 1, 1)
    end
    screen.fill()
  end
  -- the bridge
  lit(8)
  rect(x + 11, y + h - 4, 20, 1)
  rect(x + 12, y + h - 3, 3, 2)
  rect(x + 27, y + h - 3, 3, 2)
  screen.fill()
  -- the bow, stick, hair and frog
  local by = y + 7
  fill_rect(bx, by, 30, 1, 12)
  fill_rect(bx + 2, by + 2, 27, 1, 6 + 6 * e)
  fill_rect(bx, by, 3, 3, 14)
  fill_rect(bx + 29, by + 1, 1, 2, 12)
  -- rosin off the hair while played
  if e > 0.1 then
    for k = 0, 2 do
      local p = (t * 2 + k / 3) % 1
      fill_rect(strings[bowed] + (k - 1) * 2, by + 3 + rnd(4 * p), 1, 1, (12 - 9 * p) * e)
    end
  end
end

-- Manual: an organ's drawbars over its keys, still when idle; a chord goes
-- down and lights while held and the drawbars light with the level.
local DRAWBARS = {8, 8, 6, 0, 3, 0, 2, 0, 4}
local DRAWBAR_LV = {9, 9, 14, 14, 5, 14, 5, 5, 14}
local ORGAN_BLACK = {0, 1, 3, 4, 5, 7, 8}
local CHORD = {3, 5, 7}
function M.Manual(x, y, w, h, t, e, held)
  -- the drawbars out of their slot, pulled by their registration
  fill_rect(x + 1, y, 37, 1, 6)
  for k, n in ipairs(DRAWBARS) do
    local bx = x + 2 + (k - 1) * 4
    fill_rect(bx + 1, y + 1, 1, n, 2 + 6 * e)
    fill_rect(bx, y + 1 + n, 3, 2, DRAWBAR_LV[k] * (0.45 + 0.55 * e))
  end
  -- the keys, up to three of a chord down and lit while held
  local down = {}
  for k = 1, min(held, 3) do down[CHORD[k]] = true end
  for k = 0, 9 do
    local d = down[k] and 1 or 0
    fill_rect(x + k * 4, y + 14 + d, 3, 8 - d, down[k] and 15 or 6)
  end
  lit(1)
  for _, b in ipairs(ORGAN_BLACK) do rect(x + b * 4 + 2, y + 14, 3, 4) end
  screen.fill()
end

-- Hammer: piano keys and the hammer over them striking a string on a
-- note and checked under it while the key is down, the string ringing.
local WHITE = 8
local BLACK = {1, 2, 4, 5, 6}
function M.Hammer(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local since, n = since_on("Hammer", held, t)
  local key = (n * 3) % WHITE
  local down = held > 0
  -- the strings
  local a = 1.4 * e * exp(-since * 2)
  local ph = sin(t * 45)
  lit(9)
  rect(x, y + 1, w, 1)
  screen.fill()
  lit(8 + 6 * e)
  curve(x, x + w - 1, function(cx)
    return y + 3 + a * ph * sin(pi * (cx - x) / (w - 1))
  end)
  screen.fill()
  -- the hammer flies up to the string and falls back
  local kx = x + key * 5
  local lift = 0
  if since < 0.08 then lift = since / 0.08
  elseif since < 0.25 then lift = 1 - (since - 0.08) / 0.17 * 0.5
  elseif held > 0 then lift = 0.5 end
  local hy = y + 9 - rnd(4 * lift)
  fill_rect(kx + 2, hy + 3, 1, y + 12 - hy - 3, 6)
  fill_rect(kx + 1, hy, 3, 3, 13)
  fill_rect(kx + 1, hy, 3, 1, 15)
  -- the keys, the played one down and darker
  for k = 0, WHITE - 1 do
    local pressed = down and k == key
    fill_rect(x + k * 5, y + 13 + (pressed and 1 or 0), 4, 9 - (pressed and 1 or 0), pressed and 7 or 11)
  end
  for _, b in ipairs(BLACK) do
    fill_rect(x + b * 5 - 2, y + 13, 3, 5, 0)
    fill_rect(x + b * 5 - 1, y + 13, 1, 4, 3)
  end
end

-- Cipher: a vocoder, a voice's wave on the left going into the filter
-- bank and out as band blocks on the right.
function M.Cipher(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local mid = y + 11
  local amp = 1.5 + 6 * e
  -- the voice
  lit(8 + 7 * e)
  curve(x, x + 13, function(cx)
    local p = (cx - x) / 4 + t * 3
    return mid - amp * (0.6 * sin(p) + 0.4 * sin(p * 2.7 + 1)) * (0.6 + 0.4 * sin(t * 1.3))
  end)
  screen.fill()
  -- the filter bank, a stack of short bars
  lit(6)
  for k = 0, 6 do rect(x + 16, y + 1 + k * 3, 3, 2) end
  screen.fill()
  lit(3)
  rect(x + 14, mid, 2, 1)
  rect(x + 19, mid, 2, 1)
  screen.fill()
  -- the bands, stacked blocks rising with the voice
  for b = 0, 7 do
    local bx = x + 22 + b * 2.3
    local wob = 0.5 + 0.5 * sin(t * (2.1 + b * 0.9) + b * 1.7)
    local env = 0.15 + 0.85 * e * (0.4 + 0.6 * wob) * (1 - b / 14)
    local blocks = max(1, rnd(env * 7))
    for k = 0, blocks - 1 do
      fill_rect(rnd(bx), y + h - 2 - k * 3, 2, 2, k == blocks - 1 and 15 or 7 + k)
    end
  end
end

-- Cumulus: clouds drifting, the big one with a spectrum glowing inside it
-- that rises with the level.
local function cloud(cx, cy, s)
  disc(cx, cy, rnd(6 * s), rnd(3 * s))
  disc(cx - rnd(5 * s), cy + 1, rnd(4 * s), rnd(2 * s))
  disc(cx + rnd(6 * s), cy + 1, rnd(4 * s), rnd(2 * s))
  disc(cx + rnd(1 * s), cy - rnd(2 * s), rnd(4 * s), rnd(3 * s))
end
function M.Cumulus(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  -- a small cloud behind, drifting the other way
  lit(2)
  cloud(x + 30 - rnd(4 * sin(t * 0.21)), y + 5, 0.7)
  screen.fill()
  -- the big cloud
  local cx, cy = x + 18 + rnd(3 * sin(t * 0.3)), y + 12
  lit(5)
  cloud(cx, cy, 1.4)
  screen.fill()
  -- the spectrum inside it
  for b = 0, 9 do
    local bx = cx - 10 + b * 2
    local wob = 0.5 + 0.5 * sin(t * (1.1 + b * 0.37) + b * 2.3)
    local n = rnd(1 + (1 + 6 * e) * wob)
    fill_rect(bx, cy + 3 - n, 1, n, 9 + 6 * e)
  end
  -- a flat underside
  fill_rect(cx - 13, cy + 4, 26, 1, 8)
end

-- Nexus: a row of jacks with two patch cables hanging from them, one
-- inside the other; the plugs and cables light while played.
local NEXUS_JACKS = {4, 12, 20, 28, 36}
local NEXUS_CABLES = {{1, 4, 13, 0.0}, {2, 3, 6, 1.9}}
function M.Nexus(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local jy = y + 3
  fill_rect(x, y, w, 1, 2)
  lit(6)
  for _, jx in ipairs(NEXUS_JACKS) do ring(x + jx, jy, 2) end
  screen.fill()
  -- the cables, sagging between their plugs, swaying a little with the level
  for _, c in ipairs(NEXUS_CABLES) do
    local ax, bx = x + NEXUS_JACKS[c[1]], x + NEXUS_JACKS[c[2]]
    local sag = c[3] + 1.2 * e * sin(t * 2.5 + c[4])
    lit(5 + 8 * e)
    curve(ax, bx, function(cx)
      local f = (cx - ax) / (bx - ax)
      return jy + 2 + sag * 4 * f * (1 - f)
    end)
    screen.fill()
    for _, px in ipairs({ax, bx}) do
      fill_rect(px - 1, jy - 1, 3, 3, 8 + 4 * e)
      fill_rect(px, jy, 1, 1, 3 + 12 * e)
    end
  end
end

-- Diction: lips, closed and breathing when idle, opening into vowel shapes
-- while played, wide, round and small.
local VOWELS = {{9, 4}, {10, 2}, {11, 1}, {6, 4}, {4, 3}}
function M.Diction(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local cx, cy = x + 20, y + 11
  local rx, ry = 10, 0
  if e > 0.05 or held > 0 then
    local p = t * 2.5
    local k = floor(p) % #VOWELS
    local v0, v1 = VOWELS[k + 1], VOWELS[(k + 1) % #VOWELS + 1]
    local f = p - floor(p)
    f = f * f * (3 - 2 * f)
    local o = max(e, held > 0 and 0.5 or 0)
    rx = rnd(v0[1] + (v1[1] - v0[1]) * f)
    ry = rnd(o * (v0[2] + (v1[2] - v0[2]) * f))
  end
  local breathe = rnd(0.5 + 0.5 * sin(t * 0.9))
  -- the lips round the opening
  lit(9)
  disc(cx, cy, rx + 3, ry + 4 + breathe)
  screen.fill()
  lit(12)
  rect(cx - rx + 1, cy - ry - 3 - breathe, 2 * rx - 1, 1)
  screen.fill()
  -- the bow of the top lip
  fill_rect(cx, cy - ry - 4 - breathe, 1, 2, 4)
  if ry > 0 then
    lit(0)
    disc(cx, cy, rx, ry)
    screen.fill()
    -- the teeth and the tongue
    fill_rect(cx - rx + 2, cy - ry, 2 * rx - 3, 1, 13)
    if ry > 1 then fill_rect(cx - rx + 3, cy + ry - 1, 2 * rx - 5, 1, 5) end
  else
    fill_rect(cx - rx - 2, cy, 2 * rx + 5, 1, 2)
    fill_rect(cx - rx - 3, cy - 1, 1, 1, 6)
    fill_rect(cx + rx + 3, cy - 1, 1, 1, 6)
  end
end

-- Forage: a drum kit, hi-hat, snare, toms, kick and cymbal, each lighting
-- on its hits of a beat while played.
local KIT_BEAT = {"kick", "hat", "snare", "hat", "kick", "kick", "snare", "tom"}
function M.Forage(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local step = floor(t * 7) % #KIT_BEAT
  local hit = (e > 0.05) and KIT_BEAT[step + 1] or nil
  local fresh = (t * 7) % 1
  local function lv(name, base)
    if name == hit then return base + (15 - base) * e * (1 - 0.5 * fresh) end
    if name == "hat" and step % 2 == 1 and e > 0.05 then return base + 4 * e end
    return base
  end
  -- the kick, from the front
  lit(lv("kick", 3))
  disc(x + 20, y + 15, 6, 6)
  screen.fill()
  lit(10)
  ring(x + 20, y + 15, 6, 6)
  screen.fill()
  -- the toms on it
  for k, tx in ipairs({x + 16, x + 24}) do
    lit(lv("tom", 4))
    rect(tx - 3, y + 6, 7, 3)
    screen.fill()
    fill_rect(tx - 3, y + 5, 7, 1, 12)
  end
  -- the snare and its stand
  lit(lv("snare", 4))
  rect(x + 4, y + 13, 9, 3)
  screen.fill()
  fill_rect(x + 4, y + 12, 9, 1, 12)
  lit(5)
  line(x + 8, y + 16, x + 5, y + 21)
  line(x + 8, y + 16, x + 11, y + 21)
  screen.fill()
  -- the hi-hat on its stand, opening a little on its hits
  local gap = (hit == "hat" or (e > 0.05 and step % 2 == 1)) and 1 or 0
  lit(lv("hat", 9))
  rect(x + 0, y + 4, 9, 1)
  rect(x + 0, y + 5 + gap, 9, 1)
  screen.fill()
  fill_rect(x + 4, y + 6 + gap, 1, 6 - gap, 5)
  -- the cymbal over the right, shimmering
  local tilt = (e > 0.05 and hit == "snare") and 1 or 0
  lit(lv("snare", 8))
  line(x + 30, y + 3 + tilt, x + 39, y + 2)
  screen.fill()
  fill_rect(x + 35, y + 3, 1, h - 3, 5)
  -- the kick's pedal
  fill_rect(x + 19, y + 21, 3, 1, hit == "kick" and 15 or 6)
end

-- Mosaic: a grid of tiles, key zones across and layers up; a note lights
-- its zone with the layer for its level brightest, idle a tile glints.
function M.Mosaic(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local since, n = since_on("Mosaic", held, t)
  local zone = floor(hash(n + 3) * 8)
  local layer = min(3, floor(e * 4))
  local glint = floor(t * 0.8)
  local gi = floor(hash(glint) * 32)
  for r = 0, 3 do
    for c = 0, 7 do
      local lv = 2 + ((r + c) % 2)
      if gi == r * 8 + c then lv = 2 + 6 * sin(pi * (t * 0.8 % 1)) end
      if e > 0.03 and c == zone then
        lv = max(lv, 4 + 4 * e)
        if 3 - r == layer then lv = 9 + 6 * e end
      elseif e > 0.03 and abs(c - zone) == 1 and 3 - r == layer then
        lv = max(lv, 3 + 4 * e)
      end
      fill_rect(x + c * 5, y + 1 + r * 5, 4, 4, lv)
    end
  end
end

-- Dice: a loop cut into slices that swap places on the beat while
-- played, over two dice that tumble.
local SLICES = {
  function(p) return exp(-p * 5) end,
  function(p) return 0.6 * exp(-p * 2) end,
  function(p) return exp(-p * 9) * 0.9 + 0.2 end,
  function(p) return 0.4 + 0.4 * exp(-p * 3) end,
}
local ORDERS = {{1, 2, 3, 4}, {3, 1, 4, 2}, {2, 4, 1, 3}, {4, 3, 2, 1}, {1, 3, 2, 4}}
local PIPS = {
  {{1, 1}}, {{0, 0}, {2, 2}}, {{0, 0}, {1, 1}, {2, 2}},
  {{0, 0}, {2, 0}, {0, 2}, {2, 2}}, {{0, 0}, {2, 0}, {1, 1}, {0, 2}, {2, 2}},
  {{0, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {2, 2}},
}
function M.Dice(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local beat = floor(t * 4)
  local playing = e > 0.05
  local order = ORDERS[playing and (beat % #ORDERS + 1) or 1]
  local now = floor((t * 4) % 4)
  for k = 0, 3 do
    local sx = x + k * 10
    local f = SLICES[order[k + 1]]
    local here = playing and k == now
    lit(here and 15 or 8)
    for c = 0, 8 do
      local p = c / 9
      local a = rnd(4 * f(p) * (0.6 + 0.4 * hash(c * 5 + order[k + 1])))
      rect(sx + c, y + 5 - a, 1, 2 * a + 1)
    end
    screen.fill()
    fill_rect(sx + 9, y, 1, 11, 3)
  end
  -- the dice, tumbling while played
  local roll = playing and floor(t * 10) or floor(t * 0.25)
  for d = 0, 1 do
    local dx, dy = x + 9 + d * 13, y + 13
    local jig = (playing and (roll + d) % 2 == 0) and -1 or 0
    fill_rect(dx + 1, dy + jig, 7, 9, 10)
    fill_rect(dx, dy + 1 + jig, 9, 7, 10)
    fill_rect(dx + 1, dy + 1 + jig, 7, 7, 12)
    local face = PIPS[floor(hash(roll * 2 + d) * 6) + 1]
    for _, p in ipairs(face) do fill_rect(dx + 2 + p[1] * 2, dy + 2 + jig + p[2] * 2, 1, 1, 0) end
  end
end

-- Pollen: grains drifting up off a flower on the breeze, gathering into a
-- swirl and brightening with the level.
local GRAINS = {}
for k = 1, 18 do GRAINS[k] = {hash(k) * 40, hash(k + 50) * 22, 0.6 + hash(k + 99) * 1.4, hash(k + 7) * 6} end
function M.Pollen(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  -- the flower in the corner
  fill_rect(x + 4, y + 16, 1, 6, 5)
  lit(7)
  ring(x + 4, y + 15, 3, 2)
  screen.fill()
  fill_rect(x + 3, y + 14, 3, 3, 12)
  local sx, sy = x + 24 + 4 * sin(t * 0.4), y + 10 + 2 * cos(t * 0.5)
  for k, g in ipairs(GRAINS) do
    local gx = (g[1] + t * g[3] * 2) % 40
    local gy = (g[2] - t * g[3] + 2 * sin(t * 0.7 + g[4])) % 22
    local a = t * (1 + 2 * e) + k * 2 * pi / #GRAINS
    local r = 3 + (k % 3) * 2.5
    local cx, cy = sx + r * cos(a), sy + r * 0.6 * sin(a)
    local px = rnd(x + gx + (cx - x - gx) * e * 0.85)
    local py = rnd(y + gy + (cy - y - gy) * e * 0.85)
    local big = k % 4 == 0
    lit((big and 9 or 5) + 6 * e)
    rect(px, py, big and 2 or 1, big and 2 or 1)
    screen.fill()
  end
end

-- Molt: a mic and its take's waveform, with an old skin of the wave
-- peeling off it and drifting away; faster and louder while played.
function M.Molt(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  -- the mic, a grille head on a stand
  lit(9)
  disc(x + 5, y + 5, 4, 4)
  screen.fill()
  lit(3)
  for k = 0, 2 do rect(x + 2, y + 3 + k * 2, 7, 1) end
  screen.fill()
  fill_rect(x + 4, y + 10, 3, 4, 7)
  fill_rect(x + 5, y + 14, 1, 6, 5)
  fill_rect(x + 2, y + 20, 7, 1, 7)
  local mid = y + 12
  local x0, x1 = x + 13, x + w - 1
  local function env(cx)
    local p = (cx - x0) / (x1 - x0)
    return (1.5 + 6 * e) * (0.4 + 0.6 * sin(pi * p)) * (0.6 + 0.4 * sin(cx * 0.9 + t * 3))
  end
  -- the old skin, lifting off and fading
  local p = (t * (0.3 + 0.7 * e)) % 1
  local lift = rnd(1 + 8 * p)
  lit(7 * (1 - p) + 1)
  curve(x0 + rnd(10 * p), x1, function(cx) return mid - env(cx) - lift - rnd((cx - x0) * p * 0.2) end)
  screen.fill()
  -- the take
  lit(10 + 5 * e)
  for cx = x0, x1, 2 do
    local a = rnd(env(cx))
    rect(cx, mid - a, 1, 2 * a + 1)
  end
  screen.fill()
end

-- Divisi: a string ensemble on a stage, the choir on the back riser,
-- violins in the middle and cellos at the front. Still when idle; on a note
-- the players come in one after another, lighting with the level, and bow
-- together.
local DIVISI_CHOIR = {3, 8, 13, 18, 23, 28, 33}
local DIVISI_VIOLIN = {3, 12, 21, 30}
local DIVISI_CELLO = {3, 16, 29}
local function player(px, py, lv)
  fill_rect(px, py, 2, 2, lv)
  fill_rect(px - 1, py + 2, 4, 2, lv * 0.65)
end
function M.Divisi(x, y, w, h, t, e, held)
  e = max(e, held > 0 and 0.6 or 0)
  local since = since_on("Divisi", held, t)
  local seat = 0
  -- a player's level, each coming in a little after the last on a note
  local function lv(base)
    seat = seat + 1
    local d = held > 0 and util.clamp((since - 0.25 * hash(seat)) / 0.15, 0, 1) or 1
    return base + 9 * e * d
  end
  -- the risers
  lit(3)
  rect(x, y + 6, w, 1)
  rect(x, y + 13, w, 1)
  screen.fill()
  for _, px in ipairs(DIVISI_CHOIR) do player(x + px, y + 1, lv(5)) end
  -- the bows' stroke, all together
  local bow = e > 0.1 and rnd(1.2 * sin(t * 2.2)) or 0
  -- violins, a bow over the right shoulder
  for _, px in ipairs(DIVISI_VIOLIN) do
    local l = lv(6)
    player(x + px, y + 8, l)
    local b = max(0, bow)
    lit(l)
    line(x + px + 2 + b, y + 11 - b, x + px + 5 + b, y + 8 - b)
    screen.fill()
  end
  -- cellos, the instrument beside the player and the bow across it
  for _, px in ipairs(DIVISI_CELLO) do
    local l = lv(7)
    player(x + px, y + 15, l)
    fill_rect(x + px + 5, y + 14, 1, 2, l * 0.6)
    fill_rect(x + px + 4, y + 16, 3, 5, l * 0.7)
    fill_rect(x + px + 5, y + 21, 1, 1, l * 0.6)
    fill_rect(x + px + 3 + bow, y + 18, 5, 1, l + 2)
  end
end

return M
