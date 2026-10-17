-- The machine picker and the patch list, each opened on a part and closed
-- with K2. E1 changes tab (group or family), E2 moves, K3 mounts the machine
-- or loads the patch.

local machines = include("lib/machines")

local L = {}

local open = nil -- {kind = "machines"|"patches", part, tabs, tab, cursor, top}

-- A bank's patches by family, read from its file once: name -> family, and
-- the families in the order they first appear. A patch without one goes
-- under "other", last.
local families = {}
local function bank_families(machine)
  if families[machine] then return families[machine] end
  local of, order, seen = {}, {}, {}
  local f = io.open(norns.state.path .. "data/banks/" .. machine .. ".bank")
  if f then
    for line in f:lines() do
      local rest = line:match("^%s*patch%s+(.*)$")
      if rest then
        local name, attrs = rest:match('^"([^"]*)"(.*)$')
        if not name then name, attrs = rest:match("^(%S+)(.*)$") end
        local fam = attrs and attrs:match("family=(%S+)") or ""
        if name then
          of[name] = fam
          if fam ~= "" and not seen[fam] then
            seen[fam] = true
            order[#order + 1] = fam
          end
        end
      end
    end
    f:close()
  end
  families[machine] = {of = of, order = order}
  return families[machine]
end

-- The patch list's tabs: {name, patches = {{name, index}}} with index into
-- parts.patches[machine]. One "patches" tab if the bank has no families.
local function patch_tabs(parts, machine)
  local names = parts.patches[machine] or {}
  local fam = bank_families(machine)
  if #fam.order == 0 then
    local all = {}
    for k, n in ipairs(names) do all[k] = {name = n, index = k} end
    return {{name = "patches", patches = all}}
  end
  local tabs, at = {}, {}
  for _, f in ipairs(fam.order) do
    tabs[#tabs + 1] = {name = f, patches = {}}
    at[f] = tabs[#tabs]
  end
  for k, n in ipairs(names) do
    local f = fam.of[n] or ""
    if not at[f] then
      tabs[#tabs + 1] = {name = "other", patches = {}}
      at[f] = tabs[#tabs]
    end
    table.insert(at[f].patches, {name = n, index = k})
  end
  return tabs
end

-- The picker's tabs, with only the machines the plugin has.
local function machine_tabs(parts)
  local tabs = {}
  for _, g in ipairs(machines.groups) do
    local list = {}
    for _, m in ipairs(g.machines) do
      if parts.machine_info(m) then list[#list + 1] = {name = m, about = machines.about[m] or ""} end
    end
    if #list > 0 then tabs[#tabs + 1] = {name = g.name, icon = g.icon, machines = list} end
  end
  return tabs
end

-- Puts the cursor on `name` if a tab has it.
local function find(tabs, key, name)
  for t, tab in ipairs(tabs) do
    for k, row in ipairs(tab[key]) do
      if row.name == name then return t, k end
    end
  end
  return 1, 1
end

-- A patch list closes if its part's machine changes, from the OMX-27 say.
function L.active(parts)
  if open and open.kind == "patches" and parts.list[open.part].machine ~= open.machine then open = nil end
  return open ~= nil
end
function L.close() open = nil end

function L.open_machines(parts, i)
  local tabs = machine_tabs(parts)
  if #tabs == 0 then return false end
  local t, k = find(tabs, "machines", parts.list[i].machine)
  open = {kind = "machines", part = i, tabs = tabs, tab = t, cursor = k, top = 1}
  return true
end

function L.open_patches(parts, i)
  local machine = parts.list[i].machine
  if not machine or #(parts.patches[machine] or {}) == 0 then return false end
  local tabs = patch_tabs(parts, machine)
  local t, k = find(tabs, "patches", parts.list[i].patch)
  open = {kind = "patches", part = i, machine = machine, tabs = tabs, tab = t, cursor = k, top = 1}
  return true
end

local function rows()
  local tab = open.tabs[open.tab]
  return open.kind == "machines" and tab.machines or tab.patches
end

function L.enc(parts, n, d)
  if n == 1 then
    local t = util.clamp(open.tab + d, 1, #open.tabs)
    if t ~= open.tab then open.tab, open.cursor, open.top = t, 1, 1 end
  elseif n == 2 then
    open.cursor = util.clamp(open.cursor + d, 1, #rows())
  end
end

-- K3 mounts the machine and closes, or loads the patch and stays open, so
-- you can play through a family. K3 on the part's current patch loads it
-- again with its own values.
function L.key(parts, n, z)
  if z ~= 1 then return end
  if n == 2 then
    open = nil
  elseif n == 3 then
    local row = rows()[open.cursor]
    if not row then return end
    local i = open.part
    if open.kind == "machines" then
      for k, m in ipairs(parts.machines) do
        if m.name == row.name then params:set(parts.id(i, "machine"), k + 1) end
      end
      open = nil
    elseif parts.list[i].machine == open.machine then
      if row.name == parts.list[i].patch then
        parts.reset_patch(i)
        return
      end
      params:set(parts.id(i, "patch"), row.index)
    else
      open = nil -- the machine changed under the list
    end
  end
end

-- What the view draws: {kind, part, tabs, tab, cursor, top, rows, current,
-- count}. top is kept so the cursor shows in the rows the view has room for.
L.SHOWN = {machines = 5, patches = 6}
function L.screen(parts)
  local list = rows()
  local shown = L.SHOWN[open.kind]
  open.cursor = util.clamp(open.cursor, 1, math.max(1, #list))
  if open.cursor < open.top then open.top = open.cursor end
  if open.cursor > open.top + shown - 1 then open.top = open.cursor - shown + 1 end
  open.top = util.clamp(open.top, 1, math.max(1, #list - shown + 1))
  local part = parts.list[open.part]
  local s = {kind = open.kind, part = open.part, tabs = open.tabs, tab = open.tab,
    cursor = open.cursor, top = open.top, rows = list}
  if open.kind == "machines" then
    s.current = part.machine
  else
    s.current = part.patch
    s.machine = open.machine
    local row = list[open.cursor]
    s.count = string.format("%d/%d", row and row.index or 0, #(parts.patches[open.machine] or {}))
  end
  return s
end

return L
