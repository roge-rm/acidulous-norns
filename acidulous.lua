-- acidulous
-- a multitimbral patch player
--
-- 8 parts, each on its own
-- MIDI channel
-- E1 part, E2 patch, E3 volume
-- K1 held + E2 machine
-- K2 macros, K3 play a note
-- K1 held + K3 opens files for
-- Forage, Mosaic, Dice, Pollen
-- or Molt, from dust/audio or
-- a USB stick: K3 hears or opens,
-- K1 held + K3 loads, K2 back,
-- E3 pad on Forage
-- on macros: E2 pick, E3 set,
-- K1 held + E3 fine
--
-- a part with mpe on gets
-- every channel of "mpe from"
--
-- OMX-27: keys play the part,
-- AUX + top keys 1 2 octave,
-- AUX + bottom keys 1-8 part,
-- pots macros, encoder patch

engine.name = "Acidulous"

local parts = include("lib/parts")
local view = include("lib/view")
local macros = include("lib/macros")
local surfaces = include("lib/surfaces")
local timer = include("lib/timer")
local install = include("lib/install")
local picker = include("lib/picker")
-- the picker's own copy, so it sees the stick the watcher mounts
local library = picker.lib

local state = {sel = 1, k1 = false, page = "main", macro = 1}
local auditioning = nil -- the part K3 is playing
local omx_problem = nil  -- {text, at}: the OMX-27 can't do REMOTE mode

-- For the REPL: acid.midi(port, bytes) plays as if from MIDI port `port`,
-- acid.omx is the OMX-27 surface (call its key, pot, enc and enc_btn).
acid = {parts = parts, state = state, macros = macros, surfaces = surfaces, omx = surfaces.kinds[1],
  picker = picker}

-- ---- MIDI in ----------------------------------------------------------------

-- MPE controllers "mpe from" finds by itself, by port name. Both send on
-- the lower zone (master channel 1) with a bend range of 48, the MPE default,
-- once they're in MPE mode.
local MPE_DEVICES = {
  -- the Exquis's notes are on the first of its two ports, the QuNexus's on
  -- the first of its three
  {name = "Exquis", match = function(n) return n:find("^exquis") ~= nil and n:find("2$") == nil end},
  {name = "QuNexus", match = function(n) return n:find("qunexus") ~= nil and n:find("[23]$") == nil end},
}

-- The port of the first MPE controller plugged in, or nil.
local function mpe_detected()
  for i = 1, 16 do
    local v = midi.vports[i]
    if v.device and v.name then
      for _, d in ipairs(MPE_DEVICES) do
        if d.match(v.name:lower()) then return i, d end
      end
    end
  end
end

-- True if port's MIDI goes to the MPE part. "auto" is an Exquis or QuNexus
-- if one is plugged in, otherwise nothing: other controllers are picked.
local function mpe_source(port)
  local sel = params:get("mpe_device")
  if sel == 1 then return port == mpe_detected() end
  return sel == 2 or sel - 2 == port
end

-- Where each held note went, so its note off goes to the same places whatever
-- has changed since. Keyed by source (a port, or "omx"), channel and note;
-- each holds a list of {part, engine channel}. For "omx" the channel is the part.
local active = {}

local function akey(src, ch, note) return src .. ":" .. ch .. ":" .. note end

local function remember(src, ch, note, i, ech)
  local k = akey(src, ch, note)
  local list = active[k]
  if not list then list = {}; active[k] = list end
  list[#list + 1] = {i, ech}
end

local function forget_part(i)
  for k, list in pairs(active) do
    for n = #list, 1, -1 do
      if list[n][1] == i then table.remove(list, n) end
    end
    if #list == 0 then active[k] = nil end
  end
end
parts.on_notes_off = forget_part

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

-- Notes, CCs, bend and channel pressure go to every part on their channel,
-- except the MPE part. While a part is MPE the others get channel 1 in the
-- engine, so their bend and pressure stay whole-part. src and kch name the
-- note for release (the port and channel unless the caller says).
local function play(data, src, kch)
  local kind, ch, d1, d2 = unpack_midi(data)
  if not kind then return end
  if is_off(kind, d2) then
    release(src, kch or ch, d1)
    return
  end
  for i, part in ipairs(parts.list) do
    if part.channel == ch and part.machine and parts.mpe ~= i then
      local ech = parts.mpe and 1 or ch
      send_to(i, kind, ech, d1, d2)
      if kind == 0x90 then remember(src, kch or ch, d1, i, ech) end
    end
  end
end

-- Everything from the MPE source goes to the MPE part on its own channel:
-- the engine treats channels 2-16 as one finger each.
local function play_mpe(data, src)
  local kind, ch, d1, d2 = unpack_midi(data)
  if not kind then return end
  if is_off(kind, d2) then
    release(src, ch, d1)
    return
  end
  local i = parts.mpe
  if parts.list[i].machine then
    send_to(i, kind, ch, d1, d2)
    if kind == 0x90 then remember(src, ch, d1, i, ch) end
  end
end

local function midi_event(port, data)
  local kind, ch, d1, d2 = unpack_midi(data)
  if kind and is_off(kind, d2) then
    release(port, ch, d1)
    return
  end
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
-- on it play too, or straight to the part if its channel is off or it's the
-- MPE part (on the master channel).
local function part_note(i, note, vel)
  local part = parts.list[i]
  if vel == 0 then
    release("omx", i, note)
  elseif part.channel > 0 and parts.mpe ~= i then
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
  a.page = function() state.page = state.page == "main" and "macros" or "main" end
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
    if parts.mpe and parts.list[parts.mpe].machine then parts.notes_off(parts.mpe) end
  end)
  params:add_number("omx_velocity", "omx velocity", 1, 127, 100)
  parts.add_params()
  local names = {}
  for _, m in ipairs(parts.machines) do names[#names + 1] = m.name end
  for _, problem in ipairs(macros.check(names, parts.defs)) do print("acidulous: macros: " .. problem) end
  parts.set_part(1, "Reflux", "Classic")
  parts.set_part(2, "Hexbeat", "Straight")
  for i = 1, parts.COUNT do parts.start_part(i) end
  parts.on_presets = picker.presets
  parts.on_shape = picker.shape
  parts.on_hear = picker.heard
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

-- The OMX-27's screen is drawn first and copied off with screen.peek; each
-- view clears the screen before drawing, and only it reaches screen.update.
function redraw()
  if not parts.ready then
    view.starting(install.status or parts.error)
    return
  end
  state.mpe_missing = parts.mpe ~= nil and params:get("mpe_device") == 1 and mpe_detected() == nil
  surfaces.frame()
  surfaces.screens()
  local note = omx_problem and util.time() - omx_problem.at < 10 and omx_problem.text or nil
  if picker.active() then
    view.picker(picker.screen())
  elseif state.page == "macros" then
    view.macros(parts, state, part_macros(), note)
  else
    view.main(parts, state, note)
  end
end

-- E3 on a macro: about 100 detents for the whole range (0.001 with K1 held),
-- and one step a detent for a stepped param.
local function turn_macro(d)
  local mac = part_macros()[state.macro]
  if not mac or not mac.slot then return end
  local def = mac.def
  local step = state.k1 and 0.001 or 0.01
  if def.curve == "step" and def.steps > 1 then step = 1 / (def.steps - 1) end
  local sid = parts.id(state.sel, "s" .. mac.slot)
  params:set(sid, util.clamp(params:get(sid) + d * step, 0, 1))
end

function key(n, z)
  if not parts.ready then return end
  if n == 1 then
    state.k1 = z == 1
  elseif picker.active() then
    picker.key(n, z, state.k1)
  elseif n == 3 and z == 1 and state.k1 and picker.open(parts, state.sel) then
    return
  elseif n == 2 and z == 1 then
    state.page = state.page == "main" and "macros" or "main"
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

function enc(n, d)
  if not parts.ready then return end
  local i = state.sel
  if picker.active() then
    if n ~= 1 then picker.enc(n, d) end
  elseif n == 1 then
    state.sel = util.clamp(i + d, 1, parts.COUNT)
  elseif state.page == "macros" then
    if n == 2 then
      state.macro = util.clamp(state.macro + d, 1, macros.COUNT)
    elseif n == 3 then
      turn_macro(d)
    end
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
