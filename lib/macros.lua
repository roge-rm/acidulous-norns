-- Five macros per machine: the params the macro page shows, and later the
-- OMX-27's five pots. Each names a param by its ParamDef name, with a label
-- of up to 6 characters for the screen.

local M = {}

M.COUNT = 5

local function m(...)
  local list = {}
  for k, pair in ipairs({...}) do
    list[k] = {param = pair[1], label = pair[2]}
  end
  return list
end

M.table = {
  Reflux = m({"cutoff", "cutoff"}, {"resonance", "reso"}, {"envmod", "envmod"}, {"decay", "decay"}, {"accent", "accent"}),
  Hexbeat = m({"kick_tune", "kick"}, {"kick_decay", "k dec"}, {"snare_snappy", "snappy"}, {"hat_tune", "hat"}, {"accent", "accent"}),
  Trinity = m({"f1_freq", "cutoff"}, {"f1_res", "reso"}, {"f1_env", "envamt"}, {"f_decay", "f dec"}, {"a_release", "rel"}),
  Ratio = m({"morph", "morph"}, {"skew", "skew"}, {"o1_fb", "fdbk"}, {"f_freq", "cutoff"}, {"f_res", "reso"}),
  Formulate = m({"duty", "duty"}, {"bits", "bits"}, {"crush", "crush"}, {"a", "a"}, {"cutoff", "cutoff"}),
  Genesis = m({"comp", "comp"}, {"duck", "duck"}, {"drive", "drive"}, {"drift", "drift"}, {"kickdecay", "k dec"}),
  Tine = m({"mallet", "mallet"}, {"position", "pos"}, {"decay", "decay"}, {"bright", "bright"}, {"damp", "damp"}),
  Palm = m({"position", "pos"}, {"hand", "hand"}, {"decay", "decay"}, {"damp", "damp"}, {"body", "body"}),
  Tongue = m({"mouth", "mouth"}, {"ring", "ring"}, {"pluck", "pluck"}, {"focus", "focus"}, {"breath", "breath"}),
  Timber = m({"pressure", "press"}, {"breath", "breath"}, {"embouchure", "lips"}, {"vibrato", "vib"}, {"cutoff", "cutoff"}),
  Aviary = m({"rate", "rate"}, {"length", "length"}, {"sweep", "sweep"}, {"rasp", "rasp"}, {"breath", "breath"}),
  Fret = m({"tone", "tone"}, {"position", "pos"}, {"mute", "mute"}, {"drive", "drive"}, {"feedback", "fdbk"}),
  Chanter = m({"drones", "drones"}, {"reed", "reed"}, {"grace", "grace"}, {"air", "air"}, {"drift", "drift"}),
  Brazen = m({"size", "size"}, {"bell", "bell"}, {"brass", "brass"}, {"pressure", "press"}, {"vibrato", "vib"}),
  Fathom = m({"density", "dense"}, {"size", "size"}, {"decay", "decay"}, {"tone", "tone"}, {"gust", "gust"}),
  Resonance = m({"p00_tune", "1 tune"}, {"p00_decay", "1 dec"}, {"coupling", "couple"}, {"humanise", "human"}, {"accent", "accent"}),
  Draw = m({"pressure", "press"}, {"attack", "attack"}, {"release", "rel"}, {"air", "air"}, {"vibrato", "vib"}),
  Sympath = m({"bridge", "bridge"}, {"curve", "curve"}, {"sustain", "sus"}, {"bright", "bright"}, {"tarbs", "tarbs"}),
  Filament = m({"position", "pos"}, {"pressure", "press"}, {"tone", "tone"}, {"sustain", "sus"}, {"bodymix", "body"}),
  Manual = m({"ua_16", "16'"}, {"ua_8", "8'"}, {"perclvl", "perc"}, {"vibdepth", "vib"}, {"drive", "drive"}),
  Hammer = m({"hardness", "hard"}, {"tone", "tone"}, {"sustain", "sus"}, {"felt", "felt"}, {"lid", "lid"}),
  Cipher = m({"shift", "shift"}, {"stretch", "strtch"}, {"remapamt", "remap"}, {"smear", "smear"}, {"wet", "wet"}),
  -- None of the spectrum params, which rebuild the cloud and cut held notes.
  Cumulus = m({"morph", "morph"}, {"shimmer", "shimmr"}, {"cutoff", "cutoff"}, {"resonance", "reso"}, {"amprelease", "rel"}),
  -- Until a patch is loaded: a Nexus patch's macros come from the plugin
  -- (/macros), the knobs the patch sets and then its modules' knobs.
  Nexus = m({"morph", "morph"}, {"drive", "drive"}, {"glide", "glide"}, {"velocity", "vel"}, {"volume", "volume"}),
  Diction = m({"vowel", "vowel"}, {"formant", "throat"}, {"effort", "effort"}, {"breath", "breath"}, {"vibrato", "vib"}),
  -- Forage has no kit-wide tone params, only per pad: kick, snare and closed hat.
  Forage = m({"p00_pitch", "k tune"}, {"p00_decay", "k dec"}, {"p02_decay", "s dec"}, {"p07_decay", "h dec"}, {"accent", "accent"}),
  Mosaic = m({"start", "start"}, {"gpos", "gpos"}, {"f_freq", "cutoff"}, {"f_res", "reso"}, {"a_release", "rel"}),
  Dice = m({"swap", "swap"}, {"reverse", "rev"}, {"stutter", "stuttr"}, {"drop", "drop"}, {"jump", "jump"}),
  Pollen = m({"position", "pos"}, {"size", "size"}, {"density", "dense"}, {"spread", "spread"}, {"bloom", "bloom"}),
  Molt = m({"tune", "tune"}, {"rate", "rate"}, {"formant", "formnt"}, {"robot", "robot"}, {"amprelease", "rel"}),
}

-- The machine's macros with their slots: a list of {label, slot, def}, slot
-- nil if the machine has no param of that name. patch_list, if given, is the
-- patch's own list of {param, label}, filled up from the machine's.
function M.resolve(machine, defs, patch_list)
  local out = {}
  local list = M.table[machine] or {}
  if patch_list then
    local merged, seen = {}, {}
    for _, mac in ipairs(patch_list) do
      if #merged < M.COUNT then merged[#merged + 1] = mac; seen[mac.param] = true end
    end
    for _, mac in ipairs(list) do
      if #merged < M.COUNT and not seen[mac.param] then merged[#merged + 1] = mac end
    end
    list = merged
  end
  for k = 1, M.COUNT do
    local mac = list[k]
    local entry = {label = mac and mac.label or "-"}
    if mac then
      for j, d in ipairs(defs or {}) do
        if d.name == mac.param then entry.slot, entry.def = j, d; break end
      end
    end
    out[k] = entry
  end
  return out
end

-- Problems with the table for these machines: a list of strings, empty if none.
-- defs: machine -> list of {name, ...}
function M.check(machines, defs)
  local bad = {}
  for _, machine in ipairs(machines) do
    local list = M.table[machine]
    if not list or #list ~= M.COUNT then
      bad[#bad + 1] = machine .. ": needs " .. M.COUNT .. " macros"
    end
    local seen = {}
    for k, entry in ipairs(M.resolve(machine, defs[machine])) do
      local name = list and list[k] and list[k].param or "?"
      if not entry.slot then bad[#bad + 1] = machine .. ": no param " .. name end
      if seen[name] then bad[#bad + 1] = machine .. ": " .. name .. " twice" end
      seen[name] = true
      if #entry.label > 6 then bad[#bad + 1] = machine .. ": label " .. entry.label .. " too long" end
    end
  end
  return bad
end

return M
