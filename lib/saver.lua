-- The screensaver, an acid bath: ridge lines stacked in depth, each a
-- resonant 303 wave, coming slowly towards you. The filter sweeps itself, so
-- the newest lines at the back go from a soft saw to a squelchy ringing wave
-- and back. Each part with a machine raises a hump at its own place across
-- the ridges, higher while it plays, and now and then a drop from the app's
-- icon falls in and ripples the front lines. Drawn on the norns screen and,
-- with fewer lines, on the OMX-27's 128x32, and the ridges roll down the
-- OMX's keys too.

local floor, sin, cos, exp, sqrt, atan, abs, min, max =
  math.floor, math.sin, math.cos, math.exp, math.sqrt, math.atan, math.abs, math.min, math.max
local pi = math.pi
local TAU = 2 * pi

local saver = {}

saver.delay = 180 -- seconds without input

local last_input = util.time()
local began = nil -- when the current saver began
local fresh       -- clears what's left from the last run, below

function saver.touch()
  last_input = util.time()
  began = nil
  -- norns blanks its own screen after a while without its keys or encoders
  screen.ping()
end

function saver.active()
  return util.time() - last_input > saver.delay
end

-- Seconds since the saver began.
function saver.time()
  local now = util.time()
  if not began then
    began = now
    fresh()
  end
  return now - began
end

local function rnd(v) return floor(v + 0.5) end

-- ---- the wave -----------------------------------------------------------------

-- A saw through a 2-pole resonant low pass, as harmonics, one period of it.
-- The cutoff and resonance sweep slowly between a soft saw and a squelchy
-- ringing wave.
local HARM = 12
local W = 128
local PERIOD = 46 -- pixels a cycle

-- How far into the squelch, 0-1, at time t, a little further with energy.
local function morph(t, e)
  local m = 0.5 - 0.5 * cos(TAU * t / 26)
  m = m * (0.85 + 0.15 * sin(TAU * t / 61))
  return min(1, m + 0.3 * e)
end

local bs, cs = {}, {}

-- Fills tab[0 .. PERIOD-1] with one period of the wave at time t, -1 to 1.
local function make_wave(tab, t, e)
  local m = morph(t, e)
  local fc = 2.2 + 4.6 * m       -- cutoff, in harmonics
  local q = 0.7 + 5.5 * m * m    -- resonance
  for n = 1, HARM do
    local r = n / fc
    local re, im = 1 - r * r, r / q
    local mag = 1 / sqrt(re * re + im * im)
    local ph = -atan(im, re)
    local a = mag / n
    -- a falling saw: harmonic n is sin(n th) / n
    bs[n] = a * cos(ph)
    cs[n] = a * sin(ph)
  end
  local peak = 1e-6
  for c = 0, PERIOD - 1 do
    local th = TAU * c / PERIOD
    local v = 0
    for n = 1, HARM do v = v + bs[n] * sin(n * th) + cs[n] * cos(n * th) end
    tab[c] = v
    if v > peak then peak = v elseif -v > peak then peak = -v end
  end
  for c = 0, PERIOD - 1 do tab[c] = tab[c] / peak end
end

-- ---- the drop ---------------------------------------------------------------

-- The middle drip under the app's icon (view.lua's SPLASH), as hex levels.
local DROP = {
  "cff2",
  "9ff0",
  "aff0",
  "cff2",
  "eff5",
  "2fff8",
  "5fffa",
  "6fffb",
  "2eff6",
  "0153",
}
local DROP_X = {1, 1, 1, 1, 1, 0, 0, 0, 0, 0} -- each row's offset
local GRAVITY = 30 -- pixels a second squared, on the norns screen
local HIT_Y = 44   -- where it lands, on the norns screen
local drop = nil   -- {x, at, y while falling, hit (time it landed)}
local next_drop = 25

local function plan_drop(t)
  drop = {x = 20 + math.random() * 88, at = t}
  next_drop = t + 150 + math.random() * 120
end

-- Moves the drop on, once a frame. The ripple's age, or nil.
local frame_t = nil
local ripple_age = nil
local function step(t)
  if frame_t == t then return end
  frame_t = t
  if not drop and t >= next_drop then plan_drop(t) end
  ripple_age = nil
  if not drop then return end
  if not drop.hit then
    local age = t - drop.at
    drop.y = -12 + 0.5 * GRAVITY * age * age
    if drop.y + 9 >= HIT_Y then drop.hit, drop.y = drop.at + age, nil end
  else
    ripple_age = t - drop.hit
    if ripple_age > 7 then drop, ripple_age = nil, nil end
  end
end

local function draw_drop(L)
  if not drop or not drop.y then return end
  local x, y = rnd(drop.x), rnd(drop.y * L.h / 64)
  if L.h < 64 then
    -- small: just the bright middle
    screen.level(15)
    screen.rect(x + 1, y + 1, 2, 3)
    screen.rect(x, y + 3, 4, 2)
    screen.fill()
    return
  end
  for row, cols in ipairs(DROP) do
    for k = 1, #cols do
      local lv = tonumber(cols:sub(k, k), 16)
      if lv > 0 then
        screen.level(lv)
        screen.rect(x + DROP_X[row] + k - 1, y + row - 1, 1, 1)
        screen.fill()
      end
    end
  end
end

-- ---- the ridges ---------------------------------------------------------------

-- The layouts: the norns screen and the OMX-27's, where only level 8 and
-- up is lit. Lines run from the horizon at depth 0 to below the bottom
-- edge at depth 1, `life` seconds later.
local function layout(h, lines, horizon, amp, amp_e, hump, hump_e, sigma, grey)
  local L = {h = h, lines = lines, horizon = horizon, amp = amp, amp_e = amp_e,
    hump = hump, hump_e = hump_e, sigma = sigma, grey = grey,
    life = lines * 1.8, bottom = h + amp + amp_e + hump + hump_e + 6,
    waves = {}, gen = {}, gauss = {}, reach = floor(3 * sigma)}
  for k = 0, lines - 1 do L.waves[k] = {}; L.gen[k] = nil end
  for dx = 0, L.reach do L.gauss[dx] = exp(-dx * dx / (2 * sigma * sigma)) end
  return L
end
local NORNS = layout(64, 12, 6, 3, 3, 7, 12, 6, true)
local OMX = layout(32, 6, 2, 1.5, 1.5, 4, 6, 5, false)

-- each part's hump across the screen, spread out for the first few parts
local HUMP_X = {40, 90, 64, 16, 112, 28, 78, 102, 52, 8, 120, 34, 70, 96, 22, 58}

local ys, low, lift = {}, {}, {}
local hx, he = {}, {} -- this frame's humps: x and energy, by part
local humps = 0

-- Where each part's hump is and how much it plays.
local function find_humps(parts, t)
  humps = 0
  for i, part in ipairs(parts.list) do
    if part.machine and HUMP_X[i] and parts.on(i) then
      humps = humps + 1
      hx[humps] = HUMP_X[i] + 3 * sin(t * 0.21 + i * 2.3)
      he[humps] = part.energy or 0
    end
  end
end

local DRIFT = 0.12 * PERIOD -- pixels a second the waves drift left

-- Line k's depth, 0 back to 1 front, at pos (lines passed since the start).
-- Each line's wave is made when it starts at the back.
local function line_depth(L, k, pos, t, e)
  local n = L.lines
  local p = pos + k
  local gen = floor(p / n)
  local d = (p % n) / n
  if L.gen[k] ~= gen then
    L.gen[k] = gen
    make_wave(L.waves[k], t - d * L.life, e)
  end
  return d
end

-- Draws line k of L at depth d (0 back, 1 front). Fills under it with black
-- first, down to the lowest point drawn so far, so it hides what's behind.
local function draw_ridge(L, k, d, t, e, shift)
  local fade = min(1, d / 0.12)        -- grows out from the middle at the back
  local half = fade * W / 2
  local c0, c1 = max(0, floor(W / 2 - half)), min(W - 1, floor(W / 2 + half))
  if c1 < c0 then return end
  local depth = d * d
  local base = L.horizon + (L.bottom - L.horizon) * depth
  local near = 0.2 + 0.8 * d
  local amp = (L.amp + L.amp_e * e) * near
  local tab = L.waves[k]
  local off = shift + (L.gen[k] or 0) * 17 + k * 7
  -- humps, each line a little different
  for c = c0, c1 do lift[c] = 0 end
  for j = 1, humps do
    local hh = (L.hump + L.hump_e * he[j]) * near * (0.75 + 0.25 * sin((L.gen[k] or 0) * 1.3 + k + j * 2.1))
    local cx = rnd(hx[j])
    for c = max(c0, cx - L.reach), min(c1, cx + L.reach) do
      lift[c] = lift[c] + hh * L.gauss[abs(c - cx)]
    end
  end
  -- the drop's ripple, on the front lines
  local ra = ripple_age and d > 0.55 and (d - 0.55) / 0.45 or 0
  local rs = L.h / 64
  for c = c0, c1 do
    local y = base - amp * tab[floor(c + off) % PERIOD] - lift[c]
    if ra > 0 then
      local dist = abs(c - drop.x - 2)
      if dist < ripple_age * 40 then
        y = y + ra * 5 * rs * exp(-ripple_age / 1.4 - dist / 50) * sin(dist * 0.45 - ripple_age * 9)
      end
    end
    ys[c] = rnd(y)
  end
  -- black under it, down to what's already drawn
  screen.level(0)
  for c = c0, c1 do
    local y, lo = ys[c], low[c]
    if lo > y then screen.rect(c, y + 1, 1, lo - y) end
    if y > lo then low[c] = y end
  end
  screen.fill()
  local lv = 15
  if L.grey then lv = max(1, rnd((1 + 14 * d ^ 1.4) * fade)) end
  screen.level(lv)
  local py, run = ys[c0], c0
  for c = c0 + 1, c1 + 1 do
    local y = c <= c1 and ys[c] or nil
    if y ~= py then
      screen.rect(run, py, c - run, 1)
      if y then
        if y > py + 1 then screen.rect(c, py + 1, 1, y - py - 1)
        elseif y < py - 1 then screen.rect(c, y + 1, 1, py - y - 1) end
      end
      run, py = c, y
    end
  end
  screen.fill()
end

-- Draws the saver on the norns screen (omx false) or the OMX-27's 128x32 at
-- the top left. Clears first, doesn't update. t is for previews.
function saver.draw(parts, omx, t)
  t = t or saver.time()
  local e = 0
  for _, part in ipairs(parts.list) do e = max(e, part.energy or 0) end
  local L = omx and OMX or NORNS
  screen.clear()
  step(t)
  find_humps(parts, t)
  for c = 0, W - 1 do low[c] = -1 end
  local n = L.lines
  local pos = t * n / L.life
  -- back to front
  local back = (-floor(pos)) % n -- the line nearest the horizon
  for j = 0, n - 1 do
    local k = (back + j) % n
    draw_ridge(L, k, line_depth(L, k, pos, t, e), t, e, t * DRIFT)
  end
  draw_drop(L)
end

-- For previews: a drop that left `ago` seconds before t.
function saver.drop_at(t, ago, x)
  drop = {x = x or 60, at = t - ago}
  frame_t = nil
end

-- ---- the OMX-27's keys ------------------------------------------------------------

-- The ridges roll down the keys as they come towards you on the screen: the
-- top row lights as a line reaches its depth and the bottom row after it,
-- each key as bright as the ridge at its place across the screen, and dims
-- as the line passes on.

-- Each key's place across the 128 columns: the bottom keys 11-26 (B, then C
-- to C two octaves up) 8 apart, the top keys 1-10 between the white keys
-- either side of them.
local KEY_X = {}
do
  local white, black = 0, 0
  for semi = -1, 24 do
    local pc = semi % 12
    if pc == 1 or pc == 3 or pc == 6 or pc == 8 or pc == 10 then
      black = black + 1
      KEY_X[black] = white * 8 -- the edge between its two white keys
    else
      KEY_X[11 + white] = white * 8 + 4
      white = white + 1
    end
  end
end
-- The rows' depths, half a line apart on the norns layout, and the depth
-- over which a line lights a row and then leaves it: short, so each line
-- passes as its own pulse.
local TOP_D = 0.6
local BOTTOM_D = TOP_D + 0.5 / NORNS.lines
local RISE, FALL = 0.25 / NORNS.lines, 0.75 / NORNS.lines

-- How lit a row at depth row_d is by a line at depth d, 0-1.
local function passing(d, row_d)
  local dd = d - row_d
  if dd < -RISE or dd > FALL then return 0 end
  if dd < 0 then return 1 + dd / RISE end
  return 1 - dd / FALL
end

-- Line k's height at column x on the norns layout, 0-1 of the most it can be.
local function height(L, k, x, e, shift)
  local g = L.gen[k] or 0
  local v = (0.5 + 0.5 * L.waves[k][floor(x + shift + g * 17 + k * 7) % PERIOD]) * (L.amp + L.amp_e * e)
  for j = 1, humps do
    local dx = abs(x - rnd(hx[j]))
    if dx <= L.reach then
      v = v + (L.hump + L.hump_e * he[j]) * (0.75 + 0.25 * sin(g * 1.3 + k + j * 2.1)) * L.gauss[dx]
    end
  end
  return v / (L.amp + L.amp_e + L.hump + L.hump_e)
end

local function lit(led, colour, k, f)
  led(k, floor(colour[1] * f), floor(colour[2] * f), floor(colour[3] * f))
end

-- Sets the 27 LEDs with led(n, r, g, b), in colour {r, g, b} 0-255, in step
-- with the norns screen's ridges.
function saver.leds(parts, led, colour)
  local t = saver.time()
  local e = 0
  for _, part in ipairs(parts.list) do e = max(e, part.energy or 0) end
  local L = NORNS
  step(t)
  find_humps(parts, t)
  local n = L.lines
  local pos = t * n / L.life
  local shift = t * DRIFT
  for key = 1, 26 do
    local row_d = key <= 10 and TOP_D or BOTTOM_D
    local f = 0
    for k = 0, n - 1 do
      local w = passing(line_depth(L, k, pos, t, e), row_d)
      if w > 0 then f = max(f, w * height(L, k, KEY_X[key], e, shift)) end
    end
    -- a ridge is rarely more than 60% of its most, so that's full: about 15%
    lit(led, colour, key, 0.015 + 0.14 * min(1, f / 0.6))
  end
  lit(led, colour, 0, 0.05 + 0.02 * sin(TAU * t / 7))
end

fresh = function()
  drop, frame_t, ripple_age = nil, nil, nil
  next_drop = 20 + math.random() * 15
  for k = 0, NORNS.lines - 1 do NORNS.gen[k] = nil end
  for k = 0, OMX.lines - 1 do OMX.gen[k] = nil end
end

return saver
