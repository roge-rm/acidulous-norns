-- The OMX-27 in REMOTE mode (lib/omx.lua), as a surface for Acidulous.
--   bottom keys 11-26    white keys, B and then two octaves C to C
--   top keys 1-10        black keys
--   AUX (0) held:        keys 1 and 2 octave down and up, keys 11-18 pick part 1-8,
--                        key 5 arp on/off, key 6 latch, pots arp rate, gate,
--                        mode, octaves and chord type
--   AUX + key 3          tapped, scale on/off; held, a note key sets the root,
--                        the encoder steps scales and its tap is snap/degree
--   AUX + key 4          tapped, chord on/off; held, the encoder steps chord types
--   pots 0-4             the selected part's macros, with pickup
--   encoder              patch; held and turned, machine; a tap, norns page
-- Keys, the encoder and pots wake the screensaver and still do their job.

local omx = include("lib/omx")
local modcards = include("lib/modcards")

local surf = {name = "OMX-27"}
surf.actions = nil -- set by lib/surfaces.lua

local OCT_DOWN, OCT_UP = 1, 2
local PART_KEY = 11 -- AUX + keys 11-18 pick parts 1-8
local KEY_SCALE, KEY_CHORD, KEY_ARP, KEY_LATCH = 3, 4, 5, 6 -- with AUX
local MSG_TIME = 1.5 -- seconds a modifier change shows on the screen
-- the pots' modifier params while AUX is held
local AUX_POTS = {{"Arp", "rate"}, {"Arp", "gate"}, {"Arp", "mode"}, {"Arp", "octaves"}, {"Chord", "type"}}
-- notes in each scale, as Scales.h, for degree mode
local SCALE_NOTES = {7, 7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 6, 6, 5, 7, 7, 7, 5, 5, 5, 7, 7, 7, 7, 7,
  7, 7, 7, 8, 8, 6, 8, 8}
local KEYS = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
-- LED colours for scale, chord, arp and latch
local MOD_COLOURS = {[KEY_SCALE] = {0, 255, 60}, [KEY_CHORD] = {0, 200, 255},
  [KEY_ARP] = {255, 120, 0}, [KEY_LATCH] = {200, 0, 255}}
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
  SEMI[11] = -1 -- the bottom left key, B below the lowest C
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
local mod_key = nil    -- KEY_SCALE or KEY_CHORD while held after a press with AUX
local mod_used = false -- it was used for an edit, so letting go doesn't toggle
-- the last modifier change, for the screen: {what, type, at}
surf.msg = nil
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
  mod_key, mod_used = nil, false
  pots = {}
  surf.flash, surf.msg = nil, nil
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

-- ---- modifiers -------------------------------------------------------------------

-- A modifier param's option labels and label from lib/modcards.lua.
local function card_ctl(type, name)
  for _, card in ipairs(modcards[type] or {}) do
    for _, c in ipairs(card.c) do
      if c.p == name then return c end
    end
  end
end

-- The step a stepped ctl is on, 0 up.
local function step_of(P, i, ctl)
  return math.floor(P.mod_value(i, ctl) * (ctl.def.steps - 1) + 0.5)
end

local function option(P, i, type, name)
  local ctl = P.mod_ctl(i, type, name)
  local c = card_ctl(type, name)
  if not (ctl and c and c.o) then return "?" end
  return c.o[step_of(P, i, ctl) + 1] or "?"
end

local function say(what, type)
  surf.msg = {what = what, type = type, at = util.time()}
  surf.flash = nil
end

-- The ctl for a param of the type on the selected part, turning the type on
-- first if the part has none. key and scale can come from a Chord, as Scale
-- and Chord share them. nil if both slots are full.
local function edit_ctl(type, name, shared)
  local P, i = surf.actions.parts, surf.actions.selected()
  local t = type
  if shared and not P.mod_slot(i, t) and P.mod_slot(i, "Chord") then t = "Chord" end
  if not P.mod_slot(i, t) and P.toggle_mod(i, t) == "full" then
    say("full", t)
    return nil
  end
  return P.mod_ctl(i, t, name), P, i
end

-- Steps a stepped ctl by d, stopping at the ends.
local function step(P, i, ctl, d)
  local n = ctl.def.steps
  if not n or n < 2 then return end
  local s = util.clamp(step_of(P, i, ctl) + d, 0, n - 1)
  P.set_mod(i, ctl, s / (n - 1))
end

local function toggle(type)
  local a = surf.actions
  local r = a.parts.toggle_mod(a.selected(), type)
  say(r == "full" and "full" or type:lower(), type)
end

-- Latch on the Arp, or the Chord when there's no Arp.
local function toggle_latch()
  local P, i = surf.actions.parts, surf.actions.selected()
  local t = P.mod_slot(i, "Arp") and "Arp" or (P.mod_slot(i, "Chord") and "Chord")
  local ctl = t and P.mod_ctl(i, t, "latch")
  if not ctl then
    say("missing", "Arp")
    return
  end
  P.set_mod(i, ctl, P.mod_value(i, ctl) >= 0.5 and 0 or 1)
  say("latch", t)
end

-- The key of the part's Scale, or its Chord's, 0-11, or nil.
local function root(P, i)
  local t = P.mod_slot(i, "Scale") and "Scale" or (P.mod_slot(i, "Chord") and "Chord")
  local ctl = t and P.mod_ctl(i, t, "key")
  return ctl and step_of(P, i, ctl)
end

-- The notes in the part's scale while its Scale is on in degree mode, else nil.
local function degree_notes(P, i)
  if not P.mod_on(i, "Scale") then return nil end
  local mode = P.mod_ctl(i, "Scale", "mode")
  local scale = P.mod_ctl(i, "Scale", "scale")
  if not (mode and scale) or P.mod_value(i, mode) < 0.5 then return nil end
  return SCALE_NOTES[step_of(P, i, scale) + 1]
end

local function latched(P, i, type)
  local ctl = P.mod_ctl(i, type, "latch")
  return ctl ~= nil and P.mod_value(i, ctl) >= 0.5
end

-- The screen's two lines for the last modifier change, small and big, or nil.
function surf.message()
  local m = surf.msg
  if not m then return nil end
  local live = aux and mod_key ~= nil
  if not live and util.time() - m.at > MSG_TIME then return nil end
  local P, i = surf.actions.parts, surf.actions.selected()
  local what, t = m.what, m.type
  if what == "full" then return t:lower(), "slots full" end
  if what == "missing" then return t:lower() .. " off", "no " .. t:lower() end
  if what == "latch" then what = t:lower() end
  local function state(type) return type:lower() .. (P.mod_on(i, type) and " on" or " off") end
  if what == "scale" then
    local src = P.mod_slot(i, "Scale") and "Scale" or (P.mod_slot(i, "Chord") and "Chord")
    local top = state("Scale")
    if P.mod_slot(i, "Scale") then top = top .. "  " .. option(P, i, "Scale", "mode") end
    if not src then return top, "-" end
    local k = root(P, i)
    return top, (KEYS[(k or 0) + 1]) .. " " .. option(P, i, src, "scale")
  elseif what == "chord" then
    if not P.mod_slot(i, "Chord") then return state("Chord"), "-" end
    local mode = option(P, i, "Chord", "mode")
    local big = option(P, i, "Chord", "type")
    if mode == "diatonic" then big = KEYS[(root(P, i) or 0) + 1] .. " " .. option(P, i, "Chord", "scale") end
    return state("Chord") .. "  " .. mode .. (latched(P, i, "Chord") and "  latch" or ""), big
  elseif what == "arp" then
    if not P.mod_slot(i, "Arp") then return state("Arp"), "-" end
    local big = option(P, i, "Arp", "mode") .. " " .. option(P, i, "Arp", "rate")
    local oct = option(P, i, "Arp", "octaves")
    if oct ~= "1" then big = big .. " x" .. oct end
    if latched(P, i, "Arp") then big = big .. " latch" end
    return state("Arp"), big
  end
end

-- ---- what's played --------------------------------------------------------------

function surf.key(n, ev)
  local a = surf.actions
  local z = ev == "down" and 1 or (ev == "up" and 0 or nil)
  if z == nil then return end
  if z == 1 then a.touch() end
  if n == 0 then
    aux = z == 1
    return
  end
  if z == 0 then
    if n == mod_key then
      if not mod_used then toggle(n == KEY_SCALE and "Scale" or "Chord") end
      mod_key = nil
      return
    end
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
    elseif mod_key == KEY_SCALE and SEMI[n] then
      -- before the modifier keys, so keys 4 to 6 set G#, A# and C#
      mod_used = true
      local ctl, P, i = edit_ctl("Scale", "key", true)
      if ctl then
        P.set_mod(i, ctl, (SEMI[n] % 12) / 11)
        say("scale")
      end
    elseif n == KEY_SCALE or n == KEY_CHORD then
      mod_key, mod_used = n, false
      say(n == KEY_SCALE and "scale" or "chord")
    elseif n == KEY_ARP then toggle("Arp")
    elseif n == KEY_LATCH then toggle_latch()
    elseif n >= PART_KEY and n < PART_KEY + 8 then a.select(n - PART_KEY + 1) end
    return
  end
  if SEMI[n] and not held[n] then
    local part = a.selected()
    local note = BASE + 12 * surf.octave + SEMI[n]
    -- in degree mode each white key is the next scale step, the lowest C the
    -- root an octave below C4's
    local count = degree_notes(a.parts, part)
    if count then
      if BLACK[n] then return end
      note = 60 + (n - 12) + count * (surf.octave - 1)
    end
    note = util.clamp(note, 0, 127)
    held[n] = {part = part, note = note}
    a.note(part, note, a.velocity())
  end
end

function surf.enc(d)
  local a = surf.actions
  a.touch()
  if aux and mod_key then
    mod_used = true
    if mod_key == KEY_SCALE then
      local ctl, P, i = edit_ctl("Scale", "scale", true)
      if ctl then
        step(P, i, ctl, d)
        say("scale")
      end
    else
      local ctl, P, i = edit_ctl("Chord", "type")
      if ctl then
        step(P, i, ctl, d)
        say("chord")
      end
    end
    if enc_down then enc_turned = true end
  elseif enc_down then
    enc_turned = true
    a.machine(d)
  else
    a.patch(d)
  end
end

function surf.enc_btn(z)
  surf.actions.touch()
  if z == 1 then
    enc_down, enc_turned = true, false
    if aux and mod_key == KEY_SCALE then
      -- snap or degree; the tap doesn't change the page
      mod_used, enc_turned = true, true
      local ctl, P, i = edit_ctl("Scale", "mode")
      if ctl then
        P.set_mod(i, ctl, P.mod_value(i, ctl) >= 0.5 and 0 or 1)
        say("scale")
      end
    end
  else
    if enc_down and not enc_turned then surf.actions.page() end
    enc_down = false
  end
end

-- pos 0-1. A pot only sets its macro once it reaches the macro's value (or
-- passes it), so a new part or patch doesn't jump. It then holds it until
-- the value is changed some other way. With AUX held the pots set modifier
-- params (AUX_POTS) the same way.
function surf.pot(n, pos)
  local a = surf.actions
  local p = pots[n] or {}
  pots[n] = p
  if p.last and math.abs(pos - p.last) < 0.002 then return end -- jitter
  -- a jittering pot shouldn't keep the screensaver away: it counts once it
  -- has moved 1% from where it last counted
  if not p.anchor then
    p.anchor = pos -- the first report is the pot's position, not a move
  elseif math.abs(pos - p.anchor) > 0.01 then
    p.anchor = pos
    a.touch()
  end
  local last = p.last
  p.last = pos
  -- what the pot sets: key, current 0-1 value, def and a setter
  local key, cur, def, set, mod
  if aux then
    mod = AUX_POTS[n + 1]
    local P, i = a.parts, a.selected()
    local ctl = mod and P.mod_ctl(i, mod[1], mod[2])
    if not ctl then
      p.key, p.set = nil, nil
      if mod then say("missing", mod[1]) end
      return
    end
    key = i .. ":" .. mod[1] .. ":" .. mod[2]
    cur, def = P.mod_value(i, ctl), ctl.def
    set = function(v) P.set_mod(i, ctl, v); return P.mod_value(i, ctl) end
  else
    local mac = a.macros()[n + 1]
    if not mac or not mac.slot then return end
    local sid = a.slot_id(mac.slot)
    key, cur, def = sid, params:get(sid), mac.def
    set = function(v) params:set(sid, v); return params:get(sid) end
  end
  local holding = p.key == key and p.set and math.abs(cur - p.set) < 1e-4
  if not holding then
    holding = math.abs(pos - cur) < PICKUP or (last and (last - cur) * (pos - cur) <= 0)
  end
  surf.flash = {k = n + 1, pos = pos, waiting = not holding, at = util.time(), mod = mod}
  surf.msg = nil
  if not holding then
    p.key, p.set = nil, nil
    return
  end
  local v = pos
  if def.curve == "step" and def.steps > 1 then
    v = math.floor(v * (def.steps - 1) + 0.5) / (def.steps - 1)
  end
  p.key, p.set = key, set(v)
end

-- A modifier pot's label, 0-1 value and readout for the screen, or nil.
function surf.mod_readout(f)
  local P, i = surf.actions.parts, surf.actions.selected()
  local ctl = P.mod_ctl(i, f.mod[1], f.mod[2])
  if not ctl then return nil end
  local v = P.mod_value(i, ctl)
  local c = card_ctl(f.mod[1], f.mod[2])
  local text = c and c.o and c.o[step_of(P, i, ctl) + 1] or P.readout(ctl.def, v)
  return f.mod[1]:lower() .. " " .. f.mod[2], v, text
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
  if a.saving() then
    a.saver_leds(omx.led, colour)
    omx.led_show()
    return
  end
  for n = 1, 26 do omx.led(n, 0, 0, 0) end
  omx.led(0, scaled(colour, 0.6))
  local P = a.parts
  if aux and mod_key == KEY_SCALE then
    -- the keys on the root lit, to pick another
    local r = root(P, sel)
    for n in pairs(SEMI) do
      local f = SEMI[n] % 12 == r and 1 or (BLACK[n] and 0.03 or 0.08)
      omx.led(n, scaled(MOD_COLOURS[KEY_SCALE], f))
    end
  elseif aux then
    for i = 1, 8 do
      local f = i == sel and 1 or (a.has_machine(i) and 0.12 or 0.03)
      omx.led(PART_KEY + i - 1, scaled(surf.COLOURS[i], f))
    end
  else
    local degree = degree_notes(P, sel)
    for n in pairs(SEMI) do
      local f = held[n] and 1 or (BLACK[n] and (degree and 0 or 0.04) or 0.14)
      omx.led(n, scaled(colour, f))
    end
  end
  if aux then
    omx.led(OCT_DOWN, scaled({255, 255, 255}, surf.octave < 0 and 0.8 or 0.12))
    omx.led(OCT_UP, scaled({255, 255, 255}, surf.octave > 0 and 0.8 or 0.12))
    local on = {[KEY_SCALE] = P.mod_on(sel, "Scale"), [KEY_CHORD] = P.mod_on(sel, "Chord"),
      [KEY_ARP] = P.mod_on(sel, "Arp")}
    on[KEY_LATCH] = (on[KEY_ARP] and latched(P, sel, "Arp"))
      or (not P.mod_slot(sel, "Arp") and on[KEY_CHORD] and latched(P, sel, "Chord")
        and option(P, sel, "Chord", "play") == "strum keys")
    for k = KEY_SCALE, KEY_LATCH do
      omx.led(k, scaled(MOD_COLOURS[k], on[k] and 0.9 or 0.06))
    end
  end
  omx.led_show()
end

function surf.draw()
  surf.actions.draw(surf)
end

return surf
