-- The 8 parts: their params, and what they send to the plugin.
-- A part n is the plugin's part n - 1 (an engine track).

local P = {}

P.COUNT = 8
local PLUGIN = {"127.0.0.1", 57150}

-- The machines offered, in menu order: the pure-DSP ones that run in real
-- time on a Pi 4 (docs/machines.md). short is for the main page, note for
-- K3's audition.
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
}

P.machines = {}  -- the ones the plugin has, from /machines: {name, short, note}
P.defs = {}      -- machine -> list of {name, min, max, default, unit, steps, curve}
P.patches = {}   -- machine -> list of patch names
P.slots = 0      -- param slots per part: the most any machine has
P.ready = false
P.error = nil     -- shown instead of the parts, e.g. no machines
P.loading = false -- a PSET is being read
P.seen = 0        -- when the plugin last sent /load
P.load, P.worst, P.worst_at = 0, 0, 0
P.list = {}
for i = 1, P.COUNT do
  P.list[i] = {
    n = i, channel = i, machine = nil, patch = nil,
    level = 0, held = 0, last_note = 0,
    want_values = false, pending = nil,
    sent = nil,     -- the patch last sent to the plugin
    edits = {},     -- slot -> value, set while a patch is on its way
    values_at = 0, values_retried = false,
  }
end

local function send(path, args) osc.send(PLUGIN, path, args) end
P.send = send

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

function P.start(ready)
  on_ready = ready
  clock.run(function()
    -- the plugin can take a moment to start; ask again until it answers
    while not asked_machines do
      send("/machines", {})
      clock.sleep(1)
    end
    if P.error then return end
    while not P.ready do
      clock.sleep(1)
      if waiting and util.time() - waiting.asked > 2 then
        table.insert(queue, 1, waiting)
        ask_next()
      end
    end
    P.seen = util.time()
    while true do
      clock.sleep(0.5)
      watch()
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

function P.send_volume(i)
  -- the strip's gain is 0-1.5 linear
  send("/channel", {i - 1, "gain", math.min(1, db_to_gain(params:get(id(i, "volume"))) / 1.5)})
end

function P.notes_off(i)
  local part = P.list[i]
  send("/cc", {i - 1, math.max(part.channel, 1), 123, 0})
  part.held = 0
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

-- Mounts the part's machine with its patch, or empties it.
local function load_machine(i, with_values)
  local part = P.list[i]
  part.pending = nil
  local name = patch_name(i)
  part.want_values = false
  if not with_values then part.edits = {} end
  if not part.machine then
    send("/unmount", {i - 1})
    name = nil
  elseif name then
    part.want_values = with_values
    part.values_at, part.values_retried = util.time(), false
    send("/patch", {i - 1, part.machine, name})
  else
    send("/mount", {i - 1, part.machine})
  end
  part.patch = name
  part.sent = name
end

-- Runs fn after `wait` seconds unless called again first, so turning an
-- encoder through patches only loads the one it stops on.
local function later(i, wait, fn)
  local part = P.list[i]
  if part.pending then clock.cancel(part.pending) end
  part.pending = clock.run(function()
    clock.sleep(wait)
    part.pending = nil
    fn()
  end)
end

-- Sends a part's whole state, after a PSET is read. The patch goes first,
-- which mounts the machine, then the saved params on top of it.
function P.apply(i)
  local part = P.list[i]
  if part.pending then clock.cancel(part.pending); part.pending = nil end
  P.notes_off(i)
  P.send_volume(i)
  load_machine(i, false)
  -- C++ waits for room in the engine's param queue, so none of these is lost
  if part.machine then
    for j = 1, #P.defs[part.machine] do
      send("/param", {i - 1, j - 1, params:get(id(i, "s" .. j))})
    end
  end
end

function P.apply_all()
  for i = 1, P.COUNT do P.apply(i) end
end

local function add_part(i)
  local part = P.list[i]
  params:add_group("part_" .. i, "part " .. i, 4 + P.slots)

  local chans = {"off"}
  for c = 1, 16 do chans[c + 1] = tostring(c) end
  params:add_option(id(i, "channel"), "channel", chans, i + 1)
  params:set_action(id(i, "channel"), function(v)
    if not P.loading then P.notes_off(i) end
    part.channel = v - 1
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
    later(i, 0.2, function() load_machine(i, true) end)
  end)

  params:add_control(id(i, "volume"), "volume", controlspec.new(-60, 3, "lin", 0.5, 0, "dB"))
  params:set_action(id(i, "volume"), function()
    if not P.loading then P.send_volume(i) end
  end)

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
  for i = 1, P.COUNT do show_machine(i, nil, true) end
  -- params:clear() removes this when the script ends.
  params.action_read = function(filename) P.after_read(filename) end
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
  if changed and filename then
    rereading = true
    local ok, err = pcall(params.read, params, filename, true)
    rereading = false
    if not ok then print("acidulous: pset read failed: " .. tostring(err)) end
  end
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
      engine.start()
    else
      engine.load("Acidulous", function() engine.start() end)
    end
  end
  for i, part in ipairs(P.list) do
    if part.want_values and not part.values_retried and now - part.values_at > 2 then
      part.values_retried = true
      part.values_at = now
      send("/patch", {i - 1, part.machine, part.sent})
    end
  end
end

-- The plugin has an empty engine: sends every part again.
function P.resend()
  P.lost = false
  print("acidulous: plugin started again, sending the parts")
  for i, part in ipairs(P.list) do
    if part.pending then
      P.send_volume(i) -- the pending patch mounts the machine
    elseif part.want_values then
      -- the slots don't hold this patch's values yet, so ask for them again
      P.send_volume(i)
      load_machine(i, true)
    else
      P.apply(i)
    end
  end
end

-- Puts a machine and patch on a part without the debounce, for start-up.
function P.set_part(i, machine, patch)
  local index = 1
  for k, m in ipairs(P.machines) do
    if m.name == machine then index = k + 1 end
  end
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
  P.send_volume(i)
  load_machine(i, true)
end

-- For K3 and the params page.
function P.audition(i, z)
  local part = P.list[i]
  if not part.machine then return end
  local m = P.machine_info(part.machine)
  local note = m and m.note or 48
  send("/note", {i - 1, math.max(part.channel, 1), note, z == 1 and 100 or 0})
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
    if not part or not part.want_values or part.pending then return end
    if part.machine ~= args[2] or part.sent ~= args[3] then return end
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
  elseif path == "/error" then
    print("acidulous: " .. tostring(args[1]))
  end
end

return P
