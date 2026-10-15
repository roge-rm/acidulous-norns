-- One-shot and repeating timers on norns metros, used in place of clock
-- loops. A clock cancelled while it sleeps, as the script's cleanup does, can
-- still be woken once and then errors in clock.lua.
--   timer.every(seconds, fn)  runs fn every so often until cancelled
--   timer.after(seconds, fn)  runs fn once
--   timer.cancel(t)           safe with nil or a finished timer
--   timer.cancel_all()        on cleanup
-- A tick already on its way when a timer is cancelled is dropped. Each timer
-- marks its metro with a token, so cancel_all reaches the timers of every
-- module that includes this one.

local T = {}

local function release(m, running)
  m.props.acid_token = nil
  -- stopping a metro whose thread has ended logs an error, so a fired
  -- one-shot is only handed back
  if running then m:stop() end
  metro.available[m.props.id] = true
  metro.assigned[m.props.id] = false
end

local function start(seconds, count, fn)
  local m = metro.init()
  if not m then
    print("acidulous: out of metros")
    return nil
  end
  local token = {}
  m.props.acid_token = token
  m.event = function()
    if m.props.acid_token ~= token then return end
    if count == 1 then release(m, false) end
    fn()
  end
  m:start(seconds, count)
  return {m = m, token = token}
end

function T.every(seconds, fn) return start(seconds, -1, fn) end
function T.after(seconds, fn) return start(seconds, 1, fn) end

function T.cancel(t)
  if t and t.m.props.acid_token == t.token then release(t.m, true) end
end

function T.cancel_all()
  for i = 1, metro.num_script_metros do
    local m = metro.metros[i]
    if m.props.acid_token then release(m, true) end
  end
end

return T
