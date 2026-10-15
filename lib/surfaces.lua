-- Controllers ("surfaces"), each a module in lib/surfaces/. For now just the
-- OMX-27.
--
-- A surface module has:
--   name             shown in messages
--   match(name)      true for a MIDI port it drives
--   connect()        finds its port in midi.vports and takes it over; true if it did
--   lost()           it was unplugged: forget it, sending nothing
--   disconnect()     hand it back, on the script's cleanup
--   connected()      true while it's in use
--   frame()          15 times a second: update its LEDs
--   and optionally
--   wants_screen()   true while it can show a screen
--   draw()           draws its 128x32 screen at the top left of the norns screen
--   screen_send(x, y)  sends that part of the norns screen to it
-- It calls the functions in surfaces.actions (set by the script).

local S = {}

S.kinds = {
  include("lib/surfaces/omx27"),
}

S.actions = {}

function S.connect_all()
  for _, kind in ipairs(S.kinds) do
    kind.actions = S.actions
    if not kind.connected() then kind.connect() end
  end
end

-- A MIDI device came or went (norns' midi.add and midi.remove).
function S.added(dev)
  for _, kind in ipairs(S.kinds) do
    if kind.match(dev.name) then
      clock.run(function()
        clock.sleep(0.5) -- let norns finish setting the port up
        kind.connect()
      end)
    end
  end
end

function S.removed(dev)
  for _, kind in ipairs(S.kinds) do
    if kind.match(dev.name) and kind.connected() then kind.lost() end
  end
end

function S.is_surface(name)
  for _, kind in ipairs(S.kinds) do
    if kind.match(name) then return true end
  end
  return false
end

function S.frame()
  for _, kind in ipairs(S.kinds) do
    if kind.connected() then kind.frame() end
  end
end

-- Draws and sends each connected screen. Leaves the norns screen to be
-- cleared and drawn after, before screen.update.
function S.screens()
  for _, kind in ipairs(S.kinds) do
    if kind.connected() and kind.draw and kind.wants_screen() then
      kind.draw()
      kind.screen_send(0, 0)
    end
  end
end

function S.disconnect()
  for _, kind in ipairs(S.kinds) do
    if kind.connected() then kind.disconnect() end
  end
end

return S
