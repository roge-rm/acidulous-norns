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
}

-- The machine's macros with their slots: a list of {label, slot, def}, slot
-- nil if the machine has no param of that name.
function M.resolve(machine, defs)
  local out = {}
  local list = M.table[machine] or {}
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
