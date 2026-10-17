-- First run and updates: the SuperCollider plugin.
--
-- bin/Acidulous.so goes into SuperCollider's Extensions folder, where scsynth
-- looks for plugins. scsynth only loads plugins when it starts, and sclang
-- only finds the engine class when it starts, so a new plugin or a first
-- install means restarting SuperCollider. The banks ship in data/banks, so
-- nothing downloads.

local install = {}

local EXT = "/home/we/.local/share/SuperCollider/Extensions/acidulous/"
local PLUGIN = "Acidulous.so"

install.status = nil -- text for the screen while something is happening

local function sh(cmd)
  return os.execute(cmd) == true
end

local function lines(cmd)
  local out = {}
  local h = io.popen(cmd)
  if not h then return out end
  for l in h:lines() do out[#out + 1] = l end
  h:close()
  return out
end

-- Other script folders with the engine class in them. sclang compiles every
-- .sc file in the scripts, so a second copy (an old one from development, say)
-- is a duplicate class and the engine won't load.
-- Paths are compared with repeated slashes made one, as norns.state.path
-- ends in "//" when the script starts with norns.
local function other_copies()
  local mine = (norns.state.path .. "lib/Engine_Acidulous.sc"):gsub("/+", "/")
  local found = {}
  for _, f in ipairs(lines("find " .. _path.code .. " -name Engine_Acidulous.sc 2>/dev/null")) do
    if f:gsub("/+", "/") ~= mine then found[#found + 1] = f end
  end
  return found
end

-- Copies the plugin into Extensions if it's missing or changed, and removes
-- anything else in that folder, such as .sc files copied there by hand.
-- Returns true if anything changed.
local function copy_plugin()
  local src = norns.state.path .. "bin/" .. PLUGIN
  local changed = false
  for _, name in ipairs(lines("ls -A " .. EXT .. " 2>/dev/null")) do
    if name ~= PLUGIN then
      sh("rm -rf '" .. EXT .. name .. "'")
      changed = true
    end
  end
  if not sh("cmp -s '" .. src .. "' " .. EXT .. PLUGIN) then
    sh("mkdir -p " .. EXT .. " && cp '" .. src .. "' " .. EXT .. PLUGIN)
    changed = true
  end
  return changed
end

-- Restarts SuperCollider, waits for the new scsynth, then reloads this
-- script. matron keeps running through this.
local function restart()
  local script = norns.state.script
  sh("sudo systemctl restart norns-sclang.service &")
  clock.run(function()
    -- give the old server time to go, then wait for the new one
    clock.sleep(5)
    for _ = 1, 60 do
      if sh("pgrep -x scsynth > /dev/null") then break end
      clock.sleep(1)
    end
    clock.sleep(4)
    norns.script.load(script)
  end)
end

-- A restart that didn't bring the engine in leaves a marker, so the script
-- shows an error instead of restarting forever.
local function marker() return norns.state.data .. "restarted" end

-- Returns true if the script can start now. Otherwise it's restarting
-- SuperCollider, or install.status says what's wrong.
function install.check()
  local others = other_copies()
  if #others > 0 then
    local dir = others[1]:sub(#_path.code + 1):match("^[^/]+") or others[1]
    install.status = "remove the other copy, code/" .. dir
    print("acidulous: another copy of the engine is in " .. others[1] .. ", remove it and restart norns")
    return false
  end
  local changed = copy_plugin()
  local have_engine = tab.contains(engine.names or {}, "Acidulous")
  if not changed and have_engine then
    os.remove(marker())
    return true
  end
  if not changed and util.file_exists(marker()) then
    os.remove(marker())
    install.status = "engine didn't load, restart norns"
    return false
  end
  if not changed then
    sh("mkdir -p '" .. norns.state.data .. "' && touch '" .. marker() .. "'")
  end
  install.status = "restarting SuperCollider"
  print("acidulous: installing the plugin and restarting SuperCollider")
  clock.run(function()
    clock.sleep(1)
    restart()
  end)
  return false
end

return install
