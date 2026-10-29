-- A part's machine page as the app's cards. lib/cards.lua (made by
-- tools/gen-cards.py) names each card's params, matched here to the part's
-- slots by their /defs names. A machine with no cards there gets one card of
-- its macros and then its params four to a card. The part's insert effects
-- follow as cards of their own (fx_cards), then its modifiers (mod_cards).

local generated = include("lib/cards")
local modcards = include("lib/modcards")
local fxcards = include("lib/fxcards")
local macros = include("lib/macros")

local M = {}

local TYPES = {"Scale", "Chord", "Arp"} -- as parts.MOD_TYPES

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

-- The inserts' cards as the app lays them out (lib/fxcards.lua, made by
-- tools/gen-cards.py): an effect's groups, or its params four to a card in
-- engine order, then its mod card. A stepped 0-1 param with 2 steps is off
-- and on.
local KNOBS = 4 -- per effect card without groups, as many as fit
local SLOTS = 4 -- a part's insert slots; 0-1 hold the patch's, 2-3 the user's
local USER_FIRST = 2

-- A control for an insert's param d (a def from /inserts) in slot.
local function fx_control(type, slot, d, label)
  local o = (fxcards.effects[type] and fxcards.effects[type].o or {})[d.name]
  if o and #o ~= d.steps then o = nil end
  if not o and d.curve == "step" and d.steps == 2 and d.min == 0 and d.max == 1 then o = {"off", "on"} end
  local kind = "knob"
  if o and #o == 2 and o[1] == "off" then kind = "onoff"
  elseif o and #o <= 3 then kind = "switch"
  elseif o then kind = "step" end
  return {label = label or d.name, kind = kind, fx = slot, name = d.name, def = d, opts = o}
end

-- The reverb's model, by name, from the slot's model param.
local function reverb_model(e)
  local d = e.by.model
  if not d then return nil end
  local n = #fxcards.reverb_models
  return fxcards.reverb_models[util.clamp(math.floor(d.value * (n - 1) + 0.5), 0, n - 1) + 1]
end

-- Whether a reverb insert shows this param on its model: the shared ones,
-- less size on space (sized in metres), and the model's own.
local own_knobs = nil
local function reverb_shows(model, name)
  if not own_knobs then
    own_knobs = {}
    for _, list in pairs(fxcards.reverb_knobs) do
      for _, n in ipairs(list) do own_knobs[n] = true end
    end
  end
  if name == "size" then return model ~= "space" end
  if own_knobs[name] then return tab.contains(fxcards.reverb_knobs[model] or {}, name) end
  return true
end

-- The effect's cards: its groups (the reverb's model knobs after the
-- first), then a "more" card for anything not in them.
local function effect_cards(e, slot)
  local type = e.type
  local title = type:lower()
  local model = type == "Reverb" and reverb_model(e) or nil
  local shown = {}
  for _, d in ipairs(e.params) do
    if not model or reverb_shows(model, d.name) then shown[#shown + 1] = d end
  end
  local groups = fxcards.effects[type] and fxcards.effects[type].g
  local cards = {}
  if not groups then
    local split = #shown > KNOBS
    for j = 1, #shown, KNOBS do
      local list = {}
      for k = j, math.min(#shown, j + KNOBS - 1) do list[#list + 1] = fx_control(type, slot, shown[k]) end
      cards[#cards + 1] = {title = split and (title .. " " .. (j + KNOBS - 1) // KNOBS) or title, controls = list}
    end
    return cards
  end
  if model then
    groups = {groups[1], {model, fxcards.reverb_knobs[model] or {}}, table.unpack(groups, 2)}
  end
  local by, taken = {}, {}
  for _, d in ipairs(shown) do by[d.name] = d end
  local function add(t, names)
    local list = {}
    for _, n in ipairs(names) do
      if by[n] and not taken[n] then
        taken[n] = true
        list[#list + 1] = fx_control(type, slot, by[n])
      end
    end
    if #list > 0 then cards[#cards + 1] = {title = title .. " " .. t, controls = list} end
  end
  for _, g in ipairs(groups) do add(g[1], g[2]) end
  local rest = {}
  for _, d in ipairs(shown) do
    if not taken[d.name] then rest[#rest + 1] = d.name end
  end
  add("more", rest)
  return cards
end

-- A mod setting with words for its steps.
local function mod_choice(e, slot, name, label, opts)
  local d = e.mods_by[name]
  if not d then return nil end
  local kind = #opts <= 2 and "switch" or "step"
  return {label = label, kind = kind, fx = slot, name = name, def = d, opts = opts}
end

-- The insert's mod card, as the app's: the source, the controls it uses,
-- then two of the effect's params it moves, each with a depth either way.
-- Steps get a card of their own, a knob per step.
local function mod_cards(e, slot)
  local m = e.mods_by
  if not m.mod_source then return {} end
  local title = e.type:lower() .. " mod"
  local list = {}
  local function add(c) if c then list[#list + 1] = c end end
  local sources = fxcards.mod.sources
  add(mod_choice(e, slot, "mod_source", "source", sources))
  local source = math.floor(m.mod_source.value * (#sources - 1) + 0.5)
  if source == 0 then return {{title = title, controls = list}} end
  local cards = {}
  local length = 16
  if source == 1 then
    add(mod_choice(e, slot, "mod_shape", "shape", fxcards.mod.shapes))
    add(mod_choice(e, slot, "mod_rate", "rate", fxcards.mod.rates))
  elseif source == 2 then
    add(mod_choice(e, slot, "mod_rate", "rate", fxcards.mod.rates))
    add(mod_choice(e, slot, "mod_length", "length", {"8", "16"}))
    length = m.mod_length and m.mod_length.value >= 0.5 and 16 or 8
  else
    if source == 4 then
      local tracks = {}
      for k = 1, 16 do tracks[k] = tostring(k) end
      tracks[17] = "none"
      add(mod_choice(e, slot, "mod_key", "track", tracks))
    end
    if m.mod_attack then add(fx_control(e.type, slot, m.mod_attack, "attack")) end
    if m.mod_release then add(fx_control(e.type, slot, m.mod_release, "release")) end
  end
  -- the effect's params a mod can move: all but the sidechain, which picks a track
  local targets, labels = {-1}, {"none"}
  for k, d in ipairs(e.params) do
    if d.name ~= "sidechain" and k <= 64 then
      targets[#targets + 1] = k - 1
      labels[#labels + 1] = d.name
    end
  end
  for j = 1, 2 do
    local t, depth = m["mod_target" .. j], m["mod_depth" .. j]
    if t then
      local def = {name = t.name, min = 0, max = 1, default = 0, unit = "", steps = #labels, curve = "step"}
      list[#list + 1] = {label = "target " .. j, kind = "step", fx = slot, name = t.name, def = def,
        opts = labels, targets = targets}
    end
    if depth then add(fx_control(e.type, slot, depth, "depth " .. j)) end
  end
  cards[1] = {title = title, controls = list}
  if source == 2 then
    local steps = {}
    for k = 1, length do
      local d = m["mod_step" .. k]
      if d then steps[#steps + 1] = fx_control(e.type, slot, d, tostring(k)) end
    end
    if #steps > 0 then cards[2] = {title = e.type:lower() .. " steps", controls = steps} end
  end
  return cards
end

-- What the cards depend on, so they're made again when it changes.
local function fx_key(fx, picks)
  local key = {}
  for slot = 0, SLOTS - 1 do
    local e = fx[slot]
    if e then
      local m = e.mods_by or {}
      local function at(d) return d and tostring(math.floor(d.value * 64 + 0.5)) or "" end
      key[#key + 1] = table.concat({e.type, at(e.by and e.by.model), at(m.mod_source), at(m.mod_length)}, ":")
    else
      key[#key + 1] = ""
    end
  end
  if picks then key[#key + 1] = #picks end
  return table.concat(key, "|")
end

-- Cards for the part's inserts, after the machine's: fx is parts.list[i].fx,
-- slot (0 to 3) -> {type, params, mods}. picks: the effects a script part
-- can put in slots 3-4 (parts.effects), or nil for a song's track, whose
-- inserts are the song's. Then an "inserts" card picks those, followed by
-- each slot's cards. Each control has fx (the slot) and name (the param),
-- or what = "type" on the picker.
function M.fx_cards(fx, picks)
  if not fx then return {} end
  local key = fx_key(fx, picks)
  if fx.cards and fx.cards_key == key then return fx.cards end
  local cards = {}
  if picks and #picks > 0 then
    local opts = {"none"}
    for k, t in ipairs(picks) do opts[k + 1] = t:lower() end
    local def = {name = "type", min = 0, max = 1, default = 0, unit = "", steps = #opts, curve = "step"}
    local list = {}
    for slot = USER_FIRST, SLOTS - 1 do
      list[#list + 1] = {label = "fx " .. (slot + 1), kind = "step", fx = slot, what = "type", def = def, opts = opts}
    end
    cards[1] = {title = "inserts", controls = list}
  end
  for slot = 0, SLOTS - 1 do
    local e = fx[slot]
    if e and #e.params > 0 then
      e.by, e.mods_by = e.by or {}, e.mods_by or {}
      for _, c in ipairs(effect_cards(e, slot)) do cards[#cards + 1] = c end
      for _, c in ipairs(mod_cards(e, slot)) do cards[#cards + 1] = c end
    end
  end
  fx.cards, fx.cards_key = cards, key
  return cards
end

-- The part's modifier cards, after the inserts': a "mods" card with each
-- slot's type and on, then each modifier's cards from lib/modcards.lua.
-- mods is parts.list[i].mods, defs parts.mod_defs. Each control has mod (the
-- slot) and what ("type", "on") or p (the param) instead of a machine slot.
local TYPE_DEF = {name = "type", min = 0, max = 3, default = 0, unit = "", steps = 4, curve = "step"}
local mod_cache = {}
local MOD_SLOTS = 3 -- as parts.MOD_SLOTS

function M.mod_cards(mods, defs)
  local types = {}
  for slot = 0, MOD_SLOTS - 1 do types[#types + 1] = mods[slot] and mods[slot].type or "" end
  local key = table.concat(types, "|")
  for _, t in ipairs(types) do
    if t ~= "" and not defs[t] then key = nil end -- not cached until its defs come
  end
  if key and mod_cache[key] then return mod_cache[key] end
  local opts = {"none"}
  for k, t in ipairs(TYPES) do opts[k + 1] = t:lower() end
  local first = {}
  for slot = 0, MOD_SLOTS - 1 do
    first[#first + 1] = {label = "mod " .. (slot + 1), kind = "step", mod = slot, what = "type", def = TYPE_DEF, opts = opts}
    first[#first + 1] = {label = "on", kind = "onoff", mod = slot, what = "on"}
  end
  local cards = {{title = "mods", controls = first}}
  for slot = 0, MOD_SLOTS - 1 do
    local m = mods[slot]
    local by = m and defs[m.type] and defs[m.type].by
    for _, card in ipairs(by and modcards[m.type] or {}) do
      local list = {}
      for _, c in ipairs(card.c) do
        if by[c.p] then
          list[#list + 1] = {label = c.l, kind = c.k, mod = slot, p = c.p, def = by[c.p], opts = c.o}
        end
      end
      if #list > 0 then cards[#cards + 1] = {title = card.t, controls = list} end
    end
  end
  if key then mod_cache[key] = cards end
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
