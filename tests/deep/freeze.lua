-- tests/deep/freeze.lua
-- The budget and freezing ('debug.setbudget', 'debug.freeze',
-- 'debug.thaw'), an extension of this Lua: the budget callback runs every
-- 'n' steps with the user program's top coroutine ('user_co') and the
-- thread that spent the budget, and may freeze the program where it is;
-- 'thaw' goes on with it. Stock Lua 5.5 has none of this (the file returns
-- at once there); with no budget set everything behaves as stock, which
-- the upstream suite and the other deep files check.
--
-- The main thread plays the kernel here (it has no budget); the user
-- programs are coroutines that the kernel gives a budget.

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

-- resume, thawing the coroutine when it freezes (a kernel would do this)
local function kresume(co, ...)
  local r = table.pack(coroutine.resume(co, ...))
  while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
  return table.unpack(r, 1, r.n)
end

-- the debug library has stock's functions and those of the budget and
-- freezing, no others
do
  local stock = {"debug", "gethook", "getinfo", "getlocal", "getmetatable", "getregistry", "getupvalue",
                 "getuservalue", "sethook", "setlocal", "setmetatable", "setupvalue", "setuservalue",
                 "traceback", "upvalueid", "upvaluejoin"}
  local ours = {"setbudget", "getbudget", "freeze", "isfrozen", "canfreeze", "thaw", "persistable"}
  local known = {}
  for _, list in ipairs{stock, ours} do for _, k in ipairs(list) do known[k] = true end end
  for k in pairs(debug) do assert(known[k], k) end
end

-- a thread cannot thaw itself: false and the message, as a refused resume
-- gives them (B12)
do
  local ok, msg = debug.thaw(coroutine.running())
  assert(ok == false and type(msg) == "string", tostring(ok))
  local co = coroutine.create(function() return debug.thaw(coroutine.running()) end)
  local _, ok2, msg2 = coroutine.resume(co)
  assert(ok2 == false and type(msg2) == "string", tostring(ok2))
end

-- a program dropped while frozen in a library function with a buffer of
-- its own ('table.concat' here) gives all its memory back: the heap count
-- comes back to what it was
do
  collectgarbage()
  collectgarbage()
  local before = collectgarbage("count")
  local co = coroutine.create(function()
    local piece = string.rep("x", 1000)
    local t = setmetatable({}, {__index = function() return piece end, __len = function() return 3000 end})
    return table.concat(t)
  end)
  local calls = 0
  debug.setbudget(co, 1000, function()
    calls = calls + 1
    if calls == 6 then debug.freeze() end
  end)
  coroutine.resume(co)
  assert(debug.isfrozen(co))
  co = nil
  collectgarbage()
  collectgarbage()
  local grew = collectgarbage("count") - before
  assert(grew < 100, grew)
end

-- 'debug.persistable(v [, permanents])': whether a persist of the state can
-- write all that 'v' holds (a kernel asks it of a user program); if not,
-- false and why, with the path of what it cannot write
do
  local co = coroutine.create(function(f, y) y() return f end)
  coroutine.resume(co, io.stdout, coroutine.yield)
  local ok, msg = debug.persistable(co)
  assert(ok == false and msg:find("userdata", 1, true) and msg:find("(at root", 1, true), msg)
  assert(debug.persistable(co, {[io.stdout] = "out"}) == true)
  assert(debug.persistable({1, 2, {x = "y"}}) == true)
  -- a thread running now (the kernel) is at rest when the host persists
  assert(debug.persistable((coroutine.running())) == true)
  assert(coroutine.wrap(function() return debug.persistable((coroutine.running())) end)() == true)
end

------------------------------------------------------------------------
-- the budget
------------------------------------------------------------------------

-- 'getbudget': fail with no budget; n, the callback and the steps left
do
  local me = coroutine.running()
  assert(debug.getbudget(me) == nil)
  local f = function() end
  debug.setbudget(me, 1000, f)
  local n, g, left = debug.getbudget(me)
  assert(n == 1000 and g == f and left > 0 and left <= 1000)
  debug.setbudget(me, 0)
  assert(debug.getbudget(me) == nil)
  debug.setbudget(me, 10, f)
  debug.setbudget(me, 10) -- no callback: off
  assert(debug.getbudget(me) == nil)
  assert(not pcall(debug.setbudget, me, -1, f))
  assert(not pcall(debug.setbudget, me, 1, 42))
  assert(not pcall(debug.setbudget, 1, f)) -- the thread is required
  assert(not pcall(debug.getbudget))
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
    debug.setbudget(co, 1, function() calls = calls + 1 end)
    assert(coroutine.resume(co, k))
    assert(calls == instructions(work, k), calls .. " vs " .. instructions(work, k))
  end
end

-- the steps of some code, run in a coroutine with a budget: the steps
-- left before it, after it
local function spent(f)
  local co
  co = coroutine.create(function()
    local _, _, l0 = debug.getbudget(co)
    f()
    local _, _, l1 = debug.getbudget(co)
    return l0 - l1
  end)
  debug.setbudget(co, HUGE, function() error("no call expected") end)
  local ok, d = coroutine.resume(co)
  assert(ok, d)
  return d
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

-- the budget belongs to a thread and to the threads it creates, and counts
-- in every one of them, and in those it resumes while they run
do
  local seen = {}
  local inner = coroutine.wrap(function() for i = 1, 30 do end return 1 end)
  local outer = coroutine.create(function() for i = 1, 30 do end return inner() end)
  debug.setbudget(outer, 1, function(user_co, spender)
    assert(user_co == outer)
    seen[spender] = (seen[spender] or 0) + 1
  end)
  assert(coroutine.resume(outer))
  local threads = 0
  for _ in pairs(seen) do threads = threads + 1 end
  assert(threads == 2 and seen[outer] > 30) -- outer and the wrapped one
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
  debug.setbudget(co, 5, function(user_co, spender)
    depth = depth + 1
    maxdepth = math.max(maxdepth, depth)
    calls = calls + 1
    for i = 1, 100 do end -- more than the budget, and lines
    local n, f, left = debug.getbudget(user_co)
    assert(n == 5 and left == 5)
    depth = depth - 1
  end)
  assert(coroutine.resume(co))
  debug.setbudget(co, 0)
  assert(maxdepth == 1 and calls > 10)
  local plain = 0
  local co2 = coroutine.create(body)
  debug.sethook(co2, function() plain = plain + 1 end, "l")
  assert(coroutine.resume(co2))
  assert(hookcalls == plain) -- no line events from the callback's code
end

-- a program that the callback runs (another budget group) gets no callback
-- of its own inside it, also where C work of the table functions spends its
-- budget; the outer callback can still freeze its own program after it
do
  local incallback, nested, othercalls = false, 0, 0
  local other = coroutine.create(function()
    while true do
      table.move({}, 1, 50, 1) -- (C work, with check points)
      table.insert({}, 1, 0)
      for i = 1, 20 do end
      coroutine.yield()
    end
  end)
  debug.setbudget(other, 3, function()
    othercalls = othercalls + 1
    if incallback then nested = nested + 1 end
  end)
  local calls = 0
  local co = coroutine.create(function() for i = 1, 100 do end return "end" end)
  debug.setbudget(co, 7, function()
    calls = calls + 1
    incallback = true
    for i = 1, 5 do assert(coroutine.resume(other)) end
    incallback = false
    if calls == 2 then debug.freeze() end
  end)
  assert(coroutine.resume(co))
  assert(debug.isfrozen(co) and calls == 2 and nested == 0, nested)
  assert(select(2, debug.thaw(co)) == "end")
  assert(coroutine.resume(other)) -- its own callbacks come back outside
  assert(othercalls > 0)
end

-- the callback's thread has its stack and frames reserved, and the
-- collector does not trim them: a callback that calls deep allocates
-- neither (the memory in use does not change), also after collections
do
  local function rec(n) if n > 0 then return 1 + rec(n - 1) else return 0 end end
  local co = coroutine.create(function() for i = 1, 300 do end end)
  local calls, delta = 0, 0
  debug.setbudget(co, 10, function()
    calls = calls + 1
    collectgarbage("stop")
    local a = collectgarbage("count")
    rec(30)
    delta = delta + (collectgarbage("count") - a)
    collectgarbage("restart")
  end)
  for i = 1, 3 do collectgarbage() end
  assert(coroutine.resume(co))
  assert(calls > 10 and delta == 0, calls .. " " .. delta)
end

-- an error in the callback destroys the chain: the coroutines of the
-- running chain are dead, with the error, and nothing in them catches it
do
  local calls = 0
  local caught = false
  local inner = coroutine.create(function() for i = 1, 100 do end end)
  local co
  co = coroutine.create(function()
    local ok = pcall(function()
      local r = coroutine.resume(inner)
      caught = true -- (nothing after the error runs)
    end)
    caught = true
  end)
  debug.setbudget(co, 3, function()
    calls = calls + 1
    if calls == 20 then error("budget error", 0) end
  end)
  local ok, err = coroutine.resume(co)
  assert(not ok and err == "budget error" and calls == 20)
  assert(not caught)
  assert(coroutine.status(co) == "dead" and coroutine.status(inner) == "dead")
  assert(not debug.isfrozen(co))
  -- an error object that is not a string, and the dead coroutine closes
  local obj = {}
  co = coroutine.create(function() for i = 1, 100 do end end)
  debug.setbudget(co, 3, function() error(obj) end)
  ok, err = coroutine.resume(co)
  assert(not ok and err == obj)
  local ok2, err2 = coroutine.close(co)
  assert(ok2 == false and err2 == obj)
  -- the budget group goes on for other threads
  local co3 = coroutine.create(function() for i = 1, 10 do end return "fine" end)
  assert(select(2, coroutine.resume(co3)) == "fine")
end

-- the pending '__close' of the callback run as it leaves, when the callback
-- is already over: a 'freeze' there is an error, which destroys the chain
-- (as any error of those '__close' does)
do
  local closed = 0
  local co = coroutine.create(function() for i = 1, 100 do end end)
  debug.setbudget(co, 5, function()
    local x <close> = setmetatable({}, {__close = function() closed = closed + 1 debug.freeze() end})
    local y <close> = setmetatable({}, {__close = function() closed = closed + 1 end})
    debug.freeze()
  end)
  local ok, err = coroutine.resume(co)
  assert(not ok and err:find("freeze outside the budget callback", 1, true), err)
  assert(closed == 2 and coroutine.status(co) == "dead" and not debug.isfrozen(co))
end

-- turning the budget off from the callback
do
  local calls = 0
  local co = coroutine.create(function() for i = 1, 100 do end end)
  debug.setbudget(co, 2, function(user_co) calls = calls + 1 debug.setbudget(user_co, 0) end)
  assert(coroutine.resume(co))
  assert(calls == 1 and debug.getbudget(co) == nil)
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
    debug.setbudget(co, 7, function(user_co, spender)
      if spender == co then
        local info = debug.getinfo(spender, 0, "Sl")
        where[#where + 1] = info.short_src .. ":" .. info.currentline
      end
    end)
    assert(coroutine.resume(co))
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
  debug.setbudget(co, 20, function(user_co, spender)
    assert(user_co == co and spender == co)
    if froze == 0 then froze = 1 debug.freeze() end
    froze = froze + 100 -- (nothing after 'freeze' runs: not at the first call)
  end)
  local r = table.pack(coroutine.resume(co, "x", "y"))
  assert(r.n == 1 and r[1] == true) -- as if it returned nothing
  assert(debug.isfrozen(co) and froze == 1)
  assert(coroutine.status(co) == "running") -- as when it stopped
  local ok, err = coroutine.resume(co)
  assert(not ok and err == "cannot resume non-suspended coroutine")
  assert(not pcall(coroutine.close, co))
  r = table.pack(debug.thaw(co))
  assert(r.n == 4 and r[1] and r[2] == 5050 and r[3] == "x" and r[4] == "y", show(r))
  assert(not debug.isfrozen(co) and coroutine.status(co) == "dead")
  local ok2, err2 = debug.thaw(co)
  assert(not ok2 and err2 == "cannot thaw a coroutine that is not frozen")
end

-- 'freeze' takes no argument and never returns; only in the callback
do
  local ok, err = pcall(debug.freeze)
  assert(not ok and err:find("freeze outside the budget callback"))
  assert(not pcall(debug.isfrozen, 42))
  assert(not pcall(debug.thaw, 42))
  -- it leaves the callback as an error does, but a protected call there
  -- does not catch it; what is pending there is closed first
  local log = {}
  local co = coroutine.create(function() for i = 1, 50 do end return "end" end)
  debug.setbudget(co, 10, function()
    if #log > 0 then return end
    local x <close> = setmetatable({}, {__close = function() log[#log + 1] = "closed" end})
    log[#log + 1] = "start"
    local ok = pcall(function() debug.freeze() end)
    log[#log + 1] = "after pcall " .. tostring(ok)
    local ok2 = xpcall(debug.freeze, print)
    log[#log + 1] = "after xpcall"
  end)
  local r = table.pack(coroutine.resume(co))
  assert(r.n == 1 and r[1] and debug.isfrozen(co))
  assert(deepeq(log, {"start", "closed"}), show(log))
  r = table.pack(debug.thaw(co))
  assert(r[1] and r[2] == "end", show(r))
end

-- only unbudgeted code thaws, not the callback and not a budgeted thread
do
  local co = coroutine.create(function() for i = 1, 100 do end return "x" end)
  local inside
  debug.setbudget(co, 10, function(user_co, spender)
    if inside == nil then
      inside = {pcall(debug.thaw, user_co)}
      debug.freeze()
    end
  end)
  coroutine.resume(co)
  assert(debug.isfrozen(co))
  assert(inside[1] == false and inside[2]:find("cannot thaw from budgeted code"), show(inside))
  local r = table.pack(coroutine.resume(coroutine.create(function() return pcall(debug.thaw, co) end)))
  assert(r[2] == true and r[3] == true and r[4] == "x", show(r)) -- (an unbudgeted coroutine can)
  assert(not debug.isfrozen(co))
  -- a budgeted one cannot
  local co2 = coroutine.create(function() for i = 1, 100 do end return "y" end)
  local once = false
  debug.setbudget(co2, 10, function() if not once then once = true debug.freeze() end end)
  coroutine.resume(co2)
  assert(debug.isfrozen(co2))
  local b = coroutine.create(function() return pcall(debug.thaw, co2) end)
  debug.setbudget(b, HUGE, function() end)
  r = table.pack(coroutine.resume(b))
  assert(r[1] and r[2] == false and tostring(r[3]):find("cannot thaw from budgeted code"), show(r))
  assert(debug.isfrozen(co2))
  assert(select(2, debug.thaw(co2)) == "y")
end

-- runs 'f' in a coroutine to its end, answering its yields with their
-- values doubled, thawing it when it froze: a transcript of what it
-- yielded and returned. 'freeze', called from the budget callback, tells
-- whether to freeze.
local function drive(f, n, freeze)
  local co = coroutine.create(f)
  local log, freezes = {}, 0
  if n then
    debug.setbudget(co, n, function(user_co, spender)
      if freeze(user_co, spender) then
        freezes = freezes + 1
        debug.freeze()
      end
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
  return log, freezes
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

-- freezing the coroutine at every budget point, for budgets of several
-- sizes, gives the same transcript
for _, n in ipairs{1, 2, 3, 7, 50} do
  local log, freezes = drive(workload, n, function() return true end)
  assert(deepeq(log, plain), "budget " .. n .. ": " .. show(log[#log]) .. " vs " .. show(plain[#plain]))
  assert(freezes > (n == 1 and 300 or 5), "budget " .. n .. ": " .. freezes .. " freezes")
end

-- the statuses of a frozen chain: the coroutine that ran stays running,
-- those that resumed others are normal; the whole chain from the program's
-- top coroutine down is frozen
do
  local outer, middle, inner
  local seen
  inner = coroutine.create(function() for i = 1, 100 do end return "in" end)
  middle = coroutine.create(function() return kresume(inner) end)
  outer = coroutine.create(function() return select(2, kresume(middle)) end)
  debug.setbudget(outer, 10, function(user_co, spender)
    if spender == inner and not seen then
      assert(user_co == outer)
      seen = true
      debug.freeze()
    end
  end)
  local r = table.pack(coroutine.resume(outer))
  assert(r.n == 1 and r[1] == true and seen)
  assert(debug.isfrozen(outer) and debug.isfrozen(middle) and debug.isfrozen(inner))
  assert(coroutine.status(outer) == "normal")
  assert(coroutine.status(middle) == "normal")
  assert(coroutine.status(inner) == "running")
  collectgarbage() -- (a frozen chain survives a collection)
  -- only the program's top coroutine thaws; the others are read-only
  for _, t in ipairs{middle, inner} do
    local ok, err = debug.thaw(t)
    assert(ok == false and err == "cannot thaw a coroutine that is not the freeze target", err)
    assert(not pcall(coroutine.close, t))
    assert(debug.isfrozen(t))
  end
  assert(debug.isfrozen(outer))
  r = table.pack(debug.thaw(outer))
  assert(r[1] == true and r[2] == true and r[3] == "in", show(r))
  assert(coroutine.status(outer) == "dead")
  assert(not debug.isfrozen(middle) and not debug.isfrozen(inner))
end

-- a freeze goes to the program's top coroutine and no further: its
-- unbudgeted resumer (the kernel) goes on, and thaws it
do
  local kernel, user, inner
  local froze
  inner = coroutine.create(function() for i = 1, 100 do end return "in" end)
  user = coroutine.create(function() local _, v = coroutine.resume(inner) return v end)
  kernel = coroutine.create(function()
    local r = table.pack(coroutine.resume(user))
    local frozen = debug.isfrozen(user)
    while debug.isfrozen(user) do r = table.pack(debug.thaw(user)) end
    return frozen, r[2]
  end)
  debug.setbudget(user, 10, function(user_co, spender)
    if spender == inner and not froze then froze = true debug.freeze() end
  end)
  local ok, frozen, v = coroutine.resume(kernel)
  assert(ok and froze and frozen == true and v == "in")
end

-- the kernel closes a program's coroutine: its '__close' runs under the
-- program's budget, and a freeze there lands in the kernel's close as in a
-- resume (the close gives true; isfrozen tells); a thaw finishes the close
do
  local closed, inclose = 0, false
  local co = coroutine.create(function()
    local x <close> = setmetatable({}, {__close = function()
      inclose = true
      for i = 1, 100 do end
      closed = closed + 1
    end})
    coroutine.yield()
  end)
  debug.setbudget(co, 5, function() if inclose then inclose = false debug.freeze() end end)
  assert(coroutine.resume(co))
  assert(coroutine.close(co) == true)
  assert(debug.isfrozen(co) and closed == 0)
  assert(coroutine.status(co) == "running")
  local r = table.pack(debug.thaw(co))
  assert(r.n == 1 and r[1] == true)
  assert(not debug.isfrozen(co) and coroutine.status(co) == "dead" and closed == 1)
  -- the same in the close of a wrapped coroutine that failed: the function
  -- from 'coroutine.wrap' gives no values, as when a resume lands there
  local w
  closed = 0
  local maker = coroutine.create(function()
    w = coroutine.wrap(function()
      local x <close> = setmetatable({}, {__close = function()
        inclose = true
        for i = 1, 100 do end
        closed = closed + 1
      end})
      error("boom")
    end)
  end)
  debug.setbudget(maker, 5, function() if inclose then inclose = false debug.freeze() end end)
  assert(coroutine.resume(maker)) -- ('w''s coroutine is in maker's group)
  r = table.pack(w())
  assert(r.n == 0 and closed == 0)
  assert(not pcall(w)) -- (frozen: it cannot be resumed)
end

-- a freeze that cannot happen destroys the chain: where a call that cannot
-- freeze is in the way, the coroutines of the chain are dead, the kernel
-- gets "cannot freeze" from its resume, and nothing in the chain catches it
do
  local function impossible(name, f)
    local caught = false
    local co = coroutine.create(function(busy)
      local ok = pcall(f, busy)
      caught = true -- (never runs)
    end)
    local inside = 0
    local function busy() inside = inside + 1 for i = 1, 20 do end end
    debug.setbudget(co, 1, function() debug.freeze() end)
    local ok, err = kresume(co, busy)
    assert(ok == false and err == "cannot freeze", name .. ": " .. tostring(ok) .. " " .. tostring(err))
    assert(not caught and coroutine.status(co) == "dead" and not debug.isfrozen(co), name)
  end
  -- a nested chain: every budgeted coroutine from the one that spent the
  -- budget to the program's top is dead; the kernel's coroutine is not
  local inner = coroutine.create(function() for i = 1, 50 do end end)
  local top = coroutine.create(function() return coroutine.resume(inner) end)
  local kernel = coroutine.create(function() return coroutine.resume(top) end)
  debug.setbudget(top, 5, function(user_co, spender) if spender == inner then error("boom", 0) end end)
  local r = table.pack(coroutine.resume(kernel))
  assert(r[1] == true and r[2] == false and r[3] == "boom", show(r))
  assert(coroutine.status(inner) == "dead" and coroutine.status(top) == "dead")
  assert(coroutine.status(kernel) == "dead") -- (it returned)
end

-- 'canfreeze': whether a freeze here would happen (a budget in effect and
-- no call that cannot freeze in the way)
do
  local can, why = debug.canfreeze() -- the kernel runs with no budget
  assert(can == false and why == "no budget in effect")
  assert(select("#", debug.canfreeze()) == 2)
  local r = {}
  local co = coroutine.create(function()
    r.plain = debug.canfreeze()
    assert(select("#", debug.canfreeze()) == 1) -- (true alone)
    r.pcall = select(2, pcall(debug.canfreeze))
    r.wrap = coroutine.wrap(function() return debug.canfreeze() end)()
    r.resume = select(2, coroutine.resume(coroutine.create(debug.canfreeze)))
    table.sort({3, 2, 1}, function(a, b) r.sort = debug.canfreeze() return a < b end)
    r.gsub = string.gsub("a", "a", function() r.gsubv = debug.canfreeze() end)
    r.mm = setmetatable({}, {__index = function() r.index = debug.canfreeze() end}).x
    r.load = load(function() r.reader = debug.canfreeze() return nil end)
  end)
  debug.setbudget(co, 1 << 40, function() end)
  assert(coroutine.resume(co))
  for _, k in ipairs{"plain", "pcall", "wrap", "resume", "sort", "gsubv", "index", "reader"} do
    assert(r[k] == true, k)
  end
  -- the same coroutine with no budget: false everywhere
  local r2 = {}
  local co2 = coroutine.create(function() r2.plain, r2.why = debug.canfreeze() end)
  assert(coroutine.resume(co2) and r2.plain == false and r2.why == "no budget in effect")
  -- in the callback: whether freeze() will freeze, or destroy the chain
  local told = {}
  local co3 = coroutine.create(function() for i = 1, 20 do end end)
  debug.setbudget(co3, 5, function() told[#told + 1] = debug.canfreeze() debug.freeze() end)
  kresume(co3)
  assert(#told > 0)
  for _, v in ipairs(told) do assert(v == true) end
end

-- 'f', run in a coroutine with budgets of several sizes and frozen at
-- every budget point, is never destroyed and ends as a plain run does;
-- 'f' gets 'busy', which its callbacks call
local everywhere
do
  local inside = 0
  local function busy() inside = inside + 1 for i = 1, 5 do end end
  local function run(f, n)
    local co = coroutine.create(f)
    local freezes = 0
    if n then
      debug.setbudget(co, n, function() freezes = freezes + 1 debug.freeze() end)
    end
    local r = table.pack(coroutine.resume(co, busy))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    return r, freezes
  end
  function everywhere(name, f, budgets)
    inside = 0
    local plain = run(f)
    assert(plain[1], name .. ": " .. tostring(plain[2]))
    assert(inside > 0, name .. ": no callback ran")
    for _, n in ipairs(budgets or {1, 2, 3, 7}) do
      local r, freezes = run(f, n)
      assert(freezes > 0, name .. ", budget " .. n .. ": no freeze")
      assert(deepeq(r, plain), name .. ", budget " .. n .. ": " .. show(r) .. " vs " .. show(plain))
    end
  end
end

local function mt(fields) return setmetatable({}, fields) end

-- the cases of 'libcases.lua': code the library calls back
do
  local dir = debug.getinfo(1, "S").source:match("^@(.*[/\\])") or ""
  for _, c in ipairs(dofile(dir .. "libcases.lua")) do everywhere(c.name, c.f) end
end

-- message handlers that fail, in coroutines, with errors and yields after
-- them, frozen at every point: a handler that fails leaves nothing behind
do
  everywhere("a failing handler, then another error", function(busy)
    busy()
    local co = coroutine.create(function()
      local a, b = xpcall(5, function(m) error("H1") end)
      local c, d = xpcall(function() error("E2") end, function(m) return "ok:" .. m end)
      return tostring(a) .. "|" .. tostring(b) .. "|" .. tostring(c) .. "|" .. tostring(d)
    end)
    return coroutine.resume(co)
  end)
  everywhere("a failing handler of an erroring function", function(busy)
    busy()
    local co = coroutine.create(function()
      local a, b = xpcall(function() error("E1") end, function(m) error("H1") end)
      local c, d = xpcall(5, function(m) return "ok:" .. m end)
      return tostring(a) .. "|" .. tostring(b) .. "|" .. tostring(c) .. "|" .. tostring(d)
    end)
    return coroutine.resume(co)
  end)
  everywhere("a failing handler, then a yield", function(busy)
    busy()
    local co = coroutine.create(function()
      local a, b = xpcall(5, function(m) error("H1") end)
      coroutine.yield("y")
      local c, d = pcall(error, "E2")
      return tostring(a) .. "|" .. tostring(b) .. "|" .. tostring(c) .. "|" .. tostring(d)
    end)
    local r1, r2 = coroutine.resume(co)
    local r3, r4 = coroutine.resume(co)
    return tostring(r1) .. tostring(r2) .. "//" .. tostring(r3) .. tostring(r4)
  end)
  everywhere("a handler that calls nil", function(busy)
    busy()
    local co = coroutine.create(function()
      local a, b = xpcall(5, function(m) local x = nil; x() end)
      return tostring(a) .. "|" .. tostring(b)
    end)
    return coroutine.resume(co)
  end)
end

-- the C work of library loops counts, a step an element, and they freeze
-- in their middle, at their check points: with nothing that calls back
-- (plain tables, values compared in C), freezing only there
do
  local function inside(fn, work)
    local co = coroutine.create(work)
    local frozen = 0
    debug.setbudget(co, 5, function(user_co, spender)
      if debug.getinfo(spender, 0, "f").func == fn then
        frozen = frozen + 1
        debug.freeze()
      end
    end)
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    assert(r[1], r[2])
    local plain = table.pack(select(2, coroutine.resume(coroutine.create(work))))
    table.remove(r, 1)
    r.n = r.n - 1
    return frozen, r, plain
  end
  local N = 1000
  local function filled() local t = {} for i = 1, N do t[i] = (i * 37) % 101 end return t end
  local works = {
    {table.concat, function() return table.concat(filled(), ",") end},
    {table.insert, function() local t = filled() table.insert(t, 1, "x") return t[1], t[2], t[N + 1], #t end},
    {table.remove, function() local t = filled() local v = table.remove(t, 1) return v, t[1], t[N - 1], #t end},
    {table.move, function() local t = table.move(filled(), 1, N, 3, {}) return t[3], t[N + 2], #t end},
    {table.move, function() local t = filled() table.move(t, 1, N - 1, 2) return t[1], t[2], t[N] end},
    {table.unpack, function() return select("#", table.unpack(filled())), (table.unpack(filled(), N)) end},
    {table.sort, function() local t = filled() table.sort(t) return t[1], t[N // 2], t[N] end},
  }
  for i, w in ipairs(works) do
    local frozen, r, plain = inside(w[1], w[2])
    assert(frozen >= N // 10, "work " .. i .. ": " .. frozen .. " freezes")
    assert(deepeq(r, plain), "work " .. i .. ": " .. show(r) .. " vs " .. show(plain))
  end
end

-- work on long strings counts a step for 64 bytes once it is longer than a
-- slice (256 KB), and freezes between its slices
do
  local function inside(fn, work)
    local co = coroutine.create(work)
    local frozen = 0
    debug.setbudget(co, 1000, function(user_co, spender)
      if debug.getinfo(spender, 0, "f").func == fn then
        frozen = frozen + 1
        debug.freeze()
      end
    end)
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    assert(r[1], r[2])
    local plain = table.pack(coroutine.resume(coroutine.create(work)))
    return frozen, r, plain
  end
  local M = 3 << 20
  local big = string.rep("aB\n\"\\\1z\200\255", M // 10)
  local utf = string.rep("x€ü", M // 6)
  local cont = "a" .. string.rep("\x80", M)
  local digits = string.rep("7", M)
  local spaced = string.rep(" ", M // 2) .. "-1f" .. string.rep("\t", M // 2)
  local proxy = setmetatable({}, {__index = function(_, i) return i == 2 and big or "e" .. i end,
                                  __len = function() return 3 end})
  local works = {
    {string.rep, function() local s = string.rep("x", M) return #s, s == string.rep("xxxxxxxx", M // 8) end},
    {string.rep, function() local s = string.rep("ab", M // 3, ",") return #s, s:sub(-5), s:sub(M // 2, M // 2 + 4) end},
    {string.rep, function() local s = string.rep(string.rep("q", M // 2) .. "!", 2, "--") return #s, s:sub(M // 2 - 1, M // 2 + 5) end},
    {string.rep, function() local s = string.rep("x", 2.0 ^ 21) return #s, s:sub(-3) end},
    {string.rep, function() local s = string.rep("yz", "1048576", ",") return #s, s:sub(-3) end},
    {string.upper, function() local s = big:upper() return #s, s:sub(1, 20), s:sub(-20) end},
    {string.lower, function() local s = big:lower() return #s, s:sub(1, 20), s:sub(-20) end},
    {string.reverse, function() local s = big:reverse() return #s, s:sub(1, 20), s:sub(M // 2, M // 2 + 9) end},
    {string.format, function() local s = string.format("[%q]", big) return #s, s:sub(1, 30), s:sub(-30) end},
    {string.format, function() local s = string.format("%s|%5s|%-3s", big, big, big) return #s, s:sub(M - 5, M + 15) end},
    {string.format, function() local s = string.format(big .. "%%%d" .. big, 42) return #s, s:sub(#big - 3, #big + 6) end},
    {string.find, function() return big:find("y", 1, true), (big .. "yy"):find("yy", 2, true) end, 4},
    {utf8.len, function() return utf8.len(utf), utf8.len(utf, 3, -4), utf8.len(cont, 1, -1, true) end},
    {utf8.offset, function() return utf8.offset(utf, M // 3), utf8.offset(utf, -(M // 4)), utf8.offset(cont, 0, #cont) end},
    {utf8.offset, function() return utf8.offset(cont, 1), utf8.offset("€" .. cont:sub(2), 1) end, 4},
    {table.concat, function() local s = table.concat({"a", big, "b", big}, ",") return #s, s:sub(#big - 2, #big + 6) end},
    {table.concat, function() local s = table.concat({"a", "b", "c"}, big) return #s, s:sub(1, 3), s:sub(-3) end},
    {table.concat, function() local s = table.concat(proxy, "|") return #s, s:sub(1, 5), s:sub(-5) end},
    {tonumber, function() return tonumber(digits, 8), tonumber(spaced, 16), tonumber(digits .. "9", 9) end},
  }
  for i, w in ipairs(works) do
    local frozen, r, plain = inside(w[1], w[2])
    assert(frozen >= (w[3] or 8), "slices " .. i .. ": " .. frozen .. " freezes")
    assert(deepeq(r, plain), "slices " .. i .. ": " .. show(r) .. " vs " .. show(plain))
  end
end

-- pattern matching counts a step for each call of its matcher (the
-- calls a backtracking pattern makes are its work), and freezes there, deep
-- in the backtracking too; a thawed match ends as one that did not freeze,
-- also with an error
do
  local function infind(fn, work)
    local co = coroutine.create(work)
    local frozen = 0
    debug.setbudget(co, 3, function(user_co, spender)
      if debug.getinfo(spender, 0, "f").func == fn then
        frozen = frozen + 1
        debug.freeze()
      end
    end)
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    local plain = table.pack(coroutine.resume(coroutine.create(work)))
    return frozen, r, plain
  end
  local subj = string.rep("a", 16) .. "b" .. string.rep("ab", 6) .. "(x(y)z)"
  local deep = string.rep("a", 180)
  local it
  local works = {
    {string.find, function() return subj:find("a-a-a-a-b") end},
    {string.find, function() return subj:find("((a-)(a*)b)(%b())", 3) end},
    {string.find, function() return (string.rep("a", 3000) .. "b"):find(string.rep("a", 300) .. "b", 1, true) end},
    {string.find, function() return (string.rep("ab", 1500)):find(string.rep("ab", 100) .. "c") end},
    {string.match, function() return subj:match("^(a+)(.-)%f[%(](.-)$") end},
    {string.match, function() return subj:match("(a?a?a?b)()(ab)%3%3") end},
    {string.match, function() return ("x"):rep(30):match("(x*)(x*)(x*)y") end},
    {string.match, function() return deep:match(string.rep("a?", 180) .. "$") end},
    {string.match, function() return pcall(string.match, deep, string.rep("a?", 200) .. "$") end},
    {string.match, function() return pcall(string.match, subj, "a-a-a-a-b%2") end},
    {string.gsub, function() return subj:gsub("(a-)b", "<%1>") end},
    {string.gsub, function() return subj:gsub("(a*)(b?)", "%2%1", 5) end},
    {string.gsub, function() return subj:gsub("a-b", function(m) return #m end) end},
    {string.gsub, function() return subj:gsub("(a+)b", {aaaa = "4", ab = "1"}) end},
    {"gmatch", function()
      local t = {}
      it = subj:gmatch("(a-)(b)")
      for a, b in it do t[#t + 1] = #a .. b end
      return table.concat(t, ",")
    end},
  }
  for i, w in ipairs(works) do
    local fn = w[1]
    if fn == "gmatch" then
      fn = nil
      local inner = w[2]
      w[2] = function() return inner() end
      local co = coroutine.create(w[2])
      local frozen = 0
      debug.setbudget(co, 3, function(user_co, spender)
        if debug.getinfo(spender, 0, "f").func == it then
          frozen = frozen + 1
          debug.freeze()
        end
      end)
      local r = table.pack(coroutine.resume(co))
      while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
      local plain = table.pack(coroutine.resume(coroutine.create(inner)))
      assert(frozen >= 10, "gmatch: " .. frozen .. " freezes")
      assert(deepeq(r, plain), "gmatch: " .. show(r) .. " vs " .. show(plain))
    else
      local frozen, r, plain = infind(fn, w[2])
      assert(frozen >= 10, "match work " .. i .. ": " .. frozen .. " freezes")
      assert(deepeq(r, plain), "match work " .. i .. ": " .. show(r) .. " vs " .. show(plain))
    end
  end
  -- a gmatch iterator called again from the budget callback, in the middle
  -- of one of its matches, makes a match of its own (B27): the inner call
  -- takes the first match, and the call it was called in goes on with its
  -- own state to the next one (before, both used the iterator's state, and
  -- the outer call returned what the inner one left of it)
  do
    local s = string.rep("a", 40) .. "b ab"
    local it = s:gmatch("(a-)(b)")
    local inner
    local co = coroutine.create(function() return it() end)
    debug.setbudget(co, 3, function(user_co, spender)
      if inner == nil and debug.getinfo(spender, 0, "f").func == it then inner = table.pack(it()) end
    end)
    local r = table.pack(coroutine.resume(co))
    assert(inner and inner.n == 2 and inner[1] == string.rep("a", 40) and inner[2] == "b", "inner: " .. show(inner))
    assert(r[1] and r.n == 3 and r[2] == "a" and r[3] == "b", "outer: " .. show(r))
  end
  -- the budget runs out in a match that backtracks (or a plain search that
  -- compares at many places): before check points it ran to its end with no
  -- callback at all
  local co = coroutine.create(function() return string.rep("a", 22):find("a-a-a-a-a-b") end)
  local calls = 0
  debug.setbudget(co, 100, function(user_co, spender)
    if debug.getinfo(spender, 0, "f").func == string.find then calls = calls + 1 end
  end)
  assert(coroutine.resume(co))
  assert(calls > 500, calls)
end

-- 'debug.debug' freezes in the commands it runs and in the '__tostring'
-- of an error it reports (a child interpreter reads the commands)
if io.popen and arg and arg[-1] then
  local input = os.tmpname()
  local f = assert(io.open(input, "w"))
  f:write("local s = 0 for i = 1, 100 do s = s + i end io.write(s, ' ')\n",
          "error(setmetatable({}, {__tostring = function() local n = 0 ",
          "for i = 1, 50 do n = n + i end io.write(n, ' ') return 'x' end}))\n",
          "cont\n")
  f:close()
  local script = [[
    local co = coroutine.create(function() debug.debug() return "done" end)
    local freezes = 0
    debug.setbudget(co, 3, function() freezes = freezes + 1 debug.freeze() end)
    local r = {coroutine.resume(co)}
    while debug.isfrozen(co) do r = {debug.thaw(co)} end
    io.write(tostring(freezes > 50), " ", tostring(r[2]))
  ]]
  local p = io.popen(string.format("%q -e %q < %q 2>/dev/null", arg[-1], script, input))
  local out = p:read("a")
  p:close()
  os.remove(input)
  assert(out == "5050 1275 true done", out)
end

-- calls that cannot yield stay so through a freeze and a thaw, nested
-- too; after them the coroutine yields again
do
  local log = {}
  local co = coroutine.create(function()
    local inner = mt{__index = function(_, i)
      for k = 1, 20 do end
      log[#log + 1] = coroutine.isyieldable()
      log[#log + 1] = select(2, pcall(coroutine.yield))
      if i <= 2 then return i end
    end}
    local s = tostring(mt{__tostring = function()
      local n = 0
      for _, v in ipairs(inner) do n = n + v end
      log[#log + 1] = coroutine.isyieldable()
      return "n=" .. n
    end})
    log[#log + 1] = coroutine.isyieldable()
    return s, coroutine.yield("after")
  end)
  local freezes = 0
  debug.setbudget(co, 3, function() freezes = freezes + 1 debug.freeze() end)
  local r = table.pack(kresume(co))
  assert(r[1] and r[2] == "after", show(r))
  assert(coroutine.status(co) == "suspended")
  r = table.pack(kresume(co, "y"))
  assert(r[1] and r[2] == "n=3" and r[3] == "y", show(r))
  assert(freezes > 10)
  local cannot = "attempt to yield across a C-call boundary"
  assert(deepeq(log, {false, cannot, false, cannot, false, cannot, false, true}), show(log))
end

-- a kernel '__gc' runs on the thread that collects, as in stock Lua
do
  local err
  setmetatable({}, {__gc = function() err = select(2, pcall(coroutine.yield)) end})
  collectgarbage()
  assert(err == "attempt to yield from outside a coroutine")
end

-- an error raised after a thaw inside a call that cannot yield, caught
-- by a protected call further down, leaves the coroutine as yieldable as
-- before the call
do
  local calls = {
    function(f) return pcall(table.sort, {3, 2, 1}, f) end,
    function(f) return pcall(string.gsub, "ab", ".", f) end,
    function(f) return pcall(tostring, setmetatable({}, {__tostring = f})) end,
    function(f) return pcall(function() for _ in ipairs(setmetatable({}, {__index = f})) do end end) end,
  }
  for i, call in ipairs(calls) do
    local inside = false
    local co = coroutine.create(function()
      local ok, err = call(function() inside = true for k = 1, 50 do end error("x", 0) end)
      inside = false
      local y = coroutine.isyieldable()
      return ok, err, y, coroutine.yield("y")
    end)
    local froze = 0
    debug.setbudget(co, 10, function()
      if inside then froze = froze + 1 debug.freeze() end
    end)
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    assert(froze > 0 and r[1] and r[2] == "y", i .. ": " .. show(r))
    debug.setbudget(co, 0)
    r = table.pack(coroutine.resume(co, "z"))
    assert(r[1] and r[2] == false and r[3] == "x" and r[4] == true and r[5] == "z", i .. ": " .. show(r))
  end
end

-- the Lua callback runs on a thread of its own, with the program's top
-- coroutine and the thread that spent the budget as its arguments (the
-- spender reads "normal" there)
do
  local main = coroutine.running()
  local seen = {}
  local co = coroutine.create(function() for i = 1, 30 do end end)
  debug.setbudget(co, 10, function(user_co, spender)
    if not seen.cb then
      seen.cb = coroutine.running()
      seen.user = user_co
      seen.status = coroutine.status(spender)
      seen.where = debug.getinfo(spender, 0, "S").what
    end
  end)
  assert(coroutine.resume(co))
  assert(seen.cb ~= co and seen.cb ~= main and type(seen.cb) == "thread")
  assert(seen.user == co)
  assert(seen.status == "normal" and seen.where == "Lua")
end

-- a freeze changes nothing about where a C stack overflow or a stack
-- overflow happens: the same depths with freezes at every budget point
-- (the callback uses neither the stack nor the C calls of the program)
do
  local function depths(work, budget)
    collectgarbage()
    local co = coroutine.create(work)
    if budget then
      debug.setbudget(co, budget, function() debug.freeze() end)
    end
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    return r
  end
  local works = {
    function() -- a message handler that fails again and again
      local depth = 0
      local ok, err = xpcall(error, function(m) depth = depth + 1 error(m) end, "x")
      return depth, ok, (tostring(err):gsub("^[^:]*:%d+: ", ""))
    end,
    function() -- '__index' calling itself through C
      local depth = 0
      local t = setmetatable({}, {__index = function(t, k) depth = depth + 1 return t[k] end})
      local ok, err = pcall(function() return t.x end)
      return depth, ok, (tostring(err):gsub("^[^:]*:%d+: ", ""))
    end,
    function() -- coroutines resuming coroutines
      local depth = 0
      local function nest()
        depth = depth + 1
        local w = coroutine.wrap(nest)
        return w()
      end
      local ok, err = pcall(nest)
      return depth, ok, (tostring(err):gsub("^[^:]*:%d+: ", ""))
    end,
    function() -- a deep Lua recursion (big frames: fewer of them)
      local locals = {}
      for i = 1, 150 do locals[i] = "l" .. i end
      local rec = load("local depth = 0 local function rec() depth = depth + 1 local " ..
                       table.concat(locals, ", ") .. " = 1 return 1 + rec() end " ..
                       "return function() local ok, err = pcall(rec) return depth, ok, err end")()
      local depth, ok, err = rec()
      return depth, ok, (tostring(err):gsub("^[^:]*:%d+: ", ""))
    end,
  }
  for i, work in ipairs(works) do
    local plain = depths(work)
    assert(plain[1] and plain[2] > 100 and plain[3] == false, i .. ": " .. show(plain))
    for _, n in ipairs(i == 4 and {997} or {1, 3, 7}) do
      local r = depths(work, n)
      assert(deepeq(r, plain), i .. ", budget " .. n .. ": " .. show(r) .. " vs " .. show(plain))
    end
  end
end

-- a frozen coroutine never thawed is collected like any other
do
  local weak = setmetatable({}, {__mode = "k"})
  do
    local co = coroutine.create(function() for i = 1, 100 do end end)
    weak[co] = true
    local froze
    debug.setbudget(co, 5, function() if not froze then froze = true debug.freeze() end end)
    coroutine.resume(co)
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
      debug.setbudget(co, n, function() debug.freeze() end) -- (also inside the hook function)
    end
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    assert(r[1] and r[2] == 420)
    return log
  end
  local base = events()
  for _, n in ipairs{1, 2, 5} do
    assert(deepeq(events(n), base), "hook events, budget " .. n)
  end
end

-- a count hook (alone, with no line hook) is called as often with freezes
-- at every budget point, also those inside the hook function
do
  local function counts(n, count)
    local calls = 0
    local co = coroutine.create(function() for i = 1, 300 do end end)
    debug.sethook(co, function() calls = calls + 1 end, "", count)
    if n then debug.setbudget(co, n, function() debug.freeze() end) end
    local r = table.pack(coroutine.resume(co))
    while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
    assert(r[1])
    return calls
  end
  for _, count in ipairs{1, 4, 25} do
    local plain = counts(nil, count)
    assert(plain > 10)
    for _, n in ipairs{1, 2, 5, 97} do
      assert(counts(n, count) == plain, "count " .. count .. ", budget " .. n)
    end
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
  debug.setbudget(co, 2, function() freezes = freezes + 1 debug.freeze() end)
  local r = table.pack(kresume(co))
  assert(r[1] and r[2] == "in pcall")
  r = table.pack(kresume(co, "thaw"))
  assert(r[1] and r[2] == false and r[3] == "after thaw", show(r))
  assert(freezes > 20)
end

-- 'thaw' of a coroutine that froze again returns as a freeze did
do
  local co = coroutine.create(function() for i = 1, 300 do end return "end" end)
  local thaws = 0
  debug.setbudget(co, 10, function() debug.freeze() end)
  local r = table.pack(coroutine.resume(co))
  while debug.isfrozen(co) do
    assert(r.n == 1 and r[1] == true)
    r = table.pack(debug.thaw(co))
    thaws = thaws + 1
  end
  assert(r[1] and r[2] == "end" and thaws > 10)
end

-- a frozen coroutine can be thawed by another coroutine than the one that
-- resumed it
do
  local co = coroutine.create(function() for i = 1, 100 do end return "x" end)
  local once
  debug.setbudget(co, 10, function() if not once then once = true debug.freeze() end end)
  coroutine.resume(co)
  assert(debug.isfrozen(co))
  local other = coroutine.wrap(function() return debug.thaw(co) end)
  local ok, v = other()
  assert(ok and v == "x")
end

-- a callback that fires again and again (a tiny budget), freezing every
-- time, with the program making progress between the freezes
do
  local co = coroutine.create(function()
    local s = 0
    for i = 1, 2000 do s = s + i end
    return s
  end)
  local freezes = 0
  debug.setbudget(co, 1, function() freezes = freezes + 1 debug.freeze() end)
  local r = table.pack(kresume(co))
  assert(r[1] and r[2] == 2001000 and freezes > 4000, show(r) .. " " .. freezes)
end

-- 'load' stops in its middle, in a call of its reader function or at the
-- check point of a token (each token read is a step), and goes on as if
-- nothing had happened: the same function (the same bytes dumped), the
-- same error, the same calls of the reader
do
  local chunks = {
    "local a, b = 1, 2.5e3 -- c\n return a + b, 'x\\65\\u{48}\\z  \n  y', [==[long\n]]string]==], 0x1p4, #{1, 2, x = 3}",
    string.rep("local t = {1, 'two', [3] = 3.0, f = function(...) return ... end}\n", 20) .. "return t",
    "return function(a) if a then return [[\nx]] elseif not a then while false do end else repeat until true end " ..
      "for i = 1, 2 do end for k, v in pairs({}) do end goto l ::l:: return a end",
    "local x <const> = 1; local y <close> = nil; return x, ... --[[ long\ncomment ]] ;",
    "x = = 1",
    "return '\\q'",
    "return [==[ unfinished",
    string.dump(function(a, b) local s = "a long string constant, longer than forty characters" return a + b, s end),
    string.dump(function() return 1, 2.5, "s", nil, true end, true),
    -- a nested function, upvalues and their names, an integer constant, absolute line information
    string.dump(load("local up = 1099511627776 local function g(x) return x + up, G end" .. string.rep("\n", 300) .. "return g, up")),
    -- every token and escape the lexer reads
    "local s = '\\x41\\x4a\\65\\066\\a\\b\\f\\n\\r\\t\\v\\\\\\\"\\'\\\r\n\\\n\\z \r\n \t x\\u{7FFFFFFF}' " ..
      "local a = 1 << 2 >> 1 // 1 ~= 2 and 3 <= 4 and 5 >= 6 or 7 == 8 or ~9 or 2 ^ 3 % 1 .. 'c'\r\n" ..
      "--[==[ long\r\ncomment ]] ]==] -- short\n--\n--[ x\n--[=x\n" ..
      "local t = {.5, 3., 0x.8p1, 1e-2, 1E+2, 0xA.8P-1, 3 - -4} ::k:: return s, a, t, ...",
    "return 1 .. 2x",
    "x = [=",
    -- every kind of statement and expression the parser reads
    [==[
local <const> a, b <close> = 1, nil
local function f(x, y, ...) return x end
local function g(...t) return t end
do global function gf(p) return p end end
do global gx, gy <const> = 1, 2 end
do global <const> * end
do global * end
do local c = (a + 2) * -a; c = not c; c = #"x" .. 'y' .. (c) end
function T.m.n:o(s) return self, s end
function T.p(s) return s end
while a > 2 do local w = 1; break end
repeat local r = 1; r = r + 1 until r > 0
for i = 1, 10, 2 do local z = i end
for k, v in pairs{} do local z = k end
if a then local q = 1 elseif b then local q = 2 else local q = 3 end
x, y.z, w[1] = 1, 2, 3
f{1, 2; x = 3, ['y'] = 4}; f"s"; f(1, 2); obj:m{}; obj:m"s"; obj.p[a]:q(1)
t = {a = 1; [2] = 2, 3,}
::lbl:: ;; goto lbl2; ::lbl2::
return f(a)
]==],
    "return '\\xZ'", "return '\\u48'", "return '\\u{48'", "return '\\u{800000000}'", "return '\\256'",
    -- 'global' as a name (a syntax error where it is reserved, in the test build)
    "global = 1 global.x = 2 global[1] = 3 global(4) global:m() global 's' global {} " ..
      "local t = {global = 1, t.global} global\n= global return global",
  }
  local function loadwith(chunk, piece, ending)
    local calls = 0
    if not piece then return calls, load(chunk, "=chunk") end
    local pos = 1
    local r = {load(function()
      calls = calls + 1
      for i = 1, 3 do end
      if pos > #chunk then return ending end
      local p = chunk:sub(pos, pos + piece - 1)
      pos = pos + piece
      return p
    end, "=chunk")}
    return calls, r[1], r[2]
  end
  local function result(calls, f, msg)
    return {calls = calls, f = f and string.dump(f), msg = msg}
  end
  for ci, chunk in ipairs(chunks) do
    for _, how in ipairs{{false}, {1, nil}, {3, ""}} do
      local plain = result(loadwith(chunk, how[1], how[2]))
      for _, n in ipairs{1, 2, 5} do
        local co = coroutine.create(function() return result(loadwith(chunk, how[1], how[2])) end)
        local freezes = 0
        debug.setbudget(co, n, function() freezes = freezes + 1 debug.freeze() end)
        local r = table.pack(kresume(co))
        assert(r[1], r[2])
        assert(freezes > 0 and deepeq(r[2], plain), "chunk " .. ci .. ", budget " .. n .. ": " .. show(r[2]) .. " vs " .. show(plain))
      end
    end
  end
  -- a long chunk given as a string gets its check points (before, the parse
  -- was one step)
  local co = coroutine.create(function() return load(string.rep("x = 1 ", 3000)) end)
  local inload = 0
  debug.setbudget(co, 10, function(user_co, spender)
    if debug.getinfo(spender, 0, "f").func == load then inload = inload + 1 end
  end)
  assert(coroutine.resume(co))
  assert(inload > 500, inload)
end

-- deep nesting, up to and past the limit of C levels, loaded with a freeze
-- at every token: the most frames a stopped load keeps (their room is
-- checked in a debug build), and the same function or the same error
do
  for _, src in ipairs{
    "return " .. string.rep("(", 100) .. "1" .. string.rep(")", 100),
    "return " .. string.rep("- ", 150) .. "1",
    "return " .. string.rep("(", 250) .. "1" .. string.rep(")", 250),
    "return " .. string.rep("{", 100) .. string.rep("}", 100),
    string.rep("do ", 100) .. string.rep("end ", 100),
    string.rep("do ", 250) .. string.rep("end ", 250),
    string.rep("if x then ", 80) .. string.rep("end ", 80),
    "return " .. string.rep("function() return ", 40) .. "1" .. string.rep(" end", 40),
    "local a = " .. string.rep("1 + ", 1000) .. "1",
  } do
    local function run()
      local f, e = load(src)
      return f and string.dump(f) or e
    end
    local _, plain = coroutine.resume(coroutine.create(run))
    local co = coroutine.create(run)
    local freezes = 0
    debug.setbudget(co, 1, function() freezes = freezes + 1 debug.freeze() end)
    local r = table.pack(kresume(co))
    assert(r[1] and r[2] == plain and freezes > 80, src:sub(1, 30))
  end
end

-- a message handler that freezes, called for an error of the reader
-- function of 'load': 'load' gives the handled error, as without a freeze
do
  local function run()
    local n = 0
    local handler = function(m) for i = 1, 30 do end return "H:" .. tostring(m) end
    return {xpcall(load, handler, function() n = n + 1 if n == 2 then error("boom", 0) end return "return " end)}
  end
  local plain = run()
  local co = coroutine.create(run)
  local freezes = 0
  debug.setbudget(co, 3, function() freezes = freezes + 1 debug.freeze() end)
  local r = table.pack(kresume(co))
  assert(r[1] and freezes > 5 and deepeq(r[2], plain), show(r[2]) .. " vs " .. show(plain))
  assert(deepeq(plain, {true, nil, "H:boom"}), show(plain))
end

-- an error of the reader function of a load with a budget, with no freeze
-- in it: 'load' returns it
do
  local co = coroutine.create(function()
    local n = 0
    return load(function() n = n + 1 if n == 2 then error("bad", 0) end return "return " end)
  end)
  debug.setbudget(co, 1000, function() end)
  local ok, f, msg = coroutine.resume(co)
  assert(ok and f == nil and msg == "bad", msg)
end

-- 'loadfile', 'dofile' and 'require' read a file through a reader that
-- calls no code: with a budget, the file is read whole first, and its parse
-- has the check points of 'load' (each token read is a step), where it
-- stops and goes on as if nothing had happened
do
  local name = os.tmpname()
  local function write(s)
    local f = assert(io.open(name, "wb"))
    f:write(s)
    f:close()
  end
  local function result(...)
    local r = table.pack(...)
    for i = 1, r.n do
      if type(r[i]) == "function" then r[i] = string.dump(r[i]) end
    end
    return r
  end
  -- runs 'f' with a budget of 'n' that freezes (or not) at every point;
  -- the results, and the budget points that were in 'fn'
  local function budgeted(f, n, freeze)
    local co = coroutine.create(f)
    local inside = 0
    debug.setbudget(co, n, function(user_co, spender)
      if debug.getinfo(spender, 0, "f").func == freeze.fn then inside = inside + 1 end
      if freeze.on then debug.freeze() end
    end)
    local r = table.pack(kresume(co))
    assert(r[1], r[2])
    return r[2], inside
  end
  local long = "#!/usr/bin/lua\n" ..
               string.rep("local t = {1, 'two', [3] = 3.0, f = function(...) return ... end}\n", 30) ..
               "return t.f(1, 2)"
  local chunks = {long, "\xEF\xBB\xBFreturn 'bom', ...", "x = = 1",
                  string.dump(function(a) return a, "binary" end), ""}
  for ci, src in ipairs(chunks) do
    write(src)
    local plain = result(loadfile(name))
    for _, n in ipairs{1, 3} do
      for _, on in ipairs{false, true} do
        local r, inside = budgeted(function() return result(loadfile(name)) end, n, {fn = loadfile, on = on})
        assert(deepeq(r, plain), "chunk " .. ci .. ": " .. show(r) .. " vs " .. show(plain))
        if src == long then assert(inside > 100, inside) end
      end
    end
  end
  write(long)
  local plain = table.pack(dofile(name))
  local r, inside = budgeted(function() return table.pack(dofile(name)) end, 2, {fn = dofile, on = true})
  assert(deepeq(r, plain) and inside > 100, inside)
  local path = package.path
  package.path = name -- (a template with no '?': this file, whatever the name)
  local r, inside = budgeted(function() return table.pack(require("frozenfile")) end, 2,
                             {fn = package.searchers[2], on = true})
  package.path, package.loaded.frozenfile = path, nil
  assert(r[1] == 1 and r[2] == name and inside > 100, inside)
  os.remove(name)
end

-- a collection does not hold off a freeze for long: a chain of entries in
-- a weak table (each value the key of the next, in a shuffled order) is
-- converged in time linear in it, as a weak table of as many entries with
-- no chain (stock goes over the table again for each step of the chain)
do
  local n = 40000
  local function longest(make)
    local co = coroutine.create(function()
      local e = make()
      collectgarbage()
      collectgarbage()
      return e
    end)
    local last, worst
    debug.setbudget(co, 1000, function()
      local now = os.clock()
      if last and now - last > worst then worst = now - last end
      last = now
    end)
    collectgarbage()
    worst = 0
    assert(coroutine.resume(co))
    return math.max(worst, os.clock() - last) -- (to its end)
  end
  local base = longest(function()
    local e = setmetatable({}, {__mode = "k"})
    local keys = {}
    for i = 1, n do keys[i] = {} e[keys[i]] = {} end
    return {e, keys}
  end)
  local chained = longest(function()
    local e = setmetatable({}, {__mode = "k"})
    local keys = {}
    for i = 1, n do keys[i] = {} end
    local p, x = {}, 7
    for i = 1, n - 1 do p[i] = i end
    for i = n - 1, 2, -1 do
      x = (x * 1103515245 + 12345) % 2147483648
      local j = x % i + 1
      p[i], p[j] = p[j], p[i]
    end
    for _, i in ipairs(p) do e[keys[i]] = keys[i + 1] end
    return {e, keys[1]}
  end)
  assert(chained < 10 * base + 0.05, chained .. " vs " .. base)
end

-- if the host does not allow the memory of the entries that wait (the
-- test library's limit of allocations), their values are marked: what
-- they hold lives one collection more, though an entry whose key is not
-- marked is cleared as ever (a chain loses its first entry, then the rest)
if T then
  local e = setmetatable({}, {__mode = "k"})
  local keys = {}
  for i = 1, 100 do keys[i] = {} end
  for i = 1, 99 do e[keys[i]] = keys[i + 1] end
  keys = nil
  local function count()
    local n = 0
    for _ in pairs(e) do n = n + 1 end
    return n
  end
  T.alloccount(0)
  collectgarbage()
  T.alloccount()
  assert(count() == 98, count())
  collectgarbage()
  assert(count() == 0, count())
end

-- what the box of a load keeps (here, a file read whole) is given back to
-- the host when the load ends, not when the collector frees the box
if T then
  local name = os.tmpname()
  local f = assert(io.open(name, "wb"))
  f:write("--", string.rep("x", 1000000), "\nreturn 1")
  f:close()
  local co = coroutine.create(function()
    collectgarbage("stop")
    local before = T.totalmem()
    local fn = loadfile(name)
    local grew = T.totalmem() - before
    collectgarbage("restart")
    return fn, grew
  end)
  debug.setbudget(co, 1000, function() end)
  local ok, fn, grew = coroutine.resume(co)
  os.remove(name)
  assert(ok and fn() == 1 and grew < 100000, grew)
end

-- what a load with a budget keeps for a freeze (the input it read, room for
-- its frames and for the parser's arrays) is counted by the host, not in
-- the Lua heap: 'collectgarbage("count")' sees what it sees without a budget
do
  local piece = string.rep("x", 100000) .. "\n"
  local co = coroutine.create(function()
    local n, mem = 0, {}
    local f = load(function()
      n = n + 1
      if n > 20 then return nil end
      collectgarbage()
      mem[n] = collectgarbage("count")
      return "--" .. n .. piece -- (a comment, a new string each time)
    end)
    assert(f and n == 21)
    return mem[20] - mem[3]
  end)
  debug.setbudget(co, 1000, function() end)
  local ok, grew = coroutine.resume(co)
  assert(ok and grew < 400, grew)
end

-- a load stopped by a freeze holds the parser's arrays and buffer; when it
-- goes on and they cannot be counted again on their own (the test
-- library's limit of allocations), 'load' gives a memory error, as for any
-- allocation of a load, and its memory is all given back
if T then
  local src = string.rep("do local name = 'a string of some length' goto a ::a:: do goto b end ::b:: end\n", 8) ..
              "return 1"
  local plain = string.dump(load(src))
  local loaded, failed = false, 0
  for limit = 0, 1000 do
    local co = coroutine.create(function() return load(src) end)
    local n = 0
    debug.setbudget(co, 1, function()
      n = n + 1
      if n == 40 then
        debug.setbudget(co, 1000000)
        debug.freeze()
      end
    end)
    assert(coroutine.resume(co) and debug.isfrozen(co))
    local ok, a, b, c = pcall(function()
      T.alloccount(limit)
      local a, b, c = debug.thaw(co)
      T.alloccount()
      return a, b, c
    end)
    T.alloccount()
    if not ok then
      assert(a == "not enough memory", a)
    elseif not a then
      assert(b == "not enough memory", b)
    elseif not b then
      assert(c == "not enough memory", c)
      failed = failed + 1
    else
      assert(string.dump(b) == plain)
      loaded = true
      break
    end
  end
  assert(loaded and failed > 0, failed)
  collectgarbage()
end
