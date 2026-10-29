-- The parts: their params, and what they send to the plugin.
-- A part n is the plugin's part n - 1 (an engine track).

local timer = include("lib/timer")
-- bank patch names before pN_patch_name, for old PSETs
local OLD_PATCHES = include("lib/oldpatches")
-- machine params saved by an older engine, for old PSETs
local LAYOUT = include("lib/layout")

local P = {}

-- The script has 16 parts. The "parts" param shows 8 or all 16: parts past
-- it are parked, their params kept and their machines unmounted. P.active
-- is how many the pages show: the param's count, or 16 for a loaded song
-- with more than 8 tracks.
P.COUNT = 16
P.active = 8
function P.on(i) return i <= P.active end
local PLUGIN = {"127.0.0.1", 57150}

-- The machines offered, with short for the main page and note for K3's
-- audition. The order here doesn't matter: they are put in the picker's order
-- (lib/machines.lua) just below.
P.MACHINES = {
  {name = "Reflux", short = "Reflx", note = 36},
  {name = "Hexbeat", short = "Hexbt", note = 36},
  {name = "Trinity", short = "Trin"},
  {name = "Ratio", short = "Ratio"},
  {name = "Formulate", short = "Form"},
  {name = "Genesis", short = "Gen"},
  {name = "Tine", short = "Tine"},
  {name = "Palm", short = "Palm"},
  {name = "Tongue", short = "Tongu"},
  {name = "Timber", short = "Timbr"},
  {name = "Aviary", short = "Aviry"},
  {name = "Fret", short = "Fret"},
  {name = "Chanter", short = "Chant"},
  {name = "Brazen", short = "Brazn"},
  {name = "Fathom", short = "Fathm"},
  {name = "Resonance", short = "Reson"},
  {name = "Draw", short = "Draw"},
  {name = "Sympath", short = "Sympa"},
  {name = "Filament", short = "Filmt"},
  {name = "Manual", short = "Manul"},
  {name = "Hammer", short = "Hammr"},
  {name = "Cipher", short = "Ciphr"},
  {name = "Cumulus", short = "Cumls"},
  {name = "Nexus", short = "Nexus"},
  {name = "Diction", short = "Dictn"},
  {name = "Forage", short = "Forag", note = 36},
  {name = "Mosaic", short = "Mosac", note = 60},
  {name = "Dice", short = "Dice", note = 36},
  {name = "Pollen", short = "Polln", note = 60},
  {name = "Molt", short = "Molt", note = 57},
}

-- The machine order before it followed the picker. Old PSETs saved the
-- machine as an index into this list (less the machines the plugin lacked).
-- Never change it.
local OLD_ORDER = {
  "Reflux", "Hexbeat", "Trinity", "Ratio", "Formulate", "Genesis", "Tine",
  "Palm", "Tongue", "Timber", "Aviary", "Fret", "Chanter", "Brazen", "Fathom",
  "Resonance", "Draw", "Sympath", "Filament", "Manual", "Hammer", "Cipher",
  "Cumulus", "Nexus", "Diction", "Forage", "Mosaic", "Dice", "Pollen", "Molt",
}

-- Puts P.MACHINES in the picker's order: its groups in turn, alphabetical in
-- each. Any machine in no group goes last.
do
  local by = {}
  for _, m in ipairs(P.MACHINES) do by[m.name] = m end
  local sorted = {}
  for _, g in ipairs(include("lib/machines").groups) do
    for _, name in ipairs(g.machines) do
      if by[name] then sorted[#sorted + 1] = by[name]; by[name] = nil end
    end
  end
  for _, m in ipairs(P.MACHINES) do
    if by[m.name] then sorted[#sorted + 1] = m end
  end
  P.MACHINES = sorted
end

P.machines = {}  -- the ones the plugin has, from /machines: {name, short, note}
P.defs = {}      -- machine -> list of {name, min, max, default, unit, steps, curve}
P.patch_macros = {} -- machine -> patch -> list of {param, label}, from /macros
P.patches = {}   -- machine -> list of patch names
P.slots = 0      -- param slots per part: the most any machine has
P.ready = false
P.error = nil     -- shown instead of the parts, e.g. no machines
P.loading = false -- a PSET is being read
P.seen = 0        -- when the plugin last sent /load
P.load, P.worst, P.worst_at = 0, 0, 0
P.mpe = nil       -- the part getting MPE, or nil; set by the script from "mpe part"
P.MOD_TYPES = {"Scale", "Chord", "Arp"} -- the modifiers, in menu order
P.MOD_SLOTS = 3 -- modifier slots a part has, 0 to 2
P.mod_defs = {}   -- modifier -> list of defs as P.defs, with by = name -> def; from /mod_defs
P.mod_defs_asked = 0
-- A part's state. The script's own parts (own) keep their values in the
-- params. While a song is loaded the pages show its tracks instead (P.song),
-- the same shape with their values in vals, filled from the plugin's
-- replies and never saved: get and put below read and write either.
local function new_part(i, song)
  return {
    n = i, channel = i, machine = nil, patch = nil,
    song = song, -- a song's track: its values are in vals, not the params
    level = 0, held = 0, last_note = 0,
    want_values = false, pending = nil,
    sent = nil,     -- the patch last sent to the plugin
    edits = {},     -- slot -> value, set while a patch is on its way
    sends_moved = {}, -- "rev"/"dly" -> true when moved since the last /patch
    values_at = 0, values_retried = false,
    dim = false,    -- the slots hold defaults until the plugin's values come
    -- what /load has put in the machine: "pad0".."pad12" or "main" ->
    -- {kind, path, arg, name}. Saved in the part's "files" param.
    files = {},
    loading = nil,  -- {kind, name} while a load is on its way
    problem = nil,  -- {text, at}: the last load that failed
    last_path = nil, -- the last file or folder loaded, for the picker
    resend_files = false, -- send the files after the next mount
    want_default = false, -- load the machine's default after the next mount
    -- the inserts the plugin last listed, slot 0 or 1 -> {type, params},
    -- each param a def with its 0-1 value; from /inserts
    fx = {},
    -- insert params moved by hand, "slot:name" -> 0-1. Saved in the part's
    -- "fx" param and sent again over the patch's after a PSET is read.
    fx_edits = {},
    -- /patch and /unmount sent and not yet answered by /inserts, and when the
    -- last went, so only the newest one's /inserts is shown
    fx_wait = 0, fx_asked = 0,
    -- a patch's macro names, 8 strings ("" for unnamed), or nil when
    -- it names none; from /macro_names, not saved
    macro_names = nil,
    -- the saved preset the part plays instead of a bank patch, or nil:
    -- {path, index, name}. Saved in the part's "preset" param.
    preset = nil,
    -- the modifiers, slot 0 to 2 -> {type, bypass, values = name -> 0-1}
    -- or nil. They belong to the part, so a machine or patch change keeps
    -- them. Saved in the part's "mods" param.
    mods = {},
    vals = song and {} or nil,
    name = nil,     -- a song track's name from song.json
  }
end

local own = {}
for i = 1, P.COUNT do own[i] = new_part(i) end
P.own = own
P.song = {}
P.song_view = false -- the pages show the song's tracks
-- P.song_reading: a song load is on its way, so replies about the racks
-- are its read-back
P.song_reading = false

-- The part the pages and play show as part i.
function P.part(i)
  if P.song_view then return P.song[i] end
  return own[i]
end

-- P.list[i] is P.part(i), so pages read whichever is shown.
P.list = setmetatable({}, {
  __index = function(_, i)
    if type(i) == "number" and i >= 1 and i <= P.COUNT then return P.part(i) end
  end,
  __len = function() return P.COUNT end,
})

-- While a song holds the racks (lib/song.lua sets P.song_holds), the
-- script's parts stay in their params for when it goes: nothing of theirs
-- is sent, and what changes is marked owed and sent when the racks are
-- theirs again. Edits to the song's tracks go to its racks. P.song_racks:
-- replies about the racks are the song's.
P.song_holds = false
P.song_racks = false
P.owed_master = false

-- What the script's own sends may not do while a song holds the racks, as a
-- net under the part-by-part rules.
local HELD_BY_SONG = {
  ["/patch"] = true, ["/unmount"] = true, ["/preset_load"] = true, ["/mount"] = true, ["/param"] = true,
  ["/load"] = true, ["/channel"] = true, ["/fx"] = true, ["/mod"] = true, ["/mod_bypass"] = true,
  ["/mod_param"] = true, ["/send"] = true, ["/master"] = true, ["/mods_off"] = true, ["/preset_save"] = true,
}

local function send(path, args)
  if P.song_holds and HELD_BY_SONG[path] then return end
  osc.send(PLUGIN, path, args)
end
P.send = send
-- Sends past the song's hold, for the song's own master.
function P.send_to_song(path, args) osc.send(PLUGIN, path, args) end

-- Sends something for a part: a song track's to its rack while the song
-- holds the racks, the script's own only when no song holds them.
local function psend(part, path, args)
  if part.song then
    -- not while a load replaces the song, whose racks these aren't
    if P.song_holds and not P.song_reading then osc.send(PLUGIN, path, args) end
  elseif not P.song_holds then
    osc.send(PLUGIN, path, args)
  elseif path ~= "/cc" and path ~= "/mods_off" then
    -- a change to a script part's racks: sent with the rest of the part
    -- when they're its again
    part.owed = true
  end
end

local song_track -- below, with the song's tracks

-- The part replies about rack i are for.
local function rpart(i)
  if P.song_racks then return P.song[i] end
  return own[i]
end

-- True when a script part's change can't be sent now. A parked part's is
-- sent when it's shown again; while a song holds the racks it's marked owed.
-- fresh: a new machine or patch, which then loads with its own values.
local function held(part, fresh)
  if part.song then return false end
  if part.parked then
    if fresh then part.park_fresh = true end
    return true
  end
  if not P.song_holds then return false end
  part.owed = true
  if fresh then part.fresh = true end
  return true
end

-- Sends /patch or /unmount, which the plugin answers with one /inserts each.
-- A count left over for more than 2 s is a lost reply, so it starts again.
local function send_patch(part, path, args)
  if part.song then
    -- dropped by psend, so no /inserts comes for it
    if not P.song_holds or P.song_reading then return end
  elseif P.song_holds then
    part.owed = true
    return
  end
  local now = util.time()
  part.fx_wait = (now - part.fx_asked > 2) and 1 or part.fx_wait + 1
  part.fx_asked = now
  psend(part, path, args)
end

local function id(i, what) return "p" .. i .. "_" .. what end
P.id = id

-- The machine option's index for a machine name, 1 for none.
local function machine_index(machine)
  for k, m in ipairs(P.machines) do
    if m.name == machine then return k + 1 end
  end
  return 1
end

-- A bank patch's index, 0 when it isn't one of the machine's.
local function patch_index(machine, name)
  for k, n in ipairs(machine and P.patches[machine] or {}) do
    if n == name then return k end
  end
  return 0
end

-- What a song track has before the plugin says, as the params start.
local SONG_DEFAULTS = {volume = 0, pan = 0, mute = 1, solo = 1, rev = 0, dly = 0, patch = 0}

-- A part's value by its param name: the script's from its param, a song
-- track's from vals.
local function get(part, key)
  if not part.song then return params:get(id(part.n, key)) end
  local v = part.vals[key]
  if v ~= nil then return v end
  if key == "machine" then return machine_index(part.machine) end
  if key == "channel" then return part.channel + 1 end
  if key == "mpe_bend" then return params:get(id(part.n, key)) end
  local j = key:match("^s(%d+)$")
  if j then
    local d = part.machine and (P.defs[part.machine] or {})[tonumber(j)]
    return d and P.unmap(d, d.default) or 0
  end
  return SONG_DEFAULTS[key] or ""
end

-- Sets it without running anything: a param silently, or vals.
local function put(part, key, v)
  if part.song then part.vals[key] = v else params:set(id(part.n, key), v, true) end
end

function P.machine_info(name)
  for _, m in ipairs(P.machines) do
    if m.name == name then return m end
  end
end

-- ---- real units ---------------------------------------------------------------

function P.map(d, v)
  v = util.clamp(v, 0, 1)
  if d.curve == "exp" and d.min > 0 then
    return d.min * (d.max / d.min) ^ v
  elseif d.curve == "step" and d.steps > 1 then
    return d.min + math.floor(v * (d.steps - 1) + 0.5) * ((d.max - d.min) / (d.steps - 1))
  end
  return d.min + (d.max - d.min) * v
end

function P.unmap(d, x)
  if d.max == d.min then return 0 end
  if d.curve == "exp" and d.min > 0 and x > 0 then
    return util.clamp(math.log(x / d.min) / math.log(d.max / d.min), 0, 1)
  end
  return util.clamp((x - d.min) / (d.max - d.min), 0, 1)
end

local function number(x, whole)
  if whole or math.abs(x - math.floor(x + 0.5)) < 1e-6 and math.abs(x) >= 10 then
    return string.format("%d", math.floor(x + 0.5))
  end
  local a = math.abs(x)
  if a >= 100 then return string.format("%.0f", x)
  elseif a >= 10 then return string.format("%.1f", x) end
  return string.format("%.2f", x)
end

function P.readout(d, v)
  local x = P.map(d, v)
  local unit = d.unit or ""
  local whole = d.curve == "step" and math.abs(x - math.floor(x + 0.5)) < 1e-6
  if unit == "Hz" and x >= 1000 then x, unit = x / 1000, "kHz"
  elseif unit == "ms" and x >= 1000 then x, unit = x / 1000, "s" end
  local s = number(x, whole)
  if unit == "" then return s end
  if unit == "%" then return s .. "%" end
  return s .. " " .. unit
end

-- ---- start-up: machines, defs and patches ------------------------------------

-- Asks for the machines, then each machine's defs and patches one at a time,
-- so matron's OSC port isn't flooded. on_ready runs once everything is in.
local queue = {}
local waiting = nil -- {kind, machine, asked}
local on_ready
local asked_machines = false -- /machines has been answered

local function ask_next()
  waiting = table.remove(queue, 1)
  if not waiting then
    if P.ready then return end
    P.slots = 0
    for _, m in ipairs(P.machines) do P.slots = math.max(P.slots, #(P.defs[m.name] or {})) end
    P.ready = true
    if on_ready then on_ready() end
    return
  end
  waiting.asked = util.time()
  if waiting.kind == "defs" then P.defs[waiting.machine] = {} end
  send("/" .. waiting.kind, {waiting.machine})
end

local watch

-- Starts the plugin with this script's banks, wherever it's installed.
function P.start_engine()
  engine.start(norns.state.path .. "data/banks")
end

function P.start(ready)
  print("acidulous: asking the plugin for its machines")
  on_ready = ready
  local tick = 0
  local watching = false
  P.ticker = timer.every(0.5, function()
    tick = tick + 1
    if watching then
      watch()
    elseif not asked_machines then
      -- the plugin can take a moment to start; ask again until it answers
      if tick % 2 == 1 then send("/machines", {}) end
    elseif P.error then
      timer.cancel(P.ticker)
    elseif not P.ready then
      if tick % 2 == 0 and waiting and util.time() - waiting.asked > 2 then
        table.insert(queue, 1, waiting)
        ask_next()
      end
    else
      P.seen = util.time()
      watching = true
    end
  end)
  send("/machines", {})
  P.tempo_timer = timer.every(0.25, function()
    if P.ready then
      P.send_tempo()
      P.send_beat()
    end
  end)
end

-- ---- params -------------------------------------------------------------------

local function rebuild_menu()
  if _menu and _menu.rebuild_params then _menu.rebuild_params() end
end

local function db_to_gain(db)
  if db <= -60 then return 0 end
  return 10 ^ (db / 20)
end

P.db_to_gain = db_to_gain

local function send_volume(part)
  -- the strip's gain is 0-1.5 linear
  psend(part, "/channel", {part.n - 1, "gain", math.min(1, db_to_gain(get(part, "volume")) / 1.5)})
end

-- The part's channel strip: pan, mute, solo and sends. A send above 0
-- mounts its effect in the plugin.
local STRIP = {pan = "pan", mute = "mute", solo = "solo", rev = "sendreverb", dly = "senddelay"}

local function strip_value(part, what)
  local v = get(part, what)
  if what == "pan" then return (v + 1) / 2 end
  if what == "mute" or what == "solo" then return v - 1 end
  return v
end

local function send_strip_param(part, what)
  -- a send moved after a /patch keeps its level when the patch's /sends comes back
  if what == "rev" or what == "dly" then part.sends_moved[what] = true end
  psend(part, "/channel", {part.n - 1, STRIP[what], strip_value(part, what)})
end

local function send_strip(part)
  send_volume(part)
  for _, what in ipairs({"pan", "mute", "solo", "rev", "dly"}) do send_strip_param(part, what) end
end

-- ---- the sends and the master ----------------------------------------------------

-- The send effects' params as the engine has them (Reverb and Delay in
-- engine/effect/Effects.cpp), with the slot the plugin takes. time is in
-- the delay's note values at the engine's tempo.
P.DELAY_TIMES = {"1/32", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "1 bar"}
P.SEND_PARAMS = {
  {id = "rev_size", name = "rev size", slot = 0, param = "size", min = 0, max = 1, default = 0.5},
  {id = "rev_damp", name = "rev damp", slot = 0, param = "damp", min = 0, max = 1, default = 0.4},
  {id = "rev_tone", name = "rev tone", slot = 0, param = "tone", min = 500, max = 18000, default = 9000, exp = true},
  {id = "rev_predelay", name = "rev predelay", slot = 0, param = "predelay", min = 0, max = 200, default = 10, unit = "ms"},
  {id = "dly_time", name = "dly time", slot = 1, param = "time", options = P.DELAY_TIMES, default = 4},
  {id = "dly_feedback", name = "dly feedback", slot = 1, param = "feedback", min = 0, max = 0.95, default = 0.4},
  {id = "dly_tone", name = "dly tone", slot = 1, param = "tone", min = 300, max = 18000, default = 6000, exp = true},
  {id = "dly_pingpong", name = "dly ping pong", slot = 1, param = "pingpong", options = {"off", "on"}, default = 2},
}

local function send_param_value(sp)
  local v = params:get(sp.id)
  if sp.options then return (v - 1) / (#sp.options - 1) end
  if sp.exp then return math.log(v / sp.min) / math.log(sp.max / sp.min) end
  return (v - sp.min) / (sp.max - sp.min)
end

-- 0-1 for the screen's knobs.
function P.send_param_01(pid)
  for _, sp in ipairs(P.SEND_PARAMS) do
    if sp.id == pid then return send_param_value(sp) end
  end
  return 0
end

local function send_send_param(sp)
  send("/send", {sp.slot, sp.param, send_param_value(sp)})
end

-- The sends page's out strip: the reverb's size and the delay's feedback,
-- the song's while one is loaded (P.song_sends, {size, feedback} 0-1 from
-- lib/song.lua, nil for a send the song doesn't have), else the script's.
-- The script's params stay as they are during a song.
P.song_sends = nil
local OUT_SENDS = {rev_size = "size", dly_feedback = "feedback"}
local function send_spec(pid)
  for _, sp in ipairs(P.SEND_PARAMS) do
    if sp.id == pid then return sp end
  end
end

-- pid's value in its param's units, or nil for the song's missing send.
function P.out_send(pid)
  if not P.song_sends then return params:get(pid) end
  local v, sp = P.song_sends[OUT_SENDS[pid]], send_spec(pid)
  return v and sp.min + v * (sp.max - sp.min)
end

function P.out_send_01(pid)
  if not P.song_sends then return P.send_param_01(pid) end
  return P.song_sends[OUT_SENDS[pid]] or 0
end

function P.delta_out_send(pid, d)
  if not P.song_sends then
    params:delta(pid, d)
    return
  end
  local sp, key = send_spec(pid), OUT_SENDS[pid]
  local v = P.song_sends[key]
  if not v then return end
  -- steps of 0.01 in the param's units, as its controlspec has them
  local real = util.clamp(sp.min + v * (sp.max - sp.min) + d * 0.01, sp.min, sp.max)
  v = (real - sp.min) / (sp.max - sp.min)
  P.song_sends[key] = v
  P.send_to_song("/send", {sp.slot, sp.param, v})
end

-- The engine's clock runs at norns' tempo, whatever the clock source. norns
-- has no tempo callback for every source, so P.start's timer checks it a
-- few times a second and sends a change.
local sent_tempo = nil

function P.send_tempo(force)
  local bpm = clock.get_tempo()
  if force or not sent_tempo or math.abs(bpm - sent_tempo) > 0.01 then
    sent_tempo = bpm
    send("/tempo", {bpm + 0.0})
  end
end

-- The norns clock's beat, so the engine's arps, synced LFOs and delays land
-- on it. /tempo stays the tempo the engine falls back to without a beat.
-- OSC floats are 32-bit, so the beat wraps at 960, a whole number of bars
-- of 3, 4, 5, 6 or 8 beats.
function P.send_beat()
  send("/beat", {clock.get_beats() % 960 + 0.0, clock.get_tempo() + 0.0})
end

function P.release_beat()
  send("/beat_release", {})
end

-- The master level the out strip shows and moves, in dB: the song's while
-- one is loaded (P.song_master, set by lib/song.lua), else the script's.
P.song_master = nil
function P.master_db() return P.song_master or params:get("master_level") end

function P.delta_master(d)
  if not P.song_master then
    params:delta("master_level", d)
    return
  end
  P.song_master = util.clamp(util.round(P.song_master + d * 0.6, 0.5), -60, 0)
  P.send_to_song("/master", {"volume", db_to_gain(P.song_master)})
end

function P.send_master()
  if P.song_holds then P.owed_master = true return end
  P.owed_master = false
  -- the master fader is 0-1.5 linear; 0 dB here is its top, as the plugin starts
  send("/master", {"volume", db_to_gain(params:get("master_level"))})
  for _, sp in ipairs(P.SEND_PARAMS) do send_send_param(sp) end
  P.send_tempo(true)
end

local function percent(param) return string.format("%d%%", math.floor(param:get() * 100 + 0.5)) end

function P.add_master_params()
  params:add_group("mixer", "sends and master", 1 + #P.SEND_PARAMS)
  params:add_control("master_level", "master level", controlspec.new(-60, 0, "lin", 0.5, 0, "dB"))
  params:set_action("master_level", function()
    if P.loading then return end
    if P.song_holds then P.owed_master = true return end
    send("/master", {"volume", db_to_gain(params:get("master_level"))})
  end)
  for _, sp in ipairs(P.SEND_PARAMS) do
    if sp.options then
      params:add_option(sp.id, sp.name, sp.options, sp.default)
    else
      local unit = sp.unit or (sp.exp and "Hz" or "")
      params:add_control(sp.id, sp.name,
        controlspec.new(sp.min, sp.max, sp.exp and "exp" or "lin", 0, sp.default, unit, sp.exp and 0.005 or 0.01),
        unit == "" and percent or nil)
    end
    params:set_action(sp.id, function()
      if P.loading then return end
      if P.song_holds then P.owed_master = true return end
      send_send_param(sp)
    end)
  end
  -- the wavetable count a PSET was saved with, for P.after_read; never shown
  params:add_number("wave_tables", "wave tables", 0, 1024, P.WAVE_TABLES)
  params:hide("wave_tables")
  -- Trinity's destination count a PSET was saved with, for P.after_read; never shown
  params:add_number("trinity_dests", "trinity dests", 0, 1024, P.TRINITY_DESTS)
  params:hide("trinity_dests")
  -- 1 in a PSET saved with Trinity's macro params, for P.after_read; never shown
  params:add_number("macro_params", "macro params", 0, 1, 1)
  params:hide("macro_params")
  -- the machine param layout a PSET was saved with, for P.after_read; never shown
  params:add_number("param_layout", "param layout", 0, 1024, LAYOUT.NOW)
  params:hide("param_layout")
end

-- "l12", "c" or "r30", for a pan of -1 to 1.
function P.pan_text(v)
  local n = math.floor(math.abs(v) * 100 + 0.5)
  if n == 0 then return "c" end
  return (v < 0 and "l" or "r") .. n
end

-- The engine's MPE zone: the lower one, master channel 1 and members 2-16,
-- or off.
function P.send_mpe()
  if P.mpe then
    send("/mpe", {P.mpe - 1, 15, params:get(id(P.mpe, "mpe_bend"))})
  else
    send("/mpe", {0, 0, 48})
  end
end

local function notes_off(part)
  -- with an MPE zone active every part plays on channel 1
  psend(part, "/cc", {part.n - 1, P.mpe and 1 or math.max(part.channel, 1), 123, 0})
  part.held = 0
  -- the notes play remembers went to the part shown
  if P.on_notes_off and P.part(part.n) == part then P.on_notes_off(part.n) end
  -- and what the modifiers hold or latched, keeping their params
  if next(part.mods) then psend(part, "/mods_off", {part.n - 1}) end
end

function P.notes_off(i) notes_off(P.part(i)) end

-- The machine's params live in pre-made slots, renamed and given the new
-- machine's steps when the machine changes, with the unused slots hidden.
-- norns can't remove params, and fixed ids keep PSETs readable whatever the
-- machine. A slot holds the 0-1 value the plugin uses; the readout maps it.
local function set_slots(i, machine, defaults)
  local defs = machine and P.defs[machine] or {}
  for j = 1, P.slots do
    local p = params:lookup_param(id(i, "s" .. j))
    local d = defs[j]
    p.def = d
    if d then
      p.name = d.name
      p.controlspec.quantum = (d.curve == "step" and d.steps > 1) and 1 / (d.steps - 1) or 0.005
      p.controlspec.default = P.unmap(d, d.default)
      if defaults then p:set(p.controlspec.default, true) end
      params:show(p.id)
    else
      p.name = "-"
      params:hide(p.id)
    end
  end
end

local function set_patch_options(i, machine)
  local p = params:lookup_param(id(i, "patch"))
  local names = machine and P.patches[machine] or {}
  p.options = #names > 0 and names or {"-"}
  p.count = #p.options
  if p.selected > p.count then p.selected = 1 end
  if machine then params:show(p.id) else params:hide(p.id) end
end

-- Updates the menu for the part's machine. `defaults` resets the slots.
-- A song track has no menu: its vals take the machine's defaults.
local function show_machine(part, machine, defaults)
  if part.machine ~= machine then part.macro_names, part.macros = nil, nil end
  part.machine = machine
  if part.song then
    part.vals.machine = machine_index(machine)
    if defaults then
      for j, d in ipairs(machine and P.defs[machine] or {}) do part.vals["s" .. j] = P.unmap(d, d.default) end
    end
    return
  end
  set_patch_options(part.n, machine)
  set_slots(part.n, machine, defaults)
  rebuild_menu()
end

local function patch_name(part)
  local names = part.machine and P.patches[part.machine] or {}
  return names[get(part, "patch")]
end

-- ---- saved presets -----------------------------------------------------------------

-- Saved presets go in a folder per machine. File paths in them are written
-- relative to dust/audio, as the app's are to its own folder, or as
-- "acidulous:path" in the script's folder, and found again the same way.
P.PRESETS = _path.data .. "acidulous/presets/"
P.ROOTS = {(_path.audio:gsub("/+", "/")), "acidulous=" .. (norns.state.path:gsub("/+", "/"))}
-- machine -> list of {file, path, index, name, count, packed}, from
-- /preset_scan; count is the file's patches, packed that it has the app's
-- samples in it
P.saved = {}

function P.preset_dir(machine) return P.PRESETS .. machine .. "/" end

-- Asks the plugin for the machine's saved presets; /preset_scan answers.
function P.scan_presets(machine)
  if machine then send("/preset_scan", {P.preset_dir(machine)}) end
end

-- What the plugin's replies call a preset, in place of a patch name.
local function preset_tag(pr) return "@" .. pr.index .. "|" .. pr.path end

-- The part's patch as /values and /macros name it.
local function patch_key(part)
  return part.preset and preset_tag(part.preset) or part.patch
end
function P.patch_key(i) return patch_key(P.part(i)) end

-- The macros of part i's Nexus patch, or nil: a song track's own, as its
-- patch may differ from the bank's.
function P.part_macros(i)
  local part = P.part(i)
  if part.song then return part.macros end
  local m = part.machine and P.patch_macros[part.machine]
  return m and m[patch_key(part)]
end

local function saved_name(path, index)
  local file = path:match("([^/]+)$")
  for _, list in pairs(P.saved) do
    for _, s in ipairs(list) do
      if s.path == path and s.index == index then return s.name end
    end
  end
  return ((file or path):gsub("%.acpreset$", ""))
end

-- "index|path|name". want is the name it's found by when the file has
-- changed; older PSETs have only "index|path".
local function save_preset_param(part)
  if part.song then return end
  local pr = part.preset
  params:set(id(part.n, "preset"), pr and (pr.index .. "|" .. pr.path .. "|" .. (pr.name or "")) or "", true)
end

local function decode_preset(s)
  local index, path, name = (s or ""):match("^(%d+)|(/.-%.acpreset)|(.*)$")
  if not index then index, path = (s or ""):match("^(%d+)|(.+)$") end
  if not index then return nil end
  index = tonumber(index)
  if name == "" then name = nil end
  return {path = path, index = index, name = name or saved_name(path, index), want = name}
end

local function forget_preset(part)
  part.preset = nil
  save_preset_param(part)
end

-- The slot values each machine and patch last had, for the run: key ->
-- {slot -> 0-1 value}. Shown at once when the part switches back. A song's
-- tracks have their own, emptied when it goes.
local memory = {}
local song_memory = {}
local function mems(part) return part.song and song_memory or memory end

local function mem_key(part)
  return part.machine and (part.machine .. "/" .. (patch_key(part) or ""))
end

local function remember(part)
  local key = mem_key(part)
  if not key then return end
  local t = {}
  for j = 1, #P.defs[part.machine] do t[j] = get(part, "s" .. j) end
  mems(part)[key] = t
end

-- After a machine or patch switch: the slots take what the patch last had,
-- or its machine's defaults, dimmed until the plugin's values come.
local function recall(part)
  part.dim = false
  local key = mem_key(part)
  if not key then return end
  local t = mems(part)[key]
  if t then
    -- also put on top of the patch's values when they arrive, so they stay
    for j, v in pairs(t) do
      put(part, "s" .. j, v)
      part.edits[j] = v
    end
  elseif patch_name(part) or part.preset then
    for j, d in ipairs(P.defs[part.machine]) do
      put(part, "s" .. j, P.unmap(d, d.default))
    end
    part.dim = true
  end
end

-- ---- files ---------------------------------------------------------------------

-- The part's files as the "files" param holds them: a load per entry,
-- "kind,arg,path", joined with "|".
local function encode(files)
  local keys = {}
  for k in pairs(files) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b)
    local na, nb = tonumber(a:match("%d+")), tonumber(b:match("%d+"))
    if na and nb then return na < nb end
    return a < b
  end)
  local out = {}
  for _, k in ipairs(keys) do
    local f = files[k]
    out[#out + 1] = f.kind .. "," .. f.arg .. "," .. f.path
  end
  return table.concat(out, "|")
end

local function decode(s)
  local files = {}
  for entry in (s or ""):gmatch("[^|]+") do
    local kind, arg, path = entry:match("^(%a+),(%-?%d+),(.+)$")
    if kind then
      arg = tonumber(arg)
      files[kind == "pad" and ("pad" .. arg) or "main"] = {kind = kind, path = path, arg = arg}
    end
  end
  return files
end

local function save_files(part)
  if part.song then return end
  params:set(id(part.n, "files"), encode(part.files), true)
end

-- ---- insert effects --------------------------------------------------------------

-- The part's insert edits as the "fx" param holds them: "slot,name,value"
-- per edit, joined with "|".
local function encode_fx(edits)
  local keys = {}
  for k in pairs(edits) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do
    local slot, name = k:match("^(%d):(.+)$")
    out[#out + 1] = string.format("%s,%s,%.4f", slot, name, edits[k])
  end
  return table.concat(out, "|")
end

local function decode_fx(s)
  local edits = {}
  for entry in (s or ""):gmatch("[^|]+") do
    local slot, name, v = entry:match("^([01]),([^,]+),([%d%.eE+-]+)$")
    if slot and tonumber(v) then edits[slot .. ":" .. name] = util.clamp(tonumber(v), 0, 1) end
  end
  return edits
end

local function save_fx(part)
  if part.song then return end
  params:set(id(part.n, "fx"), encode_fx(part.fx_edits), true)
end

-- Drops the part's insert edits, for a newly picked patch.
local function clear_fx(part)
  part.fx_edits = {}
  save_fx(part)
end

-- An insert control's 0-1 value (ctl from panel.fx_cards).
function P.fx_value(i, ctl)
  local e = P.part(i).fx[ctl.fx]
  local d = e and e.params[ctl.index]
  return d and d.value or 0
end

function P.set_fx(i, ctl, v)
  local part = P.part(i)
  local e = part.fx[ctl.fx]
  local d = e and e.params[ctl.index]
  -- a patch on its way brings its own inserts
  if not d or part.pending then return end
  d.value = v
  part.fx_edits[ctl.fx .. ":" .. d.name] = v
  save_fx(part)
  psend(part, "/fx", {i - 1, ctl.fx, d.name, v})
end

-- Sends the part's insert edits, after its /patch. The plugin runs them after
-- the patch, so they go on top of its inserts.
local function send_fx_edits(part)
  for k, v in pairs(part.fx_edits) do
    local slot, name = k:match("^(%d):(.+)$")
    psend(part, "/fx", {part.n - 1, tonumber(slot), name, v})
  end
end


-- ---- modifiers ---------------------------------------------------------------------

-- The part's modifiers as the "mods" param holds them: per slot
-- "slot,type,bypass" then ",name=value" per param, joined with "|".
local function encode_mods(mods)
  local out = {}
  for slot = 0, P.MOD_SLOTS - 1 do
    local m = mods[slot]
    if m then
      local names = {}
      for k in pairs(m.values) do names[#names + 1] = k end
      table.sort(names)
      local e = {string.format("%d,%s,%d", slot, m.type, m.bypass and 1 or 0)}
      for _, k in ipairs(names) do e[#e + 1] = string.format("%s=%.4f", k, m.values[k]) end
      out[#out + 1] = table.concat(e, ",")
    end
  end
  return table.concat(out, "|")
end

local function decode_mods(s)
  local mods = {}
  for entry in (s or ""):gmatch("[^|]+") do
    local slot, type, bypass, rest = entry:match("^([0-9]),(%a+),([01])(.*)$")
    if slot and tonumber(slot) < P.MOD_SLOTS and tab.contains(P.MOD_TYPES, type) then
      local values = {}
      for name, v in rest:gmatch(",([%w_]+)=([%d%.eE+-]+)") do
        if tonumber(v) then values[name] = util.clamp(tonumber(v), 0, 1) end
      end
      mods[tonumber(slot)] = {type = type, bypass = bypass == "1", values = values}
    end
  end
  return mods
end

local function save_mods(part)
  if part.song then return end
  params:set(id(part.n, "mods"), encode_mods(part.mods), true)
end

-- Sends a slot's modifier with its bypass and params, or empties the slot.
local function send_mod(part, slot)
  local m, r = part.mods[slot], part.n - 1
  psend(part, "/mod", {r, slot, m and m.type or ""})
  if m then
    if m.bypass then psend(part, "/mod_bypass", {r, slot, 1}) end
    for name, v in pairs(m.values) do psend(part, "/mod_param", {r, slot, name, v}) end
  end
end

-- Sends the part's modifiers, all slots, emptying the ones it has none in.
-- A type change still waiting to be sent goes with them.
local function send_mods(part)
  for slot = 0, P.MOD_SLOTS - 1 do
    if part.mod_pending then
      timer.cancel(part.mod_pending[slot])
      part.mod_pending[slot] = nil
    end
    send_mod(part, slot)
  end
end

local function mod_def(type, name)
  local defs = P.mod_defs[type]
  return defs and defs.by[name]
end

-- A modifier param's 0-1 value: what was set, or its default.
local function mod_param_value(m, name)
  if m.values[name] then return m.values[name] end
  local d = mod_def(m.type, name)
  return d and P.unmap(d, d.default) or 0
end

-- Scale and Chord on one part share key and scale.
local SHARED = {key = true, scale = true}
local function shares(m) return m and (m.type == "Scale" or m.type == "Chord") end

-- Another slot of the part holding a Scale or Chord, or nil.
local function shared_with(part, slot)
  for s = 0, P.MOD_SLOTS - 1 do
    if s ~= slot and shares(part.mods[s]) then return part.mods[s] end
  end
end

local function set_mod_param(part, slot, name, v)
  part.mods[slot].values[name] = v
  -- a type on its way is sent with its values
  if part.mod_pending and part.mod_pending[slot] then return end
  psend(part, "/mod_param", {part.n - 1, slot, name, v})
end

-- Puts a modifier type (or nil) in the part's slot, on and with its
-- defaults. A Scale or Chord takes key and scale from one in the other slot.
-- With wait it's sent once no other type has come for 0.3 s, so turning
-- through the types mounts only the one it stops on. Held notes are let go
-- first, as their note offs would go to the new modifier.
local function set_mod_type(part, slot, type, wait)
  local old = part.mods[slot]
  if (old and old.type) == type then return end
  local m = type and {type = type, bypass = false, values = {}} or nil
  part.mods[slot] = m
  local other = shared_with(part, slot)
  if shares(m) and other then
    for name in pairs(SHARED) do m.values[name] = mod_param_value(other, name) end
  end
  save_mods(part)
  part.mod_pending = part.mod_pending or {}
  timer.cancel(part.mod_pending[slot])
  part.mod_pending[slot] = nil
  local function go()
    part.mod_pending[slot] = nil
    notes_off(part)
    send_mod(part, slot)
  end
  if wait then part.mod_pending[slot] = timer.after(0.3, go) else go() end
end
function P.set_mod_type(i, slot, type, wait) set_mod_type(P.part(i), slot, type, wait) end

-- A modifier control's 0-1 value (ctl from panel.mod_cards): the slot's
-- type, on, or a param.
function P.mod_value(i, ctl)
  local m = P.part(i).mods[ctl.mod]
  if ctl.what == "type" then
    local at = m and tab.key(P.MOD_TYPES, m.type) or 0
    return at / #P.MOD_TYPES
  elseif ctl.what == "on" then
    return (m and not m.bypass) and 1 or 0
  end
  return m and mod_param_value(m, ctl.p) or 0
end

local function set_mod(part, ctl, v)
  local m = part.mods[ctl.mod]
  if ctl.what == "type" then
    local at = math.floor(v * #P.MOD_TYPES + 0.5)
    set_mod_type(part, ctl.mod, P.MOD_TYPES[at], true)
    return
  end
  if not m then return end
  if ctl.what == "on" then
    local bypass = v < 0.5
    if bypass == m.bypass then return end
    m.bypass = bypass
    -- a type on its way brings its bypass
    if part.mod_pending and part.mod_pending[ctl.mod] then save_mods(part) return end
    -- held notes are let go, as their note offs would miss the notes
    notes_off(part)
    psend(part, "/mod_bypass", {part.n - 1, ctl.mod, bypass and 1 or 0})
  else
    local was = mod_param_value(m, ctl.p)
    set_mod_param(part, ctl.mod, ctl.p, v)
    -- the engine only lets latched notes go on the next note on, so latch
    -- off lets them go now; this also ends notes on keys still held
    if ctl.p == "latch" and was >= 0.5 and v < 0.5 and not m.bypass then notes_off(part) end
    if SHARED[ctl.p] and shares(m) then
      for s = 0, P.MOD_SLOTS - 1 do
        if s ~= ctl.mod and shares(part.mods[s]) then set_mod_param(part, s, ctl.p, v) end
      end
    end
  end
  save_mods(part)
end
function P.set_mod(i, ctl, v) set_mod(P.part(i), ctl, v) end

-- The slot holding a modifier type on the part, or nil.
local function mod_slot(part, type)
  local mods = part.mods
  for slot = 0, P.MOD_SLOTS - 1 do
    if mods[slot] and mods[slot].type == type then return slot end
  end
end
function P.mod_slot(i, type) return mod_slot(P.part(i), type) end

-- True while part i has the type mounted and on.
function P.mod_on(i, type)
  local s = P.mod_slot(i, type)
  return s ~= nil and not P.part(i).mods[s].bypass
end

-- A control for a param of the type on part i, as panel.mod_cards makes
-- them, or nil while it has none or its defs haven't come.
function P.mod_ctl(i, type, name)
  local s = P.mod_slot(i, type)
  local d = s and mod_def(type, name)
  if d then return {mod = s, p = name, def = d} end
end

-- Puts the part's modifiers in the slots layout names (slot -> old slot),
-- with their bypass and params, and sends every slot that changed.
local function rearrange(part, layout)
  local waiting = {}
  if part.mod_pending then
    for slot = 0, P.MOD_SLOTS - 1 do
      waiting[slot] = part.mod_pending[slot] ~= nil
      timer.cancel(part.mod_pending[slot])
      part.mod_pending[slot] = nil
    end
  end
  local old, new = part.mods, {}
  for slot, from in pairs(layout) do new[slot] = old[from] end
  part.mods = new
  save_mods(part)
  notes_off(part)
  for slot = 0, P.MOD_SLOTS - 1 do
    if new[slot] ~= old[slot] or waiting[slot] then send_mod(part, slot) end
  end
end

-- The chain runs slot 0 first: scale before chord before arp.
local MOD_RANK = {Scale = 1, Chord = 2, Arp = 3}

-- Turns a modifier on part i on or off. Off bypasses it and keeps its
-- params; on un-bypasses it, or mounts it in a free slot, moving the others
-- only if the chain would be out of order. Returns "on", "off", or "full"
-- when every slot holds other modifiers.
function P.toggle_mod(i, type)
  local part = P.part(i)
  local s = mod_slot(part, type)
  if s then
    local on = part.mods[s].bypass
    set_mod(part, {mod = s, what = "on"}, on and 1 or 0)
    return on and "on" or "off"
  end
  -- the first free slot that keeps the ranks rising along the slots
  local want
  for free = 0, P.MOD_SLOTS - 1 do
    if not part.mods[free] then
      local ok, last = true, 0
      for slot = 0, P.MOD_SLOTS - 1 do
        local t = slot == free and type or (part.mods[slot] and part.mods[slot].type)
        if t then
          if MOD_RANK[t] < last then ok = false end
          last = MOD_RANK[t]
        end
      end
      if ok then want = free break end
    end
  end
  if not want then
    -- none does: pack them in rank order from slot 0
    local list = {}
    for slot = 0, P.MOD_SLOTS - 1 do
      if part.mods[slot] then list[#list + 1] = {rank = MOD_RANK[part.mods[slot].type], from = slot} end
    end
    if #list >= P.MOD_SLOTS then return "full" end
    list[#list + 1] = {rank = MOD_RANK[type]}
    table.sort(list, function(a, b)
      if a.rank ~= b.rank then return a.rank < b.rank end
      return (a.from or 9) < (b.from or 9)
    end)
    local layout = {}
    for k, e in ipairs(list) do
      if e.from then layout[k - 1] = e.from else want = k - 1 end
    end
    rearrange(part, layout)
  end
  set_mod_type(part, want, type)
  return "on"
end

-- The part's key and scale as "D dor" when it has a Scale, else nil.
local KEYS = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
P.SCALE_SHORT = {"maj", "dor", "phr", "lyd", "mix", "min", "loc", "hmin", "mmin", "maj5", "min5",
  "majbl", "minbl", "egy", "hunm", "byz", "pers", "hira", "insen", "iwato", "enig", "phrdom",
  "neapm", "neapM", "alt", "lyddom", "lydaug", "loc2", "bebd", "bebM", "whole", "dimwh", "dimhw"}
function P.scale_text(i)
  local mods = P.part(i).mods

  for slot = 0, P.MOD_SLOTS - 1 do
    local m = mods[slot]
    if m and m.type == "Scale" then
      local kd, sd = mod_def("Scale", "key"), mod_def("Scale", "scale")
      if not (kd and sd) then return nil end
      local key = math.floor(P.map(kd, mod_param_value(m, "key")) + 0.5)
      local scale = math.floor(P.map(sd, mod_param_value(m, "scale")) + 0.5)
      return (KEYS[key + 1] or "?") .. " " .. (P.SCALE_SHORT[scale + 1] or tostring(scale + 1))
    end
  end
end

local function base(path) return path:match("([^/]+)/*$") or path end
local function stem(path) return (base(path):gsub("%.[^%.]*$", "")) end

-- The machines that load files.
P.SAMPLE_MACHINES = {Forage = true, Mosaic = true, Dice = true, Pollen = true, Molt = true}

-- Sends /load. kind and arg as host/src/bridge.h has them. A path with | in
-- it is refused, as | separates the files in the saved list.
local function load_file(part, kind, path, arg)
  if path:find("|", 1, true) then
    part.problem = {text = base(path) .. ": names with | can't be loaded", at = util.time()}
    return
  end
  if held(part) then return end
  part.loading = {kind = kind, name = kind == "sf2" and base(path) or stem(path)}
  part.problem = nil
  psend(part, "/load", {part.n - 1, kind, path, arg or 0, table.unpack(P.ROOTS)})
end
function P.load_file(i, kind, path, arg) load_file(P.part(i), kind, path, arg) end

-- Empties the part's files and the machine's samples. Pollen then plays the
-- norns input.
function P.clear_files(i)
  local part = P.part(i)
  part.files, part.loading, part.problem, part.last_path = {}, nil, nil, nil
  part.song_files = false
  psend(part, "/load", {i - 1, "clear", "", 0})
  save_files(part)
end

-- What a sample machine loads when a part switches to it with no files:
-- {kind, path}, from the audio that comes with norns or our own data.
P.DEFAULTS = {
  Forage = {kind = "kit", path = _path.audio .. "common/808"},
  Mosaic = {kind = "sf2", path = norns.state.path .. "data/samples/acidulous-soft-horn.sf2"},
  Dice = {kind = "sample", path = _path.audio .. "tehn/drumev.wav"},
  Pollen = {kind = "sample", path = _path.audio .. "hermit_leaves.wav"},
  Molt = {kind = "sample", path = _path.audio .. "tehn/whirl1.aif"},
}

-- Loads the machine's default if the part has no files and it's there.
local function load_default(part)
  part.want_default = false
  local d = part.machine and P.DEFAULTS[part.machine]
  if not d or next(part.files) ~= nil or part.loading then return end
  if util.file_exists(d.path) then load_file(part, d.kind, d.path, 0) end
end

-- Sends the part's saved files again, after its machine is mounted.
local function send_files(part)
  part.resend_files = false
  if not part.machine then return end
  local list = {}
  for _, f in pairs(part.files) do list[#list + 1] = f end
  table.sort(list, function(a, b) return a.arg < b.arg end)
  psend(part, "/load", {part.n - 1, "clear", "", 0})
  for _, f in ipairs(list) do load_file(part, f.kind, f.path, f.arg) end
end

-- Forage's pads, 1-13: the name of what each has, or nil.
function P.pad_names(i)
  local names = {}
  for pad = 0, 12 do
    local f = P.part(i).files["pad" .. pad]
    if f then names[pad + 1] = f.name or stem(f.path) end
  end
  return names
end

-- The last file or folder loaded, or one the part has.
function P.loaded_path(i)
  local part = P.part(i)
  if part.last_path then return part.last_path end
  for _, f in pairs(part.files) do return f.path end
end

-- What the part's machine has loaded, for the screen: "loading ...", the
-- last load that failed for 5 seconds, or the files' names. nil for a
-- machine that takes no files.
function P.material(i)
  local part = P.part(i)
  if part.loading then return "loading " .. part.loading.name end
  if part.problem and util.time() - part.problem.at < 5 then return part.problem.text end
  local m = part.machine
  -- a song track plays the song's samples until something is loaded
  if part.song_files and P.SAMPLE_MACHINES[m] and next(part.files) == nil then return "the song's" end
  if m == "Forage" then
    local n, first = 0, nil
    for pad = 0, 12 do
      local f = part.files["pad" .. pad]
      if f then
        n = n + 1
        first = first or f
      end
    end
    if n == 0 then return "empty, k1+k3" end
    return base(first.path:match("^(.*)/[^/]+$") or "") .. ", " .. n .. (n == 1 and " pad" or " pads")
  elseif m == "Mosaic" or m == "Dice" or m == "Pollen" or m == "Molt" then
    local f = part.files.main
    if f then return f.name or stem(f.path) end
    return m == "Pollen" and "live input" or "empty, k1+k3"
  end
end

-- Mounts the part's machine with its patch, or empties it.
-- Sends /preset_load for the part's saved preset. Its files come with it
-- unless the part's own files are sent after.
local function send_preset(part)
  local pr = part.preset
  send_patch(part, "/preset_load", {part.n - 1, pr.path, pr.index, preset_tag(pr), pr.files and 1 or 0,
    part.machine, pr.want or pr.name or "", table.unpack(P.ROOTS)})
end

local function load_machine(part, with_values)
  local r = part.n - 1
  part.pending = nil
  -- a script part's machine or patch whose timer fired after a song took
  -- the racks: it loads fresh with its own values when they're its again
  if held(part, true) then return end
  local name = patch_name(part)
  local pr = part.machine and part.preset
  part.want_values = false
  part.song_files = false
  if part.song then part.macros = nil end
  if not with_values then part.edits = {} end
  if not part.machine then
    part.fx = {}
    send_patch(part, "/unmount", {r})
    name = nil
  elseif pr then
    part.want_values = with_values
    part.values_at, part.values_retried = util.time(), false
    part.sends_moved = {}
    part.fx = {}
    pr.files = with_values and not part.resend_files
    send_preset(part)
    send_fx_edits(part)
  elseif name then
    part.want_values = with_values
    part.values_at, part.values_retried = util.time(), false
    part.sends_moved = {}
    part.fx = {} -- until /inserts lists the patch's
    send_patch(part, "/patch", {r, part.machine, name})
    send_fx_edits(part)
  else
    psend(part, "/mount", {r, part.machine})
  end
  part.patch = pr and pr.name or name
  part.sent = pr and preset_tag(pr) or name
  if not part.want_values then part.dim = false end
  if part.resend_files then send_files(part) end
  if part.want_default then load_default(part) end
end

-- Runs fn after `wait` seconds unless called again first, so turning an
-- encoder through patches only loads the one it stops on.
local function later(part, wait, fn)
  timer.cancel(part.pending)
  part.pending = timer.after(wait, function()
    part.pending = nil
    fn()
  end)
end

-- What a part does when its machine, patch or a slot changes: the script's
-- from its param actions, a song track's from P.set. A script part's change
-- while a song holds the racks is kept and sent when they're its again.
local function machine_changed(part, machine)
  if machine == part.machine and not part.pending then return end -- a bang
  notes_off(part)
  part.edits = {}
  clear_fx(part)
  forget_preset(part)
  -- the new machine starts empty
  part.files, part.loading, part.problem, part.last_path = {}, nil, nil, nil
  part.want_default = P.SAMPLE_MACHINES[machine] or false
  save_files(part)
  put(part, "patch", 1)
  show_machine(part, machine, true)
  part.patch = patch_name(part)
  put(part, "patch_name", part.patch or "")
  recall(part)
  if held(part, true) then return end
  later(part, 0.3, function() load_machine(part, true) end)
end

local function patch_changed(part)
  if not part.machine then return end
  local name = patch_name(part)
  if name == part.sent and not part.pending and not part.fresh then return end -- a bang
  part.patch = name
  part.edits = {}
  clear_fx(part)
  forget_preset(part)
  recall(part)
  if held(part, true) then return end
  later(part, 0.2, function() load_machine(part, true) end)
end

local function slot_changed(part, j, v)
  if not part.machine or j > #P.defs[part.machine] then return end
  -- An edit made while a patch is on its way goes on top of the patch
  -- once its values arrive.
  local mem = mems(part)[mem_key(part) or ""]
  if mem then
    mem[j] = v
  elseif not (part.pending or part.want_values) then
    remember(part)
  end
  if part.pending then
    part.edits[j] = v
    return
  end
  if held(part) then
    -- a new patch still to load takes it on top of its values
    if part.fresh or part.park_fresh then part.edits[j] = v end
    return
  end
  psend(part, "/param", {part.n - 1, j - 1, v})
  if part.want_values then part.edits[j] = v end
end

-- Loads a part's machine and patch with the patch's values (and the edits
-- made since on top), with its strip, files and modifiers.
local function load_fresh(part)
  send_strip(part)
  part.resend_files = next(part.files) ~= nil
  load_machine(part, true)
  send_mods(part)
end

-- Sends a script part's whole state, after a PSET is read. The patch goes
-- first, which mounts the machine, then the saved params and sends on top
-- of it, then its files. A machine or patch picked while a song held the
-- racks loads fresh instead, as the slots don't hold its values.
local function apply(part)
  local i = part.n
  timer.cancel(part.pending)
  part.pending = nil
  notes_off(part)
  part.owed = false
  if part.fresh then
    part.fresh = false
    load_fresh(part)
    return
  end
  send_strip(part)
  part.fx_edits = decode_fx(params:get(id(i, "fx")))
  part.preset = decode_preset(params:get(id(i, "preset")))
  part.want_default = false -- a PSET's files stay as saved
  load_machine(part, false)
  -- the PSET's sends go on top of the ones the patch carries
  send_strip_param(part, "rev")
  send_strip_param(part, "dly")
  -- C++ waits for room in the engine's param queue, so none of these is lost
  if part.machine then
    for j, d in ipairs(P.defs[part.machine]) do
      if not (part.macs_from_patch and d.name:match("^mac%d")) then
        psend(part, "/param", {i - 1, j - 1, params:get(id(i, "s" .. j))})
      end
    end
  end
  part.files = decode(params:get(id(i, "files")))
  part.loading, part.last_path = nil, nil
  -- with no files this still clears what the machine had before
  if P.SAMPLE_MACHINES[part.machine] then send_files(part) end
  part.mods = decode_mods(params:get(id(i, "mods")))
  send_mods(part)
end

-- The script's own parts on: the "parts" param's 8 or 16, which a song
-- with more tracks doesn't change.
P.own_active = 8
local function own_on(i) return i <= P.own_active end

-- Parks script part i: silences it and unmounts its machine, keeping its
-- params and what the script knows of it, for when it's shown again.
-- While a song holds the racks nothing is sent, and it's parked again
-- when they're the script's.
local function park(part)
  if not part.parked then
    -- a patch still on its way, or its values not in yet: load it fresh
    -- when it comes back, as the slots don't hold its values
    part.park_fresh = part.pending ~= nil or part.want_values or part.fresh or false
  end
  timer.cancel(part.pending)
  part.pending = nil
  part.parked = true
  part.fresh = false
  part.want_values, part.dim = false, false
  part.loading = nil
  part.level, part.held = 0, 0
  if P.song_holds then
    part.owed = true
    return
  end
  part.owed = false
  notes_off(part)
  part.fx = {}
  send_patch(part, "/unmount", {part.n - 1})
  -- a parked part's solo would keep the others quiet
  psend(part, "/channel", {part.n - 1, "solo", 0})
end

-- Brings a parked part back as it was.
local function unpark(part)
  part.parked = false
  if part.park_fresh and part.machine then
    if not held(part, true) then load_fresh(part) end
  elseif not held(part) then
    apply(part)
  end
  part.park_fresh = false
end

-- Parks the script parts past P.own_active and brings back the ones shown
-- again.
local function sync_parked()
  for i = 1, P.COUNT do
    local part = own[i]
    if own_on(i) and part.parked then
      unpark(part)
    elseif not own_on(i) and not part.parked then
      park(part)
    end
  end
  P.send_mpe()
end

function P.apply_all()
  P.send_master()
  for i = 1, P.COUNT do
    local part = own[i]
    if own_on(i) then
      part.parked = false
      apply(part)
    else
      -- its params are the PSET's now, so they're sent as they are
      part.fresh = false
      park(part)
      part.park_fresh = false
    end
  end
  P.send_mpe()
end

-- After a song held the racks and gave them back as they were (a load
-- refused before it touched them): sends what changed meanwhile.
function P.pay_owed()
  if P.owed_master then P.send_master() end
  for i = 1, P.COUNT do
    local part = own[i]
    if part.owed then
      if own_on(i) then
        part.parked = false
        apply(part)
      else
        park(part)
      end
    end
  end
  P.send_mpe()
end

-- The parts the pages show: the script's own count, or 16 for a song with
-- more than 8 tracks. Hidden parts' groups are hidden in the params menu,
-- and the "mpe part" options stop at the last part shown.
local function show_view()
  local n = P.own_active
  if P.song_view and P.song_wide then n = 16 end
  P.active = n
  for i = 9, P.COUNT do
    if own_on(i) then params:show("part_" .. i) else params:hide("part_" .. i) end
  end
  rebuild_menu()
  if P.on_active then P.on_active(n) end
end

local function show_parts(n)
  P.own_active = n
  show_view()
end


-- The "parts" param, 8 or 16. While a PSET is read its parts are sent once
-- the read is done, so the switch waits a moment for P.after_read to take
-- it over.
function P.add_count_param()
  params:add_option("parts", "parts", {"8", "16"}, 1)
  params:set_action("parts", function(v)
    show_parts(v == 2 and 16 or 8)
    if P.loading then return end
    timer.cancel(P.parts_timer)
    P.parts_timer = timer.after(0.05, function()
      P.parts_timer = nil
      sync_parked()
    end)
  end)
end

local function add_part(i)
  local part = own[i]
  params:add_group("part_" .. i, "part " .. i, 17 + P.slots)

  local chans = {"off"}
  for c = 1, 16 do chans[c + 1] = tostring(c) end
  params:add_option(id(i, "channel"), "channel", chans, i + 1)
  params:set_action(id(i, "channel"), function(v)
    if not P.loading then notes_off(part) end
    part.channel = v - 1
  end)

  -- Hidden: mirrors the global "mpe part", so older PSETs with a part's
  -- mpe on still pick it.
  params:add_option(id(i, "mpe"), "mpe", {"off", "on"}, 1)
  params:set_action(id(i, "mpe"), function(v)
    local cur = params:get("mpe_part")
    if v == 2 and cur ~= i + 2 then
      params:set("mpe_part", i + 2)
    elseif v == 1 and cur == i + 2 then
      params:set("mpe_part", 2)
    end
  end)
  params:add_number(id(i, "mpe_bend"), "mpe bend", 1, 96, 48, function(param) return param:get() .. " st" end)
  params:set_action(id(i, "mpe_bend"), function()
    if P.mpe == i and not P.loading then P.send_mpe() end
  end)

  local names = {"none"}
  for _, m in ipairs(P.machines) do names[#names + 1] = m.name end
  params:add_option(id(i, "machine"), "machine", names, 1)
  params:set_action(id(i, "machine"), function(v)
    local machine = v > 1 and P.machines[v - 1].name or nil
    params:set(id(i, "machine_name"), machine or "", true)
    if P.loading then
      show_machine(part, machine, false)
      part.dim = false
      return
    end
    machine_changed(part, machine)
  end)

  params:add_option(id(i, "patch"), "patch", {"-"}, 1)
  params:set_action(id(i, "patch"), function()
    params:set(id(i, "patch_name"), patch_name(part) or "", true)
    if P.loading then return end
    patch_changed(part)
  end)

  -- the files loaded into the machine, for PSETs; never shown
  params:add_text(id(i, "files"), "files", "")
  -- insert params moved by hand, for PSETs; never shown
  params:add_text(id(i, "fx"), "fx", "")
  -- the saved preset the part plays, "index|path", for PSETs; never shown
  params:add_text(id(i, "preset"), "preset", "")
  -- the modifiers, "slot,type,bypass,name=value..." per slot, for PSETs; never shown
  params:add_text(id(i, "mods"), "mods", "")
  -- the machine's name, for PSETs, as the machine option's index depends on
  -- which machines there are; never shown
  params:add_text(id(i, "machine_name"), "machine name", "")
  -- the bank patch's name, for PSETs, as its index changes when the bank
  -- does; never shown
  params:add_text(id(i, "patch_name"), "patch name", "")

  params:add_control(id(i, "volume"), "volume", controlspec.new(-60, 3, "lin", 0.5, 0, "dB"))
  params:set_action(id(i, "volume"), function()
    if P.loading or held(part) then return end
    send_volume(part)
  end)
  params:add_control(id(i, "pan"), "pan", controlspec.new(-1, 1, "lin", 0.01, 0, "", 0.01),
    function(param) return P.pan_text(param:get()) end)
  params:add_option(id(i, "mute"), "mute", {"off", "on"}, 1)
  params:add_option(id(i, "solo"), "solo", {"off", "on"}, 1)
  params:add_control(id(i, "rev"), "rev send", controlspec.new(0, 1, "lin", 0.01, 0, "", 0.01), percent)
  params:add_control(id(i, "dly"), "dly send", controlspec.new(0, 1, "lin", 0.01, 0, "", 0.01), percent)
  for _, what in ipairs({"pan", "mute", "solo", "rev", "dly"}) do
    params:set_action(id(i, what), function()
      if P.loading or held(part) then return end
      send_strip_param(part, what)
    end)
  end

  for j = 1, P.slots do
    local sid = id(i, "s" .. j)
    params:add_control(sid, "-", controlspec.new(0, 1, "lin", 0, 0, "", 0.005))
    local p = params:lookup_param(sid)
    p.formatter = function(param)
      if not param.def then return "" end
      return P.readout(param.def, param:get())
    end
    params:set_action(sid, function(v)
      if P.loading then return end
      slot_changed(part, j, v)
    end)

  end
end

function P.add_params()
  for i = 1, P.COUNT do add_part(i) end
  P.add_master_params()
  for i = 1, P.COUNT do
    params:hide(id(i, "mpe"))
    show_machine(own[i], nil, true)
    params:hide(id(i, "files"))
    params:hide(id(i, "fx"))
    params:hide(id(i, "preset"))
    params:hide(id(i, "mods"))
    params:hide(id(i, "machine_name"))
    params:hide(id(i, "patch_name"))
  end
  show_parts(params:get("parts") == 2 and 16 or 8)
  -- params:clear() removes this when the script ends.
  params.action_read = function(filename) P.after_read(filename) end
end

-- ---- old PSETs ---------------------------------------------------------------------

-- The wavetables there are now. A PSET saves it in "wave_tables"; one without
-- it was written when there were 8. Slots hold 0-1 values, so when the bank
-- grows every saved wave choice moves unless it's rescaled, as the engine's
-- TableCounts.h does for engine lines.
P.WAVE_TABLES = 32
local TRINITY_WAVES_BEFORE, TRINITY_WAVES = 12, 4 + P.WAVE_TABLES
local NEXUS_TABLES_BEFORE, NEXUS_TABLES = 8, P.WAVE_TABLES

-- Trinity's matrix and macro destinations (mNN_dest, macN_dN), 34 before
-- the LFO step ones were added at the end. A PSET saves the count in
-- "trinity_dests".
P.TRINITY_DESTS = 37
local TRINITY_DESTS_BEFORE = 34

-- A count a PSET file was written with: the number saved under key, before
-- if it has none, or now with no file.
local function saved_count(filename, key, before, now)
  local f = filename and io.open(filename, "r")
  if not f then return now end
  local n = before
  for line in f:lines() do
    local v = line:match('^"' .. key .. '"%s*:%s*(%d+)')
    if v then n = tonumber(v) break end
  end
  f:close()
  return n
end

-- The "wave_tables" a PSET file was written with, or 8 if it has none.
local function saved_tables(filename)
  return saved_count(filename, "wave_tables", 8, P.WAVE_TABLES)
end

-- Readies a Trinity part's macro params for a PSET from before they existed,
-- which has nothing for them, and a hidden slot can hold what another machine
-- left there. With a patch, P.apply leaves them out and /values fills them
-- with the patch's own routes. Without one they go to their defaults (knobs
-- at 0, no destinations).
local function reset_macros(i)
  if own[i].machine ~= "Trinity" then return end
  if patch_name(own[i]) then
    own[i].macs_from_patch = true
    return
  end
  for j, d in ipairs(P.defs.Trinity or {}) do
    if d.name:match("^mac%d") then params:set(id(i, "s" .. j), P.unmap(d, d.default), true) end
  end
end

-- Whether a PSET file has the param.
local function saved_has(filename, key)
  local f = filename and io.open(filename, "r")
  if not f then return true end
  local found = false
  for line in f:lines() do
    if line:match('^"' .. key .. '"%s*:') then found = true break end
  end
  f:close()
  return found
end

-- The slots holding a wtosc in a Nexus patch's graph, from its bank, as a
-- set of slot numbers.
local function wtosc_slots(patch)
  local slots, current = {}, nil
  local f = patch and io.open(norns.state.path .. "data/banks/Nexus.bank")
  if not f then return slots end
  for line in f:lines() do
    local rest = line:match("^%s*patch%s+(.*)$")
    if rest then
      current = rest:match('^"([^"]*)"') or rest:match("^(%S+)")
    elseif current == patch then
      local graph = line:match('^%s*set%s+nexus%s+"(.*)"')
      if graph then
        for s in graph:gmatch("m|(%d+)|wtosc") do slots[tonumber(s)] = true end
      end
    end
  end
  f:close()
  return slots
end

-- Rescales a part's saved wave choices from a PSET made with 8 wavetables:
-- Trinity's o1-o3_wave from 12 steps to 36, the same wave, and a Nexus wtosc's
-- table knob (its slot's first knob) from 8 tables to 32, the middle of the
-- same table.
local function migrate_waves(i)
  local machine = own[i].machine
  if machine ~= "Trinity" and machine ~= "Nexus" then return end
  local wt = machine == "Nexus" and wtosc_slots(patch_name(own[i])) or {}
  for j, d in ipairs(P.defs[machine] or {}) do
    local sid = id(i, "s" .. j)
    local v = util.clamp(params:get(sid), 0, 1)
    local new
    if machine == "Trinity" and d.name:match("^o[123]_wave$") then
      new = math.floor(v * (TRINITY_WAVES_BEFORE - 1) + 0.5) / (TRINITY_WAVES - 1)
    elseif machine == "Nexus" then
      local slot = d.name:match("^s(%d%d)_p1$")
      if slot and wt[tonumber(slot)] then
        new = (math.floor(math.min(v, 0.9999) * NEXUS_TABLES_BEFORE) + 0.5) / NEXUS_TABLES
      end
    end
    if new then params:set(sid, new, true) end
  end
end

-- Moves a Trinity part's saved destinations from a PSET made with 34 onto
-- the 37 there are now, the same destination, as the engine's
-- tables::trinityDest does.
local function migrate_dests(i)
  if own[i].machine ~= "Trinity" then return end
  for j, d in ipairs(P.defs.Trinity or {}) do
    if d.name:match("^m%d%d_dest$") or d.name:match("^mac[1-8]_d[1-4]$") then
      local sid = id(i, "s" .. j)
      local v = util.clamp(params:get(sid), 0, 1)
      params:set(sid, math.floor(v * (TRINITY_DESTS_BEFORE - 1) + 0.5) / (P.TRINITY_DESTS - 1), true)
    end
  end
end

-- Moves a part's saved machine params from a PSET made with an older param
-- layout (lib/layout.lua). The new params take their defaults, and new
-- macros come from the part's patch when it has one, as reset_macros does.
local function migrate_layout(i)
  local part = own[i]
  local defs = part.machine and P.defs[part.machine]
  if not defs then return end
  local values = {}
  for j = 1, #defs do values[j] = params:get(id(i, "s" .. j)) end
  if LAYOUT.migrate(part.machine, values, defs, P.unmap) and patch_name(own[i]) then
    part.macs_from_patch = true
  end
  for j = 1, #defs do params:set(id(i, "s" .. j), values[j], true) end
end

-- After a PSET is read, sends every part again, in order, so each machine is
-- mounted before its params arrive. A silent read doesn't run the machine
-- actions, so the menus are fixed here and the file read again to put the
-- patch and slots in the right options.
local rereading = false

-- The machine a PSET has for part i, by name. A PSET from before the name
-- was saved has an index into OLD_ORDER, less the machines the plugin lacks.
local function saved_machine(filename, i, v)
  if saved_has(filename, id(i, "machine_name")) then
    local name = params:get(id(i, "machine_name"))
    for _, m in ipairs(P.machines) do
      if m.name == name then return name end
    end
    return nil -- "none", or a machine this plugin lacks
  end
  local have = {}
  for _, m in ipairs(P.machines) do have[m.name] = true end
  local k = 1
  for _, name in ipairs(OLD_ORDER) do
    if have[name] then
      k = k + 1
      if k == v then return name end
    end
  end
  return nil
end

-- Puts part i on the bank patch a PSET means, without loading it: by its
-- saved name when this machine's bank has it, else, for a PSET from before
-- the name was saved, by the name its index had in OLD_PATCHES, else the
-- saved index as it is.
local function set_saved_patch(filename, i)
  local machine = own[i].machine
  local names = machine and P.patches[machine] or {}
  local want
  if saved_has(filename, id(i, "patch_name")) then
    want = params:get(id(i, "patch_name"))
  elseif OLD_PATCHES[machine] then
    want = OLD_PATCHES[machine][params:get(id(i, "patch"))]
  end
  for k, name in ipairs(names) do
    if name == want then
      params:set(id(i, "patch"), k, true)
      break
    end
  end
  params:set(id(i, "patch_name"), patch_name(own[i]) or "", true)
end

-- Sets the machine option to the machine's place in the menu, without
-- running its action.
local function set_machine_option(i, machine)
  local index = 1
  for k, m in ipairs(P.machines) do
    if m.name == machine then index = k + 1 end
  end
  params:set(id(i, "machine"), index, true)
  params:set(id(i, "machine_name"), machine or "", true)
end

-- Puts part i's params back to their defaults, without their actions.
local function reset_part(i)
  local at = params.lookup["part_" .. i]
  for k = at + 1, at + params:get(at) do
    local p = params.params[k]
    if p.t == params.tTEXT then
      p:set("", true)
    elseif p.t == params.tCONTROL then
      p:set(p.controlspec.default or p.controlspec.minval, true)
    elseif p.default then
      p:set(p.default, true)
    end
  end
end

function P.after_read(filename)
  if rereading then return end
  P.loading = true
  -- apply_all below parks or sends every part
  timer.cancel(P.parts_timer)
  P.parts_timer = nil
  if filename and not saved_has(filename, "parts") then
    -- a PSET from before 16 parts: 8, and parts 9-16 empty
    params:set("parts", 1, true)
    if not saved_has(filename, id(9, "machine")) then
      for i = 9, P.COUNT do reset_part(i) end
    end
  end
  show_parts(params:get("parts") == 2 and 16 or 8)
  local changed = false
  local saved = {}
  for i, part in ipairs(own) do
    -- the PSET's params replace whatever was owed
    part.owed, part.fresh = false, false
    local machine = saved_machine(filename, i, params:get(id(i, "machine")))
    saved[i] = machine or false
    set_machine_option(i, machine)
    if machine ~= part.machine then
      show_machine(part, machine, false)
      changed = true
    end
    part.channel = params:get(id(i, "channel")) - 1
  end
  P.owed_master = false
  -- the first part saved as MPE, in case a file has more than one; files
  -- with none keep the "mpe part" they saved
  local first
  for i = 1, P.COUNT do
    if params:get(id(i, "mpe")) == 2 then first = first or i end
  end
  if first then
    params:set("mpe_part", first + 2, true)
    for i = 1, P.COUNT do params:set(id(i, "mpe"), i == first and 2 or 1, true) end
  end
  rebuild_menu()
  if changed and filename then
    rereading = true
    local ok, err = pcall(params.read, params, filename, true)
    rereading = false
    if not ok then print("acidulous: pset read failed: " .. tostring(err)) end
    -- the read put the saved index back
    for i = 1, P.COUNT do set_machine_option(i, saved[i] or nil) end
  end
  for i = 1, P.COUNT do set_saved_patch(filename, i) end
  if saved_tables(filename) < P.WAVE_TABLES then
    for i = 1, P.COUNT do migrate_waves(i) end
  end
  params:set("wave_tables", P.WAVE_TABLES, true)
  if saved_count(filename, "trinity_dests", TRINITY_DESTS_BEFORE, P.TRINITY_DESTS) < P.TRINITY_DESTS then
    for i = 1, P.COUNT do migrate_dests(i) end
  end
  params:set("trinity_dests", P.TRINITY_DESTS, true)
  for i = 1, P.COUNT do
    if filename and not saved_has(filename, id(i, "preset")) then params:set(id(i, "preset"), "", true) end
    -- a PSET from before modifiers has none
    if filename and not saved_has(filename, id(i, "mods")) then params:set(id(i, "mods"), "", true) end
  end
  if filename and not saved_has(filename, "macro_params") then
    for i = 1, P.COUNT do reset_macros(i) end
  end
  params:set("macro_params", 1, true)
  if saved_count(filename, "param_layout", 1, LAYOUT.NOW) < LAYOUT.NOW then
    for i = 1, P.COUNT do migrate_layout(i) end
  end
  params:set("param_layout", LAYOUT.NOW, true)
  -- a PSET from before songs has none
  if filename and params.lookup["song"] and not saved_has(filename, "song") then params:set("song", "", true) end
  -- the song module keeps a loaded song's racks; false when the parts
  -- shouldn't be sent
  local apply = true
  if P.on_read then apply = P.on_read() ~= false end
  P.loading = false
  if apply then P.apply_all() end
end

-- Every half second once started: sends everything again if the plugin was
-- started again under the script, and asks for a patch's values once more
-- if they haven't come.
watch = function()
  local now = util.time()
  if now - P.seen > 3 and not P.lost then
    P.lost, P.lost_tries, P.lost_at = true, 0, now
    print("acidulous: the plugin stopped answering")
  end
  -- SuperCollider started again: start the engine, and if sclang lost it,
  -- load it again. Its /hello then sends the parts.
  if P.lost and now - P.lost_at > 5 then
    P.lost_at = now
    P.lost_tries = P.lost_tries + 1
    if P.lost_tries % 2 == 1 then
      P.start_engine()
    else
      engine.load("Acidulous", P.start_engine)
    end
  end
  if next(P.mod_defs) == nil and now - P.mod_defs_asked > 2 then
    P.mod_defs_asked = now
    send("/mod_defs", {})
  end
  local function retry(part)
    if part.want_values and not part.values_retried and now - part.values_at > 2 then
      part.values_retried = true
      part.values_at = now
      part.dim = false
      if part.preset then send_preset(part) else send_patch(part, "/patch", {part.n - 1, part.machine, part.sent}) end
    end
  end
  for i, part in ipairs(own) do
    if own_on(i) and not P.song_holds then retry(part) end
  end
  if P.song_racks then
    for _, part in ipairs(P.song) do retry(part) end
  end
end

-- The plugin has an empty engine: sends every part again.
function P.resend()
  P.lost = false
  print("acidulous: plugin started again, sending the parts")
  -- it has no song either
  if P.on_plugin_reset then P.on_plugin_reset() end
  P.send_master()
  if P.on_resend then P.on_resend() end
  for i, part in ipairs(own) do
    part.fx_wait = 0 -- the old plugin's replies won't come
    if not own_on(i) then
      park(part) -- the new plugin has nothing there
    elseif part.pending then
      send_strip(part) -- the pending patch mounts the machine
      part.resend_files = next(part.files) ~= nil
      send_mods(part)
    elseif part.want_values or part.fresh then
      -- the slots don't hold this patch's values yet, so ask for them again
      part.fresh, part.owed = false, false
      load_fresh(part)
    else
      apply(part)
    end
  end
  P.send_mpe()
end

-- Puts a machine and patch on a part without the debounce, for start-up.
function P.set_part(i, machine, patch)
  local index = 1
  for k, m in ipairs(P.machines) do
    if m.name == machine then index = k + 1 end
  end
  local part = own[i]
  if part.machine ~= machine then
    part.files, part.loading, part.problem, part.last_path = {}, nil, nil, nil
    part.want_default = P.SAMPLE_MACHINES[machine] or false
    save_files(part)
  end
  clear_fx(part)
  forget_preset(part)
  P.loading = true
  params:set(id(i, "machine"), index)
  show_machine(part, part.machine, true)
  local names = P.patches[machine] or {}
  local which = 1
  for k, name in ipairs(names) do
    if name == patch then which = k end
  end
  params:set(id(i, "patch"), which)
  P.loading = false
  part.patch = patch_name(part)
end

-- Loads the part's current patch again with its own values: forgets what the
-- run remembered for it and drops the edits.
function P.reset_patch(i)
  local part = P.part(i)
  if not part.machine then return end
  notes_off(part)
  mems(part)[mem_key(part) or ""] = nil
  part.edits = {}
  clear_fx(part)
  recall(part)
  if held(part, true) then return end
  later(part, 0.2, function() load_machine(part, true) end)
end

-- Puts saved preset s (from P.saved) on the part, its files too.
function P.load_preset(i, s)
  local part = P.part(i)
  if not part.machine then return end
  notes_off(part)
  timer.cancel(part.pending)
  part.pending = nil
  part.preset = {path = s.path, index = s.index, name = s.name, want = s.name}
  save_preset_param(part)
  part.patch = s.name
  part.edits = {}
  clear_fx(part)
  recall(part)
  if held(part, true) then return end
  load_machine(part, true)
end

-- A file name for a preset called name, as the app makes them.
function P.preset_file(name)
  local safe = name:gsub("^%s+", ""):gsub("%s+$", ""):gsub("[^%w _-]", "_")
  return (safe ~= "" and safe or "patch") .. ".acpreset"
end

-- Saves the part's sound as the preset at path, called name. /preset_saved
-- answers, and the part then plays it.
function P.save_preset(i, path, name)
  local part = P.part(i)
  if not part.machine then return end
  -- a song's track has what the song set, its samples and all, which the
  -- script can't name; the plugin refuses too
  if part.song or P.song_holds then
    if P.on_preset_note then P.on_preset_note("not in a song") end
    return
  end
  local args = {i - 1, path, name, #P.ROOTS}
  for _, r in ipairs(P.ROOTS) do args[#args + 1] = r end
  local function set(k, v)
    args[#args + 1] = k
    args[#args + 1] = tostring(v)
  end
  local keys = {}
  for k in pairs(part.files) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    local f = part.files[k]
    if f.kind == "pad" then
      set(string.format("p%02d_sample", f.arg), f.path)
    elseif f.kind == "sample" then
      set("sample", f.path)
    elseif f.kind == "sf2" then
      set("sf2", f.path)
      set("sf2preset", f.arg)
    end
    -- a folder's or preset's zones are the plugin's to add
  end
  part.saving = {path = path, name = name}
  psend(part, "/preset_save", args)
end

-- Takes saved preset s out of its file; /preset_deleted answers.
function P.delete_preset(s)
  send("/preset_delete", {s.path, s.index})
end

-- Sends a part's volume and machine with its patch, which fills the slots
-- with the patch's values when it's applied.
function P.start_part(i)
  send_strip(own[i])
  load_machine(own[i], true)
end

-- For K3.
function P.audition(i, z)
  local part = P.part(i)
  if not part.machine then return end
  local m = P.machine_info(part.machine)
  local note = m and m.note or 48
  -- with an MPE zone active, a non-MPE part keeps to channel 1
  local ch = P.mpe and 1 or math.max(part.channel, 1)
  if z == 1 then part.aud_ch = ch else ch = part.aud_ch or ch end
  send("/note", {i - 1, ch, note, z == 1 and 100 or 0})
  P.note(i, z == 1)
end

-- Activity for the screen.
function P.note(i, on)
  local part = P.part(i)
  if on then
    part.held = part.held + 1
    part.last_note = util.time()
  else
    part.held = math.max(0, part.held - 1)
  end
end

-- The worst block's load, in %, over which the screen says overload. With
-- more than 8 machines playing it comes at 70: the more parts, the more
-- their busy blocks pile up at once, so there's less room above.
function P.overload_at()
  local n = 0
  for i = 1, P.active do
    if P.part(i).machine then n = n + 1 end
  end

  return n > 8 and 70 or 80
end

-- ---- values the pages read and set -------------------------------------------

-- Part i's value by its param name ("volume", "pan", "patch", "s3", ...):
-- the shown part's, a param for the script's own, vals for a song track.
function P.get(i, key) return get(P.part(i), key) end

-- Sets it as turning its param would, running what the param's action
-- runs. A song track's goes to its rack and never touches the params.
function P.set(i, key, v)
  local part = P.part(i)
  if not part.song then
    params:set(id(i, key), v)
    return
  end
  -- the song is being loaded again, and the read-back replaces the tracks
  if P.song_reading then return end
  local j = tonumber(key:match("^s(%d+)$") or "")
  if key == "machine" then
    v = util.clamp(v, 1, #P.machines + 1)
    part.vals.machine = v
    machine_changed(part, v > 1 and P.machines[v - 1].name or nil)
  elseif key == "patch" then
    if not part.machine then return end
    part.vals.patch = util.clamp(v, 1, math.max(1, #(P.patches[part.machine] or {})))
    patch_changed(part)
  elseif key == "volume" then
    put(part, key, util.clamp(v, -60, 3))
    send_volume(part)
  elseif STRIP[key] then
    put(part, key, v)
    send_strip_param(part, key)
  elseif j then
    put(part, key, util.clamp(v, 0, 1))
    slot_changed(part, j, get(part, key))
  elseif key == "channel" then
    notes_off(part)
    put(part, key, v)
    part.channel = v - 1
  else
    params:set(id(i, key), v)
  end
end

-- Steps it as an encoder steps its param.
function P.delta(i, key, d)
  local part = P.part(i)
  if not part.song then
    params:delta(id(i, key), d)
    return
  end
  local v = get(part, key)
  local j = tonumber(key:match("^s(%d+)$") or "")
  local p = params:lookup_param(id(i, key))
  if j then
    local def = part.machine and (P.defs[part.machine] or {})[j]
    if not def then return end
    local q = (def.curve == "step" and def.steps > 1) and 1 / (def.steps - 1) or 0.005
    P.set(i, key, util.clamp(v + d * q, 0, 1))
  elseif key == "patch" and v < 1 then
    -- a song's own patch, not in the bank: the first turn goes to the
    -- bank's first patch, or its last turning left
    P.set(i, key, d > 0 and 1 or #(P.patches[part.machine or ""] or {}))
  elseif key == "machine" or key == "patch" or key == "mute" or key == "solo" or key == "channel" then
    P.set(i, key, math.max(1, v + d))
  elseif p and p.controlspec then
    -- the same controlspec as the script's param, as norns steps it
    local cs = p.controlspec
    P.set(i, key, cs:map(util.clamp(cs:unmap(v) + d * cs.quantum, 0, 1)))
  end
end

-- ---- a song's tracks -------------------------------------------------------------

-- The song's tracks as P.song: lib/song.lua calls these as a load starts,
-- lands or fails and when the song goes. The plugin's read-back replies
-- (/mounted, /values, /strip, /sends, /inserts, /mods ...) fill them.
P.song_wide = false -- the song has tracks past 8

-- Stops what the tracks still have on its way to their racks.
local function quiet_song_parts()
  for _, part in ipairs(P.song) do
    timer.cancel(part.pending)
    part.pending = nil
    for slot, t in pairs(part.mod_pending or {}) do
      timer.cancel(t)
      part.mod_pending[slot] = nil
    end
  end
end

local function clear_song_parts()
  quiet_song_parts()
  for i = 1, P.COUNT do P.song[i] = new_part(i, true) end
  song_memory = {}
end
clear_song_parts()

-- The shown song's tracks while another song loads, put back if that load
-- is refused with the racks untouched: {parts, memory, wide}.
local stashed = nil

-- A track's machine and patch as the read-back has them; nil machine for
-- an empty rack.
song_track = function(part, machine, patch)
  timer.cancel(part.pending)
  part.pending = nil
  if machine ~= part.machine then part.macro_names, part.macros = nil, nil end
  part.machine = machine
  part.vals.machine = machine_index(machine)
  part.want_values, part.dim, part.edits = false, false, {}
  part.preset, part.files, part.loading, part.problem, part.last_path = nil, {}, nil, nil, nil
  part.fx_edits, part.sends_moved = {}, {}
  -- the read-back's /inserts is the one to show
  part.fx_wait = 0
  -- a sample machine has the song's samples, which the script can't name
  part.song_files = machine ~= nil and P.SAMPLE_MACHINES[machine] or false
  if patch ~= nil or not machine then
    part.patch = patch ~= "" and patch or nil
    part.sent = patch or nil
    part.vals.patch = patch_index(machine, part.patch)
  end
  if not machine then
    for k in pairs(part.vals) do
      if k:match("^s%d+$") then part.vals[k] = nil end
    end
  end
end

-- A load is on its way. Another song's tracks go from the pages until its
-- read-back comes, and the shown song loaded again keeps its tracks until
-- the replies replace them. Either way edits to the tracks aren't sent
-- meanwhile and what the run remembered of their patches goes.
function P.song_load_started(same)
  stashed = nil
  if P.song_view and not same then
    quiet_song_parts()
    stashed = {parts = {}, memory = song_memory, wide = P.song_wide}
    for i = 1, P.COUNT do stashed.parts[i] = P.song[i] end
    clear_song_parts()
    P.song_wide = false
    P.song_view = false
    show_view()
  elseif not P.song_view then
    clear_song_parts()
  else
    quiet_song_parts()
    song_memory = {}
  end
  P.song_reading = true
end

-- A load ended without a song: nothing more to read. still_loaded: the song
-- shown before it is still on the racks, so its tracks come back.
function P.song_load_failed(still_loaded)
  P.song_reading = false
  if stashed and still_loaded and not P.song_view then
    for i = 1, P.COUNT do P.song[i] = stashed.parts[i] end
    song_memory = stashed.memory
    P.song_wide = stashed.wide
    P.song_view = true
    show_view()
  end
  stashed = nil
end

-- The song loaded: tracks is {part, machine, patch, name} per rack with a
-- machine. The rest are empty. Each track plays from MIDI channel n.
function P.song_loaded(tracks)
  P.song_reading = false
  stashed = nil
  local named, wide = {}, false
  for _, t in ipairs(tracks) do
    local part = P.song[t.part + 1]
    if part then
      named[part.n] = true
      if part.machine ~= t.machine then song_track(part, t.machine, t.patch) end
      part.name = t.name ~= "" and t.name or nil
      if part.n > 8 then wide = true end
    end
  end
  for i, part in ipairs(P.song) do
    if not named[i] then
      if part.machine then song_track(part, nil, nil) end
      part.name = nil
    end
    part.channel = get(part, "channel") - 1
  end
  P.song_wide = wide
  P.song_view = true
  show_view()
end

-- The song went: the script's own parts are shown again. reading: another
-- load is on its way.
function P.song_gone(reading)
  stashed = nil
  P.song_reading = reading or false
  P.song_wide = false
  P.song_view = false
  clear_song_parts()
  show_view()
end

-- A song track's name, or nil.
function P.track_name(i)
  local part = P.part(i)
  return part.song and part.name or nil
end

-- ---- replies from the plugin --------------------------------------------------

function P.osc(path, args)
  if path:sub(1, 6) == "/song_" then
    if P.on_song then P.on_song(path, args) end
  elseif path == "/levels" then
    for i = 1, P.COUNT do
      local part = rpart(i)
      part.level = math.max(args[i] or 0, part.level * 0.7)
      if P.song_racks then own[i].level = 0 end
    end
  elseif path == "/meters" then
    P.master_level = math.max(args[1] or 0, (P.master_level or 0) * 0.7)
    P.master_peak = args[1] or 0
    P.lufs = args[2]
  elseif path == "/graph" or path == "/scope" then
    if P.on_scope then P.on_scope(path, args) end
  elseif path == "/load" then
    P.seen = util.time()
    if P.lost and P.ready then P.resend() end
    P.load = args[1] or 0
    if (args[2] or 0) > P.overload_at() then
      P.worst, P.worst_at = args[2], util.time()
    end
  elseif path == "/values" then
    local i = (args[1] or -1) + 1
    local part = P.song_racks and P.song[i] or own[i]
    if not part then return end
    if part.song and P.song_reading then
      -- a song's read-back: what the track has
      song_track(part, args[2], args[3])
      for j = 4, #args do part.vals["s" .. (j - 3)] = args[j] end
      return
    end
    if part.pending then return end
    if part.machine ~= args[2] or part.sent ~= args[3] then return end
    if part.macs_from_patch then
      -- an old PSET: only the macro slots take the patch's values
      part.macs_from_patch = false
      for j, d in ipairs(P.defs[part.machine] or {}) do
        if d.name:match("^mac%d") and args[j + 3] then put(part, "s" .. j, args[j + 3]) end
      end
    end
    if not part.want_values then return end
    part.want_values = false
    for j = 4, #args do
      if part.song or params.lookup[id(i, "s" .. (j - 3))] then put(part, "s" .. (j - 3), args[j]) end
    end
    for j, v in pairs(part.edits) do
      put(part, "s" .. j, v)
      psend(part, "/param", {i - 1, j - 1, v})
    end
    part.edits = {}
    part.dim = false
    remember(part)
  elseif path == "/mounted" then
    -- only a song's racks: the script knows what it mounted
    local part = P.song_racks and P.song[(args[1] or -1) + 1]
    if not part then return end
    if args[2] == "" then
      song_track(part, nil, nil)
    elseif P.song_reading then
      song_track(part, args[2], nil)
    end
  elseif path == "/strip" then
    -- a song track's strip, read back after it loads
    local part = P.song[(args[1] or -1) + 1]
    if not part then return end
    local gain = args[2] or 1
    part.vals.volume = gain > 0 and util.clamp(20 * math.log(gain, 10), -60, 3) or -60
    part.vals.pan = util.clamp((args[3] or 0.5) * 2 - 1, -1, 1)
    part.vals.mute = (args[4] or 0) == 1 and 2 or 1
    part.vals.solo = (args[5] or 0) == 1 and 2 or 1
  elseif path == "/mods" then
    -- a song track's modifiers, read back after it loads
    local part = P.song_racks and P.song[(args[1] or -1) + 1]
    if not part then return end
    local mods, k = {}, 2
    while k + 3 <= #args do
      local slot, type, bypass, count = args[k], args[k + 1], args[k + 2], args[k + 3]
      k = k + 4
      local values = {}
      for _ = 1, count do
        if args[k] and args[k + 1] then values[args[k]] = args[k + 1] end
        k = k + 2
      end
      if type ~= "" and tab.contains(P.MOD_TYPES, type) then
        mods[slot] = {type = type, bypass = bypass == 1, values = values}
      end
    end
    part.mods = mods
  elseif path == "/sends" then
    -- a patch set the part's sends, or gave the player's back
    local i = (args[1] or -1) + 1
    local part = rpart(i)
    -- a reply to an earlier script's resend can come before the params exist
    if not part or not (part.song or params.lookup[id(i, "rev")]) then return end
    if not part.sends_moved.rev then put(part, "rev", args[2]) end
    if not part.sends_moved.dly then put(part, "dly", args[3]) end
  elseif path == "/inserts" then
    -- the part's inserts after a patch or unmount, with the edits on top
    local i = (args[1] or -1) + 1
    local part = rpart(i)
    if not part then return end
    -- an earlier /patch's, with a later one on its way
    part.fx_wait = math.max(part.fx_wait - 1, 0)
    if part.fx_wait > 0 then return end
    local fx, k = {}, 2
    while k + 2 <= #args do
      local slot, type, count = args[k], args[k + 1], args[k + 2]
      k = k + 3
      -- count params, then the slot's mod params ("~mod") the same way
      local function defs(n)
        local list = {}
        for _ = 1, n do
          if k + 7 > #args then break end
          local d = {
            name = args[k], min = args[k + 1], max = args[k + 2], default = args[k + 3],
            unit = args[k + 4], steps = args[k + 5], curve = args[k + 6], value = args[k + 7],
          }
          local edit = part.fx_edits[slot .. ":" .. d.name]
          if edit then d.value = edit end
          list[#list + 1] = d
          k = k + 8
        end
        return list
      end
      local list = defs(count)
      local mods = {}
      if k <= #args then
        local mcount = args[k]
        k = k + 1
        mods = defs(mcount)
      end
      fx[slot] = {type = type, params = list, mods = mods}
    end
    part.fx = fx
  elseif path == "/macro_names" then
    local part = rpart((args[1] or -1) + 1)
    if not part then return end
    local names, any = {}, false
    for n = 1, 8 do
      names[n] = args[n + 1] or ""
      if names[n] ~= "" then any = true end
    end
    part.macro_names = any and names or nil
  elseif path == "/macros" then
    local list = {}
    for j = 4, #args - 1, 2 do list[#list + 1] = {param = args[j], label = args[j + 1]} end
    if P.song_racks then
      -- the song's track's own, which may not be the bank patch's
      local part = P.song[(args[1] or -1) + 1]
      if part then part.macros = list end
      return
    end
    P.patch_macros[args[2]] = P.patch_macros[args[2]] or {}
    P.patch_macros[args[2]][args[3]] = list

  elseif path == "/preset_scan" then
    local folder = args[1] or ""
    local machine = folder:match("([^/]+)/*$")
    if not machine then return end
    local dir = folder:match("/$") and folder or folder .. "/"
    local list = {}
    for k = 2, #args - 5, 6 do
      if args[k + 3] == machine then
        list[#list + 1] = {file = args[k], path = dir .. args[k], index = args[k + 1], name = args[k + 2],
          count = args[k + 4], packed = args[k + 5] == 1}
      end
    end
    P.saved[machine] = list
    if P.on_saved then P.on_saved(machine) end
  elseif path == "/preset_saved" then
    local i = (args[1] or -1) + 1
    local part = rpart(i)
    if not part then return end
    local saving = part.saving
    part.saving = nil
    local ok = args[3] == "ok"
    if not ok then print("acidulous: saving " .. tostring(args[2]) .. ": " .. tostring(args[3])) end
    if ok and saving and saving.path == args[2] and part.machine then
      -- the part plays what it saved, with the values it has
      local values = {}
      for j = 1, #(P.defs[part.machine] or {}) do values[j] = get(part, "s" .. j) end
      part.preset = {path = saving.path, index = 0, name = saving.name, want = saving.name}
      save_preset_param(part)
      part.patch = saving.name
      if not part.pending and not part.want_values then part.sent = preset_tag(part.preset) end
      mems(part)[mem_key(part)] = values
    end
    if P.on_preset_note then P.on_preset_note(ok and "saved" or args[3]) end
    P.scan_presets(part.machine)
  elseif path == "/preset_deleted" then
    local ok = args[3] == "ok"
    if not ok then print("acidulous: deleting " .. tostring(args[1]) .. ": " .. tostring(args[3])) end
    local file, gone = args[1], args[2]
    if ok then
      -- the run's memory of the file's patches, which may have moved
      local tail = "|" .. file
      for _, m in ipairs({memory, song_memory}) do
        for k in pairs(m) do
          if k:sub(-#tail) == tail then m[k] = nil end
        end
      end
    end
    local all = {}
    for i = 1, P.COUNT do all[#all + 1] = own[i]; all[#all + 1] = P.song[i] end
    for _, part in ipairs(all) do
      -- a part playing it keeps its sound, but no longer as that preset
      if ok and part.preset and part.preset.path == file then
        if gone < 0 or part.preset.index == gone then
          forget_preset(part)
        elseif part.preset.index > gone then
          part.preset.index = part.preset.index - 1
          save_preset_param(part)
          if not part.pending and not part.want_values then part.sent = preset_tag(part.preset) end
        end
      end
      -- Mosaic zones loaded from it
      local f = part.files.main
      if ok and f and f.kind == "zones" and f.path == file and gone >= 0 and f.arg > gone then
        f.arg = f.arg - 1
        save_files(part)
      end
    end
    if P.on_preset_note then P.on_preset_note(ok and "deleted" or args[3]) end
    local machine = args[1]:match("([^/]+)/[^/]+$")
    P.scan_presets(machine)
  elseif path == "/preset_loaded" then
    local i = (args[1] or -1) + 1
    local part = rpart(i)
    if not part then return end
    local tag, name, err, index = args[2], args[3], args[4] or "", args[5]
    local mine = part.preset and preset_tag(part.preset) == tag
    if err ~= "" then
      print("acidulous: " .. tag .. ": " .. err)
      part.problem = {text = err, at = util.time()}
      if mine then
        -- the part has its machine at its defaults, not the preset
        if P.on_preset_note then P.on_preset_note(err) end
        forget_preset(part)
        part.patch = nil
      end
    elseif mine and name ~= "" then
      part.preset.name, part.preset.want = name, name
      part.patch = name
      -- found by name at another index, after a delete
      if index and index ~= part.preset.index then
        local m = mems(part)
        local old = mem_key(part)
        part.preset.index = index
        local new = mem_key(part)
        m[new], m[old] = m[old], nil
        if part.sent == tag then part.sent = preset_tag(part.preset) end
      end
      save_preset_param(part)
    end
  elseif path == "/hello" then
    if P.ready then P.resend() end
  elseif path == "/machines" then
    if asked_machines then return end
    asked_machines = true
    P.mod_defs_asked = util.time()
    send("/mod_defs", {})
    local have = {}
    for _, name in ipairs(args) do have[name] = true end
    for _, m in ipairs(P.MACHINES) do
      if have[m.name] then
        P.machines[#P.machines + 1] = m
        queue[#queue + 1] = {kind = "defs", machine = m.name}
        queue[#queue + 1] = {kind = "patches", machine = m.name}
      end
    end
    if #P.machines == 0 then
      P.error = "no machines in the plugin"
      print("acidulous: " .. P.error)
      return
    end
    ask_next()
  elseif path == "/mod_defs" then
    local list, by, k = {}, {}, 3
    for _ = 1, args[2] or 0 do
      if k + 6 > #args then break end
      local d = {
        name = args[k], min = args[k + 1], max = args[k + 2], default = args[k + 3],
        unit = args[k + 4], steps = args[k + 5], curve = args[k + 6],
      }
      list[#list + 1], by[d.name] = d, d
      k = k + 7
    end
    list.by = by
    P.mod_defs[args[1]] = list
  elseif path == "/def" then
    local d = P.defs[args[1]]
    if d then
      d[args[2] + 1] = {
        name = args[3], min = args[4], max = args[5], default = args[6],
        unit = args[7], steps = args[8], curve = args[9],
      }
    end
  elseif path == "/defs_end" then
    if waiting and waiting.kind == "defs" and waiting.machine == args[1] then
      if #P.defs[args[1]] == args[2] then ask_next()
      else -- lost some: ask again
        waiting.asked = 0
      end
    end
  elseif path == "/patches" then
    local machine = table.remove(args, 1)
    P.patches[machine] = args
    if waiting and waiting.kind == "patches" and waiting.machine == machine then ask_next() end
  elseif path == "/loaded" then
    local part = rpart((args[1] or -1) + 1)
    if not part then return end
    local kind, fpath, arg, name = args[2], args[3], args[4], args[5]
    -- loaded into the machine the part had before a change, and gone with it
    if args[6] ~= part.machine then return end
    if part.loading and part.loading.kind == kind then part.loading = nil end
    if kind == "pad" then
      part.files["pad" .. arg] = fpath ~= "" and {kind = kind, path = fpath, arg = arg, name = name} or nil
    elseif kind == "sf2" or kind == "folder" or kind == "sample" or kind == "zones" then
      part.files.main = fpath ~= "" and {kind = kind, path = fpath, arg = arg, name = name} or nil
    end
    if fpath ~= "" and kind ~= "pad" then part.last_path = fpath
    elseif kind == "pad" and fpath ~= "" and not part.loading then part.last_path = fpath end
    save_files(part)
  elseif path == "/shape" then
    local file, seconds, rate, cut = args[1], args[2], args[3], args[4]
    local cols = {}
    for k = 5, #args do cols[#cols + 1] = args[k] end
    if P.on_shape then P.on_shape(file, seconds, rate, cut, cols) end
  elseif path == "/hear" then
    if P.on_hear then P.on_hear(args[1], args[2]) end
  elseif path == "/presets" then
    local file = table.remove(args, 1)
    if P.on_presets then P.on_presets(file, args) end
  elseif path == "/error" then
    print("acidulous: " .. tostring(args[1]))
    local part = args[2] and rpart(args[2] + 1)

    -- the plugin sends "" for no machine
    local machine = args[6] ~= "" and args[6] or nil
    if part and machine == part.machine then
      if part.loading and (part.loading.kind == args[3] or args[3] ~= "pad") then part.loading = nil end
      part.problem = {text = args[1], at = util.time()}
    end
  end
end

return P
