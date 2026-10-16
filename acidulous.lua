-- acidulous
-- a multitimbral patch player
--
-- 8 parts, each on its own
-- MIDI channel
-- K1 held + E1 page: parts,
-- card, mixer, sends
-- parts: E1 part, E2 patch,
-- E3 volume, K2 patch list,
-- K1 held + K2 machines,
-- K3 play a note
-- lists: E1 tab, E2 move,
-- K3 load, K2 close
-- K1 held + K3 opens files for
-- Forage, Mosaic, Dice, Pollen
-- or Molt, from dust/audio or
-- a USB stick: K3 hears or opens,
-- K1 held + K3 loads, K2 back,
-- E3 pad on Forage
-- K1 held + K3 on Nexus: scope,
-- E2 module, K2 close
-- card: E1 card, E2 knob,
-- E3 value, K1 held + E3 fine
-- mixer: E1 part, E2 pan,
-- E3 level, K2 mute, K3 solo
-- sends: E1 part, E2 rev, E3 dly
--
-- an MPE controller plays the
-- selected part, or the one
-- set in "mpe part"
--
-- OMX-27: keys play the part,
-- AUX + top keys 1 2 octave,
-- AUX + bottom keys 1-8 part,
-- pots macros, encoder patch

engine.name = "Acidulous"

local parts = include("lib/parts")
local view = include("lib/view")
local macros = include("lib/macros")
local panel = include("lib/panel")
local surfaces = include("lib/surfaces")
local timer = include("lib/timer")
local install = include("lib/install")
local picker = include("lib/picker")
local lists = include("lib/lists")
local scope = include("lib/scope")
-- the picker's own copy, so it sees the stick the watcher mounts
local library = picker.lib

local PAGES = {"parts", "card", "mixer", "sends"}
-- cards: part -> its card page {machine, card, knob, first, tab_first, pad}
-- out: the out strip is selected on the mixer and sends pages
local state = {sel = 1, k1 = false, page = "parts", cards = {}, out = false}
local auditioning = nil -- the part K3 is playing
local omx_problem = nil  -- {text, at}: the OMX-27 can't do REMOTE mode

-- For the REPL: acid.midi(port, bytes) plays as if from MIDI port `port`,
-- acid.omx is the OMX-27 surface (call its key, pot, enc and enc_btn).
acid = {parts = parts, state = state, macros = macros, panel = panel, surfaces = surfaces, omx = surfaces.kinds[1],
  picker = picker, scope = scope}

-- ---- MIDI in ----------------------------------------------------------------

-- MPE controllers "mpe from" finds by itself, by port name. Both send on
-- the lower zone (master channel 1) once they're in MPE mode. bend is the
-- member bend range in semitones for one that doesn't send RPN 0; the
-- QuNexus doesn't, and bends 2.
local MPE_DEVICES = {
  -- the Exquis's notes are on the first of its two ports, the QuNexus's on
  -- the first of its three
  {name = "Exquis", match = function(n) return n:find("^exquis") ~= nil and n:find("2$") == nil end},
  {name = "QuNexus", bend = 2, match = function(n) return n:find("qunexus") ~= nil and n:find("[23]$") == nil end},
}

-- Ports found to be MPE while the script runs, by port name: {upper = true
-- for the upper zone}, or false once an MPE Configuration Message turned MPE
-- off. Ports not in here go by MPE_DEVICES.
local mpe_ports = {}
-- What "auto" has seen of a port it doesn't know yet, by port name:
-- {notes = note ons so far, held = ch -> notes, played = ch -> true,
-- expr = ch -> true, done = true once it gave up}.
local probes = {}
local rpn = {} -- port name -> channel -> {msb, lsb}, for the MCM and RPN 0
local port_bend = {} -- port name -> member bend range from its RPN 0
-- port name -> part -> the range last put on that part's "mpe bend"
local bend_set = {}

local PROBE_NOTES = 16 -- note ons "auto" watches before giving up on a port

local function port_name(port)
  local v = midi.vports[port]
  return v and v.name or ("port " .. tostring(port))
end

local function named_device(name)
  local n = name:lower()
  for _, d in ipairs(MPE_DEVICES) do
    if d.match(n) then return d end
  end
end

local function named_mpe(name) return named_device(name) ~= nil end

local function is_mpe_port(port)
  local name = port_name(port)
  local known = mpe_ports[name]
  if known ~= nil then return known ~= false end
  return named_mpe(name)
end

-- Surfaces like the OMX-27 sequence on many channels, so they're never probed.
local function is_surface(name)
  for _, k in ipairs(surfaces.kinds) do
    if k.match and k.match(name) then return true end
  end
  return false
end

local function found(name, upper, how)
  mpe_ports[name] = {upper = upper}
  probes[name] = nil
  print("acidulous: " .. name .. " is MPE (" .. how .. ")")
end

-- The MPE Configuration Message, RPN 6 on channel 1 (lower zone) or 16
-- (upper): its value is the member count, 0 turns MPE off. And RPN 0 on a
-- member channel, the bend range a controller says it sends. True for the
-- message's last CC, which goes no further.
local function watch_rpn(name, kind, ch, d1, d2)
  if kind ~= 0xB0 then return end
  if d1 ~= 101 and d1 ~= 100 and d1 ~= 6 then return end
  local r = rpn[name]
  if not r then r = {}; rpn[name] = r end
  local c = r[ch]
  if not c then c = {}; r[ch] = c end
  local master = ch == 1 or ch == 16
  if d1 == 101 then c.msb = d2
  elseif d1 == 100 then c.lsb = d2
  elseif c.msb == 0 and c.lsb == 0 and not master then
    if d2 > 0 and port_bend[name] ~= d2 then
      port_bend[name] = d2
      bend_set[name] = nil
      print("acidulous: " .. name .. " bends " .. d2)
    end
    return true
  elseif c.msb == 0 and c.lsb == 6 and master then
    if d2 > 0 then
      found(name, ch == 16, "mcm, " .. d2 .. " members")
    elseif mpe_ports[name] ~= false then
      mpe_ports[name] = false
      print("acidulous: " .. name .. " turned MPE off")
    end
    return true
  end
end

-- "auto" by playing: within a port's first notes, note ons on two or more of
-- channels 2-16, each with its own bend or pressure. Two notes held at once on
-- one of those channels means it isn't MPE, and so does running out of notes.
local function watch_play(name, kind, ch, d1, d2)
  if mpe_ports[name] ~= nil or named_mpe(name) or is_surface(name) then return end
  local p = probes[name]
  if not p then
    p = {notes = 0, held = {}, played = {}, expr = {}}
    probes[name] = p
  end
  if p.done then return end
  if kind == 0x90 and d2 > 0 then
    p.notes = p.notes + 1
    if ch >= 2 then
      if (p.held[ch] or 0) > 0 then p.done = true; return end
      p.held[ch] = 1
      p.played[ch] = true
    end
  elseif kind == 0x80 or kind == 0x90 then
    if ch >= 2 then p.held[ch] = 0 end
  elseif (kind == 0xE0 or kind == 0xD0) and ch >= 2 then
    p.expr[ch] = true
  else
    return
  end
  local n = 0
  for c = 2, 16 do
    if p.played[c] and p.expr[c] then n = n + 1 end
  end
  if n >= 2 then found(name, false, "playing")
  elseif p.notes >= PROBE_NOTES then p.done = true end
end

-- True while the controller "mpe from" picks is there, for the screen.
local function mpe_present()
  local sel = params:get("mpe_device")
  if sel > 2 then return midi.vports[sel - 2].device ~= nil end
  for i = 1, 16 do
    local v = midi.vports[i]
    if v.device and v.name and (sel == 2 or is_mpe_port(i)) then return true end
  end
  return false
end

-- True if port's MIDI goes to the MPE part. "auto" is any port named in
-- MPE_DEVICES or found to be MPE.
local function mpe_source(port)
  local sel = params:get("mpe_device")
  if sel == 1 then return is_mpe_port(port) end
  return sel == 2 or sel - 2 == port
end

-- Where each held note went, so its note off goes to the same places whatever
-- has changed since. Keyed by source (a port, or "omx"), channel and note;
-- each holds a list of {part, engine channel, note = note, mpe = true if from
-- the MPE source}. For "omx" the channel is the part.
local active = {}
-- part -> {sus, bend}: the MPE source moved the part's sustain or master bend
local mpe_touched = {}

local function akey(src, ch, note) return src .. ":" .. ch .. ":" .. note end

local function remember(src, ch, note, i, ech, mpe)
  local k = akey(src, ch, note)
  local list = active[k]
  if not list then list = {}; active[k] = list end
  list[#list + 1] = {i, ech, note = note, mpe = mpe}
end

local function is_off(kind, d2) return kind == 0x80 or (kind == 0x90 and d2 == 0) end

-- Sends one message to part i, on the engine's channel ch.
local function send_to(i, kind, ch, d1, d2)
  local part, p = parts.list[i], i - 1
  if kind == 0x90 or kind == 0x80 then
    local vel = kind == 0x90 and d2 or 0
    parts.send("/note", {p, ch, d1, vel})
    parts.note(i, vel > 0)
  elseif kind == 0xB0 then
    parts.send("/cc", {p, ch, d1, d2})
    if d1 == 123 or d1 == 120 then part.held = 0 end
  elseif kind == 0xE0 then
    parts.send("/bend", {p, ch, d1 + (d2 << 7)})
  elseif kind == 0xD0 then
    parts.send("/pressure", {p, ch, d1})
  end
end

-- Sends note offs for part i's held notes and forgets them: all of them, or
-- only those from the MPE source.
local function release_part(i, only_mpe)
  for k, list in pairs(active) do
    for n = #list, 1, -1 do
      local t = list[n]
      if t[1] == i and (t.mpe or not only_mpe) then
        send_to(i, 0x80, t[2], t.note, 0)
        table.remove(list, n)
      end
    end
    if #list == 0 then active[k] = nil end
  end
end
parts.on_notes_off = function(i) release_part(i, false) end

local function unpack_midi(data)
  local status = data[1]
  if not status or status < 0x80 or status >= 0xF0 then return nil end
  return status & 0xF0, (status & 0x0F) + 1, data[2] or 0, data[3] or 0
end

-- A note off goes to wherever the note on went.
local function release(src, kch, note)
  local k = akey(src, kch, note)
  local list = active[k]
  if not list then return end
  active[k] = nil
  for _, t in ipairs(list) do send_to(t[1], 0x80, t[2], note, 0) end
end

-- The part "mpe part" gives the MPE source: the selected one when following.
local function mpe_wanted()
  local v = params:get("mpe_part")
  if v == 1 then return state.sel end
  if v == 2 then return nil end
  return v - 2
end

-- Lifts the sustain and master bend the MPE source left on part i.
local function lift_mpe(i)
  local t = mpe_touched[i]
  if not t then return end
  if t.sus then send_to(i, 0xB0, 1, 64, 0) end
  if t.bend then send_to(i, 0xE0, 1, 0, 64) end
  mpe_touched[i] = nil
end

-- Moves the MPE source to the part it should be on, releasing the notes it
-- holds on the old one first, and its sustain and master bend.
local function sync_mpe()
  local want = mpe_wanted()
  if want == parts.mpe then return end
  local old = parts.mpe
  parts.mpe = want
  if old then
    release_part(old, true)
    lift_mpe(old)
  end
  parts.send_mpe()
end

-- Notes, CCs, bend and channel pressure go to every part on their channel.
-- While a part is MPE every part gets channel 1 in the engine, so bend and
-- pressure from here stay whole-part. src and kch name the note for release
-- (the port and channel unless the caller says).
local function play(data, src, kch)
  local kind, ch, d1, d2 = unpack_midi(data)
  if not kind then return end
  if is_off(kind, d2) then
    release(src, kch or ch, d1)
    return
  end
  for i, part in ipairs(parts.list) do
    if part.channel == ch and part.machine then
      local ech = parts.mpe and 1 or ch
      send_to(i, kind, ech, d1, d2)
      if kind == 0x90 then remember(src, kch or ch, d1, i, ech) end
    end
  end
end

-- On a note from the MPE source, sets part i's "mpe bend" to the range the
-- port sent or MPE_DEVICES gives, once per part and range, so turning it by
-- hand afterwards sticks.
local function default_bend(name, i)
  local d = named_device(name)
  local range = port_bend[name] or (d and d.bend)
  if not range then return end
  local done = bend_set[name] or {}
  bend_set[name] = done
  if done[i] == range then return end
  done[i] = range
  params:set(parts.id(i, "mpe_bend"), range)
end

-- Everything from the MPE source goes to the MPE part on its own channel:
-- the engine treats channels 2-16 as one finger each. An upper zone is
-- turned over to the lower one the engine has (16 is the master, 15 down
-- the members).
local function play_mpe(data, src)
  local kind, ch, d1, d2 = unpack_midi(data)
  if not kind then return end
  local known = mpe_ports[port_name(src)]
  local ech = known and known.upper and 17 - ch or ch
  if is_off(kind, d2) then
    release(src, ch, d1)
    return
  end
  local i = parts.mpe
  if kind == 0x90 then default_bend(port_name(src), i) end
  if parts.list[i].machine then
    send_to(i, kind, ech, d1, d2)
    if kind == 0x90 then remember(src, ch, d1, i, ech, true) end
    if ech == 1 and (kind == 0xE0 or (kind == 0xB0 and d1 == 64)) then
      local t = mpe_touched[i] or {}
      mpe_touched[i] = t
      if kind == 0xE0 then t.bend = true else t.sus = true end
    end
  end
end

local function midi_event(port, data)
  local kind, ch, d1, d2 = unpack_midi(data)
  if kind and is_off(kind, d2) then
    release(port, ch, d1)
    if params:get("mpe_device") == 1 then watch_play(port_name(port), kind, ch, d1, d2) end
    return
  end
  if kind then
    local name = port_name(port)
    if watch_rpn(name, kind, ch, d1, d2) then return end
    if params:get("mpe_device") == 1 then watch_play(name, kind, ch, d1, d2) end
  end
  sync_mpe()
  if parts.mpe and mpe_source(port) then
    play_mpe(data, port)
    return
  end
  local dev = params:get("midi_device")
  if dev > 1 and dev - 1 ~= port then return end
  play(data, port)
end
acid.midi = midi_event

-- A note for part i from the OMX-27: on the part's channel, so parts layered
-- on it play too, or straight to the part on channel 1 if its channel is off.
local function part_note(i, note, vel)
  local part = parts.list[i]
  if vel == 0 then
    release("omx", i, note)
  elseif part.channel > 0 then
    play({0x90 | (part.channel - 1), note, vel}, "omx", i)
  elseif part.machine then
    send_to(i, 0x90, 1, note, vel)
    remember("omx", i, note, i, 1)
  end
end

local function device_names()
  local names = {"all"}
  for i = 1, 16 do names[i + 1] = i .. ". " .. midi.vports[i].name end
  return names
end

local function mpe_device_names()
  local names = device_names()
  table.insert(names, 1, "auto")
  return names
end

-- Every port is listened to; the device param picks which are played.
-- Devices plugged in later land on a port, so only the names change.
local function listen(i)
  midi.connect(i).event = function(data) midi_event(i, data) end
end

local function connect_midi()
  for i = 1, 16 do listen(i) end
end

local function refresh_devices()
  -- norns puts the device on its port after this is called
  timer.after(0.5, function()
    for _, pid in ipairs({"midi_device", "mpe_device"}) do
      local p = params:lookup_param(pid)
      if p then p.options = pid == "mpe_device" and mpe_device_names() or device_names() end
    end
  end)
end

-- ---- OMX-27 -------------------------------------------------------------------

local part_macros -- below

local function start_surfaces()
  local a = surfaces.actions
  a.note = part_note
  a.velocity = function() return params:get("omx_velocity") end
  a.selected = function() return state.sel end
  a.select = function(i) state.sel = i end
  a.has_machine = function(i) return parts.list[i].machine ~= nil end
  a.patch = function(d)
    if parts.list[state.sel].machine then params:delta(parts.id(state.sel, "patch"), d) end
  end
  a.machine = function(d) params:delta(parts.id(state.sel, "machine"), d) end
  -- the encoder's tap closes a list, or swaps the parts and card pages
  a.page = function()
    if lists.active(parts) then lists.close()
    else state.page, state.out = state.page == "parts" and "card" or "parts", false end
  end
  a.macros = function() return part_macros() end
  a.slot_id = function(slot) return parts.id(state.sel, "s" .. slot) end
  a.midi = midi_event
  -- the OMX-27's port is a regular MIDI port again
  a.port_freed = function(port) if port then listen(port) end end
  a.old_firmware = function(version)
    local v = version and string.format("%d.%d.%d", version[1], version[2], version[3]) or "no answer"
    omx_problem = {text = "OMX-27 firmware too old", at = util.time()}
    print("acidulous: OMX-27 firmware " .. v .. " has no REMOTE mode, needs 1.15.4")
  end
  a.answered = function() omx_problem = nil end
  a.draw = function(surf) view.omx(parts, state, part_macros(), surf) end
  surfaces.connect_all()
end

-- ---- start-up ----------------------------------------------------------------

local function ready()
  params:add_separator("acidulous", "acidulous")
  params:add_option("midi_device", "midi from", device_names(), 1)
  -- notes held on the old device would never get their note offs
  params:set_action("midi_device", function()
    for i = 1, parts.COUNT do
      if parts.list[i].machine then parts.notes_off(i) end
    end
  end)
  params:add_option("mpe_device", "mpe from", mpe_device_names(), 1)
  params:set_action("mpe_device", function()
    if parts.mpe and parts.list[parts.mpe].machine then
      parts.notes_off(parts.mpe)
      lift_mpe(parts.mpe)
    end
  end)
  -- which part the MPE controller plays: "follow" is the selected one. The
  -- parts' hidden "mpe" params mirror it, for PSETs from before it.
  local mpe_parts = {"follow", "off"}
  for i = 1, parts.COUNT do mpe_parts[i + 2] = tostring(i) end
  params:add_option("mpe_part", "mpe part", mpe_parts, 1)
  params:set_action("mpe_part", function(v)
    for i = 1, parts.COUNT do params:set(parts.id(i, "mpe"), v == i + 2 and 2 or 1, true) end
    sync_mpe()
  end)
  params:add_number("omx_velocity", "omx velocity", 1, 127, 100)
  parts.add_params()
  local names = {}
  for _, m in ipairs(parts.machines) do names[#names + 1] = m.name end
  for _, problem in ipairs(macros.check(names, parts.defs)) do print("acidulous: macros: " .. problem) end
  for _, problem in ipairs(panel.check(names, parts.defs)) do print("acidulous: cards: " .. problem) end
  parts.set_part(1, "Reflux", "Classic")
  parts.set_part(2, "Hexbeat", "Straight")
  for i = 1, parts.COUNT do parts.start_part(i) end
  parts.on_presets = picker.presets
  parts.on_shape = picker.shape
  parts.on_hear = picker.heard
  parts.on_scope = function(path, args) scope.osc(parts, path, args) end
  parts.on_resend = function() scope.resend(parts) end
  library.watch(picker.usb_changed)
  connect_midi()
  start_surfaces()
  midi.add = function(dev)
    refresh_devices()
    surfaces.added(dev)
  end
  -- norns also calls this again with no device
  midi.remove = function(dev)
    refresh_devices()
    if dev then surfaces.removed(dev) end
  end
end

function init()
  -- by name: norns holds its own redraw in the global while init runs
  timer.every(1 / 15, function() redraw() end)
  if not install.check() then return end
  osc.event = function(path, args) parts.osc(path, args) end
  parts.start_engine()
  parts.start(ready)
end

function cleanup()
  library.stop()
  picker.close()
  scope.close(parts)
  timer.cancel_all()
  surfaces.disconnect()
  for i = 1, parts.COUNT do
    if parts.list[i].machine then parts.notes_off(i) end
  end
  active = {}
  parts.on_notes_off = nil
  for i = 1, 16 do midi.vports[i].event = nil end
  acid = nil
end

-- ---- screen and controls -----------------------------------------------------

-- The selected part's macros, with their slots and defs.
part_macros = function()
  local part = parts.list[state.sel]
  if not part.machine then return {} end
  local own = parts.patch_macros[part.machine]
  return macros.resolve(part.machine, parts.defs[part.machine], own and own[part.patch])
end

-- The selected part's card page state, its cards and pads.
local function part_cards()
  local i = state.sel
  local part = parts.list[i]
  local ps = state.cards[i]
  if not ps or ps.machine ~= part.machine then
    ps = {machine = part.machine, card = 1, knob = 1, first = 1, tab_first = 1, pad = 0}
    state.cards[i] = ps
  end
  if not part.machine then return ps, {}, nil end
  local defs = parts.defs[part.machine]
  local pads = panel.pads(part.machine, defs)
  if pads then
    local ok = false
    for _, p in ipairs(pads) do ok = ok or p == ps.pad end
    if not ok then ps.pad = pads[1] end
  end
  local cards = panel.cards(part.machine, defs, ps.pad)
  ps.card = util.clamp(ps.card, 1, math.max(1, #cards))
  local card = cards[ps.card]
  ps.knob = util.clamp(ps.knob, 1, card and #card.controls or 1)
  return ps, cards, pads
end

-- The master's meters, asked for while the mixer shows, again every 2
-- seconds in case the plugin started again.
local meters = {on = false, at = 0}
local function want_meters(on)
  local now = util.time()
  if on ~= meters.on or (on and now - meters.at > 2) then
    parts.send("/meters", {on and 1 or 0})
    meters.on, meters.at = on, now
  end
end

-- The OMX-27's screen is drawn first and copied off with screen.peek; each
-- view clears the screen before drawing, and only it reaches screen.update.
-- The splash on load, as in the app: it fades in, stays while the engine
-- starts and at least SPLASH_HOLD seconds, then fades out.
local SPLASH_IN, SPLASH_HOLD, SPLASH_OUT = 0.18, 1.2, 0.4
local splash_at = util.time()
local splash_out = nil -- when it started fading out

local function draw_splash()
  if not splash_at then return false end
  local now = util.time()
  local t = now - splash_at
  if not splash_out and parts.ready and t >= SPLASH_HOLD then splash_out = now end
  local fade = math.min(1, t / SPLASH_IN)
  if splash_out then
    fade = 1 - (now - splash_out) / SPLASH_OUT
    if fade <= 0 then
      splash_at = nil
      return false
    end
  end
  view.splash(fade, not parts.ready and "starting" or nil)
  return true
end

function redraw()
  if install.status or parts.error then
    splash_at = nil
    view.starting(install.status or parts.error)
    return
  end
  if draw_splash() then return end
  if not parts.ready then
    view.starting()
    return
  end
  sync_mpe()
  local present = mpe_present()
  state.mpe_mark = present and parts.mpe or nil
  state.mpe_missing = not present and params:get("mpe_part") > 2
  surfaces.frame()
  surfaces.screens()
  local note = omx_problem and util.time() - omx_problem.at < 10 and omx_problem.text or nil
  scope.check(parts)
  local overlay = picker.active() or lists.active(parts) or scope.active()
  want_meters(state.page == "mixer" and not overlay)
  if picker.active() then
    view.picker(picker.screen())
  elseif lists.active(parts) then
    local s = lists.screen(parts)
    if s.kind == "machines" then view.machines(parts, s) else view.patches(parts, s) end
  elseif scope.active() then
    view.scope(parts, scope.screen())
  elseif state.page == "card" then
    local ps, cards, pads = part_cards()
    view.card(parts, state, cards, ps, pads, panel, note)
  elseif state.page == "mixer" then
    view.mixer(parts, state, note)
  elseif state.page == "sends" then
    view.sends(parts, state, note)
  else
    view.main(parts, state, note)
  end
end

-- Centre is sticky: a turn that crosses it stops there, and it takes
-- STICK detents the same way to leave it.
local STICK = 3
local held = {}

local function sticky(key, old, new, centre, d)
  if math.abs(old - centre) < 1e-6 then
    local h = held[key] or 0
    if h * d < 0 then h = 0 end
    h = h + d
    if math.abs(h) < STICK then
      held[key] = h
      return centre
    end
    held[key] = nil
    return new
  end
  held[key] = nil
  if (old - centre) * (new - centre) <= 0 then return centre end
  return new
end

-- E3 on a card's control: about 100 detents for the whole range (0.001 with
-- K1 held), one position a detent for a stepped one, or the next pad.
local function turn_control(d)
  local ps, cards, pads = part_cards()
  local card = cards[ps.card]
  local ctl = card and card.controls[ps.knob]
  if not ctl then return end
  if ctl.kind == "pad" then
    for j, p in ipairs(pads or {}) do
      if p == ps.pad then
        ps.pad = pads[util.clamp(j + d, 1, #pads)]
        break
      end
    end
    return
  end
  local step = state.k1 and 0.001 or 0.01
  local n = panel.positions(ctl)
  if n > 1 then step = 1 / (n - 1) end
  local sid = parts.id(state.sel, "s" .. ctl.slot)
  local old = params:get(sid)
  local v = util.clamp(old + d * step, 0, 1)
  local def = ctl.def
  if def and def.min < 0 and def.max > 0 and def.curve ~= "step" then
    v = sticky(sid, old, v, parts.unmap(def, 0), d)
  end
  params:set(sid, v)
end

-- K1+E1 moves a page every PAGE_DETENTS detents, so it's easy to stop on one.
local PAGE_DETENTS = 3
local page_turn = 0

local function step_page(d)
  if page_turn * d < 0 then page_turn = 0 end
  page_turn = page_turn + d
  if math.abs(page_turn) < PAGE_DETENTS then return end
  d = page_turn > 0 and 1 or -1
  page_turn = 0
  for k, p in ipairs(PAGES) do
    if p == state.page then
      state.page = PAGES[util.clamp(k + d, 1, #PAGES)]
      if state.page == "parts" or state.page == "card" then state.out = false end
      return
    end
  end
  state.page = "parts"
end

-- E1 on the mixer and sends pages: the parts, then the out strip.
local function step_strip(d)
  if state.out then
    if d < 0 then state.out = false end
  elseif state.sel + d > parts.COUNT then
    state.sel, state.out = parts.COUNT, true
  else
    state.sel = util.clamp(state.sel + d, 1, parts.COUNT)
  end
end

local function toggle(pid) params:set(pid, params:get(pid) == 2 and 1 or 2) end

-- Encoder ticks a step takes. Picking from a list, a part, a card or a
-- stepped control goes one step at a time; turning a value speeds up when
-- you turn fast.
local SELECT_SENS = 3
local VALUE_SENS = 3
local enc_mode = {}

local function selecting(n)
  if n == 1 or picker.active() or lists.active(parts) or scope.active() then return true end
  if n == 2 then return state.page ~= "mixer" and state.page ~= "sends" end
  if state.page ~= "card" then return false end
  local ps, cards = part_cards()
  local card = cards[ps.card]
  local ctl = card and card.controls[ps.knob]
  return ctl ~= nil and (ctl.kind == "pad" or panel.positions(ctl) > 1)
end

-- Sets acceleration and sensitivity for what each encoder does now. Only on a
-- change, as setting them drops the ticks so far.
local function set_encoders()
  for n = 1, 3 do
    local sel = selecting(n)
    if enc_mode[n] ~= sel then
      enc_mode[n] = sel
      norns.enc.accel(n, not sel)
      norns.enc.sens(n, sel and SELECT_SENS or VALUE_SENS)
    end
  end
end

local function handle_key(n, z)
  if not parts.ready then return end
  if n == 1 then
    state.k1 = z == 1
    page_turn = 0
  elseif n == 3 and z == 0 and auditioning then
    parts.audition(auditioning, 0)
    auditioning = nil
  elseif picker.active() then
    picker.key(n, z, state.k1)
  elseif lists.active(parts) then
    lists.key(parts, n, z)
  elseif scope.active() and n == 2 then
    if z == 1 then scope.close(parts) end
  elseif n == 3 and state.k1 and not scope.active() then
    -- the Nexus scope or the sample picker, by machine
    if z == 1 and not scope.open(parts, state.sel) then picker.open(parts, state.sel) end
  elseif state.page == "mixer" and not scope.active() and (n == 2 or n == 3) then
    if z == 1 and not state.out then toggle(parts.id(state.sel, n == 2 and "mute" or "solo")) end
  elseif n == 2 and z == 1 and state.page == "parts" then
    -- the patch list, or the machine picker with K1 or on an empty part
    if state.k1 or not lists.open_patches(parts, state.sel) then lists.open_machines(parts, state.sel) end
  elseif n == 2 and z == 1 and state.page == "card" then
    state.page = "parts"
  elseif n == 3 then
    if z == 1 then
      if auditioning then parts.audition(auditioning, 0) end
      auditioning = state.sel
      parts.audition(auditioning, 1)
    elseif auditioning then
      parts.audition(auditioning, 0)
      auditioning = nil
    end
  end
end

local function handle_enc(n, d)
  if not parts.ready then return end
  local i = state.sel
  if picker.active() then
    if n ~= 1 then picker.enc(n, d) end
  elseif lists.active(parts) then
    lists.enc(parts, n, d)
  elseif scope.active() then
    scope.enc(parts, n, d)
  elseif n == 1 and state.k1 then
    step_page(d)
  elseif state.page == "mixer" or state.page == "sends" then
    local mixer = state.page == "mixer"
    if n == 1 then
      step_strip(d)
    elseif state.out then
      if mixer then
        if n == 3 then params:delta("master_level", d) end
      else
        params:delta(n == 2 and "rev_size" or "dly_feedback", d)
      end
    elseif mixer and n == 2 then
      local pid = parts.id(i, "pan")
      local old = params:get(pid)
      params:delta(pid, d)
      params:set(pid, sticky(pid, old, params:get(pid), 0, d))
    elseif mixer then
      params:delta(parts.id(i, "volume"), d)
    else
      params:delta(parts.id(i, n == 2 and "rev" or "dly"), d)
    end
  elseif state.page == "card" then
    local ps, cards = part_cards()
    if n == 1 then
      local c = util.clamp(ps.card + d, 1, math.max(1, #cards))
      if c ~= ps.card then ps.card, ps.knob, ps.first = c, 1, 1 end
    elseif n == 2 then
      local card = cards[ps.card]
      ps.knob = util.clamp(ps.knob + d, 1, card and #card.controls or 1)
    elseif n == 3 then
      turn_control(d)
    end
  elseif n == 1 then
    state.sel = util.clamp(i + d, 1, parts.COUNT)
  elseif state.page ~= "parts" then
    return
  elseif n == 2 then
    if state.k1 then
      params:delta(parts.id(i, "machine"), d)
    elseif parts.list[i].machine then
      params:delta(parts.id(i, "patch"), d)
    end
  elseif n == 3 then
    params:delta(parts.id(i, "volume"), d)
  end
end

function key(n, z)
  handle_key(n, z)
  if parts.ready then set_encoders() end
end

function enc(n, d)
  handle_enc(n, d)
  if parts.ready then set_encoders() end
end
