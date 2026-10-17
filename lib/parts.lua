-- The 8 parts: their params, and what they send to the plugin.
-- A part n is the plugin's part n - 1 (an engine track).

local timer = include("lib/timer")

local P = {}

P.COUNT = 8
local PLUGIN = {"127.0.0.1", 57150}

-- The machines offered, in menu order: the ones that run in real time on a
-- Pi 4 (docs/machines.md), with the ones that play files last. short is for
-- the main page, note for K3's audition.
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
P.list = {}
for i = 1, P.COUNT do
  P.list[i] = {
    n = i, channel = i, machine = nil, patch = nil,
    level = 0, held = 0, last_note = 0,
    want_values = false, pending = nil,
    sent = nil,     -- the patch last sent to the plugin
    edits = {},     -- slot -> value, set while a patch is on its way
    sends_moved = {}, -- "rev"/"dly" -> true when moved since the last /patch
    values_at = 0, values_retried = false,
    -- what /load has put in the machine: "pad0".."pad12" or "main" ->
    -- {kind, path, arg, name}. Saved in the part's "files" param.
    files = {},
    loading = nil,  -- {kind, name} while a load is on its way
    problem = nil,  -- {text, at}: the last load that failed
    last_path = nil, -- the last file or folder loaded, for the picker
    resend_files = false, -- send the files after the next mount
    -- the inserts the plugin last listed, slot 0 or 1 -> {type, params},
    -- each param a def with its 0-1 value; from /inserts
    fx = {},
    -- insert params moved by hand, "slot:name" -> 0-1. Saved in the part's
    -- "fx" param and sent again over the patch's after a PSET is read.
    fx_edits = {},
    -- /patch and /unmount sent and not yet answered by /inserts, and when the
    -- last went, so only the newest one's /inserts is shown
    fx_wait = 0, fx_asked = 0,
    -- a Trinity patch's macro names, 8 strings ("" for unnamed), or nil when
    -- it names none; from /macro_names, not saved
    macro_names = nil,
  }
end

local function send(path, args) osc.send(PLUGIN, path, args) end
P.send = send

-- Sends /patch or /unmount, which the plugin answers with one /inserts each.
-- A count left over for more than 2 s is a lost reply, so it starts again.
local function send_patch(i, path, args)
  local part, now = P.list[i], util.time()
  part.fx_wait = (now - part.fx_asked > 2) and 1 or part.fx_wait + 1
  part.fx_asked = now
  send(path, args)
end

local function id(i, what) return "p" .. i .. "_" .. what end
P.id = id

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

function P.send_volume(i)
  -- the strip's gain is 0-1.5 linear
  send("/channel", {i - 1, "gain", math.min(1, db_to_gain(params:get(id(i, "volume"))) / 1.5)})
end

-- The part's channel strip: pan, mute, solo and sends. A send above 0
-- mounts its effect in the plugin.
local STRIP = {pan = "pan", mute = "mute", solo = "solo", rev = "sendreverb", dly = "senddelay"}

local function strip_value(i, what)
  local v = params:get(id(i, what))
  if what == "pan" then return (v + 1) / 2 end
  if what == "mute" or what == "solo" then return v - 1 end
  return v
end

function P.send_strip_param(i, what)
  -- a send moved after a /patch keeps its level when the patch's /sends comes back
  if what == "rev" or what == "dly" then P.list[i].sends_moved[what] = true end
  send("/channel", {i - 1, STRIP[what], strip_value(i, what)})
end

function P.send_strip(i)
  P.send_volume(i)
  for _, what in ipairs({"pan", "mute", "solo", "rev", "dly"}) do P.send_strip_param(i, what) end
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

function P.send_master()
  -- the master fader is 0-1.5 linear; 0 dB here is its top, as the plugin starts
  send("/master", {"volume", db_to_gain(params:get("master_level"))})
  for _, sp in ipairs(P.SEND_PARAMS) do send_send_param(sp) end
end

local function percent(param) return string.format("%d%%", math.floor(param:get() * 100 + 0.5)) end

function P.add_master_params()
  params:add_group("mixer", "sends and master", 1 + #P.SEND_PARAMS)
  params:add_control("master_level", "master level", controlspec.new(-60, 0, "lin", 0.5, 0, "dB"))
  params:set_action("master_level", function()
    if not P.loading then send("/master", {"volume", db_to_gain(params:get("master_level"))}) end
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
      if not P.loading then send_send_param(sp) end
    end)
  end
  -- the wavetable count a PSET was saved with, for P.after_read; never shown
  params:add_number("wave_tables", "wave tables", 0, 1024, P.WAVE_TABLES)
  params:hide("wave_tables")
  -- 1 in a PSET saved with Trinity's macro params, for P.after_read; never shown
  params:add_number("macro_params", "macro params", 0, 1, 1)
  params:hide("macro_params")
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

function P.notes_off(i)
  local part = P.list[i]
  -- with an MPE zone active every part plays on channel 1
  send("/cc", {i - 1, P.mpe and 1 or math.max(part.channel, 1), 123, 0})
  part.held = 0
  if P.on_notes_off then P.on_notes_off(i) end
end

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
local function show_machine(i, machine, defaults)
  if P.list[i].machine ~= machine then P.list[i].macro_names = nil end
  P.list[i].machine = machine
  set_patch_options(i, machine)
  set_slots(i, machine, defaults)
  rebuild_menu()
end

local function patch_name(i)
  local part = P.list[i]
  local names = part.machine and P.patches[part.machine] or {}
  return names[params:get(id(i, "patch"))]
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

local function save_files(i)
  params:set(id(i, "files"), encode(P.list[i].files), true)
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

local function save_fx(i)
  params:set(id(i, "fx"), encode_fx(P.list[i].fx_edits), true)
end

-- Drops the part's insert edits, for a newly picked patch.
local function clear_fx(i)
  P.list[i].fx_edits = {}
  save_fx(i)
end

-- An insert control's 0-1 value (ctl from panel.fx_cards).
function P.fx_value(i, ctl)
  local e = P.list[i].fx[ctl.fx]
  local d = e and e.params[ctl.index]
  return d and d.value or 0
end

function P.set_fx(i, ctl, v)
  local part = P.list[i]
  local e = part.fx[ctl.fx]
  local d = e and e.params[ctl.index]
  -- a patch on its way brings its own inserts
  if not d or part.pending then return end
  d.value = v
  part.fx_edits[ctl.fx .. ":" .. d.name] = v
  save_fx(i)
  send("/fx", {i - 1, ctl.fx, d.name, v})
end

-- Sends the part's insert edits, after its /patch. The plugin runs them after
-- the patch, so they go on top of its inserts.
local function send_fx_edits(i)
  for k, v in pairs(P.list[i].fx_edits) do
    local slot, name = k:match("^(%d):(.+)$")
    send("/fx", {i - 1, tonumber(slot), name, v})
  end
end

local function base(path) return path:match("([^/]+)/*$") or path end
local function stem(path) return (base(path):gsub("%.[^%.]*$", "")) end

-- The machines that load files.
P.SAMPLE_MACHINES = {Forage = true, Mosaic = true, Dice = true, Pollen = true, Molt = true}

-- Sends /load. kind and arg as host/src/bridge.h has them. A path with | in
-- it is refused, as | separates the files in the saved list.
function P.load_file(i, kind, path, arg)
  local part = P.list[i]
  if path:find("|", 1, true) then
    part.problem = {text = base(path) .. ": names with | can't be loaded", at = util.time()}
    return
  end
  part.loading = {kind = kind, name = kind == "sf2" and base(path) or stem(path)}
  part.problem = nil
  send("/load", {i - 1, kind, path, arg or 0})
end

-- Sends the part's saved files again, after its machine is mounted.
local function send_files(i)
  local part = P.list[i]
  part.resend_files = false
  if not part.machine then return end
  local list = {}
  for k, f in pairs(part.files) do list[#list + 1] = f end
  table.sort(list, function(a, b) return a.arg < b.arg end)
  send("/load", {i - 1, "clear", "", 0})
  for _, f in ipairs(list) do P.load_file(i, f.kind, f.path, f.arg) end
end

-- Forage's pads, 1-13: the name of what each has, or nil.
function P.pad_names(i)
  local names = {}
  for pad = 0, 12 do
    local f = P.list[i].files["pad" .. pad]
    if f then names[pad + 1] = f.name or stem(f.path) end
  end
  return names
end

-- The last file or folder loaded, or one the part has.
function P.loaded_path(i)
  local part = P.list[i]
  if part.last_path then return part.last_path end
  for _, f in pairs(part.files) do return f.path end
end

-- What the part's machine has loaded, for the screen: "loading ...", the
-- last load that failed for 5 seconds, or the files' names. nil for a
-- machine that takes no files.
function P.material(i)
  local part = P.list[i]
  if part.loading then return "loading " .. part.loading.name end
  if part.problem and util.time() - part.problem.at < 5 then return part.problem.text end
  local m = part.machine
  if m == "Forage" then
    local n, first = 0, nil
    for pad = 0, 12 do
      local f = part.files["pad" .. pad]
      if f then
        n = n + 1
        first = first or f
      end
    end
    if n == 0 then return "no samples" end
    return base(first.path:match("^(.*)/[^/]+$") or "") .. ", " .. n .. (n == 1 and " pad" or " pads")
  elseif m == "Mosaic" or m == "Dice" or m == "Pollen" or m == "Molt" then
    local f = part.files.main
    if f then return f.name or stem(f.path) end
    return m == "Pollen" and "live input" or "nothing loaded"
  end
end

-- Mounts the part's machine with its patch, or empties it.
local function load_machine(i, with_values)
  local part = P.list[i]
  part.pending = nil
  local name = patch_name(i)
  part.want_values = false
  if not with_values then part.edits = {} end
  if not part.machine then
    part.fx = {}
    send_patch(i, "/unmount", {i - 1})
    name = nil
  elseif name then
    part.want_values = with_values
    part.values_at, part.values_retried = util.time(), false
    part.sends_moved = {}
    part.fx = {} -- until /inserts lists the patch's
    send_patch(i, "/patch", {i - 1, part.machine, name})
    send_fx_edits(i)
  else
    send("/mount", {i - 1, part.machine})
  end
  part.patch = name
  part.sent = name
  if part.resend_files then send_files(i) end
end

-- Runs fn after `wait` seconds unless called again first, so turning an
-- encoder through patches only loads the one it stops on.
local function later(i, wait, fn)
  local part = P.list[i]
  timer.cancel(part.pending)
  part.pending = timer.after(wait, function()
    part.pending = nil
    fn()
  end)
end

-- Sends a part's whole state, after a PSET is read. The patch goes first,
-- which mounts the machine, then the saved params and sends on top of it,
-- then its files.
function P.apply(i)
  local part = P.list[i]
  timer.cancel(part.pending)
  part.pending = nil
  P.notes_off(i)
  P.send_strip(i)
  part.fx_edits = decode_fx(params:get(id(i, "fx")))
  load_machine(i, false)
  -- the PSET's sends go on top of the ones the patch carries
  P.send_strip_param(i, "rev")
  P.send_strip_param(i, "dly")
  -- C++ waits for room in the engine's param queue, so none of these is lost
  if part.machine then
    for j, d in ipairs(P.defs[part.machine]) do
      if not (part.macs_from_patch and d.name:match("^mac%d")) then
        send("/param", {i - 1, j - 1, params:get(id(i, "s" .. j))})
      end
    end
  end
  part.files = decode(params:get(id(i, "files")))
  part.loading, part.last_path = nil, nil
  -- with no files this still clears what the machine had before
  if P.SAMPLE_MACHINES[part.machine] then send_files(i) end
end

function P.apply_all()
  P.send_master()
  for i = 1, P.COUNT do P.apply(i) end
  P.send_mpe()
end

local function add_part(i)
  local part = P.list[i]
  params:add_group("part_" .. i, "part " .. i, 13 + P.slots)

  local chans = {"off"}
  for c = 1, 16 do chans[c + 1] = tostring(c) end
  params:add_option(id(i, "channel"), "channel", chans, i + 1)
  params:set_action(id(i, "channel"), function(v)
    if not P.loading then P.notes_off(i) end
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
    if P.loading then
      show_machine(i, machine, false)
      return
    end
    if machine == part.machine and not part.pending then return end -- a bang
    P.notes_off(i)
    part.edits = {}
    clear_fx(i)
    -- the new machine starts empty
    part.files, part.loading, part.problem, part.last_path = {}, nil, nil, nil
    save_files(i)
    params:set(id(i, "patch"), 1, true)
    show_machine(i, machine, true)
    part.patch = patch_name(i)
    later(i, 0.3, function() load_machine(i, true) end)
  end)

  params:add_option(id(i, "patch"), "patch", {"-"}, 1)
  params:set_action(id(i, "patch"), function()
    if P.loading or not part.machine then return end
    local name = patch_name(i)
    if name == part.sent and not part.pending then return end -- a bang
    part.patch = name
    part.edits = {}
    clear_fx(i)
    later(i, 0.2, function() load_machine(i, true) end)
  end)

  -- the files loaded into the machine, for PSETs; never shown
  params:add_text(id(i, "files"), "files", "")
  -- insert params moved by hand, for PSETs; never shown
  params:add_text(id(i, "fx"), "fx", "")

  params:add_control(id(i, "volume"), "volume", controlspec.new(-60, 3, "lin", 0.5, 0, "dB"))
  params:set_action(id(i, "volume"), function()
    if not P.loading then P.send_volume(i) end
  end)
  params:add_control(id(i, "pan"), "pan", controlspec.new(-1, 1, "lin", 0.01, 0, "", 0.01),
    function(param) return P.pan_text(param:get()) end)
  params:add_option(id(i, "mute"), "mute", {"off", "on"}, 1)
  params:add_option(id(i, "solo"), "solo", {"off", "on"}, 1)
  params:add_control(id(i, "rev"), "rev send", controlspec.new(0, 1, "lin", 0.01, 0, "", 0.01), percent)
  params:add_control(id(i, "dly"), "dly send", controlspec.new(0, 1, "lin", 0.01, 0, "", 0.01), percent)
  for _, what in ipairs({"pan", "mute", "solo", "rev", "dly"}) do
    params:set_action(id(i, what), function()
      if not P.loading then P.send_strip_param(i, what) end
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
      if P.loading or not part.machine or j > #P.defs[part.machine] then return end
      -- An edit made while a patch is on its way goes on top of the patch
      -- once its values arrive.
      if part.pending then
        part.edits[j] = v
        return
      end
      send("/param", {i - 1, j - 1, v})
      if part.want_values then part.edits[j] = v end
    end)
  end
end

function P.add_params()
  for i = 1, P.COUNT do add_part(i) end
  P.add_master_params()
  for i = 1, P.COUNT do
    params:hide(id(i, "mpe"))
    show_machine(i, nil, true)
    params:hide(id(i, "files"))
    params:hide(id(i, "fx"))
  end
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

-- The "wave_tables" a PSET file was written with, or 8 if it has none.
local function saved_tables(filename)
  local f = filename and io.open(filename, "r")
  if not f then return P.WAVE_TABLES end
  local n = 8
  for line in f:lines() do
    local v = line:match('^"wave_tables"%s*:%s*(%d+)')
    if v then n = tonumber(v) break end
  end
  f:close()
  return n
end

-- Readies a Trinity part's macro params for a PSET from before they existed,
-- which has nothing for them, and a hidden slot can hold what another machine
-- left there. With a patch, P.apply leaves them out and /values fills them
-- with the patch's own routes. Without one they go to their defaults (knobs
-- at 0, no destinations).
local function reset_macros(i)
  if P.list[i].machine ~= "Trinity" then return end
  if patch_name(i) then
    P.list[i].macs_from_patch = true
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
  local machine = P.list[i].machine
  if machine ~= "Trinity" and machine ~= "Nexus" then return end
  local wt = machine == "Nexus" and wtosc_slots(patch_name(i)) or {}
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

-- After a PSET is read, sends every part again, in order, so each machine is
-- mounted before its params arrive. A silent read doesn't run the machine
-- actions, so the menus are fixed here and the file read again to put the
-- patch and slots in the right options.
local rereading = false

function P.after_read(filename)
  if rereading then return end
  P.loading = true
  local changed = false
  for i, part in ipairs(P.list) do
    local v = params:get(id(i, "machine"))
    local machine = v > 1 and P.machines[v - 1] and P.machines[v - 1].name or nil
    if machine ~= part.machine then
      show_machine(i, machine, false)
      changed = true
    end
    part.channel = params:get(id(i, "channel")) - 1
  end
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
  end
  if saved_tables(filename) < P.WAVE_TABLES then
    for i = 1, P.COUNT do migrate_waves(i) end
  end
  params:set("wave_tables", P.WAVE_TABLES, true)
  if filename and not saved_has(filename, "macro_params") then
    for i = 1, P.COUNT do reset_macros(i) end
  end
  params:set("macro_params", 1, true)
  P.loading = false
  P.apply_all()
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
  for i, part in ipairs(P.list) do
    if part.want_values and not part.values_retried and now - part.values_at > 2 then
      part.values_retried = true
      part.values_at = now
      send_patch(i, "/patch", {i - 1, part.machine, part.sent})
    end
  end
end

-- The plugin has an empty engine: sends every part again.
function P.resend()
  P.lost = false
  print("acidulous: plugin started again, sending the parts")
  P.send_master()
  if P.on_resend then P.on_resend() end
  for i, part in ipairs(P.list) do
    part.fx_wait = 0 -- the old plugin's replies won't come
    if part.pending then
      P.send_strip(i) -- the pending patch mounts the machine
      part.resend_files = next(part.files) ~= nil
    elseif part.want_values then
      -- the slots don't hold this patch's values yet, so ask for them again
      P.send_strip(i)
      part.resend_files = next(part.files) ~= nil
      load_machine(i, true)
    else
      P.apply(i)
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
  local part = P.list[i]
  if part.machine ~= machine then
    part.files, part.loading, part.problem, part.last_path = {}, nil, nil, nil
    save_files(i)
  end
  clear_fx(i)
  P.loading = true
  params:set(id(i, "machine"), index)
  show_machine(i, P.list[i].machine, true)
  local names = P.patches[machine] or {}
  local which = 1
  for k, name in ipairs(names) do
    if name == patch then which = k end
  end
  params:set(id(i, "patch"), which)
  P.loading = false
  P.list[i].patch = patch_name(i)
end

-- Sends a part's volume and machine with its patch, which fills the slots
-- with the patch's values when it's applied.
function P.start_part(i)
  P.send_strip(i)
  load_machine(i, true)
end

-- For K3.
function P.audition(i, z)
  local part = P.list[i]
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
  local part = P.list[i]
  if on then
    part.held = part.held + 1
    part.last_note = util.time()
  else
    part.held = math.max(0, part.held - 1)
  end
end

-- ---- replies from the plugin --------------------------------------------------

function P.osc(path, args)
  if path == "/levels" then
    for i = 1, P.COUNT do
      local part = P.list[i]
      part.level = math.max(args[i] or 0, part.level * 0.7)
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
    if (args[2] or 0) > 80 then
      P.worst, P.worst_at = args[2], util.time()
    end
  elseif path == "/values" then
    local i = (args[1] or -1) + 1
    local part = P.list[i]
    if not part or part.pending then return end
    if part.machine ~= args[2] or part.sent ~= args[3] then return end
    if part.macs_from_patch then
      -- an old PSET: only the macro slots take the patch's values
      part.macs_from_patch = false
      for j, d in ipairs(P.defs[part.machine] or {}) do
        if d.name:match("^mac%d") and args[j + 3] then params:set(id(i, "s" .. j), args[j + 3], true) end
      end
    end
    if not part.want_values then return end
    part.want_values = false
    for j = 4, #args do
      local sid = id(i, "s" .. (j - 3))
      if params.lookup[sid] then params:set(sid, args[j], true) end
    end
    for j, v in pairs(part.edits) do
      params:set(id(i, "s" .. j), v, true)
      send("/param", {i - 1, j - 1, v})
    end
    part.edits = {}
  elseif path == "/sends" then
    -- a patch set the part's sends, or gave the player's back
    local i = (args[1] or -1) + 1
    local part = P.list[i]
    if not part then return end
    if not part.sends_moved.rev then params:set(id(i, "rev"), args[2], true) end
    if not part.sends_moved.dly then params:set(id(i, "dly"), args[3], true) end
  elseif path == "/inserts" then
    -- the part's inserts after a patch or unmount, with the edits on top
    local i = (args[1] or -1) + 1
    local part = P.list[i]
    if not part then return end
    -- an earlier /patch's, with a later one on its way
    part.fx_wait = math.max(part.fx_wait - 1, 0)
    if part.fx_wait > 0 then return end
    local fx, k = {}, 2
    while k + 2 <= #args do
      local slot, type, count = args[k], args[k + 1], args[k + 2]
      k = k + 3
      local list = {}
      for _ = 1, count do
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
      fx[slot] = {type = type, params = list}
    end
    part.fx = fx
  elseif path == "/macro_names" then
    local part = P.list[(args[1] or -1) + 1]
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
    P.patch_macros[args[2]] = P.patch_macros[args[2]] or {}
    P.patch_macros[args[2]][args[3]] = list
  elseif path == "/hello" then
    if P.ready then P.resend() end
  elseif path == "/machines" then
    if asked_machines then return end
    asked_machines = true
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
    local part = P.list[(args[1] or -1) + 1]
    if not part then return end
    local kind, fpath, arg, name = args[2], args[3], args[4], args[5]
    -- loaded into the machine the part had before a change, and gone with it
    if args[6] ~= part.machine then return end
    if part.loading and part.loading.kind == kind then part.loading = nil end
    if kind == "pad" then
      part.files["pad" .. arg] = fpath ~= "" and {kind = kind, path = fpath, arg = arg, name = name} or nil
    elseif kind == "sf2" or kind == "folder" or kind == "sample" then
      part.files.main = fpath ~= "" and {kind = kind, path = fpath, arg = arg, name = name} or nil
    end
    if fpath ~= "" and kind ~= "pad" then part.last_path = fpath
    elseif kind == "pad" and fpath ~= "" and not part.loading then part.last_path = fpath end
    save_files(part.n)
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
    local part = args[2] and P.list[args[2] + 1]
    -- the plugin sends "" for no machine
    local machine = args[6] ~= "" and args[6] or nil
    if part and machine == part.machine then
      if part.loading and (part.loading.kind == args[3] or args[3] ~= "pad") then part.loading = nil end
      part.problem = {text = args[1], at = util.time()}
    end
  end
end

return P
