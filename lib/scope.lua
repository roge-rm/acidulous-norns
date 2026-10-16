-- The Nexus scope: the part's patch as a chain of modules, and the output of
-- the one E2 picks, from the plugin's /graph and /scope.

local timer = include("lib/timer")

local S = {}

-- {part, mods = {{slot, type}...} once /graph is in, at, trace, pending}
local sc = nil

function S.active() return sc ~= nil end
function S.screen() return sc end

-- Asks for the patch's modules. False if the part isn't a Nexus.
function S.open(parts, i)
  if parts.list[i].machine ~= "Nexus" then return false end
  sc = {part = i}
  parts.send("/graph", {i - 1})
  return true
end

function S.close(parts)
  if not sc then return end
  timer.cancel(sc.pending)
  parts.send("/scope", {sc.part - 1, -1})
  sc = nil
end

-- Points the scope at the module at `at`. Each move mounts the patch again,
-- so a turn through the chain only sends the one it stops on.
local function point(parts, wait)
  timer.cancel(sc.pending)
  local i, slot = sc.part, sc.mods[sc.at].slot
  sc.pending = timer.after(wait, function()
    if sc and sc.part == i then
      sc.pending = nil
      parts.send("/scope", {i - 1, slot})
    end
  end)
end

function S.enc(parts, n, d)
  if n ~= 2 or not sc.mods or #sc.mods == 0 then return end
  local at = util.clamp(sc.at + d, 1, #sc.mods)
  if at == sc.at then return end
  sc.at = at
  sc.trace = nil
  point(parts, 0.15)
end

-- Closes when the part stops being a Nexus.
function S.check(parts)
  if sc and parts.list[sc.part].machine ~= "Nexus" then S.close(parts) end
end

-- After the plugin starts again, the modules are asked for again.
function S.resend(parts)
  if sc then parts.send("/graph", {sc.part - 1}) end
end

function S.osc(parts, path, args)
  if not sc or args[1] ~= sc.part - 1 then return end
  if path == "/graph" then
    -- a new patch: the same slot if it has one, otherwise the last module
    local keep = sc.mods and sc.at and sc.mods[sc.at] and sc.mods[sc.at].slot
    local mods = {}
    for k = 3, #args - 1, 2 do mods[#mods + 1] = {slot = args[k], type = args[k + 1]} end
    sc.mods, sc.at, sc.trace = mods, nil, nil
    for k, m in ipairs(mods) do
      if m.slot == keep then sc.at = k end
    end
    if not sc.at and #mods > 0 then sc.at = #mods end
    if sc.at then point(parts, 0.01) end
  elseif path == "/scope" then
    -- only the framed module's, not the last one's while E2 moves
    local m = sc.mods and sc.at and sc.mods[sc.at]
    if not m or args[2] ~= m.slot then return end
    local trace = {}
    for k = 3, #args do trace[k - 2] = args[k] end
    sc.trace = trace
  end
end

return S
