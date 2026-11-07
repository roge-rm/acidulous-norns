local here = arg[0]:match("^(.*)/[^/]+$")
local which = arg[2]
local CASES = {"1", "2", "3", "4", "5", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21"}
if not which then
  -- every case, each in a fresh run
  local failed = 0
  for _, c in ipairs(CASES) do
    print("-- case " .. c)
    local ok = os.execute(string.format("lua5.4 %q %q %s", arg[0], arg[1] or ".", c))
    if not ok then failed = failed + 1 end
  end
  print(failed == 0 and "all cases ok" or (failed .. " case(s) failed"))
  os.exit(failed == 0 and 0 or 1)
end
local P, S = dofile(here .. "/harness.lua")

local function readback(path, tracks)
  for _, t in ipairs(tracks) do
    P.osc("/mounted", {t[1], t[2]})
    local v = {t[1], t[2], t[3]}
    for j = 1, #P.defs[t[2]] do v[#v + 1] = t.vals or 0.1 * j end
    P.osc("/values", v)
    P.osc("/sends", {t[1], 0.2, 0.1})
    P.osc("/strip", {t[1], 1, 0.5, 0, 0})
    P.osc("/inserts", {t[1]})
    P.osc("/mods", {t[1]})
  end
  P.osc("/song_sends", {0.38, 0.5})
  P.osc("/song_master", {0.5})
  local out = {path, "/d/acidulous/songs/" .. path, "", 0, #tracks}
  for _, t in ipairs(tracks) do
    out[#out + 1] = t[1]; out[#out + 1] = t[2]; out[#out + 1] = t[3]; out[#out + 1] = t[4]
  end
  P.osc("/song_loaded", out)
end
local function refuse(path) P.osc("/song_loaded", {path, "", "no such file", 0, 0}) end
local A = {{0, "Reflux", "Squelch", "bass"}, {1, "Hexbeat", "Custom", "drums"}}
local B = {{0, "Reflux", "Squelch", "acid"}, {2, "Bare", "", "pad"}}

if which == "1" then
  -- a patch turned just before a song takes the racks
  params:set("p1_patch", 3)
  S.load("A")
  SENT_CLEAR()
  ADVANCE(0.5)
  CHECK(not FIND("/patch"), "the patch timer firing while held sends no /patch")
  CHECK(P.own[1].owed and P.own[1].fresh, "and marks part 1 owed and fresh")
  -- the machine with no patches: the /mount branch
  params:set("p2_machine", 4)
  S.load("A")
  ADVANCE(0.5)
  CHECK(P.own[2].owed and P.own[2].fresh, "a machine timer firing while held marks part 2 owed and fresh")
  SENT_CLEAR()
  refuse("A")
  CHECK(FIND("/patch", function(a) return a[1] == 0 and a[3] == "Third" end) ~= nil, "after a refused load part 1 gets /patch Third")
  CHECK(FIND("/mount", function(a) return a[1] == 1 and a[2] == "Bare" end) ~= nil, "and part 2 gets /mount Bare")
  CHECK(not P.own[1].owed and not P.own[2].owed, "and nothing is owed")
  -- frozen clips: the param is sent, and a part with no machine shows frozen
  SENT_CLEAR()
  params:set("song_frozen", 3)
  S.load("F")
  CHECK(FIND("/song_load", function(a) return a[1] == "F" and a[3] == "frozen" end) ~= nil, "/song_load carries the frozen clips mode")
  P.osc("/song_frozen", {2, 0, 0, "lead", 1, 1, "bass"})
  readback("F", {{1, "Reflux", "Squelch", "bass"}})
  CHECK(P.song[1].frozen and not P.song[1].machine and P.song[1].name == "lead", "a frozen part with no machine is marked and named")
  CHECK(P.song[2].frozen and P.song[2].machine == "Reflux", "a frozen part with a machine is marked")
  CHECK(not P.song[3].frozen, "other parts aren't")
  params:set("song_frozen", 1)
  S.load("G")
  CHECK(FIND("/song_load", function(a) return a[1] == "G" and a[3] == "auto" end) ~= nil, "auto is sent by default")
  readback("G", {{0, "Reflux", "Squelch", "bass"}})
  CHECK(not P.song[1].frozen and not P.song[2].frozen, "a song with no frozen parts marks none")
elseif which == "2" then
  -- inserts, modifiers and files changed during a song-page load from no song
  local drive = {name = "drive", value = 0.2}
  P.own[1].fx = {[0] = {type = "Distortion", params = {drive}, by = {drive = drive}, mods = {}, mods_by = {}}}
  S.load("A")
  CHECK(P.song_holds and P.song_racks and not P.song_view, "holds and racks, the script's parts shown")
  SENT_CLEAR()
  P.set_fx(1, {fx = 0, name = "drive"}, 0.9)
  P.set_mod_type(2, 0, "Arp", false)
  P.clear_files(3)
  CHECK(#SENT == 0 or not (FIND("/fx") or FIND("/mod") or FIND("/load")), "nothing sent while held")
  CHECK(P.own[1].owed and P.own[2].owed and P.own[3].owed, "parts 1, 2 and 3 marked owed")
  SENT_CLEAR()
  refuse("A")
  CHECK(FIND("/fx", function(a) return a[1] == 0 and a[3] == "drive" and math.abs(a[4] - 0.9) < 1e-6 end) ~= nil,
    "the refused load sends part 1's /fx drive 0.9")
  CHECK(FIND("/mod", function(a) return a[1] == 1 and a[2] == 0 and a[3] == "Arp" end) ~= nil, "and part 2's /mod Arp")
  CHECK(FIND("/load", function(a) return a[1] == 2 and a[2] == "clear" end) ~= nil, "and part 3's /load clear")
elseif which == "3" then
  S.load("A")
  readback("A", A)
  CHECK(P.song_view and P.part(1).name == "bass" and P.part(1).patch == "Squelch", "A's tracks shown")
  SENT_CLEAR()
  P.set(1, "s1", 0.77)
  CHECK(FIND("/param", function(a) return a[1] == 0 and math.abs(a[3] - 0.77) < 1e-6 end) ~= nil, "a slot edit goes to A's rack")
  -- B starts loading: A's tracks go
  S.load("B")
  CHECK(not P.song_view and not P.part(1).song, "loading B shows the script's parts")
  SENT_CLEAR()
  P.set(1, "s1", 0.3)
  CHECK(not FIND("/param"), "an edit then isn't sent")
  CHECK(P.own[1].owed, "and is owed to the script's part")
  readback("B", B)
  CHECK(P.song_view and P.part(1).name == "acid", "B's tracks shown")
  CHECK(math.abs(P.get(1, "s1") - 0.1) < 1e-6, "B's track 1 has B's value, not A's edit (" .. P.get(1, "s1") .. ")")
  -- switching patch and back on B's track 1 recalls nothing of A's
  P.set(1, "patch", 1)
  CHECK(math.abs(P.get(1, "s1") - 0.5) < 1e-6, "patch Classic on B's track starts at defaults (" .. P.get(1, "s1") .. ")")
  -- B refused while A is loaded: A's tracks come back
  S.load("A")
  readback("A", A)
  P.set(1, "s2", 0.66)
  S.load("C")
  CHECK(not P.song_view, "loading C hides A")
  refuse("C")
  CHECK(P.song_view and P.part(1).name == "bass" and math.abs(P.get(1, "s2") - 0.66) < 1e-6, "C refused: A's tracks are back with the edit")
  -- the same song again keeps its tracks, edits blocked until the read-back
  S.load("A")
  CHECK(P.song_view and P.part(1).song, "loading A again keeps A shown")
  SENT_CLEAR()
  P.set(1, "s1", 0.9)
  P.delta(1, "volume", 3)
  CHECK(#SENT == 0, "edits during A's reload aren't sent (" .. #SENT .. " sent)")
  readback("A", A)
  SENT_CLEAR()
  P.set(1, "s1", 0.9)
  CHECK(FIND("/param") ~= nil, "after the read-back edits go again")
elseif which == "4" then
  S.load("A")
  readback("A", A)
  CHECK(P.get(2, "patch") == 0 and P.part(2).patch == "Custom", "track 2's song patch isn't in the bank: index 0, name Custom")
  SENT_CLEAR()
  P.delta(2, "patch", -1)
  CHECK(P.get(2, "patch") == 2, "turning left goes to the last patch (" .. P.get(2, "patch") .. ")")
  S.load("A")
  readback("A", A)
  P.delta(2, "patch", 1)
  CHECK(P.get(2, "patch") == 1, "turning right goes to the first patch (" .. P.get(2, "patch") .. ")")
  ADVANCE(0.5)
  CHECK(FIND("/patch", function(a) return a[1] == 1 and a[3] == "Init" end) ~= nil, "and loads Init")
elseif which == "5" then
  -- a PSET's song still waiting when the plugin says /hello
  params:set("song", "A", true)
  CHECK(S.read() == false, "the PSET's song is on its way")
  ADVANCE(0.1)
  SENT_CLEAR()
  P.osc("/hello", {})
  ADVANCE(1)
  CHECK(FIND("/song_load", function(a) return a[1] == "A" end) ~= nil, "A is still loaded after the /hello")
  -- and a later PSET without a song cancels it
  params:set("song", "A", true)
  S.read()
  params:set("song", "", true)
  S.read()
  SENT_CLEAR()
  ADVANCE(1)
  CHECK(not FIND("/song_load"), "a PSET without a song cancels it")
elseif which == "7" then
  local size, fdbk = params:get("rev_size"), params:get("dly_feedback")
  CHECK(P.out_send("rev_size") == size, "no song: the script's size")
  S.load("A")
  readback("A", A)
  CHECK(math.abs(P.out_send("rev_size") - 0.38) < 1e-6 and math.abs(P.out_send("dly_feedback") - 0.475) < 1e-6,
    "song: its size 0.38 and feedback 0.475")
  SENT_CLEAR()
  P.delta_out_send("rev_size", 5)
  P.delta_out_send("dly_feedback", -10)
  CHECK(FIND("/send", function(a) return a[1] == 0 and a[2] == "size" and math.abs(a[3] - 0.43) < 1e-6 end) ~= nil,
    "size +5 sends the song's 0.43")
  CHECK(FIND("/send", function(a) return a[1] == 1 and a[2] == "feedback" and math.abs(a[3] - 0.375 / 0.95) < 1e-6 end) ~= nil,
    "feedback -10 sends 0.375 in the param's units")
  CHECK(params:get("rev_size") == size and params:get("dly_feedback") == fdbk and not P.owed_master, "the script's params untouched")
  P.osc("/song_sends", {-1, 0.5})
  CHECK(P.out_send("rev_size") == nil, "a song without a reverb shows none")
  P.osc("/song_unloaded", {1})
  CHECK(P.out_send("rev_size") == size, "after unload the script's again")
elseif which == "8" then
  -- the "fx" param over 4 slots: the user's types, edits and mod params
  P.effects = {"Delay", "Reverb", "Chorus"}
  P.osc("/inserts", {0})
  SENT_CLEAR()
  P.set_fx(1, {fx = 2, what = "type"}, 1 / 3)
  P.set_fx(1, {fx = 2, what = "type"}, 0)
  P.set_fx(1, {fx = 2, what = "type"}, 3 / 3)
  CHECK(not FIND("/insert"), "turning through the effects sends nothing yet")
  ADVANCE(0.5)
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 2 and a[3] == "Chorus" end) ~= nil,
    "the one it stops on, Chorus, goes to slot 2 (fx 3)")
  CHECK(P.own[1].fx_wait == 1, "and its /inserts reply is waited for")
  local delay = {2, "Chorus", 1, "rate", 0, 1, 0, "", 17, "step", 0.5, 2,
    "mod_source", 0, 1, 0, "", 5, "step", 0, "mod_target1", 0, 1, 0, "", 65, "step", 0}
  local reply = {0}
  for _, v in ipairs(delay) do reply[#reply + 1] = v end
  P.osc("/inserts", reply)
  CHECK(P.own[1].fx_wait == 0 and P.own[1].fx[2] and P.own[1].fx[2].type == "Chorus", "the reply shows Chorus")
  P.set_fx(1, {fx = 2, name = "rate"}, 0.25)
  P.set_fx(1, {fx = 2, name = "mod_source"}, 0.25)
  P.set_fx(1, {fx = 2, name = "mod_target1", targets = {-1, 0}}, 1)
  CHECK(FIND("/fx", function(a) return a[2] == 2 and a[3] == "mod_target1" and math.abs(a[4] - 1 / 64) < 1e-6 end) ~= nil,
    "a mod target goes as (index + 1)/64")
  local saved = params:get("p1_fx")
  CHECK(saved == "2=Chorus|2,mod_source,0.2500|2,mod_target1,0.0156|2,rate,0.2500", "saved as " .. saved)
  local edits, inserts = P.decode_fx(saved)
  CHECK(inserts[2] == "Chorus" and inserts[3] == nil and math.abs(edits["2:rate"] - 0.25) < 1e-6
    and math.abs(edits["2:mod_source"] - 0.25) < 1e-6, "and read back")
  CHECK(P.encode_fx(edits, inserts) == saved, "the same again")
  -- an old PSET: slots 0-1 only, the Delay's pingpong
  edits, inserts = P.decode_fx("0,drive,0.2000|1,pingpong,1.0000|0,pingpong,0.0000")
  CHECK(next(inserts) == nil, "an old fx param has slots 3-4 empty")
  CHECK(math.abs(edits["1:mode"] - 1 / 3) < 1e-6 and edits["0:mode"] == 0 and not edits["1:pingpong"]
    and math.abs(edits["0:drive"] - 0.2) < 1e-6, "pingpong on is mode ping-pong, off normal")
  -- a new patch keeps the user's slot and its edits
  P.set(1, "patch", 2)
  ADVANCE(0.5)
  CHECK(P.decode_fx(params:get("p1_fx"))["2:rate"] ~= nil, "a new patch keeps slot 3's edits")
  CHECK(not FIND("/insert", function(a) return a[3] == "" end), "and doesn't empty it")
elseif which == "9" then
  -- a song takes the racks and gives them back: the user's slots go again
  P.effects = {"Delay", "Reverb", "Chorus"}
  P.set_insert(1, 3, "Delay", false)
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 3 and a[3] == "Delay" end) ~= nil, "Delay to slot 3 (fx 4)")
  S.load("A")
  SENT_CLEAR()
  P.set_insert(1, 2, "Reverb", false)
  CHECK(not FIND("/insert"), "a pick while a song loads isn't sent")
  CHECK(P.own[1].owed, "and is owed")
  readback("A", A)
  P.osc("/song_unloaded", {1})
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 3 and a[3] == "Delay" end) ~= nil
    and FIND("/insert", function(a) return a[1] == 0 and a[2] == 2 and a[3] == "Reverb" end) ~= nil,
    "after the unload both go again")
  local fx = FIND("/fx", function(a) return a[1] == 0 end)
  local order = {}
  for k, m in ipairs(SENT) do
    if m.args[1] == 0 then order[m.path] = order[m.path] or k end
  end
  CHECK(order["/patch"] < order["/insert"] and (not fx or order["/insert"] < order["/fx"]), "after the patch, before the edits")
elseif which == "10" then
  -- an old PSET's delay ping pong is the mode
  local function pset(lines)
    local name = os.tmpname()
    local f = io.open(name, "w")
    f:write(table.concat(lines, "\n") .. "\n")
    f:close()
    return name
  end
  for _, c in ipairs({{1, 1, "off: normal"}, {2, 2, "on: ping-pong"}}) do
    local name = pset({'"dly_pingpong": ' .. c[1], '"parts": 1'})
    params:set("dly_mode", 3, true)
    P.after_read(name)
    os.remove(name)
    CHECK(params:get("dly_mode") == c[2], "dly_pingpong " .. c[3] .. " (" .. params:get("dly_mode") .. ")")
  end
  local name = pset({'"dly_pingpong": 2', '"dly_mode": 3', '"parts": 1'})
  params:set("dly_mode", 3, true)
  P.after_read(name)
  os.remove(name)
  CHECK(params:get("dly_mode") == 3, "a PSET with dly_mode keeps it")
  SENT_CLEAR()
  P.send_master()
  CHECK(FIND("/send", function(a) return a[1] == 1 and a[2] == "mode" and math.abs(a[3] - 2 / 3) < 1e-6 end) ~= nil,
    "multi-tap goes as mode 2/3")
  CHECK(not FIND("/send", function(a) return a[2] == "pingpong" end), "and no pingpong")
  params:set("rev_model", 6)
  CHECK(FIND("/send", function(a) return a[1] == 0 and a[2] == "model" and math.abs(a[3] - 5 / 11) < 1e-6 end) ~= nil,
    "rev model space goes as 5/11")
elseif which == "11" then
  -- the insert cards: the picker, an effect's groups, the reverb's model
  -- knobs and the mod card by source
  local panel = include("lib/panel")
  local function def(name, steps, v)
    return {name = name, min = 0, max = 1, default = 0, unit = "", steps = steps or 0,
      curve = steps and "step" or "lin", value = v or 0}
  end
  local function slot(type, names, mods)
    local e = {type = type, params = {}, mods = {}, by = {}, mods_by = {}}
    for _, d in ipairs(names) do e.params[#e.params + 1] = d; e.by[d.name] = d end
    for _, d in ipairs(mods) do e.mods[#e.mods + 1] = d; e.mods_by[d.name] = d end
    return e
  end
  local function mods(source)
    local list = {def("mod_source", 5, source / 4), def("mod_shape", 7), def("mod_rate", 17), def("mod_length", 2, 0),
      def("mod_attack"), def("mod_release"), def("mod_key", 17), def("mod_target1", 65), def("mod_depth1"),
      def("mod_target2", 65), def("mod_depth2")}
    for k = 1, 16 do list[#list + 1] = def("mod_step" .. k) end
    list[9].min = -1
    return list
  end
  local reverb = {def("size"), def("damp"), def("model", 12, 5 / 11), def("width"), def("drip"), def("gain")}
  local fx = {[0] = slot("Reverb", reverb, mods(0)), [2] = slot("Delay", {def("time", 8), def("mode", 4), def("taps", 3)}, mods(2))}
  local cards = panel.fx_cards(fx, {"Delay", "Reverb"})
  local titles = {}
  for _, c in ipairs(cards) do titles[#titles + 1] = c.title end
  local got = table.concat(titles, ",")
  CHECK(got == "inserts,reverb tank,reverb space,reverb tail,reverb mod,delay echo,delay mode,delay mod,delay steps",
    "cards " .. got)
  CHECK(#cards[1].controls == 2 and cards[1].controls[2].label == "fx 4" and #cards[1].controls[1].opts == 3,
    "the picker has fx 3 and fx 4, none and 2 effects")
  local tank = {}
  for _, c in ipairs(cards[2].controls) do tank[#tank + 1] = c.name end
  CHECK(table.concat(tank, ",") == "model,damp", "space has no size (" .. table.concat(tank, ",") .. ")")
  CHECK(cards[2].controls[1].opts[6] == "space", "the model names its step")
  local m = {}
  for _, c in ipairs(cards[8].controls) do m[#m + 1] = c.label end
  CHECK(table.concat(m, ",") == "source,rate,length,target 1,depth 1,target 2,depth 2", "steps mod: " .. table.concat(m, ","))
  CHECK(#cards[9].controls == 8, "8 steps at length 8")
  CHECK(#cards[8].controls[4].opts == 4, "targets: none and the delay's 3 params")
  P.own[1].fx = fx
  P.set_fx(1, cards[8].controls[4], 1)
  CHECK(math.abs(fx[2].mods_by.mod_target1.value - 3 / 64) < 1e-6, "the last target is the third param, 3/64")
  CHECK(P.fx_value(1, cards[8].controls[4]) == 1, "and reads back at the end")
  fx[2].mods_by.mod_source.value = 0.25
  local again = panel.fx_cards(fx, {"Delay", "Reverb"})
  CHECK(again[#again].title == "delay mod" and again[#again].controls[2].label == "shape", "an LFO mod has its shape")
  fx[2].mods_by.mod_source.value = 1
  again = panel.fx_cards(fx, nil)
  CHECK(again[1].title ~= "inserts" and again[#again].controls[2].label == "track", "a song's track has no picker; a keyed mod its track")
elseif which == "12" then
  -- an old PSET resets the new send params
  params:set("rev_model", 6, true)
  params:set("dly_taps", 3, true)
  params:set("rev_width", 20, true)
  local name = os.tmpname()
  local f = io.open(name, "w")
  f:write('"parts": 1\n')
  f:close()
  P.after_read(name)
  os.remove(name)
  CHECK(params:get("rev_model") == 1 and params:get("dly_taps") == 2 and params:get("rev_width") == 9,
    "rev model, dly taps and rev width back to their defaults")
  -- picks wait for /effects, and only its names go
  P.osc("/inserts", {0})
  P.own[1].fx_wait = 0
  SENT_CLEAR()
  P.set_insert(1, 2, "Chorus", false)
  CHECK(not FIND("/insert") and P.own[1].fx_wait == 0, "no /insert before /effects")
  P.osc("/effects", {"Delay", "Chorus"})
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 2 and a[3] == "Chorus" end) ~= nil, "Chorus once /effects comes")
  P.osc("/inserts", {0})
  P.own[1].fx_wait = 0
  SENT_CLEAR()
  P.set_insert(1, 3, "Bogus", false)
  CHECK(not FIND("/insert", function(a) return a[3] == "Bogus" end) and P.own[1].fx_wait == 0,
    "an effect the plugin hasn't got isn't sent or waited for")
  -- no machine: the slots are emptied, and go again with the next machine
  SENT_CLEAR()
  params:set("p1_machine", 1)
  ADVANCE(0.5)
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 2 and a[3] == "" end) ~= nil, "no machine empties fx 3")
  SENT_CLEAR()
  P.set_insert(1, 2, "Delay", false)
  CHECK(not FIND("/insert"), "a pick with no machine waits")
  params:set("p1_machine", 2)
  ADVANCE(0.5)
  CHECK(FIND("/insert", function(a) return a[1] == 0 and a[2] == 2 and a[3] == "Delay" end) ~= nil,
    "and goes with the next machine")
elseif which == "13" then
  -- the frozen clips mode loads a loaded song again
  S.load("A")
  readback("A", A)
  SENT_CLEAR()
  params:set("song_frozen", 2)
  CHECK(not FIND("/song_load"), "the reload waits")
  ADVANCE(1)
  CHECK(FIND("/song_load", function(a) return a[1] == "A" and a[3] == "notes" end) ~= nil, "a new mode in the menu loads the song again")
  readback("A", A)
  SENT_CLEAR()
  -- a PSET: same song, another mode
  params:set("song", "A", true)
  params:set("song_frozen", 3, true)
  CHECK(S.read() == false, "the PSET reads")
  CHECK(not FIND("/song_load"), "the reload waits")
  ADVANCE(1)
  local n = 0
  for _, m in ipairs(SENT) do if m.path == "/song_load" then n = n + 1 end end
  CHECK(n == 1 and FIND("/song_load", function(a) return a[1] == "A" and a[3] == "frozen" end) ~= nil,
    "a PSET with the same song and another mode loads it once, in that mode")
  readback("A", A)
  SENT_CLEAR()
  params:set("song", "A", true)
  S.read()
  ADVANCE(1)
  CHECK(not FIND("/song_load"), "and the same mode again loads nothing")
  -- the mode flipped and back before the reload goes loads nothing
  SENT_CLEAR()
  params:set("song_frozen", 1)
  ADVANCE(0.2)
  params:set("song_frozen", 3)
  ADVANCE(1)
  CHECK(not FIND("/song_load"), "a mode flipped and back loads nothing")
  params:set("song_frozen", 2)
  ADVANCE(0.2)
  params:set("song_frozen", 1)
  ADVANCE(1)
  CHECK(FIND("/song_load", function(a) return a[1] == "A" and a[3] == "auto" end) ~= nil,
    "flipped twice to a third mode loads it once")
elseif which == "14" then
  -- a failed patch's values aren't remembered, so picking it again is clean
  local part = P.own[1]
  local n = #P.defs[part.machine]
  local function values(tag, v)
    local a = {0, part.machine, tag}
    for j = 1, n do a[#a + 1] = v end
    return a
  end
  params:set("p1_patch", 2)
  ADVANCE(1)
  local tag2 = part.sent
  P.osc("/values", values(tag2, 0.3))
  params:set("p1_s1", 0.9)
  params:set("p1_patch", 3)
  ADVANCE(1)
  local tag3 = part.sent
  P.osc("/patch_failed", {0, part.machine, tag3})
  P.osc("/values", values(tag3, 0.6))
  CHECK(params:get("p1_s1") == 0.6, "the failed patch shows what the plugin holds")
  params:set("p1_patch", 2)
  ADVANCE(1)
  P.osc("/values", values(tag2, 0.3))
  params:set("p1_patch", 3)
  ADVANCE(1)
  SENT_CLEAR()
  P.osc("/values", values(tag3, 0.2))
  CHECK(params:get("p1_s1") == 0.2 and not part.dim, "picking it again loads it clean, no recalled edits")
  CHECK(not FIND("/param"), "and sends no edits on top")
  params:set("p1_patch", 2)
  ADVANCE(1)
  P.osc("/values", values(tag2, 0.3))
  CHECK(params:get("p1_s1") == 0.3 or params:get("p1_s1") == 0.9, "another patch's own memory is kept")
elseif which == "15" then
  -- Diction's voices and Forage's slices are kept as the part's files and saved
  local part = P.own[1]
  part.machine = "Diction"
  P.osc("/loaded", {0, "voice", "/a/Bo/bank.json", 0, "Bo", "Diction"})
  P.osc("/loaded", {0, "voice", "/a/Cy/bank.json", 1, "Cy", "Diction"})
  CHECK(part.files.voice0 and part.files.voice1 and part.files.voice1.path == "/a/Cy/bank.json",
    "both voices kept")
  CHECK(params:get("p1_files"):find("voice,1,/a/Cy/bank.json", 1, true) ~= nil, "and in the files param")
  SENT_CLEAR()
  P.save_preset(1, "/p/x.acpreset", "X")
  local function has(args, k, v)
    for j = 1, #args - 1 do
      if args[j] == k and args[j + 1] == v then return true end
    end
    return false
  end
  CHECK(FIND("/preset_save", function(a) return has(a, "voice", "/a/Bo/bank.json") and
    has(a, "morphvoice", "/a/Cy/bank.json") end) ~= nil, "saved as voice and morphvoice")
  part.machine = "Forage"
  part.files = {}
  P.osc("/loaded", {0, "slices", "/a/break.wav", 0, "break", "Forage"})
  CHECK(part.files.slices and P.material(1) == "break", "the slices kept and shown")
  SENT_CLEAR()
  P.save_preset(1, "/p/y.acpreset", "Y")
  CHECK(FIND("/preset_save", function(a) return has(a, "slice_sample", "/a/break.wav") end) ~= nil,
    "saved as slice_sample")
elseif which == "16" then
  -- the effect preset list on fx 3: scan, load, another type, save, delete
  local entered
  package.preload.textentry = function() return {enter = function(cb, default, title) entered = {cb = cb, default = default, title = title} end} end
  local L = include("lib/lists")
  P.on_saved_fx = function() L.refresh(P, nil) end
  P.on_preset_note = function(text) L.note(text) end
  local part = P.own[1]
  P.osc("/effects", {"Delay", "Chorus"})
  P.set_insert(1, 2, "Delay")
  P.osc("/inserts", {0})
  part.fx_edits["2:mod_source"] = 0.5
  part.fx_edits["2:time"] = 0.9
  SENT_CLEAR()
  local dir = "/d/acidulous/presets/fx/"
  CHECK(L.open_fx(P, 1, 2) and FIND("/preset_scan", function(a) return a[1] == dir and a[2] == 1 end) ~= nil,
    "opening it scans the fx folder and its folders")
  P.osc("/preset_scan", {dir, "Chorus/wide.acpreset", 0, "Wide", "fx.Chorus", 1, 0,
    "Delay/slap.acpreset", 0, "Slap", "fx.Delay", 1, 1, "Reflux.acpreset", 0, "R", "Reflux", 1, 0})
  local sc = L.screen(P, false)
  CHECK(#P.saved_fx == 2 and P.saved_fx[2].type == "Delay" and P.saved_fx[2].path == dir .. "Delay/slap.acpreset",
    "effect rows kept with their type, a machine's left out")
  CHECK(sc.tabs[1].name == "delay" and #sc.tabs[1].patches == 2 and sc.tabs[2].name == "all" and
    #sc.tabs[2].patches == 2 and sc.title:find("fx 3 delay", 1, true) ~= nil, "a delay tab after new preset, then all")
  CHECK(sc.hint == "k1+k3", "new preset hints k1+k3")
  L.enc(P, 2, 1)
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  local m = FIND("/fx_preset_load")
  CHECK(m and m.args[1] == 0 and m.args[2] == 2 and m.args[3] == dir .. "Delay/slap.acpreset" and m.args[5] == "Slap",
    "K3 loads Slap into fx 3")
  P.osc("/fx_preset_loaded", {0, 2, "Delay", "Slap", "", "time", 0.3, 0.5, "mix", 0.5, 0.5})
  local fx = params:get("p1_fx")
  CHECK(fx:find("2=Delay", 1, true) and fx:find("2,time,0.3000", 1, true) and not fx:find("mix", 1, true) and
    fx:find("2,mod_source,0.5000", 1, true), "the fx param holds its type, its changed params and the slot's mod")
  CHECK(L.screen(P, false).rows[2].current, "and the row shows as current")
  -- a Chorus from the all tab switches the slot
  L.enc(P, 1, 1)
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  CHECK(FIND("/fx_preset_load", function(a) return a[3] == dir .. "Chorus/wide.acpreset" end) ~= nil, "K3 on all loads Wide")
  P.osc("/fx_preset_loaded", {0, 2, "Chorus", "Wide", "", "rate", 0.7, 0.2})
  fx = params:get("p1_fx")
  CHECK(part.inserts[2] == "Chorus" and fx:find("2=Chorus", 1, true) and not fx:find("mod_source", 1, true) and
    not fx:find("time", 1, true), "the slot is a Chorus now, the Delay's edits and mod gone")
  sc = L.screen(P, false)
  CHECK(sc.tabs[1].name == "chorus" and sc.tabs[sc.tab].name == "all" and sc.rows[sc.cursor].name == "Chorus: Wide",
    "the list follows the type and stays on the row")
  -- K1+K3 on new preset: name entry, then /fx_preset_save in fx/Chorus/
  L.enc(P, 1, -1)
  L.enc(P, 2, -5)
  SENT_CLEAR()
  L.key(P, 3, 1, true)
  CHECK(entered and entered.default == "Wide" and entered.title == "save chorus as", "K1+K3 asks a name, from the preset's")
  entered.cb("Mine")
  m = FIND("/fx_preset_save")
  CHECK(m and m.args[2] == 2 and m.args[3] == dir .. "Chorus/Mine.acpreset" and m.args[4] == "Mine", "and saves it")
  SENT_CLEAR()
  P.osc("/fx_preset_saved", {0, 2, dir .. "Chorus/Mine.acpreset", "ok"})
  CHECK(FIND("/preset_scan") ~= nil and part.fx_presets[2].path == dir .. "Chorus/Mine.acpreset", "saved: rescanned, current")
  -- K1+K3 on a saved row asks, then deletes
  L.enc(P, 2, 1)
  L.key(P, 3, 1, true)
  sc = L.screen(P, true)
  CHECK(sc.confirm == "delete Wide?", "K1+K3 on a saved one asks")
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  CHECK(FIND("/preset_delete", function(a) return a[1] == dir .. "Chorus/wide.acpreset" and a[2] == 0 end) ~= nil,
    "and deletes it")
  SENT_CLEAR()
  P.osc("/preset_deleted", {dir .. "Chorus/wide.acpreset", 0, "ok"})
  CHECK(FIND("/preset_scan", function(a) return a[1] == dir end) ~= nil, "then rescans the fx folder")
  L.key(P, 2, 1, false)
  CHECK(not L.active(P), "K2 closes it")
elseif which == "18" then
  -- .acpreset import from the file picker
  local picker = include("lib/picker")
  local lib = picker.lib
  local copies, taken, listing = {}, {}, {}
  util.file_exists = function(p) return taken[p] == true end
  lib.list = function(dir, exts)
    local out = {}
    for _, name in ipairs(listing) do
      if exts[lib.ext(name)] then out[#out + 1] = {name = name, path = dir .. "/" .. name} end
    end
    return out
  end
  local copy_ok = true
  lib.copy_file = function(src, dest, done) copies[#copies + 1] = {src = src, dest = dest, done = done} end
  P.effects = {"Delay", "Chorus"}
  P.MOD_PRESET_TYPES = {Arp = true, Chord = true}
  -- sends the file's scan reply, then finishes the copy
  local function import(file, info, finish)
    local msg = {file, file:match("([^/]+)$"), 0}
    if info then for _, v in ipairs({info.name, info.unit, 1, 0}) do msg[#msg + 1] = v end end
    SENT_CLEAR()
    picker.key(3, 1, true)
    CHECK(FIND("/preset_scan", function(a) return a[1] == file end) ~= nil, "K1+K3 asks the plugin about " .. file)
    copies = {}
    P.osc("/preset_scan", msg)
    if finish and copies[1] then copies[1].done(true, "") end
    return copies[1]
  end
  local function pick_row(name)
    for k, row in ipairs(picker.screen().rows) do if row == name then
      while picker.screen().cursor < k do picker.enc(2, 1) end
      return
    end end
    error("no row " .. name)
  end

  listing = {"bass.acpreset", "kick.wav", "note.txt", "d.acpreset", "e.acpreset", "f.acpreset"}
  P.on_scanned_file = picker.scanned
  CHECK(picker.open(P, 3), "the picker opens on Forage")
  -- the roots: dust/audio. Enter it.
  picker.key(3, 1, false)
  local rows = picker.screen().rows
  CHECK(rows[2] == "bass.acpreset" and rows[3] == "kick.wav" and #rows == 6, "presets list with audio, other files not")
  pick_row("bass.acpreset")
  CHECK(picker.screen().right == "k1+k3 import", "the hint on a preset")
  SENT_CLEAR()
  picker.key(3, 1, false)
  CHECK(FIND("/preset_scan") == nil and FIND("/load") == nil and FIND("/hear") == nil, "K3 alone does nothing to it")

  -- a machine preset into its folder
  local c = import("/a/bass.acpreset", {name = "Bass", unit = "Hexbeat"}, false)
  CHECK(c and c.dest == "/d/acidulous/presets/Hexbeat/bass.acpreset" and c.src == "/a/bass.acpreset",
    "a machine preset is copied to presets/<machine>/")
  CHECK(picker.screen().busy ~= nil, "the screen says it is copying")
  SENT_CLEAR()
  c.done(true, "")
  CHECK(FIND("/preset_scan", function(a) return a[1] == "/d/acidulous/presets/Hexbeat/" end) ~= nil,
    "then the machine's presets are scanned again")
  CHECK(picker.screen().title == "in Hexbeat saved presets" and not picker.screen().busy, "and it says where it went")

  -- a name taken
  taken["/d/acidulous/presets/Hexbeat/bass.acpreset"] = true
  taken["/d/acidulous/presets/Hexbeat/bass 2.acpreset"] = true
  c = import("/a/bass.acpreset", {name = "Bass", unit = "Hexbeat"}, false)
  CHECK(c and c.dest == "/d/acidulous/presets/Hexbeat/bass 3.acpreset", "a name taken gets 3 after 2")
  c.done(true, "")
  CHECK(picker.screen().title:find("bass 3", 1, true) ~= nil, "the new name shows")

  -- an effect and a modifier preset
  c = import("/a/bass.acpreset", {name = "Wide", unit = "fx.Chorus"}, false)
  CHECK(c and c.dest == "/d/acidulous/presets/fx/Chorus/bass.acpreset", "an effect preset goes to presets/fx/<type>/")
  SENT_CLEAR()
  c.done(true, "")
  CHECK(FIND("/preset_scan", function(a) return a[1] == "/d/acidulous/presets/fx/" and a[2] == 1 end) ~= nil,
    "the fx folders are scanned again")
  pick_row("d.acpreset")
  c = import("/a/d.acpreset", {name = "Up", unit = "mod.Arp"}, false)
  CHECK(c and c.dest == "/d/acidulous/presets/mod/Arp/d.acpreset", "a modifier preset goes to presets/mod/<type>/")
  c.done(false, "no space")
  CHECK(picker.screen().title == "copy failed: no space", "a failed copy says so")

  -- refused
  local function refused(file, info, text, what)
    SENT_CLEAR()
    copies = {}
    picker.key(3, 1, true)
    local msg = {file, file:match("([^/]+)$"), 0}
    if info then for _, v in ipairs({info.name, info.unit, 1, 0}) do msg[#msg + 1] = v end end
    P.osc("/preset_scan", info and msg or {file})
    CHECK(#copies == 0 and picker.screen().title == text and not picker.screen().busy, what)
  end
  pick_row("e.acpreset")
  refused("/a/e.acpreset", nil, "not a preset", "an unreadable file is refused")
  refused("/a/e.acpreset", {name = "S", unit = "mod.Scale"}, "no Scale presets", "a Scale preset is refused")
  refused("/a/e.acpreset", {name = "S", unit = "fx.Nope"}, "no Nope here", "an unknown effect is refused")
  refused("/a/e.acpreset", {name = "S", unit = "Nowhere"}, "no Nowhere here", "an unknown machine is refused")
  refused("/a/e.acpreset", {name = "S", unit = "../x"}, "no ../x here", "a path in a unit is refused")

  -- a part with a machine that takes no files lists presets too
  picker.close()
  CHECK(picker.open(P, 1), "the picker opens on Reflux")
  picker.key(3, 1, false)
  CHECK(#picker.screen().rows == 4 and picker.screen().rows[1] == "bass.acpreset", "with presets only")
  picker.close()
elseif which == "17" then
  -- the modifier preset list: scan, a preset of another type put back in
  -- order, the mods param, save, a load into the slot of its type, delete
  local entered
  package.preload.textentry = function() return {enter = function(cb, default, title) entered = {cb = cb, default = default, title = title} end} end
  local L = include("lib/lists")
  P.on_saved_mod = function() L.refresh(P, nil) end
  P.on_preset_note = function(text) L.note(text) end
  local part = P.own[1]
  P.osc("/mod_defs", {"Scale", 2, "key", 0, 11, 0, "", 12, "step", "scale", 0, 32, 0, "", 33, "step"})
  P.osc("/mod_defs", {"Chord", 3, "type", 0, 24, 0, "", 25, "step", "strum", 0, 200, 0, "ms", 0, "lin",
    "latch", 0, 1, 0, "", 2, "step"})
  P.osc("/mod_defs", {"Arp", 2, "rate", 0, 9, 5, "", 10, "step", "gate", 5, 200, 60, "%", 0, "lin"})
  P.set_mod_type(1, 0, "Arp", false)
  P.set_mod_type(1, 1, "Scale", false)
  SENT_CLEAR()
  local dir = "/d/acidulous/presets/mod/"
  CHECK(L.open_mod(P, 1, 1) and FIND("/preset_scan", function(a) return a[1] == dir and a[2] == 1 end) ~= nil,
    "opening it scans the mod folder and its folders")
  P.osc("/preset_scan", {dir, "Arp/up.acpreset", 0, "Up", "mod.Arp", 1, 0,
    "Chord/wide.acpreset", 0, "Wide", "mod.Chord", 1, 0, "x.acpreset", 0, "X", "fx.Delay", 1, 0})
  local sc = L.screen(P, false)
  CHECK(#P.saved_mod == 2 and P.saved_mod[2].type == "Chord" and P.saved_mod[2].path == dir .. "Chord/wide.acpreset",
    "modifier rows kept with their type, an effect's left out")
  CHECK(#sc.tabs == 1 and sc.tabs[1].name == "all" and #sc.rows == 2 and sc.title:find("mod 2 scale", 1, true) ~= nil,
    "a Scale has no presets: just all")
  L.key(P, 3, 1, true)
  CHECK(L.screen(P, true).confirm == "delete Up?", "K1+K3 on a saved row asks to delete")
  L.key(P, 2, 1, false)
  CHECK(L.active(P), "K2 says no and stays open")
  L.enc(P, 2, 1)
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  local m = FIND("/mod_preset_load")
  CHECK(part.mods[0].type == "Chord" and part.mods[1].type == "Arp" and part.mods[2] == nil and
    m and m.args[1] == 0 and m.args[2] == 0 and m.args[3] == dir .. "Chord/wide.acpreset" and m.args[5] == "Wide",
    "a Chord preset on the Scale's slot: a Chord, then put in order before the Arp, and loaded there")
  P.osc("/mod_preset_loaded", {0, 0, "Chord", "Wide", "", "type", 0.5, "strum", 0, "latch", 0})
  local mods = params:get("p1_mods")
  CHECK(mods:find("0,Chord,0,type=0.5000", 1, true) and not mods:find("strum", 1, true) and
    mods:find("1,Arp,0", 1, true), "the mods param holds its type and changed params")
  sc = L.screen(P, false)
  CHECK(sc.title:find("mod 1 chord", 1, true) and sc.tabs[1].name == "chord" and #sc.tabs[1].patches == 2,
    "the list follows to its slot and type")
  CHECK(sc.tabs[sc.tab].name == "all" and sc.rows[sc.cursor].current, "and stays on the row, now current")
  -- K1+K3 on new preset: name entry, then /mod_preset_save in mod/Chord/
  L.enc(P, 1, -1)
  L.enc(P, 2, -5)
  SENT_CLEAR()
  L.key(P, 3, 1, true)
  CHECK(entered and entered.default == "Wide" and entered.title == "save chord as", "K1+K3 asks a name, from the preset's")
  entered.cb("Mine")
  m = FIND("/mod_preset_save")
  CHECK(m and m.args[2] == 0 and m.args[3] == dir .. "Chord/Mine.acpreset" and m.args[4] == "Mine", "and saves it")
  SENT_CLEAR()
  P.osc("/mod_preset_saved", {0, 0, dir .. "Chord/Mine.acpreset", "ok"})
  CHECK(FIND("/preset_scan") ~= nil and part.mod_presets[0].path == dir .. "Chord/Mine.acpreset",
    "saved: rescanned, current")
  -- an Arp preset goes to the Arp's slot, the Chord stays
  L.enc(P, 1, 1)
  L.enc(P, 2, -5)
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  m = FIND("/mod_preset_load")
  CHECK(m and m.args[2] == 1 and FIND("/mod") == nil and part.mods[0].type == "Chord",
    "an Arp preset loads into the Arp's slot")
  P.osc("/mod_preset_loaded", {0, 1, "Arp", "Up", "", "rate", 0.7, "gate", 0.2})
  CHECK(params:get("p1_mods"):find("1,Arp,0,gate=0.2000,rate=0.7000", 1, true), "its values in the mods param")
  sc = L.screen(P, false)
  CHECK(sc.title:find("mod 2 arp", 1, true) and sc.tabs[1].name == "arp", "the list on the Arp")
  -- delete it
  L.key(P, 3, 1, true)
  SENT_CLEAR()
  L.key(P, 3, 1, false)
  CHECK(FIND("/preset_delete", function(a) return a[1] == dir .. "Arp/up.acpreset" and a[2] == 0 end) ~= nil,
    "K1+K3 then K3 deletes it")
  SENT_CLEAR()
  P.osc("/preset_deleted", {dir .. "Arp/up.acpreset", 0, "ok"})
  local said, was = nil, P.on_preset_note
  P.on_preset_note = function(text) said = text end
  P.osc("/preset_deleted", {dir .. "Arp/up.acpreset", 0, "ok", "kept"})
  P.on_preset_note = was
  CHECK(said == "media kept, in use", "a kept folder is said")
  CHECK(FIND("/preset_scan", function(a) return a[1] == dir end) ~= nil and part.mod_presets[1] == nil,
    "then rescans the mod folder, the slot's preset forgotten")
  -- a Scale's slot can't be saved from
  P.set_mod_type(1, 2, "Scale", false)
  L.close()
  L.open_mod(P, 1, 2)
  P.osc("/preset_scan", {dir})
  L.key(P, 3, 1, true)
  CHECK(not L.screen(P, true).confirm, "an empty all tab: nothing to save or delete")
  L.key(P, 2, 1, false)
  CHECK(not L.active(P), "K2 closes it")
elseif which == "19" then
  -- Divisi in an old PSET, the macro destinations, a Venue's room
  local LAYOUT = include("lib/layout")
  local function pset(lines)
    local name = os.tmpname()
    local f = io.open(name, "w")
    f:write(table.concat(lines, "\n") .. "\n")
    f:close()
    return name
  end
  CHECK(include("lib/machines").about.Divisi ~= nil, "Divisi is in the picker")
  local has = false
  for _, m in ipairs(include("lib/parts").MACHINES) do has = has or m.name == "Divisi" end
  CHECK(has, "and in the machines offered")
  P.machines = {{name = "Divisi"}, {name = "Reflux"}, {name = "Hexbeat"}, {name = "Forage"}}
  P.defs.Divisi = {}
  for j = 1, 4 do P.defs.Divisi[j] = {name = "k" .. j, min = 0, max = 1, default = 0.5, unit = "", steps = 0, curve = "lin"} end
  P.patches.Divisi = {"Init"}
  -- indexes from before Divisi: the old order, less machines the plugin lacks
  params:set("p1_machine", 2, true)
  params:set("p2_machine", 3, true)
  local name = pset({'"parts": 1', '"p1_machine": 2', '"p2_machine": 3'})
  P.after_read(name)
  os.remove(name)
  CHECK(P.own[1].machine == "Reflux" and P.own[2].machine == "Hexbeat", "an old PSET keeps Reflux and Hexbeat")
  params:set("p1_machine_name", "Divisi", true)
  params:set("p1_machine", 1, true)
  name = pset({'"parts": 1', '"p1_machine": 1', '"p1_machine_name": "Divisi"'})
  P.after_read(name)
  os.remove(name)
  CHECK(P.own[1].machine == "Divisi", "a PSET with the machine's name lands on Divisi")

  -- macro destinations: the same destination on more steps, in old PSETs only
  local function tine_defs(extra)
    local d = {}
    for j = 1, 92 do d[j] = {name = "k" .. j, min = 0, max = 1, default = 0, unit = "", steps = 0, curve = "lin"} end
    d[10].name, d[11].name = "mac1_d1", "mac8_d4"
    d[10].steps, d[10].curve, d[10].max = 17, "step", 16
    for j = 93, 92 + extra do d[j] = {name = "n" .. j, min = 0, max = 1, default = 0.5, unit = "", steps = 0, curve = "lin"} end
    return d
  end
  local d = tine_defs(4)
  local v = {}
  for j = 1, #d - 4 do v[j] = 0 end
  v[10], v[11], v[12] = 3 / 16, 1, 0.3
  LAYOUT.migrate("Tine", v, d, P.unmap, 2)
  CHECK(math.abs(v[10] - 3 / 17) < 1e-9 and math.abs(v[11] - 16 / 17) < 1e-9, "Tine's destination 3 of 17 steps is 3 of 18, 16 is 16")
  CHECK(v[12] == 0.3, "other params stay")
  CHECK(v[93] == 0.5 and v[96] == 0.5, "the new params take their defaults")
  for _, c in ipairs({{"Palm", 15, 17, 156}, {"Chanter", 14, 17, 91}, {"Fathom", 14, 15, 154}}) do
    local n = c[4]
    local dd = {}
    for j = 1, n + 2 do dd[j] = {name = "k" .. j, min = 0, max = 1, default = 0, unit = "", steps = 0, curve = "lin"} end
    dd[5].name = "mac3_d2"
    local vv = {}
    for j = 1, n do vv[j] = 0 end
    vv[5] = 2 / (c[2] - 1)
    LAYOUT.migrate(c[1], vv, dd, P.unmap, 2)
    CHECK(math.abs(vv[5] - 2 / (c[3] - 1)) < 1e-9, c[1] .. ": destination 2 stays 2")
  end
  -- a layout 1 PSET has no macros: they come as defaults, not rescaled
  v = {}
  for j = 1, 19 do v[j] = 0 end
  LAYOUT.migrate("Tine", v, d, P.unmap, 1)
  CHECK(v[10] == 0 and v[93] == 0.5, "a layout 1 PSET gets defaults, not a rescale")

  -- a Venue: room and file in the fx param
  P.effects = {"Delay", "Venue"}
  local part = P.own[1]
  P.set_insert(1, 2, "Venue", false)
  CHECK(FIND("/insert", function(a) return a[2] == 2 and a[3] == "Venue" end) ~= nil, "Venue goes to slot 3")
  local venue = {0, 2, "Venue", 3, "length", 0, 1, 0.5, "", 0, "lin", 0.5, "decay", 0, 1, 0.5, "", 0, "lin", 0.5,
    "stretch", 0, 1, 0.5, "", 0, "lin", 0.5, 0}
  local reply = {0}
  for k, x in ipairs(venue) do reply[k + 1] = x end
  reply[2] = 2
  P.own[1].fx_wait = 1
  P.osc("/inserts", {0, 2, "Venue", 3, "length", 0, 1, 0.5, "", 0, "lin", 0.5, "decay", 0, 1, 0.5, "", 0, "lin", 0.5,
    "stretch", 0, 1, 0.5, "", 0, "lin", 0.5, 0})
  CHECK(part.fx[2] and part.fx[2].type == "Venue", "the reply shows the Venue")
  P.osc("/impulse_loaded", {0, 2, "factory:wooden-hall", ""})
  local panel = include("lib/panel")
  local function room()
    for _, c in ipairs(panel.fx_cards(part.fx, P.effects)) do
      for _, ctl in ipairs(c.controls) do if ctl.what == "room" then return ctl end end
    end
  end
  local ctl = room()
  CHECK(ctl and math.abs(P.fx_value(1, ctl) - 1 / 5) < 1e-9 and panel.option(ctl, 1 / 5) == "wooden hall",
    "the room control shows the wooden hall")
  CHECK(params:get("p1_fx") == "2=Venue", "the default room isn't saved")
  SENT_CLEAR()
  P.set_fx(1, ctl, 4 / 5)
  local m = FIND("/impulse")
  CHECK(m and m.args[1] == 0 and m.args[2] == 2 and m.args[3] == "factory:closet" and m.args[4] ~= nil,
    "turning to closet sends /impulse with the roots")
  CHECK(params:get("p1_fx") == "2=Venue|2@factory:closet", "and the pick is saved: " .. params:get("p1_fx"))
  P.osc("/impulse_loaded", {0, 2, "factory:closet", ""})
  CHECK(panel.option(room(), 4 / 5) == "closet" and math.abs(P.fx_value(1, room()) - 4 / 5) < 1e-9, "the card shows closet")
  P.osc("/impulse_loaded", {0, 2, "factory:closet", "no such room"})
  CHECK(room().opts[6] == "error" and math.abs(P.fx_value(1, room()) - 1) < 1e-9, "a factory room that fails shows error")
  P.osc("/impulse_loaded", {0, 2, "factory:closet", ""})
  SENT_CLEAR()
  P.set_impulse(1, 2, "/d/audio/my hall.wav")
  P.osc("/impulse_loaded", {0, 2, "/d/audio/my hall.wav", ""})
  CHECK(params:get("p1_fx") == "2=Venue|2@/d/audio/my hall.wav", "a file is saved")
  ctl = room()
  CHECK(ctl.opts[6] == "my hall" and P.fx_value(1, ctl) == 1 and P.impulse_path(1, 2) == "/d/audio/my hall.wav",
    "the card names the file")
  SENT_CLEAR()
  P.set_fx(1, ctl, 1)
  CHECK(not FIND("/impulse"), "the file step sends nothing")
  P.set_impulse(1, 2, "/d/a|b.wav")
  CHECK(not FIND("/impulse"), "a name with | is refused")
  -- read back from a PSET: sent after the insert
  local edits, inserts, impulses = P.decode_fx("2=Venue|2,length,0.5000|2@factory:stone-church|3@")
  CHECK(impulses[2] == "factory:stone-church" and impulses[3] == "" and inserts[2] == "Venue", "decoded")
  CHECK(P.encode_fx(edits, inserts, impulses) == "2=Venue|2,length,0.5000|2@factory:stone-church|3@", "and encoded again")
  params:set("p1_fx", "2=Venue|2@factory:stone-church", true)
  name = pset({'"parts": 1', '"p1_fx": "2=Venue|2@factory:stone-church"', '"p1_machine_name": "Divisi"', '"p1_machine": 1'})
  SENT_CLEAR()
  P.after_read(name)
  os.remove(name)
  ADVANCE(0.5)
  P.osc("/effects", {"Delay", "Venue"})
  CHECK(part.impulse_pick[2] == "factory:stone-church", "the PSET's room is the part's pick")
  CHECK(FIND("/impulse", function(a) return a[2] == 2 and a[3] == "factory:stone-church" end) ~= nil,
    "and goes to the plugin after the insert")
  -- a preset brings its own impulse
  SENT_CLEAR()
  P.load_fx_preset(1, 2, {path = "/d/p.acpreset", index = 0, name = "Hall"})
  CHECK(FIND("/fx_preset_load", function(a) return #a > 5 end) ~= nil, "an effect preset is sent with the roots")
  P.osc("/impulse_loaded", {0, 2, "factory:metal-plate", ""})
  CHECK(part.impulse_pick[2] == "factory:metal-plate" and params:get("p1_fx"):find("2@factory:metal-plate", 1, true),
    "its impulse is the pick")
  P.osc("/fx_preset_loaded", {0, 2, "Delay", "Echo", ""})
  CHECK(part.impulse_pick[2] == nil, "another effect's preset drops it")
elseif which == "20" then
  -- a song's Venues: the track's room control and the out strip's send room
  P.effects = {"Delay", "Venue"}
  local before = params:get("p1_fx")
  S.load("V")
  local hall = "/d/acidulous/songs/V/media/hall.wav"
  P.osc("/mounted", {0, "Reflux"})
  P.osc("/inserts", {0, 1, "Venue", 3, "length", 0, 1, 0.5, "", 0, "lin", 0.5, "decay", 0, 1, 0.5, "", 0, "lin", 0.5,
    "stretch", 0, 1, 0.5, "", 0, "lin", 0.5, 0})
  P.osc("/impulse_loaded", {0, 1, hall, ""})
  P.osc("/song_sends", {-1, 0.5})
  P.osc("/send_impulse_loaded", {0, hall, ""})
  P.osc("/song_loaded", {"V", "/d/acidulous/songs/V", "", 0, 1, 0, "Reflux", "", "lead"})
  local part = P.part(1)
  CHECK(part.song and part.fx[1] and part.fx[1].type == "Venue", "the song's track shows its Venue")
  local panel = include("lib/panel")
  local function room()
    for _, c in ipairs(panel.fx_cards(part.fx, P.effects)) do
      for _, ctl in ipairs(c.controls) do if ctl.what == "room" then return ctl end end
    end
  end
  local ctl = room()
  CHECK(ctl and ctl.opts[6] == "hall" and P.fx_value(1, ctl) == 1, "the room control shows the song's file")
  SENT_CLEAR()
  P.set_fx(1, ctl, 4 / 5)
  local m = FIND("/impulse")
  CHECK(m and m.args[1] == 0 and m.args[2] == 1 and m.args[3] == "factory:closet", "turning it sends /impulse")
  CHECK(params:get("p1_fx") == before, "and saves nothing")
  P.osc("/impulse_loaded", {0, 1, "factory:closet", ""})
  CHECK(panel.option(room(), 4 / 5) == "closet", "the card shows closet")
  -- the send
  CHECK(P.out_room("rev_size") == "hall" and P.out_room("dly_feedback") == nil, "the out strip names the send's file")
  CHECK(P.out_room_01("rev_size") == 1, "at the file's step")
  SENT_CLEAR()
  P.delta_out_send("rev_size", 1)
  m = FIND("/send_impulse")
  CHECK(m and m.args[1] == 0 and m.args[2] == "factory:small-room", "turning it sends /send_impulse")
  P.osc("/send_impulse_loaded", {0, "factory:small-room", ""})
  CHECK(P.out_room("rev_size") == "small room" and P.out_room_01("rev_size") == 0, "and shows the room")
  SENT_CLEAR()
  P.delta_out_send("rev_size", -1)
  CHECK(not FIND("/send_impulse"), "nothing below the first room")
  P.delta_out_send("dly_feedback", 1)
  CHECK(not FIND("/send_impulse") and FIND("/send", function(a) return a[1] == 1 and a[2] == "feedback" end) ~= nil,
    "the delay's feedback moves as before")
  P.osc("/song_unloaded", {1})
  CHECK(P.out_room("rev_size") == nil and next(P.song_send_impulses) == nil, "the unload forgets the send's room")
elseif which == "21" then
  -- the patchfx param goes after the patch's /patch, and not at all with none saved
  local function pset(lines)
    local name = os.tmpname()
    local f = io.open(name, "w")
    f:write(table.concat(lines, "\n") .. "\n")
    f:close()
    return name
  end
  local function index(path)
    for k, m in ipairs(SENT) do if m.path == path and m.args[1] == 0 then return k end end
  end
  P.machines = {{name = "Reflux"}}
  P.defs.Reflux = {{name = "k1", min = 0, max = 1, default = 0.5, unit = "", steps = 0, curve = "lin"}}
  P.patches.Reflux = {"Init"}
  params:set("p1_machine_name", "Reflux", true)
  params:set("p1_machine", 1, true)
  params:set("p1_patchfx", "1,1,0,0,0,0", true)
  -- a PSET from before patchfx
  local name = pset({'"parts": 1', '"p1_machine": 1', '"p1_machine_name": "Reflux"'})
  SENT_CLEAR()
  P.after_read(name)
  os.remove(name)
  ADVANCE(0.5)
  CHECK(index("/patch") ~= nil, "an old PSET sends the patch")
  CHECK(index("/patch_fx") == nil and params:get("p1_patchfx") == "", "and no /patch_fx over what /patch set")
  SENT_CLEAR()
  P.resend()
  ADVANCE(0.5)
  CHECK(index("/patch") ~= nil and index("/patch_fx") == nil, "nor after a restart")
  -- a PSET with it
  params:set("p1_patchfx", "1,1,0,0,0,0", true)
  name = pset({'"parts": 1', '"p1_machine": 1', '"p1_machine_name": "Reflux"', '"p1_patchfx": "1,1,0,0,0,0"'})
  SENT_CLEAR()
  P.after_read(name)
  ADVANCE(0.5)
  local at = index("/patch_fx")
  CHECK(at and index("/patch") and index("/patch") < at and SENT[at].args[2] == 1 and SENT[at].args[3] == 1,
    "a saved patchfx goes after the patch")
  os.remove(name)
  -- a part picked fresh while a song held the racks, sent when they're back
  P.own[1].owed, P.own[1].fresh = true, true
  SENT_CLEAR()
  P.pay_owed()
  ADVANCE(0.5)
  at = index("/patch_fx")
  CHECK(at and index("/patch") and index("/patch") < at, "and on a fresh part's load")
end
print(FAILS == 0 and "all ok" or (FAILS .. " failed"))
os.exit(FAILS == 0 and 0 or 1)
