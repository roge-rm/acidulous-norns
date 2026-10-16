-- One-shot and repeating timers on norns metros, used in place of clock
-- loops. A clock cancelled while it sleeps, as the script's cleanup does, can
-- still be woken once and then errors in clock.lua.
--   timer.every(seconds, fn)  runs fn every so often until cancelled
--   timer.after(seconds, fn)  runs fn once
--   timer.cancel(t)           safe with nil or a finished timer
--   timer.cancel_all()        on cleanup
-- A tick already on its way when a timer is cancelled is dropped. Each timer
-- marks its metro with its record, so cancel_all and the watchdog reach the
-- timers of every module that includes this one.
--
-- A one-shot runs as a repeating metro stopped on its first tick. A norns
-- one-shot metro's thread outlives its tick by a period, and starting the
-- metro again just as that thread ends leaves matron's metro state wrong.
--
-- Matron can also stop one of these metros' threads by itself: a metro
-- stopped before its new thread has run (norns' menu timer, on a script
-- load) is left marked running with the id of a thread that's gone, and when
-- it's stopped again it cancels whichever thread now has that id, which is
-- often the next one started. So each tick checks the other timers, and one
-- that has stopped ticking moves to a fresh metro. The dead metro is left
-- assigned, as stopping it again could cancel yet another thread.

local T = {}

local function now() return util.time() end

local function release(rec)
  local m = rec.m
  if m.props.acid_rec ~= rec then return end
  m.props.acid_rec = nil
  m:stop()
  metro.available[m.props.id] = true
  metro.assigned[m.props.id] = false
end

local check

-- Puts rec on a free metro and starts it.
local function run_on(rec, m)
  rec.m = m
  m.props.acid_rec = rec
  m.event = function()
    if m.props.acid_rec ~= rec then return end
    rec.last = now()
    if rec.once then release(rec) end
    rec.fn()
    check()
  end
  m:start(rec.seconds, -1)
end

local function start(seconds, once, fn)
  local m = metro.init()
  if not m then
    print("acidulous: out of metros")
    return nil
  end
  local rec = {seconds = seconds, once = once, fn = fn, last = now()}
  run_on(rec, m)
  return rec
end

local checked = 0

-- Moves any timer that's well past its tick to a fresh metro. Runs at most
-- twice a second, from whichever timer ticks.
check = function()
  local t = now()
  local gap = t - checked
  if gap < 0.5 then return end
  checked = t
  -- a long gap means matron was busy and the ticks are still queued
  local busy = gap > 1.5
  for i = 1, metro.num_script_metros do
    local m = metro.metros[i]
    local rec = m.props.acid_rec
    if rec and busy then
      rec.last = t
    elseif rec and t - rec.last > rec.seconds * 3 + 1 then
      local fresh = metro.init()
      if fresh then
        print("acidulous: metro " .. i .. " stopped ticking, moved to " .. fresh.props.id)
        m.props.acid_rec = nil
        m.event = nil
        rec.last = t
        run_on(rec, fresh)
      end
    end
  end
end

function T.every(seconds, fn) return start(seconds, false, fn) end
function T.after(seconds, fn) return start(seconds, true, fn) end

function T.cancel(t)
  if t then release(t) end
end

function T.cancel_all()
  for i = 1, metro.num_script_metros do
    local rec = metro.metros[i].props.acid_rec
    if rec then release(rec) end
  end
end

return T
