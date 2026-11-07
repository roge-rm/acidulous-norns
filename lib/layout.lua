-- Moves a part's saved machine params from a PSET written with the engine at
-- 5a75dc18 (layout 1) or 8d1ab328 (layout 2) onto the engine now (layout 3).
-- No param moved: the new ones all come after the old, and a few old ones
-- changed range or what they do. A PSET saves the layout in "param_layout";
-- one without it is 1.

local L = {}

L.NOW = 3

-- Each machine's param count in layout 1. The params after these are new.
L.COUNTS_1 = {
  Reflux = 14, Trinity = 253, Ratio = 179, Manual = 169, Cumulus = 63,
  Formulate = 36, Pollen = 51, Brazen = 40, Timber = 43, Cipher = 92,
  Filament = 87, Nexus = 195, Hexbeat = 37, Genesis = 49, Resonance = 118,
  Forage = 198, Dice = 104, Mosaic = 95, Molt = 22, Diction = 57, Bias = 28,
  Hammer = 58, Tongue = 32, Draw = 27, Fret = 25, Tine = 19, Sympath = 21,
  Palm = 83, Chanter = 18, Aviary = 83, Fathom = 82,
}

-- Each machine's param count in layout 2, for the ones that got params after
-- the macros, and the steps of their macro destinations (macN_dK) in layouts
-- 2 and 3.
L.COUNTS_2 = {Tine = 92, Palm = 156, Chanter = 91, Fathom = 154, Sympath = 93}
L.DESTS_2 = {Tine = 17, Palm = 15, Chanter = 14, Fathom = 14}
L.DESTS_3 = {Tine = 18, Palm = 17, Chanter = 17, Fathom = 15}

local function clamp01(v)
  return v < 0 and 0 or (v > 1 and 1 or v)
end

-- A stepped 0-1 value saved over `before` steps, the same step over `now`.
local function restep(v, before, now)
  return math.floor(clamp01(v) * (before - 1) + 0.5) / (now - 1)
end

-- Filament's sympathetic strings are 15 times louder, so a level saved
-- before is scaled by sqrt(0.2 / 3), as the engine's filament/SymLevel.h does.
local SYM = math.sqrt(0.2 / 3)
-- Filament's matrix destinations, and the strings' place among them.
local FIL_DESTS, FIL_SYMPATHETIC = 18, 12

-- The fixes per machine, each given get(name) and set(name, v) on 0-1 values.
local FIX = {
  -- tension, clang and duplex did nothing before; now they ring at any value
  -- above 0
  Hammer = function(get, set)
    set("tension", 0)
    set("clang", 0)
    set("duplex", 0)
  end,
  -- pattern and each pad's song went from 6 patterns to 8 (coo and caw)
  Aviary = function(get, set)
    set("pattern", restep(get("pattern"), 6, 8))
    for p = 1, 16 do
      local name = string.format("p%02d_song", p)
      set(name, restep(get(name), 6, 8))
    end
  end,
  -- express got a 4th option (pedal). On the mod wheel, wheel down was a
  -- quarter of the level and is now full, so the volume is quartered.
  Manual = function(get, set)
    local mode = math.floor(clamp01(get("express")) * 2 + 0.5)
    set("express", mode / 3)
    if mode == 1 then set("volume", clamp01(get("volume")) * 0.25) end
  end,
  -- symlevel and matrix depths aimed at the strings scaled as above;
  -- bodydamp's top went from 1 to 29/27
  Filament = function(get, set)
    set("symlevel", clamp01(get("symlevel")) * SYM)
    set("bodydamp", clamp01(get("bodydamp")) * 27 / 29)
    for m = 1, 8 do
      local dst = math.floor(clamp01(get("m" .. m .. "_dst")) * (FIL_DESTS - 1) + 0.5)
      if dst == FIL_SYMPATHETIC then
        local amt = clamp01(get("m" .. m .. "_amt"))
        set("m" .. m .. "_amt", ((amt * 2 - 1) * SYM + 1) / 2)
      end
    end
  end,
}

-- Moves values (slot j -> 0-1 value, for every one of defs) of a machine
-- from layout `from` in place. defs are the machine's defs now, unmap(d, x)
-- maps a default to 0-1. The new params take their defaults, which play as
-- before. Returns true when the machine has new macros, which a part with a
-- patch should take from it.
function L.migrate(machine, values, defs, unmap, from)
  from = from or 1
  local macros = false
  if from >= 2 then
    local before = L.COUNTS_2[machine]
    if not before then return false end
    for j = before + 1, #defs do
      values[j] = unmap(defs[j], defs[j].default)
    end
    -- more macro destinations: the same destination, so a new step
    local was, now = L.DESTS_2[machine], L.DESTS_3[machine]
    if was then
      for j = 1, math.min(before, #defs) do
        if defs[j].name:match("^mac[1-8]_d[1-4]$") then
          values[j] = restep(values[j], was, now)
        end
      end
    end
    return false
  end
  local before = L.COUNTS_1[machine]
  if not before then return false end
  for j = before + 1, #defs do
    local d = defs[j]
    values[j] = unmap(d, d.default)
    if d.name:match("^mac%d") then macros = true end
  end
  local fix = FIX[machine]
  if fix then
    local at = {}
    for j = 1, before do
      if defs[j] then at[defs[j].name] = j end
    end
    fix(function(name) return values[at[name]] end, function(name, v) values[at[name]] = v end)
  end
  return macros
end

return L
