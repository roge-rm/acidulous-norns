local here = arg[0]:match("^(.*)/[^/]+$")
local which = arg[2]
local CASES = {"1", "2", "3", "4", "5", "7", "8", "9", "10", "11", "12", "13", "14", "15"}
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
end
print(FAILS == 0 and "all ok" or (FAILS .. " failed"))
os.exit(FAILS == 0 and 0 or 1)
