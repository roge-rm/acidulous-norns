-- OMX-27 in REMOTE mode (Quixotic7 firmware 1.15.4 and later). The script
-- gets every key, the encoder, its button and the pots, and sets the 27 key
-- LEDs. Protocol: SYSEX_SPEC.md "REMOTE mode" in github.com/Quixotic7/OMX-27.

local timer = include("lib/timer")

local omx = {}

local HEAD = {0xF0, 0x7D, 0x00, 0x00}
local MODE_MI, MODE_REMOTE = 0, 9
local EVENTS = {[0] = "up", "down", "hold", "quick"}

-- called with the firmware version {major, minor, point}, or nil if the
-- OMX-27 didn't answer, when it's too old for REMOTE mode
omx.old_firmware = function(version) end
omx.answered = function() end -- it answered after old_firmware, new enough
local MIN_VERSION = {1, 15, 4}
local version_reply = nil
local connecting = nil -- counts connects, so only the latest acts
local asking = nil     -- the timer asking for its version

-- callbacks, set by the script
omx.key = function(n, ev) end -- n 0-26 (0 is AUX), ev "down" "up" "hold" "quick"
omx.enc = function(d) end
omx.enc_btn = function(z) end
omx.pot = function(n, v, hires) end -- n 0-4, v 0-127, hires 0-16383
-- MIDI that isn't sysex, from port `port`, while it isn't in REMOTE mode
omx.midi = function(data, port) end

local dev
local port_n -- dev's port in midi.vports
local dev_id -- the norns device id, which stays the same while it's plugged in
-- true once the OMX has been asked into REMOTE; it ignores LEDs and frames before that
local remote = false
local rx = {}
local leds, shown = {}, {}

-- screen: one frame in flight; the OMX acks each frame with 52 04
local frame_pending = false
local frame_sent_at = 0
local KEEP_ALIVE = 30 -- seconds; the OMX's own screensaver starts after 3 min without a frame
local last_chunks = {} -- chunks the OMX has acked, as shown
local sent_chunks = {}   -- chunks in the frame in flight, kept once it's acked

local function send(cmd, payload)
  local m = {table.unpack(HEAD)}
  m[#m + 1] = cmd
  for _, b in ipairs(payload or {}) do m[#m + 1] = b end
  m[#m + 1] = 0xF7
  dev:send(m)
end

local function handle(m)
  -- m is a whole sysex message, F0 to F7
  if #m < 7 or m[2] ~= 0x7D then return end
  if m[5] == 0x0F and m[6] == 0x02 and #m >= 10 then
    -- the config reply: F0 7D 00 00 0F 02 major minor point ...
    version_reply = {m[7], m[8], m[9]}
    return
  end
  if m[5] == 0x52 then
    if m[6] == 0x04 then
      frame_pending = false
      for c, chunk in pairs(sent_chunks) do last_chunks[c] = chunk end
      sent_chunks = {}
    end
    return
  end
  if m[5] ~= 0x51 then return end
  local kind = m[6]
  if kind == 0x00 then
    omx.key(m[7], EVENTS[m[8]])
  elseif kind == 0x01 then
    omx.enc((m[7] == 2 and 1 or -1) * m[8])
  elseif kind == 0x02 then
    omx.enc_btn(m[7])
  elseif kind == 0x03 then
    omx.pot(m[7], m[8], (m[9] << 7) | m[10])
  end
end

local function on_midi(data)
  if not remote and #rx == 0 and data[1] and data[1] >= 0x80 and data[1] < 0xF0 then
    omx.midi(data, port_n)
    return
  end
  -- matron hands sysex over in pieces; put whole messages back together
  for _, b in ipairs(data) do
    if b == 0xF0 then
      rx = {b}
    elseif #rx > 0 then
      rx[#rx + 1] = b
      if b == 0xF7 then
        handle(rx)
        rx = {}
      end
    end
  end
end

local function new_enough(v)
  if not v then return false end
  for i = 1, 3 do
    if v[i] ~= MIN_VERSION[i] then return v[i] > MIN_VERSION[i] end
  end
  return true
end

-- Finds the OMX-27, checks its firmware and, if it's new enough, puts it in
-- REMOTE mode. Too old, or no answer, and omx.old_firmware is called instead.
function omx.connect()
  -- norns keeps the names of unplugged devices in its ports, without a device
  for i, v in ipairs(midi.vports) do
    if v.device and omx.is_omx(v.name) then
      dev = midi.connect(i)
      port_n = i
      dev_id = v.device.id
      dev.event = on_midi
      remote = false
      for n = 0, 26 do leds[n] = {0, 0, 0}; shown[n] = {-1, -1, -1} end
      last_chunks, sent_chunks = {}, {}
      frame_pending = false
      version_reply = nil
      -- a newer connect (it was plugged in again) takes over from this one
      connecting = (connecting or 0) + 1
      local mine = connecting
      local asked_at = util.time()
      local function go_remote()
        send(0x51, {0x05, MODE_REMOTE})
        -- everything again in full, now that it's listening
        for n = 0, 26 do shown[n] = {-1, -1, -1} end
        last_chunks, sent_chunks = {}, {}
        frame_pending = false
        remote = true
      end
      -- No answer at first: it may only be slow to start. Keep asking once a
      -- second for a minute, and if it answers new enough, carry on as if it
      -- had at once.
      local function slow_asks()
        local tries = 0
        asking = timer.every(1, function()
          tries = tries + 1
          if mine ~= connecting or not dev or tries > 60 then
            timer.cancel(asking)
            return
          end
          if version_reply then
            timer.cancel(asking)
            if new_enough(version_reply) then
              go_remote()
              omx.answered()
            end
            return
          end
          send(0x1F)
        end)
      end
      -- Ask until it answers: one just switched on can take over 6 s to
      -- start up before it does. Old firmware never answers, so a long wait
      -- only delays the update prompt.
      timer.cancel(asking)
      send(0x1F)
      local i = 0
      asking = timer.every(0.1, function()
        i = i + 1
        if not (version_reply or mine ~= connecting or not dev or i >= 200) then
          if i % 5 == 0 then send(0x1F) end
          return
        end
        timer.cancel(asking)
        if mine ~= connecting or not dev then return end
        if version_reply then
          print(string.format("omx-27: firmware %d.%d.%d, answered after %.1f s", version_reply[1],
            version_reply[2], version_reply[3], util.time() - asked_at))
        end
        if new_enough(version_reply) then
          go_remote()
          return
        end
        omx.old_firmware(version_reply)
        if not version_reply then slow_asks() end
      end)
      return true
    end
  end
  return false
end

-- True if a device name is an OMX-27 (any board).
-- RP2040 units are "omx-27-v3"; Teensy units keep Teensyduino's "Teensy MIDI".
function omx.is_omx(name)
  if name == nil then return false end
  name = name:lower()
  return name:find("omx") ~= nil or name:find("teensy midi") ~= nil
end

-- true while an OMX-27 is in use
function omx.port_open() return dev ~= nil end

-- true once it's in REMOTE mode
function omx.is_remote() return dev ~= nil and remote end

-- true if d, a norns device, is the one in use
function omx.owns(d)
  return dev ~= nil and d ~= nil and dev_id ~= nil and d.id == dev_id
end

-- its port in midi.vports, while in use
function omx.port() return dev and port_n end

-- The OMX-27 was unplugged: forget it without sending anything.
function omx.lost()
  if dev then dev.event = nil end
  dev, dev_id = nil, nil
  remote = false
  frame_pending = false
end

function omx.disconnect()
  if dev then
    send(0x51, {0x05, MODE_MI})
    dev.event = nil
    dev, dev_id = nil, nil
    remote = false
  end
end

-- r, g, b 0-255
function omx.led(n, r, g, b)
  leds[n] = {r >> 1, g >> 1, b >> 1}
end

-- Sends the LEDs that changed since the last call.
function omx.led_show()
  if not dev or not remote then return end
  local payload, any = {0, 27}, false
  for n = 0, 26 do
    local c, s = leds[n], shown[n]
    if c[1] ~= s[1] or c[2] ~= s[2] or c[3] ~= s[3] then any = true end
    payload[#payload + 1] = c[1]
    payload[#payload + 1] = c[2]
    payload[#payload + 1] = c[3]
  end
  if not any then return end
  send(0x5A, payload)
  send(0x5B)
  for n = 0, 26 do shown[n] = {leds[n][1], leds[n][2], leds[n][3]} end
end

-- 8 bytes for every 7: a byte of high bits, then the 7 bytes' low 7 bits.
local function enc7(data)
  local out = {}
  for i = 1, #data, 7 do
    local hi, n = 0, #out + 1
    out[n] = 0
    for j = 0, 6 do
      local b = data[i + j]
      if b == nil then break end
      hi = hi | (((b >> 7) & 1) << j)
      out[#out + 1] = b & 0x7F
    end
    out[n] = hi
  end
  return out
end

-- Makes the next screen_send send the whole frame, not just what changed.
function omx.screen_refresh()
  last_chunks, sent_chunks = {}, {}
end

-- Sends the 128x32 region of the norns screen at (x, y) to the OMX screen.
-- Pixels at level 8 or above are lit. Returns false if the last frame isn't
-- shown yet, so drawing can never run ahead of the OMX.
function omx.screen_send(x, y)
  if not dev or not remote then return false end
  if frame_pending then
    if util.time() - frame_sent_at < 0.25 then return false end
    -- no ack: the frame may have been dropped, so its chunks count as unsent
    sent_chunks = {}
  end
  local px = screen.peek(x or 0, y or 0, 128, 32)
  if not px then return false end
  -- SSD1306 pages, turned 180 degrees as the panel is mounted
  local fb = {}
  for i = 1, 512 do fb[i] = 0 end
  for yy = 0, 31 do
    local row = yy * 128
    for xx = 0, 127 do
      if px:byte(row + xx + 1) >= 8 then
        local rx, ry = 127 - xx, 31 - yy
        local i = rx + (ry >> 3) * 128 + 1
        fb[i] = fb[i] | (1 << (ry & 7))
      end
    end
  end
  local any = false
  for c = 0, 15 do
    local chunk, same = {}, last_chunks[c] ~= nil
    for i = 1, 32 do
      chunk[i] = fb[c * 32 + i]
      if same and last_chunks[c][i] ~= chunk[i] then same = false end
    end
    if not same then
      local payload = {c}
      for _, b in ipairs(enc7(chunk)) do payload[#payload + 1] = b end
      send(0x5C, payload)
      sent_chunks[c] = chunk
      any = true
    end
  end
  if any or util.time() - frame_sent_at > KEEP_ALIVE then
    send(0x5D)
    frame_pending = true
    frame_sent_at = util.time()
  end
  return true
end

return omx
