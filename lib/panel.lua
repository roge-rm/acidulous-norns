-- A part's machine page as the app's cards. lib/cards.lua (made by
-- tools/gen-cards.py) names each card's params, matched here to the part's
-- slots by their /defs names. A machine with no cards there gets one card of
-- its macros and then its params four to a card.

local generated = include("lib/cards")
local macros = include("lib/macros")

local M = {}

local cache = {} -- machine .. ":" .. pad -> cards

-- p with {pad} or {pad+1} filled in as two digits.
local function fill(p, pad)
  return (p:gsub("{pad([+-]?%d*)}", function(off)
    return string.format("%02d", pad + (tonumber(off) or 0))
  end))
end

local function slots(defs)
  local by = {}
  for j, d in ipairs(defs) do by[d.name] = j end
  return by
end

-- The pads a machine's cards can show: a list of pad numbers (0 based) for
-- which its first per-pad param exists, or nil if it has none.
local pads_cache = {}
function M.pads(machine, defs)
  if pads_cache[machine] ~= nil then return pads_cache[machine] or nil end
  local by, first = slots(defs or {}), nil
  for _, card in ipairs(generated[machine] or {}) do
    for _, c in ipairs(card.c) do
      if c.p:find("{pad", 1, true) then first = c.p; break end
    end
    if first then break end
  end
  local list = nil
  if first then
    list = {}
    for pad = 0, 31 do
      if by[fill(first, pad)] then list[#list + 1] = pad end
    end
    if #list == 0 then list = nil end
  end
  pads_cache[machine] = list or false
  return list
end

local function fallback(machine, defs)
  local cards = {}
  local mine = {}
  for _, mac in ipairs(macros.resolve(machine, defs)) do
    if mac.slot then mine[#mine + 1] = {label = mac.label, kind = "knob", slot = mac.slot, def = mac.def} end
  end
  if #mine > 0 then cards[1] = {title = "macros", controls = mine} end
  for j = 1, #defs, 4 do
    local list = {}
    for k = j, math.min(#defs, j + 3) do
      local d = defs[k]
      list[#list + 1] = {label = d.name:sub(1, 6), kind = "knob", slot = k, def = d}
    end
    cards[#cards + 1] = {title = "params " .. (j + 3) // 4, controls = list}
  end
  return cards
end

-- The machine's cards for this pad: a list of {title, controls}, each
-- control {label, kind, slot, def, opts}, or {label = "pad", kind = "pad"}
-- first on a card of per-pad params.
function M.cards(machine, defs, pad)
  pad = pad or 0
  local key = machine .. ":" .. pad
  if cache[key] then return cache[key] end
  defs = defs or {}
  local by = slots(defs)
  local cards = {}
  for _, card in ipairs(generated[machine] or {}) do
    local list, padded = {}, false
    for _, c in ipairs(card.c) do
      local slot = by[fill(c.p, pad)]
      if slot then
        list[#list + 1] = {label = c.l, kind = c.k, slot = slot, def = defs[slot], opts = c.o}
        if c.p:find("{pad", 1, true) then padded = true end
      end
    end
    if padded then table.insert(list, 1, {label = "pad", kind = "pad"}) end
    if #list > 0 then cards[#cards + 1] = {title = card.t, controls = list} end
  end
  if #cards == 0 then cards = fallback(machine, defs) end
  cache[key] = cards
  return cards
end

-- Params in lib/cards.lua these machines don't have, as "machine: param".
function M.check(machines, defs)
  local bad = {}
  for _, machine in ipairs(machines) do
    local by = slots(defs[machine] or {})
    local pads = M.pads(machine, defs[machine])
    for _, card in ipairs(generated[machine] or {}) do
      for _, c in ipairs(card.c) do
        if not by[fill(c.p, pads and pads[1] or 0)] then bad[#bad + 1] = machine .. ": " .. c.p end
      end
    end
    if not generated[machine] then bad[#bad + 1] = machine .. ": no cards, macros and params instead" end
  end
  return bad
end

-- How many positions a stepped control has, 0 for a smooth one.
function M.positions(ctl)
  local d = ctl.def
  if d and d.curve == "step" and d.steps > 1 then return d.steps end
  if ctl.kind == "onoff" then return 2 end
  if (ctl.kind == "switch" or ctl.kind == "step") and ctl.opts then return #ctl.opts end
  return 0
end

-- The option label for a stepped control at v, or nil.
function M.option(ctl, v)
  local n = M.positions(ctl)
  if n < 2 or not ctl.opts then return nil end
  return ctl.opts[math.floor(v * (n - 1) + 0.5) + 1]
end

return M
