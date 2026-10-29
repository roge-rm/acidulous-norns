-- A song from the app: loading one, its scenes and the transport.
-- The plugin unpacks a zip into dust/data/acidulous/songs/<name>/ and
-- replays it into the racks. While a song holds them, the script's own parts,
-- master level and tempo stay in their params and aren't sent (see
-- P.song_holds), the pages show the song's tracks as the parts (P.song in
-- lib/parts.lua, edits kept for the session only) and the out strips move
-- the song's master, reverb size and delay feedback. Its tempo rules: the
-- plugin ignores the script's /tempo and /beat, and norns' clock runs at the
-- song's tempo when its source is internal, without touching the clock_tempo
-- param. Unloading puts the clock back and sends the script's parts again.
--   S.add_params()          the hidden "song" param and "frozen clips", saved in PSETs
--   S.osc(path, args)       the plugin's /song_ replies
--   S.read()                after a PSET is read; false when its parts
--                           shouldn't be sent
--   S.key(n, z, k1), S.enc(n, d), S.screen()   the song page
--   S.cleanup()             on the script's cleanup

local timer = include("lib/timer")

local S = {}

S.DIR = _path.data .. "acidulous/songs/"

local parts
local loaded = nil  -- the path the loaded song came from
local folder = nil  -- where the plugin unpacked it
local loading = nil -- the path asked for, until /song_loaded
local frozen = {}   -- {part, machine, name} per part playing frozen clips, from /song_frozen
local scenes = {}   -- {name, id, repeats}, in play order
local pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
local cursor = 1    -- the scene E2 is on
local list = nil    -- the songs list while it's open: {rows, cursor}
local problem = nil -- {text, at}
local took_tempo = false -- norns' clock runs at the song's tempo
local quiet_unload = false -- an unload for a PSET, whose parts are already on their way
-- The racks hold neither the script's parts nor a song: a PSET's song is on
-- its way in place of its parts, and they're sent if it fails.
local parts_owed = false
local pending_load = nil -- the timer of a load still to be sent
local pending_path = nil -- and the song it loads

local function cancel_pending()
  timer.cancel(pending_load)
  pending_load, pending_path = nil, nil
end

-- Loads path after wait seconds, unless cancelled first.
local load_later

-- What the parts module needs to know: song_holds, messages sent now would
-- land on a song's racks; song_racks, replies about the racks are the
-- song's.
local function hold(holds, racks)
  parts.song_holds = holds
  if racks ~= nil then parts.song_racks = racks end
end

local reset
function S.init(p)
  parts = p
  parts.on_plugin_reset = function() reset() end
end

function S.loaded() return loaded ~= nil end

local function internal_clock() return params:string("clock_source") == "internal" end

-- norns' clock follows the song's tempo, when it's norns' own clock. The
-- clock_tempo param keeps the script's own, so norns' saved tempo is left
-- alone.
local function follow(bpm)
  if not bpm or not internal_clock() then return end
  took_tempo = true
  if math.abs(clock.get_tempo() - bpm) < 0.05 then return end
  clock.internal.set_tempo(bpm)
end

local function give_back_tempo()
  if took_tempo and internal_clock() then clock.internal.set_tempo(params:get("clock_tempo")) end
  took_tempo = false
end

-- When the script stops: norns' clock gets its tempo back. The plugin and
-- the song go with the engine.
function S.cleanup() give_back_tempo() end

-- The plugin started again with nothing loaded: the parts go to it, then the
-- song that was loaded goes again.
reset = function()
  -- a PSET's song still waiting to be sent counts as asked for
  local was = pending_path or loading or loaded
  loaded, folder, loading, scenes = nil, nil, nil, {}
  pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
  parts.song_master, parts.song_sends = nil, nil
  quiet_unload, parts_owed = false, false
  cancel_pending()
  hold(false, false)
  parts.song_gone()
  -- norns' clock goes back to the script's tempo; the song takes it again
  -- if it loads
  give_back_tempo()
  if was then load_later(0.5, was) end
end

-- Which racks play the zip's frozen clips instead of their notes; read when
-- a song loads.
S.FROZEN = {"auto", "notes", "frozen"}

function S.add_params()
  params:add_text("song", "song", "")
  params:hide("song")
  params:add_option("song_frozen", "frozen clips", S.FROZEN, 1)
end

load_later = function(wait, path)
  cancel_pending()
  pending_path = path
  pending_load = timer.after(wait, function()
    pending_load, pending_path = nil, nil
    S.load(path)
  end)
end

function S.load(path)
  cancel_pending()
  loading = path
  parts.song_load_started(path == loaded)
  parts.send("/song_load", {path, "", S.FROZEN[params:get("song_frozen")] or "auto"})
  hold(true, true)
end

function S.unload(quiet)
  cancel_pending()
  quiet_unload = quiet or false
  parts.send("/song_unload", {})
  -- what's sent after it lands on the emptied racks; the unload's own
  -- replies come before /song_unloaded. The pages show the script's parts.
  hold(false)
  parts.song_gone(loading ~= nil)
end

-- After a PSET is read: its parts are the script's own, kept in the params.
-- With the song already loaded it keeps playing on its racks. With another
-- song, that song replaces what's loaded, and the parts are sent only if it
-- fails. With no song, a loaded one is unloaded and the parts are sent.
function S.read()
  local want = params:get("song") or ""
  -- the PSET's master level is the script's; the out strip keeps the song's
  if want ~= "" and want == loaded then
    cancel_pending()
    return false
  end
  if want == "" then
    if loaded or loading or pending_load then S.unload(true) end
    return true
  end
  if loaded then S.unload(true) end
  parts_owed = true
  hold(true)
  load_later(0.2, want)
  return false
end

-- The parts go to the racks again, when they're the script's again.
local function send_parts()
  parts_owed = false
  parts.apply_all()
end

function S.osc(path, args)
  if path == "/song_scenes" then
    scenes = {}
    local k = 2
    for _ = 1, args[1] or 0 do
      scenes[#scenes + 1] = {name = args[k] or "", id = args[k + 1] or "", repeats = args[k + 3] or 1}
      k = k + 4
    end
  elseif path == "/song_frozen" then
    frozen = {}
    local k = 2
    for _ = 1, args[1] or 0 do
      frozen[#frozen + 1] = {part = args[k], machine = args[k + 1] == 1, name = args[k + 2] or ""}
      k = k + 3
    end
  elseif path == "/song_loaded" then
    local err = args[3] or ""
    if args[1] == loading then loading = nil end
    if err ~= "" then
      problem = {text = err, at = util.time()}
      print("acidulous: song: " .. err)
      if not loading then parts.song_load_failed(loaded ~= nil) end
      -- nothing loaded: the racks have the script's parts, or none if
      -- they're owed. Parts changed while the load was on its way are sent.
      if not loaded and not loading then
        hold(false, false)
        params:set("song", "", true)
        if parts_owed then send_parts() else parts.pay_owed() end
      end
      return
    end
    loaded, folder = args[1], args[2]
    parts_owed = false
    hold(true, true)
    params:set("song", loaded, true)
    -- the tracks: (part, machine, patch, name) per rack with a machine
    local tracks, k = {}, 6
    for _ = 1, args[5] or 0 do
      tracks[#tracks + 1] = {part = args[k], machine = args[k + 1], patch = args[k + 2], name = args[k + 3] or ""}
      k = k + 4
    end
    parts.song_loaded(tracks, frozen)
    frozen = {}
    pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
    cursor = 1
    parts.send("/song_position", {})
  elseif path == "/song_sends" then
    -- the sends page's out strip shows and moves the song's Reverb size and
    -- Delay feedback, nil for a send the song doesn't have
    local size, fdbk = args[1] or -1, args[2] or -1
    parts.song_sends = {size = size >= 0 and size or nil, feedback = fdbk >= 0 and fdbk or nil}
  elseif path == "/song_master" then
    -- the out strip shows the song's master; moving it moves the song's
    local v = args[1] or 1
    parts.song_master = v > 0 and math.max(-60, 20 * math.log(v, 10)) or -60
  elseif path == "/song_unloaded" then
    loaded, folder, scenes = nil, nil, {}
    pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
    parts.song_master, parts.song_sends = nil, nil
    parts.song_gone(loading ~= nil)
    give_back_tempo()

    if params.lookup["song"] then params:set("song", "", true) end
    -- After a quiet unload a PSET's parts are on their way already. A load
    -- on its way (a failed one that emptied the racks, or a PSET's song)
    -- keeps the racks, and the parts go if it fails. The plugin puts the
    -- script's master back itself.
    local emptied = args[1] == 1 and not quiet_unload
    quiet_unload = false
    if loading or parts_owed then
      if emptied then parts_owed = true end
    else
      hold(false, false)
      if emptied then send_parts() else parts.pay_owed() end
    end
  elseif path == "/song_position" then
    pos = {playing = args[1] == 1, scene = args[2] or 0, pass = args[3] or 0, queued = args[5] or -1, bpm = args[6]}
    -- one still on its way after the song went doesn't take the clock
    if loaded then follow(pos.bpm) end
  end
end

-- ---- the songs list ----------------------------------------------------------

local function base(path) return path:match("([^/]+)/*$") or path end

-- Zips in the songs folder and dust/audio, and songs already unpacked.
local function scan()
  local rows = {}
  if loaded then rows[1] = {name = "none", path = nil} end
  local seen = {}
  local function add(name, path)
    if seen[path] then return end
    seen[path] = true
    rows[#rows + 1] = {name = name, path = path}
  end
  -- an unpacked folder only when its zip isn't there too
  local found, zips = {}, {}
  local entries = util.scandir(S.DIR) or {}
  for _, entry in ipairs(entries) do
    if entry:lower():match("%.zip$") then
      local name = entry:gsub("%.[zZ][iI][pP]$", "")
      zips[name] = true
      found[#found + 1] = {name = name, path = S.DIR .. entry}
    end
  end
  for _, entry in ipairs(entries) do
    local name = base(entry)
    if entry:match("/$") and not zips[name] and util.file_exists(S.DIR .. entry .. "engine.txt") then
      found[#found + 1] = {name = name, path = S.DIR .. name}
    end
  end
  local f = io.popen("find '" .. _path.audio .. "' -maxdepth 4 -iname '*.zip' 2>/dev/null | sort")
  if f then
    for line in f:lines() do
      found[#found + 1] = {name = line:sub(#_path.audio + 1):gsub("%.[zZ][iI][pP]$", ""), path = line}
    end
    f:close()
  end
  table.sort(found, function(a, b) return a.name:lower() < b.name:lower() end)
  for _, r in ipairs(found) do add(r.name, r.path) end
  return rows
end

local function open_list()
  list = {rows = scan(), cursor = 1}
  for k, r in ipairs(list.rows) do
    if r.path and r.path == loaded then list.cursor = k end
  end
end

function S.list_open() return list ~= nil end

-- ---- controls ------------------------------------------------------------------

function S.key(n, z, k1)
  if z ~= 1 then return end
  if list then
    if n == 2 then
      list = nil
    elseif n == 3 then
      local row = list.rows[list.cursor]
      list = nil
      if not row then return end
      if row.path then S.load(row.path) else S.unload() end
    end
    return
  end
  if n == 2 and (k1 or not loaded) then
    open_list()
  elseif not loaded then
    return
  elseif n == 2 then
    if pos.playing then
      parts.send("/song_stop", {})
    else
      parts.send("/song_play", {})
    end
    parts.send("/song_position", {})
  elseif n == 3 then
    local k = cursor - 1
    if not pos.playing then
      parts.send("/song_play", {k})
    elseif pos.queued == k then
      parts.send("/song_queue", {-1})
    else
      parts.send("/song_queue", {k})
    end
    -- the position comes back with the change
    parts.send("/song_position", {})
  end
end

function S.enc(n, d)
  if list then
    if n == 2 then list.cursor = util.clamp(list.cursor + d, 1, math.max(1, #list.rows)) end
  elseif n == 2 and #scenes > 0 then
    cursor = util.clamp(cursor + d, 1, #scenes)
  end
end

-- What the song page draws.
function S.screen()
  local note = problem and util.time() - problem.at < 10 and problem.text or nil
  if list then
    local rows = {}
    for k, r in ipairs(list.rows) do rows[k] = r.name end
    if #rows == 0 then rows[1] = "no songs" end
    return {kind = "list", pick = {title = "songs", rows = rows, cursor = list.cursor}, note = note}
  end
  return {
    kind = "song",
    name = folder and base(folder) or nil,
    loading = loading and base(loading):gsub("%.[zZ][iI][pP]$", "") or nil,
    scenes = scenes, cursor = cursor, pos = pos, note = note,
  }
end

return S
