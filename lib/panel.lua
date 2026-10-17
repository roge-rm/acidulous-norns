-- A part's machine page as the app's cards. lib/cards.lua (made by
-- tools/gen-cards.py) names each card's params, matched here to the part's
-- slots by their /defs names. A machine with no cards there gets one card of
-- its macros and then its params four to a card. The part's insert effects
-- follow as cards of their own (fx_cards).

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

-- The cards with a patch's macro names (parts.list[i].macro_names): a mac1..
-- mac8 knob takes its name cut to 6 letters, or goes when it has none, and a
-- "macro N" card is titled with its name. Unnamed macros keep their card, so
-- their routes can still be set.
local function named(cards, names)
  local out = {}
  for _, card in ipairs(cards) do
    local list = {}
    for _, ctl in ipairs(card.controls) do
      local n = ctl.p and tonumber(ctl.p:match("^mac(%d)$"))
      if not n then
        list[#list + 1] = ctl
      elseif (names[n] or "") ~= "" then
        local c = {}
        for k, v in pairs(ctl) do c[k] = v end
        c.label, c.cut = names[n]:sub(1, (utf8.offset(names[n], 7) or #names[n] + 1) - 1), true
        list[#list + 1] = c
      end
    end
    local n = tonumber(card.title:match("^macro (%d)$"))
    local title = n and (names[n] or "") ~= "" and names[n]:lower() or card.title
    if #list > 0 then out[#out + 1] = {title = title, controls = list, env = card.env} end
  end
  return out
end

-- The machine's cards for this pad: a list of {title, controls, env}, env set on the amp envelope card, each
-- control {label, kind, slot, def, opts, p}, or {label = "pad", kind = "pad"}
-- first on a card of per-pad params. names: the patch's macro names, or nil.
function M.cards(machine, defs, pad, names)
  pad = pad or 0
  local key = machine .. ":" .. pad
  if names then
    local nkey = key .. ":" .. table.concat(names, "|")
    if not cache[nkey] then cache[nkey] = named(M.cards(machine, defs, pad), names) end
    return cache[nkey]
  end
  if cache[key] then return cache[key] end
  defs = defs or {}
  local by = slots(defs)
  local cards = {}
  for _, card in ipairs(generated[machine] or {}) do
    local list, padded = {}, false
    for _, c in ipairs(card.c) do
      local slot = by[fill(c.p, pad)]
      if slot then
        list[#list + 1] = {label = c.l, kind = c.k, slot = slot, def = defs[slot], opts = c.o, p = c.p}
        if c.p:find("{pad", 1, true) then padded = true end
      end
    end
    if padded then table.insert(list, 1, {label = "pad", kind = "pad"}) end
    if #list > 0 then cards[#cards + 1] = {title = card.t, controls = list, env = card.e} end
  end
  if #cards == 0 then cards = fallback(machine, defs) end
  cache[key] = cards
  return cards
end

-- Option names for effects' stepped params, by effect and param, used when
-- the count matches the param's steps. A stepped 0-1 param with 2 steps is
-- off and on.
local LFO_RATES = {"1/32", "1/16t", "1/16", "1/8t", "1/16d", "1/8", "1/4t", "1/8d", "1/4",
  "1/2t", "1/4d", "1/2", "1/2d", "1 bar", "2 bars", "4 bars", "8 bars"}
local FX_OPTS = {
  Delay = {time = {"1/32", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "1 bar"}},
  Distortion = {mode = {"soft", "hard", "fold", "tube"}},
  Compressor = {pumprate = {"1/16", "1/8", "1/4", "1/2"}},
  Filter = {mode = {"lp", "bp", "hp"}, lforate = LFO_RATES},
  Phaser = {rate = LFO_RATES, stages = {"2", "4", "6", "8"}},
  Flanger = {rate = LFO_RATES},
  Chorus = {rate = LFO_RATES, voices = {"2", "3", "4"}},
  Tremolo = {rate = LFO_RATES, shape = {"sine", "tri", "square"}},
  Rotary = {speed = {"brake", "slow", "fast"}},
  Amp = {stack = {"us", "uk", "modern"}},
  Horn = {kind = {"brass", "clarinet", "oboe", "flute"}},
  Mouth = {rate = LFO_RATES, move = {"lfo", "level", "key"}},
  Acid = {mode = {"lp", "bp"}},
  Slicer = {rate = {"1/4", "1/8", "1/16", "1/32"}},
  Magneto = {mode = {"sp", "lp2", "lp4", "hq", "xlp"}},
}

local KNOBS = 4 -- per effect card, as many as fit

-- Cards for the part's inserts, after the machine's: fx is parts.list[i].fx,
-- slot (0 or 1) -> {type, params}. Each control has fx (the slot) and index
-- (the param) instead of a machine slot. An effect with more params than a
-- card holds gets numbered cards.
function M.fx_cards(fx)
  if not fx then return {} end
  if fx.cards then return fx.cards end
  local cards = {}
  for slot = 0, 1 do
    local e = fx[slot]
    if e and #e.params > 0 then
      local title = e.type:lower()
      local opts = FX_OPTS[e.type] or {}
      local split = #e.params > KNOBS
      for j = 1, #e.params, KNOBS do
        local list = {}
        for k = j, math.min(#e.params, j + KNOBS - 1) do
          local d = e.params[k]
          local o = opts[d.name]
          if o and #o ~= d.steps then o = nil end
          if not o and d.curve == "step" and d.steps == 2 and d.min == 0 and d.max == 1 then o = {"off", "on"} end
          list[#list + 1] = {label = d.name, kind = o and "step" or "knob", fx = slot, index = k, def = d, opts = o}
        end
        cards[#cards + 1] = {title = split and (title .. " " .. (j + KNOBS - 1) // KNOBS) or title, controls = list}
      end
    end
  end
  fx.cards = cards
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
