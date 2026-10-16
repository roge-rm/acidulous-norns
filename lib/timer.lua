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
-- assigned, as stopping it again could cancel yet another thread. If it does
-- tick again its thread is alive after all, so it's stopped and freed then.

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

-- A tick this late means matron was busy. It's under the 1 s margin a timer
-- gets before it counts as stopped, so a stall long enough to make a live
-- timer look stopped is always seen first by the tick that ends it.
local STALL = 0.5

-- Puts rec on a free metro and starts it.
local function run_on(rec, m)
  rec.m = m
  m.props.acid_rec = rec
  m.event = function()
    if m.props.acid_rec ~= rec then return end
    local t = now()
    rec.last = t
    -- how far behind its schedule this tick is, which is how long matron's
    -- event queue held it
    rec.due = rec.due + rec.seconds
    local late = t - rec.due
    if late < 0 or late > STALL then rec.due = t end
    if rec.once then release(rec) end
    rec.fn()
    check(t, late > STALL)
  end
  rec.due = now()
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

-- A metro moved off because it stopped ticking. If it ticks after all, its
-- thread is running, so stopping it is safe and it goes back in the pool.
local function retire(m)
  m.props.acid_rec = nil
  m.event = function()
    if m.props.acid_rec then return end
    m.event = nil
    m:stop()
    metro.available[m.props.id] = true
    metro.assigned[m.props.id] = false
  end
end

-- Moves any timer that's well past its tick to a fresh metro. Runs at most
-- twice a second, from whichever timer ticks. stalled is set when the
-- ticking timer was held up by matron, and then every timer's ticks are
-- still queued, so none counts as stopped.
check = function(t, stalled)
  if stalled then
    checked = t
    for i = 1, metro.num_script_metros do
      local rec = metro.metros[i].props.acid_rec
      if rec then rec.last = t end
    end
    return
  end
  if t - checked < 0.5 then return end
  checked = t
  for i = 1, metro.num_script_metros do
    local m = metro.metros[i]
    local rec = m.props.acid_rec
    if rec and t - rec.last > rec.seconds * 3 + 1 then
      local fresh = metro.init()
      if fresh then
        print("acidulous: metro " .. i .. " stopped ticking, moved to " .. fresh.props.id)
        retire(m)
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
