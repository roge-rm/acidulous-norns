local here = arg[0]:match("^(.*)/[^/]+$")
local which = arg[2]
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
elseif which == "2" then
  -- inserts, modifiers and files changed during a song-page load from no song
  P.own[1].fx = {[0] = {type = "Distortion", params = {{name = "drive", value = 0.2}}}}
  S.load("A")
  CHECK(P.song_holds and P.song_racks and not P.song_view, "holds and racks, the script's parts shown")
  SENT_CLEAR()
  P.set_fx(1, {fx = 0, index = 1}, 0.9)
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
end
print(FAILS == 0 and "all ok" or (FAILS .. " failed"))
os.exit(FAILS == 0 and 0 or 1)
