-- acidulous
-- a multitimbral patch player
--
-- 8 parts, each on its own
-- MIDI channel
-- E1 part, E2 patch, E3 volume
-- K1 held + E2 machine
-- K2 params, K3 play a note

engine.name = "Acidulous"

local parts = include("lib/parts")
local view = include("lib/view")

local state = {sel = 1, k1 = false, page = "main", param = 1, first = 1}
local auditioning = nil -- the part K3 is playing

-- For the REPL: acid.midi(port, bytes) plays as if from MIDI port `port`.
acid = {parts = parts, state = state}

-- ---- MIDI in ----------------------------------------------------------------

-- Notes, CCs, bend and channel pressure go to every part on their channel.
local function midi_event(port, data)
  local dev = params:get("midi_device")
  if dev > 1 and dev - 1 ~= port then return end
  local status = data[1]
  if not status or status < 0x80 or status >= 0xF0 then return end
  local kind, ch = status & 0xF0, (status & 0x0F) + 1
  local d1, d2 = data[2] or 0, data[3] or 0
  for i, part in ipairs(parts.list) do
    if part.channel == ch and part.machine then
      local p = i - 1
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
  end
end
acid.midi = midi_event

local function device_names()
  local names = {"all"}
  for i = 1, 16 do names[i + 1] = i .. ". " .. midi.vports[i].name end
  return names
end

-- Every port is listened to; the device param picks which are played.
-- Devices plugged in later land on a port, so only the names change.
local function connect_midi()
  for i = 1, 16 do
    local port = midi.connect(i)
    port.event = function(data) midi_event(i, data) end
  end
end

local function refresh_devices()
  clock.run(function()
    clock.sleep(0.5) -- norns puts the device on its port after this is called
    local p = params:lookup_param("midi_device")
    if p then p.options = device_names() end
  end)
end

-- ---- start-up ----------------------------------------------------------------

local function ready()
  params:add_separator("acidulous", "acidulous")
  params:add_option("midi_device", "midi from", device_names(), 1)
  parts.add_params()
  parts.set_part(1, "Reflux", "Classic")
  parts.set_part(2, "Hexbeat", "Straight")
  for i = 1, parts.COUNT do parts.start_part(i) end
  connect_midi()
  midi.add = function() refresh_devices() end
  midi.remove = function() refresh_devices() end
end

function init()
  osc.event = function(path, args) parts.osc(path, args) end
  engine.start()
  parts.start(ready)
  clock.run(function()
    while true do
      clock.sleep(1 / 15)
      redraw()
    end
  end)
end

function cleanup()
  for i = 1, parts.COUNT do
    if parts.list[i].machine then parts.notes_off(i) end
  end
  for i = 1, 16 do midi.vports[i].event = nil end
  acid = nil
end

-- ---- screen and controls -----------------------------------------------------

local function slot_value(j) return params:get(parts.id(state.sel, "s" .. j)) end

function redraw()
  if not parts.ready then
    view.starting()
  elseif state.page == "params" then
    view.params(parts, state, slot_value)
  else
    view.main(parts, state)
  end
end

local function part_defs()
  local part = parts.list[state.sel]
  return part.machine and parts.defs[part.machine] or {}
end

function key(n, z)
  if not parts.ready then return end
  if n == 1 then
    state.k1 = z == 1
  elseif n == 2 and z == 1 then
    state.page = state.page == "main" and "params" or "main"
    state.param, state.first = 1, 1
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
  if n == 1 then
    state.sel = util.clamp(i + d, 1, parts.COUNT)
    state.param, state.first = 1, 1
  elseif state.page == "params" then
    local count = #part_defs()
    if count == 0 then return end
    if n == 2 then
      state.param = util.clamp(state.param + d, 1, count)
      if state.param < state.first then state.first = state.param end
      if state.param >= state.first + view.PARAM_ROWS then state.first = state.param - view.PARAM_ROWS + 1 end
    elseif n == 3 then
      params:delta(parts.id(i, "s" .. state.param), d)
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
