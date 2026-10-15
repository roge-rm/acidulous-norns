-- The OMX-27 in REMOTE mode (lib/omx.lua), as a surface for Acidulous.
--   bottom keys 12-26    white keys, two octaves C to C
--   top keys 1-10        black keys
--   AUX (0) held:        keys 1 and 2 octave down and up, keys 12-19 pick part 1-8
--   pots 0-4             the selected part's macros, with pickup
--   encoder              patch; held and turned, machine; a tap, norns page

local omx = include("lib/omx")

local surf = {name = "OMX-27"}
surf.actions = nil -- set by lib/surfaces.lua

local OCT_DOWN, OCT_UP = 1, 2
local PART_KEY = 12 -- AUX + keys 12-19 pick parts 1-8
local BASE = 48     -- the lowest key's note at octave 0
local FLASH = 1.2   -- seconds a turned pot shows on the screen
local PICKUP = 0.02 -- a pot this close to the value takes over

-- OMX key -> semitones above the lowest key, and which are black
local SEMI, BLACK = {}, {}
do
  local w, b = 0, 0
  for k = 0, 24 do
    local pc = k % 12
    if pc == 1 or pc == 3 or pc == 6 or pc == 8 or pc == 10 then
      SEMI[1 + b], BLACK[1 + b] = k, true
      b = b + 1
    else
      SEMI[12 + w] = k
      w = w + 1
    end
  end
end

-- one colour a part, r g b 0-255
surf.COLOURS = {
  {255, 0, 0}, {255, 110, 0}, {255, 230, 0}, {0, 255, 40},
  {0, 220, 255}, {0, 40, 255}, {160, 0, 255}, {255, 0, 140},
}

surf.octave = 0
local aux = false
local held = {}     -- OMX key -> {part, note} it's playing
local enc_down = false
local enc_turned = false
-- pots: last position (0-1), the param it holds and the value it last set
local pots = {}
-- the last pot turned, for the screen: {k (1-5), pos, waiting, at}
surf.flash = nil

surf.match = omx.is_omx
function surf.connected() return omx.port_open() end
function surf.owns(d) return omx.owns(d) end
function surf.remote() return omx.is_remote() end
function surf.wants_screen() return omx.is_remote() end
surf.screen_send = omx.screen_send

-- Note offs for every key down.
local function release_all()
  for n, h in pairs(held) do
    surf.actions.note(h.part, h.note, 0)
    held[n] = nil
  end
end

local function reset()
  aux, enc_down, enc_turned = false, false, false
  pots = {}
  surf.flash = nil
end

function surf.lost()
  release_all()
  local port = omx.port()
  omx.lost()
  reset()
  surf.actions.port_freed(port)
end

function surf.disconnect()
  release_all()
  local port = omx.port()
  omx.disconnect()
  reset()
  surf.actions.port_freed(port)
end

-- ---- what's played --------------------------------------------------------------

function surf.key(n, ev)
  local a = surf.actions
  local z = ev == "down" and 1 or (ev == "up" and 0 or nil)
  if z == nil then return end
  if n == 0 then
    aux = z == 1
    return
  end
  if z == 0 then
    local h = held[n]
    if h then
      a.note(h.part, h.note, 0)
      held[n] = nil
    end
    return
  end
  if aux then
    if n == OCT_DOWN then surf.octave = math.max(surf.octave - 1, -4)
    elseif n == OCT_UP then surf.octave = math.min(surf.octave + 1, 4)
    elseif n >= PART_KEY and n < PART_KEY + 8 then a.select(n - PART_KEY + 1) end
    return
  end
  if SEMI[n] and not held[n] then
    local part = a.selected()
    local note = util.clamp(BASE + 12 * surf.octave + SEMI[n], 0, 127)
    held[n] = {part = part, note = note}
    a.note(part, note, a.velocity())
  end
end

function surf.enc(d)
  local a = surf.actions
  if enc_down then
    enc_turned = true
    a.machine(d)
  else
    a.patch(d)
  end
end

function surf.enc_btn(z)
  if z == 1 then
    enc_down, enc_turned = true, false
  else
    if enc_down and not enc_turned then surf.actions.page() end
    enc_down = false
  end
end

-- pos 0-1. A pot only sets its macro once it reaches the macro's value (or
-- passes it), so a new part or patch doesn't jump. It then holds it until
-- the value is changed some other way.
function surf.pot(n, pos)
  local a = surf.actions
  local p = pots[n] or {}
  pots[n] = p
  if p.last and math.abs(pos - p.last) < 0.002 then return end -- jitter
  local last = p.last
  p.last = pos
  local mac = a.macros()[n + 1]
  if not mac or not mac.slot then return end
  local sid = a.slot_id(mac.slot)
  local cur = params:get(sid)
  local holding = p.sid == sid and p.set and math.abs(cur - p.set) < 1e-4
  if not holding then
    holding = math.abs(pos - cur) < PICKUP or (last and (last - cur) * (pos - cur) <= 0)
  end
  surf.flash = {k = n + 1, pos = pos, waiting = not holding, at = util.time()}
  if not holding then
    p.sid, p.set = nil, nil
    return
  end
  local v = pos
  local def = mac.def
  if def.curve == "step" and def.steps > 1 then
    v = math.floor(v * (def.steps - 1) + 0.5) / (def.steps - 1)
  end
  params:set(sid, v)
  p.sid, p.set = sid, params:get(sid)
end

-- the turned pot, while it's still showing
function surf.showing()
  local f = surf.flash
  if f and util.time() - f.at < FLASH then return f end
end

function surf.connect()
  omx.key = surf.key
  omx.enc = surf.enc
  omx.enc_btn = surf.enc_btn
  omx.pot = function(n, v, hires) surf.pot(n, hires / 16383) end
  omx.midi = function(data, port) surf.actions.midi(port, data) end
  omx.old_firmware = function(version) surf.actions.old_firmware(version) end
  omx.answered = function() surf.actions.answered() end
  release_all()
  reset()
  local ok = omx.connect()
  if ok then print("omx-27: connected on port " .. tostring(omx.port())) end
  return ok
end

-- ---- LEDs and screen ---------------------------------------------------------------

local function scaled(c, f)
  return math.floor(c[1] * f), math.floor(c[2] * f), math.floor(c[3] * f)
end

function surf.frame()
  local a = surf.actions
  local sel = a.selected()
  local colour = surf.COLOURS[sel]
  for n = 1, 26 do omx.led(n, 0, 0, 0) end
  omx.led(0, scaled(colour, 0.6))
  if aux then
    omx.led(OCT_DOWN, scaled({255, 255, 255}, surf.octave < 0 and 0.8 or 0.12))
    omx.led(OCT_UP, scaled({255, 255, 255}, surf.octave > 0 and 0.8 or 0.12))
    for i = 1, 8 do
      local f = i == sel and 1 or (a.has_machine(i) and 0.12 or 0.03)
      omx.led(PART_KEY + i - 1, scaled(surf.COLOURS[i], f))
    end
  else
    for n in pairs(SEMI) do
      local f = held[n] and 1 or (BLACK[n] and 0.04 or 0.14)
      omx.led(n, scaled(colour, f))
    end
  end
  omx.led_show()
end

function surf.draw()
  surf.actions.draw(surf)
end

return surf
