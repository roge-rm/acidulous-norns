-- acidulous
-- test script: plays the first machines and prints the plugin's load
--
-- From the REPL: acid.play("reflux"), "hexbeat", "trinity", "ratio",
-- "formulate" or "four" (Reflux, Hexbeat, Trinity and Ratio at once).
-- acid.stop() stops the notes, acid.load() prints the last load reading.

engine.name = "Acidulous"

local PLUGIN = {"127.0.0.1", 57150}
-- Normalised machine volume, so nothing plays at full scale.
local VOLUME = 0.35

acid = {verbose = true}
local mode = nil
local held = {} -- part -> notes to release
local load = {now = 0, worst = 0, peak = 0, peak_worst = 0}
local count = 0

local function send(path, args) osc.send(PLUGIN, path, args) end

local function note(part, n, vel)
  send("/note", {part, 1, n, vel})
end

local function release(part)
  for _, n in ipairs(held[part] or {}) do note(part, n, 0) end
  held[part] = {}
end

local function play_notes(part, notes, vel)
  release(part)
  for _, n in ipairs(notes) do note(part, n, vel) end
  held[part] = notes
end

-- The parts and their patches. Reflux 0, Hexbeat 1, Trinity 2, Ratio 3,
-- Formulate 4.
local PATCHES = {
  {0, "Reflux", "Classic"},
  {1, "Hexbeat", "Straight"},
  {2, "Trinity", "Glass Pad"},
  {3, "Ratio", "Tine"},
  {4, "Formulate", "Pulse Lead"},
}

local ACID = {36, 36, 48, 36, 39, 36, 43, 36, 36, 46, 36, 48, 41, 36, 39, 43}
local ACCENT = {1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0}
-- Kick 36, snare 38, closed hat 43, open hat 44.
local BEAT = {
  {36, 43}, {43}, {43}, {43}, {38, 43}, {43}, {43}, {44},
  {36, 43}, {43}, {36, 43}, {43}, {38, 43}, {43}, {43}, {44},
}
local CHORDS = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 48, 53, 57}, {43, 50, 55, 59}}
local ARP = {60, 64, 67, 72, 67, 64}

local function step(s)
  local i = (s - 1) % 16 + 1
  local bar = (s - 1) // 16 % 4 + 1
  local on = function(m) return mode == m or (mode == "four" and m ~= "formulate") end
  if on("reflux") then
    play_notes(0, {ACID[i]}, ACCENT[i] == 1 and 110 or 70)
  end
  if on("hexbeat") then
    for _, n in ipairs(BEAT[i]) do note(1, n, n == 43 and 60 or 90) end
  end
  if on("trinity") and (i == 1 or i == 9) then play_notes(2, CHORDS[bar], 70) end
  if on("ratio") and (i == 5 or i == 13) then play_notes(3, CHORDS[bar], 70) end
  if on("formulate") then play_notes(4, {ARP[(s - 1) % #ARP + 1]}, 80) end
end

local function sequence()
  local s = 1
  while true do
    clock.sync(1 / 4)
    if mode then step(s) end
    s = s + 1
  end
end

function acid.play(m)
  acid.stop()
  load.peak, load.peak_worst = 0, 0
  mode = m
  print("playing " .. m)
end

function acid.stop()
  mode = nil
  for part = 0, 4 do
    release(part)
    send("/cc", {part, 1, 123, 0})
  end
end

function acid.load()
  print(string.format("load %.1f%% worst %.1f%%, highest load %.1f%% worst %.1f%%",
    load.now, load.worst, load.peak, load.peak_worst))
end

local function on_osc(path, args, from)
  if path == "/load" then
    load.now, load.worst = args[1], args[2]
    load.peak = math.max(load.peak, args[1])
    load.peak_worst = math.max(load.peak_worst, args[2])
    count = count + 1
    if acid.verbose and count % 4 == 0 then acid.load() end
  elseif path == "/patch" then
    print("patch " .. args[1] .. " " .. args[2] .. " " .. args[3])
    send("/param", {args[1], "volume", VOLUME})
  elseif path == "/error" then
    print("acidulous error: " .. tostring(args[1]))
  elseif path == "/mounted" then
    print("mounted " .. args[1] .. " " .. args[2])
  end
end

function init()
  osc.event = on_osc
  engine.start()
  clock.run(function()
    clock.sleep(0.5)
    for _, p in ipairs(PATCHES) do send("/patch", p) end
  end)
  clock.run(sequence)
end

function cleanup()
  acid.stop()
  acid = nil
end

function redraw()
  screen.clear()
  screen.move(0, 10)
  screen.text("acidulous test")
  screen.move(0, 30)
  screen.text(mode or "stopped")
  screen.update()
end
