-- A fake norns for lib/parts.lua and lib/song.lua. Run from the repo root:
-- lua5.4 host/tests/lua/tests.lua .
local ROOT = arg[1] or "."
NOW = 0
SENT = {}
local timers = {}

local fake_timer = {}
function fake_timer.after(wait, fn)
  local rec = {at = NOW + wait, fn = fn}
  timers[#timers + 1] = rec
  return rec
end
function fake_timer.every(wait, fn)
  local rec = {at = NOW + wait, fn = fn, every = wait}
  timers[#timers + 1] = rec
  return rec
end
function fake_timer.cancel(rec) if rec then rec.dead = true end end
function ADVANCE(t)
  local stop = NOW + t
  while true do
    local best
    for _, r in ipairs(timers) do
      if not r.dead and not r.every and r.at <= stop and (not best or r.at < best.at) then best = r end
    end
    if not best then break end
    NOW = best.at
    best.dead = true
    best.fn()
  end
  NOW = stop
end

function include(name)
  if name == "lib/timer" then return fake_timer end
  return dofile(ROOT .. "/" .. name .. ".lua")
end

util = {
  time = function() return NOW end,
  clamp = function(v, a, b) return math.min(math.max(v, a), b) end,
  round = function(v, q) q = q or 1; return math.floor(v / q + 0.5) * q end,
  file_exists = function() return false end,
  scandir = function() return {} end,
}
tab = {
  contains = function(t, v) for _, x in pairs(t) do if x == v then return true end end return false end,
  key = function(t, v) for k, x in pairs(t) do if x == v then return k end end end,
}
osc = {send = function(_, path, args) SENT[#SENT + 1] = {path = path, args = args} end}
clock = {get_tempo = function() return 120 end, get_beats = function() return 0 end, internal = {set_tempo = function() end}}
engine = {}
norns = {state = {path = "/x/acidulous/"}}
_path = {audio = "/a/", data = "/d/"}

controlspec = {}
function controlspec.new(min, max, warp, step, default, units, quantum)
  local cs = {minval = min, maxval = max, warp = warp, step = step, default = default or min, quantum = quantum or 0.01}
  function cs:unmap(v) return (v - self.minval) / (self.maxval - self.minval) end
  function cs:map(v) return self.minval + v * (self.maxval - self.minval) end
  return cs
end

params = {params = {}, lookup = {}, tTEXT = 1, tCONTROL = 2, tOPTION = 3, tNUMBER = 4, tGROUP = 5}
local Param = {}
Param.__index = Param
function Param:get()
  if self.t == params.tOPTION then return self.selected end
  return self.value
end
function Param:set(v, silent)
  if self.t == params.tOPTION then
    v = util.clamp(math.floor(v), 1, self.count)
    self.selected = v
  elseif self.t == params.tCONTROL then
    v = util.clamp(v, self.controlspec.minval, self.controlspec.maxval)
    self.value = v
  elseif self.t == params.tNUMBER then
    v = util.clamp(math.floor(v), self.min, self.max)
    self.value = v
  else
    self.value = v
  end
  if not silent and self.action then self.action(self:get()) end
end
local function add(p)
  setmetatable(p, Param)
  params.params[#params.params + 1] = p
  params.lookup[p.id] = #params.params
  return p
end
function params:add_group(id, name, n)
  if n == nil then n, name = name, id end
  add({id = id, t = params.tGROUP, value = n})
end
function params:add_option(id, name, options, default)
  add({id = id, t = params.tOPTION, options = options, count = #options, selected = default or 1, default = default or 1})
end
function params:add_control(id, name, cs, formatter)
  add({id = id, t = params.tCONTROL, controlspec = cs, value = cs.default})
end
function params:add_number(id, name, min, max, default)
  add({id = id, t = params.tNUMBER, min = min, max = max, value = default, default = default})
end
function params:add_text(id, name, v) add({id = id, t = params.tTEXT, value = v or ""}) end
function params:lookup_param(id) return self.params[self.lookup[id]] end
function params:get(id) return self:lookup_param(id):get() end
function params:set(id, v, silent) self:lookup_param(id):set(v, silent) end
function params:string(id) local p = self:lookup_param(id); return p.options and p.options[p.selected] or tostring(p:get()) end
function params:set_action(id, fn) self:lookup_param(id).action = fn end
function params:hide() end
function params:show() end
function params:delta(id, d)
  local p = self:lookup_param(id)
  if p.t == params.tCONTROL then p:set(p.controlspec:map(p.controlspec:unmap(p.value) + d * p.controlspec.quantum))
  else p:set(p:get() + d) end
end

-- ---- the modules ----
local P = include("lib/parts")
local S = include("lib/song")
P.machines = {{name = "Reflux"}, {name = "Hexbeat"}, {name = "Bare"}, {name = "Forage"}}
local function defs(n)
  local t = {}
  for j = 1, n do t[j] = {name = "k" .. j, min = 0, max = 1, default = 0.5, unit = "", steps = 0, curve = "lin"} end
  return t
end
P.defs = {Reflux = defs(4), Hexbeat = defs(3), Bare = defs(2), Forage = defs(2)}
P.patches = {Reflux = {"Classic", "Squelch", "Third"}, Hexbeat = {"Init", "Squared"}, Bare = {}, Forage = {"Init"}}
P.slots = 4
P.add_count_param()
P.add_params()
S.add_params()
S.init(P)
P.on_song = S.osc
P.on_read = S.read
P.ready = true
P.seen = 0
P.set_part(1, "Reflux", "Classic")
P.set_part(2, "Hexbeat", "Squared")
P.start_part(1)
P.start_part(2)
P.set_part(3, "Forage", "Init")
P.start_part(3)
for i = 9, 16 do P.own[i].parked = true end

FAILS = 0
function CHECK(ok, what)
  print((ok and "ok  : " or "FAIL: ") .. what)
  if not ok then FAILS = FAILS + 1 end
end
function SENT_CLEAR() SENT = {} end
function FIND(path, pred)
  for _, m in ipairs(SENT) do
    if m.path == path and (not pred or pred(m.args)) then return m end
  end
end
return P, S
