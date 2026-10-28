-- A song from the app: loading one, its scenes and the transport.
-- The plugin unpacks a zip into dust/data/acidulous/songs/<name>/ and
-- replays it into the parts. While a song is loaded its tempo rules: the
-- plugin ignores the script's /tempo and /beat, and norns' clock follows the
-- song when its source is internal. Unloading puts the clock back and sends
-- the script's parts again.
--   S.add_params()          the hidden "song" param, saved in PSETs
--   S.osc(path, args)       the plugin's /song_ replies
--   S.read()                after a PSET is read, before the parts are sent
--   S.key(n, z, k1), S.enc(n, d), S.screen()   the song page

local timer = include("lib/timer")

local S = {}

S.DIR = _path.data .. "acidulous/songs/"

local parts
local loaded = nil  -- the path the loaded song came from
local folder = nil  -- where the plugin unpacked it
local loading = nil -- the path asked for, until /song_loaded
local scenes = {}   -- {name, id, repeats}, in play order
local pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
local cursor = 1    -- the scene E2 is on
local list = nil    -- the songs list while it's open: {rows, cursor}
local problem = nil -- {text, at}
local old_tempo = nil -- norns' clock tempo before the song took it
local quiet_unload = false -- an unload for a PSET, whose parts are already on their way
local old_master = nil -- the script's master level in dB while a song has its own

function S.init(p) parts = p end

function S.loaded() return loaded ~= nil end

local function internal_clock() return params:string("clock_source") == "internal" end

-- norns' clock follows the song's tempo, when it's norns' own clock.
local function follow(bpm)
  if not bpm or not internal_clock() then return end
  if math.abs(clock.get_tempo() - bpm) < 0.05 then return end
  if not old_tempo then old_tempo = params:get("clock_tempo") end
  params:set("clock_tempo", bpm)
end

local function give_back_tempo()
  if old_tempo and internal_clock() then params:set("clock_tempo", old_tempo) end
  old_tempo = nil
end

function S.add_params()
  params:add_text("song", "song", "")
  params:hide("song")
end

function S.load(path)
  loading = path
  parts.send("/song_load", {path})
end

function S.unload(quiet)
  quiet_unload = quiet or false
  parts.send("/song_unload", {})
end

-- After a PSET is read: its song, or none. An unload goes now, before the
-- parts; a load goes after them, as it replaces them.
function S.read()
  local want = params:get("song") or ""
  if want == (loaded or "") then return end
  if loaded then S.unload(true) end
  if want ~= "" then timer.after(0.2, function() S.load(want) end) end
end

function S.osc(path, args)
  if path == "/song_scenes" then
    scenes = {}
    local k = 2
    for _ = 1, args[1] or 0 do
      scenes[#scenes + 1] = {name = args[k] or "", id = args[k + 1] or "", repeats = args[k + 3] or 1}
      k = k + 4
    end
  elseif path == "/song_loaded" then
    local err = args[3] or ""
    if args[1] == loading then loading = nil end
    if err ~= "" then
      problem = {text = err, at = util.time()}
      print("acidulous: song: " .. err)
      return
    end
    loaded, folder = args[1], args[2]
    params:set("song", loaded, true)
    pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
    cursor = 1
    parts.send("/song_position", {})
  elseif path == "/song_master" then
    -- the out strip shows the song's master; moving it moves the song's
    if not old_master then old_master = params:get("master_level") end
    local v = args[1] or 1
    params:set("master_level", v > 0 and math.max(-60, 20 * math.log(v, 10)) or -60, true)
  elseif path == "/song_unloaded" then
    loaded, folder, scenes = nil, nil, {}
    pos = {playing = false, scene = 0, pass = 0, queued = -1, bpm = nil}
    give_back_tempo()
    if params.lookup["song"] then params:set("song", "", true) end
    -- a PSET's master and parts are on their way already
    if old_master and not quiet_unload then params:set("master_level", old_master, true) end
    old_master = nil
    if args[1] == 1 and not quiet_unload then parts.apply_all() end
    quiet_unload = false
  elseif path == "/song_position" then
    pos = {playing = args[1] == 1, scene = args[2] or 0, pass = args[3] or 0, queued = args[5] or -1, bpm = args[6]}
    follow(pos.bpm)
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
