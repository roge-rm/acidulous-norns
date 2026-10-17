-- The machine picker and the patch list, each opened on a part and closed
-- with K2. E1 changes tab (group or family), E2 moves, K3 mounts the machine
-- or loads the patch. In the patch list K1+K3 saves the part's sound, or
-- deletes a saved one.

local machines = include("lib/machines")
local textentry = require("textentry")

local L = {}

local open = nil -- {kind = "machines"|"patches", part, tabs, tab, cursor, top, confirm}
local shown_note = nil -- {text, at}: what the last save or delete did

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
-- parts.patches[machine]. One "patches" tab if the bank has no families. A
-- "saved" tab comes first when the machine has saved presets, its rows
-- {name, saved} with saved from parts.saved.
local function patch_tabs(parts, machine)
  local names = parts.patches[machine] or {}
  local fam = bank_families(machine)
  local tabs, at = {}, {}
  local saved = parts.saved[machine] or {}
  if #saved > 0 then
    local rows = {}
    for k, sv in ipairs(saved) do rows[k] = {name = sv.name, saved = sv} end
    tabs[1] = {name = "saved", patches = rows}
  end
  if #fam.order == 0 then
    local all = {}
    for k, n in ipairs(names) do all[k] = {name = n, index = k} end
    if #all > 0 then tabs[#tabs + 1] = {name = "patches", patches = all} end
    return tabs
  end
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

-- Whether a patch list row is what the part plays: its saved preset, or its
-- bank patch when it has none.
local function is_current(part, row)
  local pr = part.preset
  if row.saved then return pr ~= nil and pr.path == row.saved.path and pr.index == row.saved.index end
  return pr == nil and row.name == part.patch
end

local function find_current(tabs, part)
  for t, tab in ipairs(tabs) do
    for k, row in ipairs(tab.patches) do
      if is_current(part, row) then return t, k end
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
  if not machine then return false end
  -- a file copied in shows up once the plugin has looked again
  parts.scan_presets(machine)
  local tabs = patch_tabs(parts, machine)
  if #tabs == 0 then return false end
  local t, k = find_current(tabs, parts.list[i])
  open = {kind = "patches", part = i, machine = machine, tabs = tabs, tab = t, cursor = k, top = 1}
  shown_note = nil
  return true
end

-- The machine's saved presets changed: the open list takes them, keeping the
-- cursor on the row it was on.
function L.refresh(parts, machine)
  if not open or open.kind ~= "patches" or open.machine ~= machine then return end
  local tab = open.tabs[open.tab]
  local row = tab and tab.patches[open.cursor]
  local tabs = patch_tabs(parts, machine)
  if #tabs == 0 then
    open = nil
    return
  end
  local t, k, found = 1, 1, false
  if row then
    for tt, tb in ipairs(tabs) do
      for kk, r in ipairs(tb.patches) do
        local same = row.saved and r.saved and r.saved.path == row.saved.path and r.saved.index == row.saved.index
        if not found and (same or (not row.saved and not r.saved and r.name == row.name)) then
          t, k, found = tt, kk, true
        end
      end
    end
    -- a deleted row: stay where it was on the saved tab
    if not found and row.saved and tabs[1].name == "saved" then k = open.cursor end
  end
  -- after a save, on the preset it made
  if open.follow then t, k = find_current(tabs, parts.list[open.part]) end
  open.follow = nil
  open.tabs, open.tab, open.cursor = tabs, t, k
end

function L.note(text) shown_note = {text = text, at = util.time()} end

local function rows()
  local tab = open.tabs[open.tab]
  return open.kind == "machines" and tab.machines or tab.patches
end

function L.enc(parts, n, d)
  if open.confirm then return end
  if n == 1 then
    local t = util.clamp(open.tab + d, 1, #open.tabs)
    if t ~= open.tab then open.tab, open.cursor, open.top = t, 1, 1 end
  elseif n == 2 then
    open.cursor = util.clamp(open.cursor + d, 1, #rows())
  end
end

-- Saves part i's sound as a preset called name, asking first if that
-- replaces a file.
local function save_as(parts, i, name)
  name = name and name:gsub("^%s+", ""):gsub("%s+$", "") or ""
  if name == "" or not open then return end
  local path = parts.preset_dir(open.machine) .. parts.preset_file(name)
  local function save()
    parts.save_preset(i, path, name)
    if open then open.follow = true end
  end
  if util.file_exists(path) then
    open.confirm = {text = "replace " .. parts.preset_file(name):gsub("%.acpreset$", "") .. "?", yes = save}
  else
    save()
  end
end

-- K1+K3: saves the part's sound under a name typed in, starting from its
-- patch's, or on a saved row deletes it after asking.
local function save_or_delete(parts, row)
  local i = open.part
  if row and row.saved then
    local sv = row.saved
    open.confirm = {text = "delete " .. sv.name .. "?", yes = function() parts.delete_preset(sv) end}
    return
  end
  if L.before_text then L.before_text() end
  local machine = open.machine
  textentry.enter(function(name)
    if open and open.machine == machine and parts.list[i].machine == machine then save_as(parts, i, name) end
  end, parts.list[i].patch or "", "save " .. machine:lower() .. " as")
end

-- K3 mounts the machine and closes, or loads the patch and stays open, so
-- you can play through a family. K3 on the part's current patch loads it
-- again with its own values. While asking, K3 is yes and K2 no.
function L.key(parts, n, z, k1)
  if z ~= 1 then return end
  if open.confirm then
    local c = open.confirm
    open.confirm = nil
    if n == 3 then c.yes() end
    return
  end
  if n == 2 then
    open = nil
  elseif n == 3 then
    local row = rows()[open.cursor]
    local i = open.part
    if open.kind == "patches" and k1 then
      if parts.list[i].machine == open.machine then save_or_delete(parts, row) end
      return
    end
    if not row then return end
    if open.kind == "machines" then
      for k, m in ipairs(parts.machines) do
        if m.name == row.name then params:set(parts.id(i, "machine"), k + 1) end
      end
      open = nil
    elseif parts.list[i].machine == open.machine then
      if is_current(parts.list[i], row) then
        parts.reset_patch(i)
      elseif row.saved then
        parts.load_preset(i, row.saved)
      else
        params:set(parts.id(i, "patch"), row.index)
      end
    else
      open = nil -- the machine changed under the list
    end
  end
end

-- What the view draws: {kind, part, tabs, tab, cursor, top, rows, current,
-- count, hint, confirm}. top is kept so the cursor shows in the rows the view
-- has room for. A patch row's current is set when the part plays it.
L.SHOWN = {machines = 5, patches = 6}
function L.screen(parts, k1)
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
    for _, row in ipairs(list) do row.current = is_current(part, row) end
    local row = list[open.cursor]
    if row and row.saved then
      s.count = string.format("%d/%d", open.cursor, #list)
    else
      s.count = string.format("%d/%d", row and row.index or 0, #(parts.patches[open.machine] or {}))
    end
    if shown_note and util.time() - shown_note.at < 3 then s.count = shown_note.text end
    if k1 then
      s.hint = row and row.saved and "delete" or "save"
    else
      s.hint = row and row.current and "reset" or "k3"
    end
    s.confirm = open.confirm and open.confirm.text
  end
  return s
end

return L
