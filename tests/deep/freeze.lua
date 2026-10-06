-- tests/deep/freeze.lua
-- The budget and freezing ('debug.setbudget', 'debug.freeze',
-- 'debug.thaw'), an extension of this Lua: the budget callback runs every
-- 'n' steps, and may freeze a running coroutine where it is; 'thaw' goes
-- on with it. Stock Lua 5.5 has none of this (the file returns at once
-- there); with no budget set everything behaves as stock, which the
-- upstream suite and the other deep files check.

local debug = require "debug"
if not debug.setbudget then return end

local function deepeq(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then
    return a == b or (a ~= a and b ~= b)
  end
  for k, v in pairs(a) do if not deepeq(v, rawget(b, k)) then return false end end
  for k in pairs(b) do if rawget(a, k) == nil then return false end end
  return true
end

local function show(t)
  if type(t) ~= "table" then return tostring(t) end
  local parts = {}
  for i = 1, (t.n or #t) do parts[#parts + 1] = show(t[i]) end
  return "{" .. table.concat(parts, ",") .. "}"
end

local HUGE = 1 << 50

------------------------------------------------------------------------
-- the budget
------------------------------------------------------------------------

-- 'getbudget': fail with no budget; n, the callback and the steps left
do
  assert(debug.getbudget() == nil)
  local f = function() end
  debug.setbudget(1000, f)
  local n, g, left = debug.getbudget()
  assert(n == 1000 and g == f and left > 0 and left <= 1000)
  debug.setbudget(0)
  assert(debug.getbudget() == nil)
  debug.setbudget(10, f)
  debug.setbudget(10) -- no callback: off
  assert(debug.getbudget() == nil)
  assert(not pcall(debug.setbudget, -1, f))
  assert(not pcall(debug.setbudget, 1, 42))
end

-- the instructions of a coroutine, by a count hook
local function instructions(f, ...)
  local co = coroutine.create(f)
  local n = 0
  debug.sethook(co, function() n = n + 1 end, "", 1)
  assert(coroutine.resume(co, ...))
  return n
end

-- with a budget of 1, the callback runs before every instruction of a
-- coroutine (a step each): as many times as a count hook of 1
do
  local function work(k)
    local s = 0
    for i = 1, k do s = s + i * 2 end
    local t = {}
    for i = 1, 10 do t[i] = s % i end
    return s
  end
  for _, k in ipairs{0, 1, 5, 40} do
    local co = coroutine.create(work)
    local calls = 0
    debug.setbudget(1, function(t) if t == co then calls = calls + 1 end end)
    assert(coroutine.resume(co, k))
    debug.setbudget(0)
    assert(calls == instructions(work, k), calls .. " vs " .. instructions(work, k))
  end
end

-- the steps of some code: the steps left before it, after it
local function spent(f)
  debug.setbudget(HUGE, function() error("no call expected") end)
  local _, _, l0 = debug.getbudget()
  f()
  local _, _, l1 = debug.getbudget()
  debug.setbudget(0)
  return l0 - l1
end

-- a step for each instruction, each C function call and each hand-off
-- between C and Lua
do
  local base = spent(function() end)
  local function body() for i = 1, 100 do local x = i + 1 end end
  local nobody = instructions(function() end)
  -- (a Lua function: its instructions and a call)
  local lua = spent(body) - base
  assert(lua == instructions(body) - nobody, lua .. " " .. instructions(body))
  -- C calls: one more step each
  local ty = type
  local function ccalls() for i = 1, 100 do ty(i) end end
  local c = spent(ccalls) - base
  assert(c == instructions(ccalls) - nobody + 100, c .. " " .. instructions(ccalls))
  -- a C function calling back into Lua: the call, and a hand-off each way
  -- (the comparator's instructions are counted as they run)
  local function cmp(a, b) return a < b end
  local function sorts() table.sort({3, 1, 2}, cmp) end
  local withsort = spent(sorts) - base
  assert(withsort > instructions(sorts) - nobody)
end

-- the budget is shared by all threads and counts in every one of them
do
  local inner = coroutine.wrap(function() for i = 1, 30 do end return 1 end)
  local outer = coroutine.create(function() for i = 1, 30 do end return inner() end)
  local seen = {}
  debug.setbudget(1, function(t) seen[t] = (seen[t] or 0) + 1 end)
  assert(coroutine.resume(outer))
  debug.setbudget(0)
  local threads = 0
  for _ in pairs(seen) do threads = threads + 1 end
  assert(threads >= 3) -- main, outer and the wrapped one
end

-- the callback runs unchecked: no hooks, no budget (it does not call
-- itself), and its steps are not counted
do
  local depth, maxdepth, calls = 0, 0, 0
  local hookcalls = 0
  local function body()
    for i = 1, 200 do end
  end
  local co = coroutine.create(body)
  debug.sethook(co, function() hookcalls = hookcalls + 1 end, "l")
  debug.setbudget(5, function(t)
    depth = depth + 1
    maxdepth = math.max(maxdepth, depth)
    calls = calls + 1
    for i = 1, 100 do end -- more than the budget, and lines
    local n, f, left = debug.getbudget()
    assert(n == 5 and left == 5)
    depth = depth - 1
  end)
  assert(coroutine.resume(co))
  debug.setbudget(0)
  assert(maxdepth == 1 and calls > 10)
  local plain = 0
  local co2 = coroutine.create(body)
  debug.sethook(co2, function() plain = plain + 1 end, "l")
  assert(coroutine.resume(co2))
  assert(hookcalls == plain) -- no line events from the callback's code
end

-- an error in the callback is an error where the budget ran out; the
-- budget goes on
do
  local calls = 0
  debug.setbudget(3, function()
    calls = calls + 1
    if calls == 2 then error("budget error", 0) end
  end)
  local ok, err = pcall(function() for i = 1, 100 do end end)
  local after = calls
  for i = 1, 30 do end
  debug.setbudget(0)
  assert(not ok and err == "budget error")
  assert(calls > after) -- it goes on
end

-- turning the budget off from the callback
do
  local calls = 0
  debug.setbudget(2, function() calls = calls + 1 debug.setbudget(0) end)
  for i = 1, 100 do end
  assert(calls == 1 and debug.getbudget() == nil)
end

-- the same program makes the same callbacks, at the same places
do
  local function trace()
    local where = {}
    local co = coroutine.create(function()
      local t = {}
      for i = 1, 50 do t[#t + 1] = i * i end
      table.sort(t, function(a, b) return a > b end)
      return #t
    end)
    debug.setbudget(7, function(th)
      if th == co then
        local info = debug.getinfo(2, "Sl")
        where[#where + 1] = info.short_src .. ":" .. info.currentline
      end
    end)
    assert(coroutine.resume(co))
    debug.setbudget(0)
    return where
  end
  local a, b = trace(), trace()
  assert(#a > 10 and deepeq(a, b))
end

------------------------------------------------------------------------
-- freezing
------------------------------------------------------------------------

-- freeze a coroutine where its budget runs out, then thaw it
do
  local co
  local froze = 0
  co = coroutine.create(function(a, b)
    local s = 0
    for i = 1, 100 do s = s + i end
    return s, a, b
  end)
  debug.setbudget(20, function(t)
    if t == co and froze == 0 and debug.freeze(co) then froze = froze + 1 end
  end)
  local r = table.pack(coroutine.resume(co, "x", "y"))
  assert(r.n == 1 and r[1] == true) -- as if it returned nothing
  assert(debug.isfrozen(co) and froze == 1)
  assert(coroutine.status(co) == "running") -- as when it stopped
  local ok, err = coroutine.resume(co)
  assert(not ok and err == "cannot resume non-suspended coroutine")
  assert(not pcall(coroutine.close, co))
  r = table.pack(debug.thaw(co))
  debug.setbudget(0)
  assert(r.n == 4 and r[1] and r[2] == 5050 and r[3] == "x" and r[4] == "y", show(r))
  assert(not debug.isfrozen(co) and coroutine.status(co) == "dead")
  local ok2, err2 = debug.thaw(co)
  assert(not ok2 and err2 == "cannot thaw a coroutine that is not frozen")
end

-- 'freeze' only in the callback, only for a coroutine in the running chain
do
  local co = coroutine.create(function() coroutine.yield() end)
  assert(debug.freeze(co) == false) -- not in the callback
  assert(coroutine.resume(co)) -- suspended
  local other = coroutine.create(function() end)
  local results = {}
  debug.setbudget(1, function(t)
    if #results == 0 then
      results[1] = debug.freeze(co)        -- suspended
      results[2] = debug.freeze(other)     -- not started
      results[3] = debug.freeze(coroutine.running()) -- the main thread here
    end
  end)
  for i = 1, 5 do end
  debug.setbudget(0)
  assert(results[1] == false and results[2] == false)
  -- (the stand-alone interpreter runs the script from a C function that
  -- calls it with plain 'lua_pcall', so the main thread cannot freeze)
  assert(results[3] == false)
  assert(not pcall(debug.freeze, 42))
  assert(not pcall(debug.isfrozen, 42))
  assert(not pcall(debug.thaw, 42))
end

-- runs 'f' in a coroutine to its end, answering its yields with their
-- values doubled, thawing it when it froze: a transcript of what it
-- yielded and returned. 'freeze', called from the budget callback with
-- the thread that spent the budget, freezes what it wants.
local function drive(f, n, freeze)
  local co = coroutine.create(f)
  local log, freezes = {}, 0
  if n then
    debug.setbudget(n, function(t)
      if freeze(co, t) then freezes = freezes + 1 end
    end)
  end
  local r = table.pack(coroutine.resume(co))
  while true do
    if debug.isfrozen(co) then
      r = table.pack(debug.thaw(co))
    elseif coroutine.status(co) == "suspended" then
      log[#log + 1] = r
      local answer = {}
      for i = 2, r.n do answer[#answer + 1] = type(r[i]) == "number" and r[i] * 2 or r[i] end
      r = table.pack(coroutine.resume(co, table.unpack(answer)))
    else
      log[#log + 1] = r
      break
    end
  end
  debug.setbudget(0)
  return log, freezes
end

-- resume, thawing the coroutine when it freezes (a kernel would do this);
-- 'managed' has the coroutines resumed so, which can be frozen alone
local managed = setmetatable({}, {__mode = "k"})
local function kresume(co, ...)
  managed[co] = true
  local r = table.pack(coroutine.resume(co, ...))
  while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
  return table.unpack(r, 1, r.n)
end

-- a workload of many kinds of code
local function workload()
  local out = {}
  local function add(...) for i = 1, select("#", ...) do out[#out + 1] = (select(i, ...)) end end
  -- loops, arithmetic, tables, strings
  local s = 0
  for i = 1, 30 do s = s + i * i % 7 end
  add(s, ("x"):rep(3) .. tostring(s))
  -- closures and upvalues, varargs, tail calls
  local function counter() local c = 0 return function(d) c = c + (d or 1) return c end end
  local c = counter()
  c() c(5)
  add(c())
  local function sum(...) local t = 0 for _, v in ipairs{...} do t = t + v end return t end
  local function tail(k, acc) if k == 0 then return acc end return tail(k - 1, acc + k) end
  add(sum(1, 2, 3, 4), tail(20, 0))
  -- metamethods called by the VM
  local mt = {}
  mt.__index = function(t, k) return k * 10 end
  mt.__add = function(a, b) return rawget(a, "v") + b end
  mt.__eq = function(a, b) return rawget(a, "v") == rawget(b, "v") end
  mt.__lt = function(a, b) return rawget(a, "v") < rawget(b, "v") end
  mt.__len = function(t) return 99 end
  mt.__concat = function(a, b) return "cat" end
  mt.__call = function(self, x) return x + 1 end
  local o1 = setmetatable({v = 1}, mt)
  local o2 = setmetatable({v = 2}, mt)
  add(o1[4], o1 + 5, o1 == o2, o1 < o2, #o1, o1 .. "z", o1(41))
  -- generic for with a Lua iterator
  local function range(k) return function(_, i) if i < k then return i + 1 end end, nil, 0 end
  local r = 0
  for i in range(12) do r = r + i end
  add(r)
  -- to-be-closed variables
  do
    local closed = {}
    do
      local x <close> = setmetatable({}, {__close = function() closed[#closed + 1] = "x" end})
      local y <close> = setmetatable({}, {__close = function() closed[#closed + 1] = "y" end})
      for i = 1, 5 do end
    end
    add(table.concat(closed))
  end
  -- protected calls and errors, before and after the budget runs out
  local ok, err = pcall(function() for i = 1, 10 do end error("e1", 0) end)
  add(ok, err)
  local ok2, v2 = pcall(function() for i = 1, 10 do end return "fine" end)
  add(ok2, v2)
  local ok3, h = xpcall(function() for i = 1, 10 do end error({code = 7}) end,
                        function(e) return "handled " .. e.code end)
  add(ok3, h)
  local ok4, nested = pcall(pcall, function() error("deep", 0) end)
  add(ok4, nested)
  -- yields of its own
  local y1 = coroutine.yield(1, 2)
  add(y1)
  local y2 = coroutine.yield("three")
  add(y2)
  -- coroutines inside: wrapped, resumed, nested
  local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end return "done" end)
  add(gen(), gen(), gen(), gen())
  local inner = coroutine.create(function(a)
    local b = coroutine.yield(a + 1)
    local deeper = coroutine.wrap(function(x) for i = 1, 5 do x = x + i end return x end)
    return b * 2, deeper(b)
  end)
  add(kresume(inner, 10))
  add(kresume(inner, 4))
  add(coroutine.status(inner))
  -- library functions with callbacks into Lua
  local t = {5, 3, 9, 1}
  table.sort(t, function(a, b) return a > b end)
  add(table.concat(t, ","))
  add((string.gsub("abc", "%w", function(ch) return ch:upper() end)))
  add(string.format("%s", setmetatable({}, {__tostring = function() return "ts" end})))
  return out
end

-- the plain run, to compare against
local plain = drive(workload)

-- freezing the coroutine at every budget point it can, for budgets of
-- several sizes, gives the same transcript
for _, n in ipairs{1, 2, 3, 7, 50} do
  local log, freezes = drive(workload, n, function(co, t) return debug.freeze(co) end)
  assert(deepeq(log, plain), "budget " .. n .. ": " .. show(log[#log]) .. " vs " .. show(plain[#plain]))
  assert(freezes > (n == 1 and 300 or 5), "budget " .. n .. ": " .. freezes .. " freezes")
end

-- freezing whatever thread spent the budget (the innermost coroutine,
-- which lands in its resumer: 'kresume' and 'drive' thaw them)
for _, n in ipairs{1, 3, 11} do
  local log, freezes = drive(workload, n, function(co, t)
    if t == co or managed[t] then return debug.freeze(t) end
  end)
  assert(deepeq(log, plain), "inner, budget " .. n .. ": " .. show(log[#log]))
  assert(freezes > 5)
end

-- the statuses of a frozen chain: the coroutine that ran stays running,
-- those that resumed others are normal
do
  local outer, middle, inner
  local seen
  inner = coroutine.create(function() for i = 1, 100 do end return "in" end)
  middle = coroutine.create(function() return kresume(inner) end)
  outer = coroutine.create(function() return select(2, kresume(middle)) end)
  debug.setbudget(10, function(t)
    if t == inner and not seen and debug.freeze(outer) then seen = true end
  end)
  local r = table.pack(coroutine.resume(outer))
  assert(r.n == 1 and r[1] == true and seen)
  assert(debug.isfrozen(outer) and debug.isfrozen(middle) and debug.isfrozen(inner))
  assert(coroutine.status(outer) == "normal")
  assert(coroutine.status(middle) == "normal")
  assert(coroutine.status(inner) == "running")
  collectgarbage() -- (a frozen chain survives a collection)
  r = table.pack(debug.thaw(outer))
  debug.setbudget(0)
  assert(r[1] == true and r[2] == true and r[3] == "in", show(r))
  assert(coroutine.status(outer) == "dead")
  assert(not debug.isfrozen(middle) and not debug.isfrozen(inner))
end

-- freezing a coroutine in the middle of the chain lands in its resumer,
-- which goes on; the rest of the chain stays frozen with it
do
  local outer, middle, inner
  local froze
  inner = coroutine.create(function() for i = 1, 100 do end return "in" end)
  middle = coroutine.create(function() local _, v = coroutine.resume(inner) return v end)
  outer = coroutine.create(function()
    local r = table.pack(coroutine.resume(middle))
    local frozen = debug.isfrozen(middle)
    while debug.isfrozen(middle) do r = table.pack(debug.thaw(middle)) end
    return frozen, r[2]
  end)
  debug.setbudget(10, function(t)
    if t == inner and not froze then froze = debug.freeze(middle) end
  end)
  local ok, frozen, v = coroutine.resume(outer)
  debug.setbudget(0)
  assert(ok and froze and frozen == true and v == "in")
end

-- freezing is refused where a call that cannot freeze is in the way: the
-- callback still runs there
do
  local function refused(name, f)
    local co = coroutine.create(f)
    local runs, freezes, refusals = 0, 0, 0
    local main = coroutine.running()
    debug.setbudget(1, function(t)
      if t ~= main then -- (co, or a coroutine running inside it)
        runs = runs + 1
        if debug.freeze(co) then freezes = freezes + 1 else refusals = refusals + 1 end
      end
    end)
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    debug.setbudget(0)
    assert(r[1], name .. ": " .. tostring(r[2]))
    assert(refusals > 0, name .. ": no refusal")
    return r
  end
  local inside = 0
  local function busy() inside = inside + 1 for i = 1, 20 do end end
  refused("a sort comparator", function() table.sort({3, 2, 1}, function(a, b) busy() return a < b end) end)
  refused("a gsub function", function() string.gsub("ab", ".", function(c) busy() return c end) end)
  refused("a __tostring for format", function()
    string.format("%s", setmetatable({}, {__tostring = function() busy() return "x" end}))
  end)
  refused("a load reader", function()
    local parts = {"return ", "1"}
    local i = 0
    load(function() busy() i = i + 1 return parts[i] end)
  end)
  refused("a message handler", function()
    xpcall(error, function(m) busy() return m end, "x")
  end)
  refused("a hook", function()
    debug.sethook(function() busy() end, "", 1000)
    for i = 1, 3000 do end
    debug.sethook()
  end)
  refused("a finalizer", function()
    setmetatable({}, {__gc = function() busy() end})
    collectgarbage()
  end)
  refused("a __close run by coroutine.close", function()
    local c = coroutine.create(function()
      local x <close> = setmetatable({}, {__close = function() busy() end})
      coroutine.yield()
    end)
    coroutine.resume(c)
    coroutine.close(c)
  end)
  refused("require", function()
    package.preload["freeze_mod"] = function() busy() return true end
    require("freeze_mod")
    package.preload["freeze_mod"], package.loaded["freeze_mod"] = nil, nil
  end)
  assert(inside >= 9)
end

-- a frozen coroutine never thawed is collected like any other
do
  local weak = setmetatable({}, {__mode = "k"})
  do
    local co = coroutine.create(function() for i = 1, 100 do end end)
    weak[co] = true
    local froze
    debug.setbudget(5, function(t) if t == co and not froze then froze = debug.freeze(co) end end)
    coroutine.resume(co)
    debug.setbudget(0)
    assert(froze and debug.isfrozen(co))
  end
  collectgarbage()
  collectgarbage()
  assert(next(weak) == nil)
end

-- hooks see the same events whether or not the coroutine froze between
-- them
do
  local function events(n)
    local log = {}
    local co = coroutine.create(function()
      local function f(x) return x * 2 end
      local s = 0
      for i = 1, 20 do s = s + f(i) end
      return s
    end)
    debug.sethook(co, function(ev, line) log[#log + 1] = ev .. ":" .. tostring(line) end, "crl", 3)
    if n then
      debug.setbudget(n, function(t) if t == co then debug.freeze(co) end end)
    end
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    debug.setbudget(0)
    assert(r[1] and r[2] == 420)
    return log
  end
  local base = events()
  for _, n in ipairs{1, 2, 5} do
    assert(deepeq(events(n), base), "hook events, budget " .. n)
  end
end

-- after a thaw, a coroutine can still yield (inside a pcall too), and its
-- protected calls still catch errors
do
  local co = coroutine.create(function()
    local ok, v = pcall(function()
      for i = 1, 50 do end
      local got = coroutine.yield("in pcall")
      for i = 1, 50 do end
      error("after " .. got, 0)
    end)
    assert(coroutine.isyieldable())
    return ok, v
  end)
  local freezes = 0
  debug.setbudget(2, function(t) if t == co and debug.freeze(co) then freezes = freezes + 1 end end)
  local r = table.pack(kresume(co))
  assert(r[1] and r[2] == "in pcall")
  r = table.pack(kresume(co, "thaw"))
  debug.setbudget(0)
  assert(r[1] and r[2] == false and r[3] == "after thaw", show(r))
  assert(freezes > 20)
end

-- 'thaw' of a coroutine that froze again returns as a freeze did
do
  local co = coroutine.create(function() for i = 1, 300 do end return "end" end)
  local thaws = 0
  debug.setbudget(10, function(t) if t == co then debug.freeze(co) end end)
  local r = table.pack(coroutine.resume(co))
  while debug.isfrozen(co) do
    assert(r.n == 1 and r[1] == true)
    r = table.pack(debug.thaw(co))
    thaws = thaws + 1
  end
  debug.setbudget(0)
  assert(r[1] and r[2] == "end" and thaws > 10)
end

-- a frozen coroutine can be thawed by another coroutine than the one that
-- resumed it
do
  local co = coroutine.create(function() for i = 1, 100 do end return "x" end)
  local once
  debug.setbudget(10, function(t) if t == co and not once then once = debug.freeze(co) end end)
  coroutine.resume(co)
  assert(debug.isfrozen(co))
  local other = coroutine.wrap(function() return debug.thaw(co) end)
  local ok, v = other()
  debug.setbudget(0)
  assert(ok and v == "x")
end
