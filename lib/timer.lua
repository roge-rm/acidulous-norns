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
-- A one-shot runs as a repeating metro stopped on its first tick. A norns
-- one-shot metro's thread outlives its tick by a period, and starting the
-- metro again just as that thread ends leaves matron's metro state wrong,
-- which can later stop another metro's thread.

local T = {}

local function release(m)
  m.props.acid_token = nil
  m:stop()
  metro.available[m.props.id] = true
  metro.assigned[m.props.id] = false
end

local function start(seconds, once, fn)
  local m = metro.init()
  if not m then
    print("acidulous: out of metros")
    return nil
  end
  local token = {}
  m.props.acid_token = token
  m.event = function()
    if m.props.acid_token ~= token then return end
    if once then release(m) end
    fn()
  end
  m:start(seconds, -1)
  return {m = m, token = token}
end

function T.every(seconds, fn) return start(seconds, false, fn) end
function T.after(seconds, fn) return start(seconds, true, fn) end

function T.cancel(t)
  if t and t.m.props.acid_token == t.token then release(t.m) end
end

function T.cancel_all()
  for i = 1, metro.num_script_metros do
    local m = metro.metros[i]
    if m.props.acid_token then release(m) end
  end
end

return T
