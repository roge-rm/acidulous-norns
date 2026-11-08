-- The file picker for a sample machine's part, opened with K1 + K3.
-- E2 moves. K3 opens a folder, hears a file, or opens a SoundFont's
-- presets. K1 + K3 loads the file. K3 loads "use this folder" or a preset. K2 goes back a folder,
-- and closes at the top.
--   Forage: E3 picks the pad a file goes on. A folder puts its first 13
--     files by name on pads 1-13.
--   Mosaic: a SoundFont and then its preset, or a folder of WAVs
--   Dice, Pollen, Molt: a file. Pollen's first list also has "live input",
--     which empties the part (K3 or K1 + K3).
-- A Venue's card opens it too (K1 + K3 on the card) for an impulse file;
-- K1 + K3 or K3 on a file picks it.
-- Files on the USB stick are copied to dust/audio/acidulous when loaded.
-- .acpreset files (the app's presets) show in every folder, on any part with a
-- machine. K1 + K3 on one copies it into the saved presets of its machine
-- (the patch list's "saved" tab), or the fx 3 and 4 or the mods card lists.

local lib = include("lib/library")
local timer = include("lib/timer")

local P = {}

P.lib = lib

P.PAD_NAMES = {"kick", "rim", "snare", "clap", "low tom", "mid tom", "hi tom",
  "closed hat", "open hat", "crash", "ride", "cowbell", "clave"}

local AUDIO = lib.AUDIO_EXT
P.TAKES = {
  Forage = {folder = "kit", file = "pad", exts = AUDIO},
  Mosaic = {folder = "folder", file = "sf2", exts = {sf2 = true}},
  Dice = {file = "sample", exts = AUDIO},
  Pollen = {file = "sample", exts = AUDIO},
  Molt = {file = "sample", exts = AUDIO},
}

local st = nil
local parts
local shapes = {} -- path -> {seconds, rate, cut, cols}, from /shape
local shape_count = 0
local heard = nil -- {path, at, seconds} while a file plays

function P.takes(machine) return P.TAKES[machine] end

local function is_preset(row)
  return row and row.path and not row.dir and lib.PRESET_EXT[lib.ext(row.path)] ~= nil
end
function P.active() return st ~= nil end

local function stop_hearing()
  if heard then
    parts.send("/hear", {})
    heard = nil
  end
end

local function close()
  if not st then return end
  timer.cancel(st.asking)
  -- always, as a /hear may be on its way back
  parts.send("/hear", {})
  heard = nil
  st = nil
end

local function say(text)
  st.note, st.note_at = text, util.time()
end

-- A copy shows "copying" for at least this long, so it can be read.
local MIN_BUSY = 0.5

local function busy(text)
  st.mode, st.busy, st.busy_at = "busy", text, util.time()
end

-- Runs fn once the busy screen has been up MIN_BUSY, unless the picker has
-- closed or moved on by then.
local function after_busy(was, fn)
  local wait = MIN_BUSY - (util.time() - (was.busy_at or 0))
  if wait <= 0 then return fn() end
  timer.after(wait, function() if st == was then fn() end end)
end

local function is_audio(row)
  return row and row.path and not row.dir and AUDIO[lib.ext(row.path)]
end

-- Asks for the selected file's shape once the cursor has stopped on it.
local function ask_shape()
  timer.cancel(st.asking)
  local row = st.mode == "browse" and st.rows[st.cursor]
  if not is_audio(row) or shapes[row.path] then return end
  local path = row.path
  st.asking = timer.after(0.2, function()
    if st then st.asking = nil end
    if not shapes[path] then parts.send("/shape", {path}) end
  end)
end

-- The rows for the current folder, or the roots.
local function fill()
  st.rows = {}
  if not st.dir then
    if st.machine == "Pollen" then st.rows[1] = {label = "live input", live = true} end
    for _, r in ipairs(lib.roots()) do
      st.rows[#st.rows + 1] = {label = r.name, path = r.path, dir = true}
    end
  else
    if st.t.folder then st.rows[1] = {label = "use this folder", this = true} end
    for _, e in ipairs(lib.list(st.dir, st.exts)) do
      st.rows[#st.rows + 1] = {label = e.dir and (e.name .. "/") or e.name, path = e.path, dir = e.dir}
    end
    if #st.rows == 0 then st.rows[1] = {label = "nothing here", none = true} end
  end
  st.cursor = util.clamp(st.cursor or 1, 1, #st.rows)
  ask_shape()
end

local function root_of(path)
  for _, r in ipairs(lib.roots()) do
    if path == r.path or path:sub(1, #r.path + 1) == r.path .. "/" then return r end
  end
end

-- Opens dir with the row for path `select` picked, if it's there.
local function enter(dir, select)
  st.dir = dir
  st.cursor = 1
  fill()
  if select then
    for k, row in ipairs(st.rows) do
      if row.path == select then st.cursor = k end
    end
    ask_shape()
  end
end

local function back()
  if st.mode ~= "browse" then
    st.mode = "browse"
    fill()
    return
  end
  if not st.dir then close(); return end
  local r = root_of(st.dir)
  if not r or st.dir == r.path then
    local was = r and r.path
    st.dir = nil
    st.cursor = 1
    fill()
    for k, row in ipairs(st.rows) do
      if row.path == was then st.cursor = k end
    end
  else
    enter(lib.parent(st.dir), st.dir)
  end
end

-- After the copy, if it was on the stick.
local function load(path, is_dir)
  local i = st.i
  if st.slot then
    parts.set_impulse(i, st.slot, path)
    close()
  elseif is_dir then
    parts.load_file(i, st.t.folder, path, 0)
    close()
  elseif st.t.file == "pad" then
    parts.load_file(i, "pad", path, st.pad - 1)
    say("pad " .. st.pad .. ": " .. lib.stem(path))
    -- on to the next pad, for building a kit
    st.pad = st.pad % 13 + 1
  elseif st.t.file == "sf2" then
    st.mode, st.file, st.presets = "presets", path, nil
    st.cursor = 1
    parts.send("/presets", {path})
  else
    parts.load_file(i, st.t.file, path, 0)
    close()
  end
end

local function pick(path, is_dir)
  if not lib.on_usb(path) then
    load(path, is_dir)
    return
  end
  busy("copying " .. lib.base(path))
  local was = st
  lib.import(path, is_dir, function(ok, dest, why)
    if st ~= was then return end
    after_busy(was, function()
      st.mode = "browse"
      if ok then
        load(dest, is_dir)
      else
        say("copy failed" .. (why ~= "" and (": " .. why) or ""))
      end
    end)
  end)
end

-- Where a preset with this unit from /preset_scan goes: the folder, what to
-- rescan and the words for the message, or nil and why not.
local function preset_home(unit)
  local safe = unit:match("^[%w _%-]+$")
  local kind, type = unit:match("^(%a+)%.(.+)$")
  if kind == "fx" then
    if not type:match("^[%w _%-]+$") or (#parts.effects > 0 and not tab.contains(parts.effects, type)) then
      return nil, "no " .. type .. " here"
    end
    return parts.fx_preset_dir(type), "fx 3 and 4 presets", parts.scan_fx_presets
  elseif kind == "mod" then
    if not parts.MOD_PRESET_TYPES[type] then return nil, "no " .. type .. " presets" end
    return parts.mod_preset_dir(type), "mods card presets", parts.scan_mod_presets
  end
  if not safe or not parts.defs[unit] then return nil, "no " .. unit .. " here" end
  return parts.preset_dir(unit), unit .. " saved presets", function() parts.scan_presets(unit) end
end

-- A name for the file in dir that isn't taken: "name", "name 2", "name 3"...
local function free_name(dir, stem)
  local name, n = stem, 1
  while util.file_exists(dir .. name .. ".acpreset") and n < 1000 do
    n = n + 1
    name = stem .. " " .. n
  end
  return name
end

-- Asks the plugin what the preset is, then copies it to its folder.
local function import(path)
  st.mode, st.busy = "busy", "reading " .. lib.base(path)
  st.importing = path
  parts.send("/preset_scan", {path})
end

-- /preset_scan of the file: {name, unit, count}, or nil if it can't be read.
function P.scanned(path, info)
  if not st or st.importing ~= path then return end
  local was = st
  local function fail(text)
    was.importing, was.mode = nil, "browse"
    say(text)
  end
  if not info then return fail("not a preset") end
  local dir, where, rescan = preset_home(info.unit)
  if not dir then return fail(where) end
  local name = free_name(dir, lib.stem(path))
  busy("copying " .. lib.base(path))
  lib.copy_file(path, dir .. name .. ".acpreset", function(ok, why, same)
    if st ~= was then return end
    after_busy(was, function()
      was.importing, was.mode = nil, "browse"
      if not ok then return say("copy failed" .. (why ~= "" and (": " .. why) or "")) end
      if same then return say("already in " .. where) end
      rescan()
      say(name == lib.stem(path) and ("in " .. where) or (name .. ", in " .. where))
    end)
  end, "acpreset")
end

-- Opens the picker for part i, in the folder of what it has loaded if that's
-- still there. False if the part's machine takes no files.
function P.open(the_parts, i, slot)
  parts = the_parts
  local part = parts.list[i]
  if not part.machine then return false end
  -- a machine that takes no files still lists presets
  local t = slot and {file = "impulse", exts = AUDIO} or P.TAKES[part.machine] or {exts = {}}
  local exts = {acpreset = true}
  for e in pairs(t.exts) do exts[e] = true end
  st = {i = i, machine = slot and "Venue" or part.machine, t = t, exts = exts, mode = "browse", cursor = 1, pad = 1,
    slot = slot}
  if t.file == "pad" then
    local names = parts.pad_names(i)
    for pad = 1, 13 do
      if not names[pad] then st.pad = pad; break end
    end
  end
  local from = slot and parts.impulse_path(i, slot) or not slot and parts.loaded_path(i)
  local dir = from and lib.parent(from)
  if dir and root_of(dir) and util.file_exists(dir) then
    enter(dir, from)
  else
    st.dir = nil
    fill()
  end
  return true
end

function P.close() close() end

-- The stick went in or out.
function P.usb_changed()
  if not st then return end
  if st.dir and not root_of(st.dir) then
    st.dir = nil
    st.mode = "browse"
  end
  if st.mode == "browse" then fill() end
end

-- Replies from the plugin.
function P.presets(path, names)
  if not st or st.mode ~= "presets" or st.file ~= path then return end
  if #names == 0 then
    st.mode = "browse"
    fill()
    say("no presets in " .. lib.base(path))
    return
  end
  st.presets = names
end

function P.shape(path, seconds, rate, cut, cols)
  if shape_count > 64 then shapes, shape_count = {}, 0 end
  shapes[path] = {seconds = seconds, rate = rate, cut = cut == 1, cols = cols}
  shape_count = shape_count + 1
end

function P.heard(path, seconds)
  if path == "" or not st then
    heard = nil
  else
    heard = {path = path, at = util.time(), seconds = seconds}
  end
end

-- k1: K1 is held.
function P.key(n, z, k1)
  if not st or z ~= 1 then return end
  if n == 2 then
    back()
  elseif n == 3 then
    if st.mode == "presets" then
      if st.presets then
        parts.load_file(st.i, "sf2", st.file, st.cursor - 1)
        close()
      end
    elseif st.mode == "browse" then
      local row = st.rows[st.cursor]
      if not row or row.none then return end
      if row.live then
        parts.clear_files(st.i)
        close()
      elseif row.this then
        pick(st.dir, true)
      elseif row.dir then
        enter(row.path)
      elseif is_preset(row) then
        if k1 then import(row.path) else say("k1+k3 imports it") end
      elseif k1 or not is_audio(row) then
        stop_hearing()
        pick(row.path, false)
      elseif heard and heard.path == row.path then
        stop_hearing()
      else
        parts.send("/hear", {row.path})
      end
    end
  end
end

local function count()
  if st.mode == "presets" then return st.presets and #st.presets or 0 end
  if st.mode == "browse" then return #st.rows end
  return 0
end

function P.enc(n, d)
  if not st then return end
  if n == 2 then
    st.cursor = util.clamp(st.cursor + d, 1, math.max(1, count()))
    if st.mode == "browse" then ask_shape() end
  elseif n == 3 and st.t.file == "pad" and st.mode == "browse" then
    st.pad = util.clamp(st.pad + d, 1, 13)
  end
end

local function seconds_text(s)
  if s < 10 then return string.format("%.1f s", s) end
  if s < 60 then return string.format("%d s", math.floor(s + 0.5)) end
  return string.format("%d:%02d", math.floor(s / 60), math.floor(s % 60))
end

-- What view.picker draws: {title, right, rows = {label...}, cursor, busy,
-- short, info, cols, playhead (0-1)}. short: 4 rows with the selected file's
-- shape below, otherwise 8.
function P.screen()
  local out = {cursor = st.cursor, rows = {}}
  local note = st.note and util.time() - st.note_at < 2 and st.note or nil
  local title = st.i .. " " .. st.machine
  if st.t.file == "pad" then title = title .. "  pad " .. st.pad end
  out.title = note or title
  if st.mode == "busy" then
    out.busy = st.busy
    return out
  end
  if st.mode == "presets" then
    out.title = note or (st.i .. " " .. st.machine .. "  " .. lib.base(st.file))
    if not st.presets then out.busy = "reading presets" end
    for k, name in ipairs(st.presets or {}) do out.rows[k] = k .. " " .. name end
    return out
  end
  for k, row in ipairs(st.rows) do out.rows[k] = row.label end
  if not st.dir or st.t.exts ~= AUDIO then return out end
  out.short = true
  local row = st.rows[st.cursor]
  if is_preset(row) then out.right = "k1+k3 import" end
  if is_audio(row) then
    out.right = "k3 hear"
    local s = shapes[row.path]
    if s and s.seconds > 0 then
      out.info = seconds_text(s.seconds) .. (s.cut and "+" or "") .. "  " .. string.format("%gk", s.rate / 1000)
      out.cols = s.cols
    elseif s then
      out.info = "can't read it"
    end
    if heard and heard.path == row.path and heard.seconds > 0 then
      local t = (util.time() - heard.at) / heard.seconds
      if t <= 1 then out.playhead = t else heard = nil end
    end
  end
  return out
end

return P
