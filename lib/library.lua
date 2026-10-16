-- Where samples come from: dust/audio, and a USB stick, mounted read-only at
-- /mnt/acidulous-usb while it's in. Files picked from the stick are copied
-- into dust/audio/acidulous/usb/, keeping their path on the stick, so PSETs
-- still find them once it's out. Mounting and copying run in the
-- background; the plugin decodes.

local timer = include("lib/timer")

local lib = {}

lib.MOUNT = "/mnt/acidulous-usb"
lib.AUDIO = _path.audio
lib.COPIES = _path.audio .. "acidulous"
-- scratch for background jobs, in the script's data folder
local function work() return norns.state.data .. "library" end

lib.AUDIO_EXT = {wav = true, aif = true, aiff = true, flac = true}

lib.device = nil -- the stick's partition while it's mounted

local function q(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

local function read_all(path)
  local f = io.open(path, "r")
  if not f then return nil end
  local s = f:read("a")
  f:close()
  return s
end

local function capture(cmd)
  local p = io.popen(cmd)
  local s = p:read("a")
  p:close()
  return s
end

-- The first partition of the first USB disk, or the disk itself if it has
-- none. Controllers' own drives aren't sticks: an RP2040's update drive (an
-- OMX-27 being updated) or a Launchpad's small "Easy Start" drive, told apart
-- by the disk's vendor, model and size.
local function find_device()
  local devs = capture("ls /dev/sd[a-z]* 2>/dev/null")
  local skip = {}
  for d in devs:gmatch("[^\n]+") do
    local disk = d:match("^/dev/(sd[a-z])$")
    if disk then
      local sys = "/sys/block/" .. disk
      local vendor = (read_all(sys .. "/device/vendor") or ""):lower()
      local model = (read_all(sys .. "/device/model") or ""):lower()
      local sectors = tonumber(read_all(sys .. "/size") or "") or 0
      if sectors * 512 < 64 * 1024 * 1024 or vendor:find("novation")
        or (vendor:find("rpi") and model:find("rp2")) then
        skip[disk] = true
      end
    end
  end
  local disk, part
  for d in devs:gmatch("[^\n]+") do
    if not skip[d:match("^/dev/(sd[a-z])")] then
      if d:match("^/dev/sd[a-z]$") then disk = disk or d
      elseif d:match("^/dev/sd[a-z]%d+$") then part = part or d end
    end
  end
  return part or disk
end

-- Runs a shell command in the background and calls done(ok, output),
-- checking every quarter second, so sudo, mount and cp never hold up matron.
local runs = 0
local function run(cmd, done)
  runs = runs + 1
  local out, marker = work() .. "/out" .. runs, work() .. "/done" .. runs
  os.remove(marker)
  os.execute("mkdir -p " .. q(work()))
  os.execute("(" .. cmd .. " > " .. q(out) .. " 2>&1; echo $? > " .. q(marker) .. ") &")
  local t
  t = timer.every(0.25, function()
    local code = read_all(marker)
    if not code then return end
    timer.cancel(t)
    local text = read_all(out) or ""
    os.remove(marker)
    os.remove(out)
    done(tonumber(code) == 0, text)
  end)
end

-- Read-only where possible. norns' usbmount may already have the stick
-- mounted read-write, and then only a mount with the same options works.
-- Nothing here writes to it either way.
local function mount_cmd(dev)
  local m = lib.MOUNT
  return "sudo umount " .. m .. " 2>/dev/null; sudo mkdir -p " .. m
    .. " && { sudo mount -o ro " .. dev .. " " .. m .. " || sudo mount " .. dev .. " " .. m .. "; }"
end

-- Watches for a stick going in or out every 2 seconds, and calls
-- on_change() when it does.
function lib.watch(on_change)
  local failed = nil -- a stick that wouldn't mount, left alone until it's out
  local mounting = nil
  lib.watcher = timer.every(2, function()
    if mounting then return end
    local dev = find_device()
    if dev and dev ~= lib.device and dev ~= failed then
      mounting = dev
      run(mount_cmd(dev), function(ok)
        mounting = nil
        if not lib.watcher then return end
        if ok then
          lib.device = dev
          if on_change then on_change() end
        else
          failed = dev
        end
      end)
    elseif not dev then
      failed = nil
      if lib.device then
        lib.device = nil
        run("sudo umount -l " .. lib.MOUNT, function() end)
        if on_change then on_change() end
      end
    end
  end)
end

-- On cleanup: lets go of the stick so it can be pulled out, without waiting.
function lib.stop()
  timer.cancel(lib.watcher)
  lib.watcher = nil
  if lib.device then
    os.execute("sudo umount -l " .. lib.MOUNT .. " >/dev/null 2>&1 &")
    lib.device = nil
  end
end

-- The places to start from: {name, path}.
function lib.roots()
  local out = {{name = "dust/audio", path = lib.AUDIO:gsub("/$", "")}}
  if lib.device then out[#out + 1] = {name = "usb stick", path = lib.MOUNT} end
  return out
end

function lib.on_usb(path)
  return path:sub(1, #lib.MOUNT + 1) == lib.MOUNT .. "/" or path == lib.MOUNT
end

function lib.ext(name) return (name:match("%.([^%./]+)$") or ""):lower() end
function lib.base(path) return path:match("([^/]+)/*$") or path end
function lib.parent(path) return path:match("^(.*)/[^/]+/*$") or path end
function lib.stem(path) return (lib.base(path):gsub("%.[^%.]*$", "")) end

-- A folder's folders and the files with an extension in exts (a set), each
-- {name, path, dir}, folders first, sorted without case. Hidden ones are
-- left out. Reading a folder is quick, even on a stick.
function lib.list(dir, exts)
  local folders, files = {}, {}
  local out = capture("ls -1Ap " .. q(dir) .. " 2>/dev/null")
  for name in out:gmatch("[^\n]+") do
    if name:sub(1, 1) ~= "." then
      if name:sub(-1) == "/" then
        name = name:sub(1, -2)
        folders[#folders + 1] = {name = name, path = dir .. "/" .. name, dir = true}
      elseif exts[lib.ext(name)] then
        files[#files + 1] = {name = name, path = dir .. "/" .. name}
      end
    end
  end
  local function by_name(a, b) return a.name:lower() < b.name:lower() end
  table.sort(folders, by_name)
  table.sort(files, by_name)
  for _, f in ipairs(files) do folders[#folders + 1] = f end
  return folders
end

-- Where a file or folder from the stick is copied to: usb/ and its path on
-- the stick, so two folders with the same name don't mix.
function lib.copy_target(path)
  local rel = path == lib.MOUNT and "" or path:sub(#lib.MOUNT + 2)
  return lib.COPIES .. "/usb" .. (rel ~= "" and ("/" .. rel) or "")
end

local PATTERNS = "\\( -iname '*.wav' -o -iname '*.aif' -o -iname '*.aiff' -o -iname '*.flac' \\)"

-- Copies a file, or a folder's audio files (not its subfolders), from the
-- stick, and checks every copy has its file's size. done(ok, new_path, why).
function lib.import(path, is_dir, done)
  local dest = lib.copy_target(path)
  local cmd
  if is_dir then
    local list = work() .. "/sizes"
    cmd = "mkdir -p " .. q(work()) .. " " .. q(dest)
      .. " && find " .. q(path) .. " -maxdepth 1 -type f ! -name '.*' " .. PATTERNS
      .. " -printf '%f\\t%s\\n' > " .. q(list)
      .. " && find " .. q(path) .. " -maxdepth 1 -type f ! -name '.*' " .. PATTERNS
      .. " -exec cp -t " .. q(dest) .. " {} +"
      .. " && while IFS=\"$(printf '\\t')\" read -r n s; do"
      .. " [ \"$(stat -c %s " .. q(dest) .. "/\"$n\")\" = \"$s\" ] || { echo \"$n didn't copy\"; exit 1; }; done < " .. q(list)
  else
    cmd = "mkdir -p " .. q(lib.parent(dest)) .. " && cp " .. q(path) .. " " .. q(dest)
      .. " && [ \"$(stat -c %s " .. q(path) .. ")\" = \"$(stat -c %s " .. q(dest) .. ")\" ]"
      .. " || { echo \"it didn't copy\"; exit 1; }"
  end
  run(cmd, function(ok, text)
    done(ok, dest, (text:gsub("\n.*", "")))
  end)
end

return lib
