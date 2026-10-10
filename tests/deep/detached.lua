-- tests/deep/detached.lua
-- 'detached(co [, permanents])': whether a thread holds nothing that the
-- rest of the VM also holds (spec section 15): the objects reachable both
-- from the thread and from outside it. A thread that shares something is
-- not self-contained, and a persistence of it cannot be trusted. True, or
-- false, a reason (the VM also holds what it shares, or only other threads
-- do) and the list of what is shared. Stock Lua 5.5 has none of this (the
-- file returns at once there).

local debug = require "debug"
if not debug.detached then return end

local VM = "refers to an object the VM also holds"
local THREAD = "refers to an object another thread also holds"

-- (a collection first: it clears what dead registers of this thread's stack
-- still hold, which the report would count as outside references, as the
-- collector does)
local function detached(co, permanents)
  collectgarbage()
  return debug.detached(co, permanents)
end

-- (a report refers to what it reports, and what a report refers to is then
-- outside: the tests keep nothing of one past the call)
local function has(co, value, permanents)
  local ok, why, r = detached(co, permanents)
  if ok then return nil, 0 end
  for _, e in ipairs(r) do
    if e.value == value then return e.type, #r end
  end
  return nil, #r
end

-- a program with an environment of its own, and nothing shared: clean
local function program(env, source)
  local co = coroutine.create(assert(load(source, "=user", "t", env)))
  assert(coroutine.resume(co))
  return co
end

do
  local co = program({yield = coroutine.yield, string = "text"},
                     "local t = {1, {2, 3}} local f = function() return t end yield() return f")
  local ok, why, r = detached(co)
  assert(ok == true and why == nil and r == nil) -- (strings and C functions are exempt)
end

-- a kernel object that the program refers to
do
  local K = {name = "kernel"}
  local co = program({yield = coroutine.yield, K = K, n = 5},
                     "local mine = {} yield() return K, mine")
  local ok, why, list = detached(co)
  assert(ok == false and why == VM and type(list) == "table" and #list >= 1)
  local t, n = has(co, K)
  assert(t == "table" and n >= 1)
  -- exempt as a permanent, by key or by value
  assert(has(co, K, {[K] = "K"}) == nil)
  assert(has(co, K, {K = K}) == nil)
  local _, n2 = has(co, K, {[K] = true})
  assert(n2 == n - 1, n .. " " .. n2)
  assert(select("#", detached(co, {[K] = true})) >= 1)
  assert(not pcall(debug.detached, co, 42))
  assert(not pcall(debug.detached, 42))
end

-- an object shared between two programs, that nothing else holds: only
-- other threads do
do
  local function make()
    local shared = {}
    local a = program({yield = coroutine.yield, shared = shared}, "local x = shared yield()")
    local b = program({yield = coroutine.yield, shared = shared}, "local y = shared yield()")
    return a, b
  end
  local a, b = make()
  -- (clear the dead registers where 'make' ran, which would still hold it)
  local function wipe()
    local a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20
  end
  wipe()
  local ok, why, list = detached(a)
  assert(ok == false and why == THREAD and #list == 1 and list[1].type == "table")
  list = nil -- (a report refers to what it reports)
  ok, why = detached(b)
  assert(ok == false and why == THREAD)
  -- the program that holds it, and the kernel too: the VM also holds it
  local shared = {}
  a = program({yield = coroutine.yield, shared = shared}, "local x = shared yield()")
  b = program({yield = coroutine.yield, shared = shared}, "local y = shared yield()")
  assert(has(a, shared) == "table")
  assert(has(b, shared) == "table")
  ok, why = detached(a)
  assert(ok == false and why == VM)
end

-- a variable shared by a user function and a kernel function: an upvalue
do
  local counter = 0
  local function inc() counter = counter + 1 end
  local function get() return counter end
  local co = program({yield = coroutine.yield, inc = inc, get = get}, "inc() yield() inc()")
  assert(has(co, inc) == "function" and has(co, get) == "function")
  local _, _, r = detached(co)
  local up
  for _, e in ipairs(r) do if e.type == "upvalue" then up = e.value end end
  assert(math.type(up) == "integer")
end

-- a thread handle that the kernel keeps for an inner coroutine
do
  local handle
  local co = program({yield = coroutine.yield, create = coroutine.create, set = function(h) handle = h end},
                     "local inner = create(function() yield() end) set(inner) yield()")
  assert(has(co, handle) == "thread")
end

-- a frozen program (with a chain of coroutines) can be checked, and what it
-- shares is still reported afterwards
do
  local K = {}
  local co = coroutine.create(assert(load(
    "local inner = coroutine.create(function() for i = 1, 200 do local x = K end end) coroutine.resume(inner)",
    "=user", "t", {K = K, coroutine = {create = coroutine.create, resume = coroutine.resume}})))
  local n = 0
  debug.setbudget(co, 20, function() n = n + 1 if n == 3 then debug.freeze() end end)
  assert(coroutine.resume(co))
  assert(debug.isfrozen(co))
  assert(has(co, K) == "table")
  assert(debug.isfrozen(co)) -- (nothing changed)
  assert(debug.thaw(co))
end
