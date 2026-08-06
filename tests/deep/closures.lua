-- tests/deep/closures.lua
-- Client-side closure and upvalue surface: getupvalue/setupvalue,
-- upvalueid/upvaluejoin, upvalue sharing, _ENV, closure independence.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

local debug = require "debug"

-- two closures over the same local share the upvalue (same id, same storage)
do
  local x = 10
  local function get() return x end
  local function set(v) x = v end
  assert(get() == 10)
  set(42)
  assert(get() == 42 and x == 42)
  local id1 = debug.upvalueid(get, 1)
  local id2 = debug.upvalueid(set, 1)
  assert(id1 == id2)
end

-- getupvalue reports name and value; out-of-range returns nil
do
  local up = 5
  local function f() return up end
  local name, val = debug.getupvalue(f, 1)
  assert(name == "up" and val == 5)
  assert(debug.getupvalue(f, 2) == nil)
end

-- setupvalue writes through to the shared variable and returns the name
do
  local up = 5
  local function f() return up end
  assert(debug.setupvalue(f, 1, 99) == "up")
  assert(f() == 99)
  assert(up == 99)
  assert(debug.setupvalue(f, 2, 0) == nil) -- out of range
end

-- functions using globals capture _ENV as an upvalue
do
  local f = load("return some_global_name")
  local name, env = debug.getupvalue(f, 1)
  assert(name == "_ENV")
  assert(env == _G)
end

-- upvaluejoin makes two closures share one upvalue
do
  local a = 1
  local b = 2
  local function fa() return a end
  local function fb() return b end
  assert(fa() == 1 and fb() == 2)
  debug.upvaluejoin(fb, 1, fa, 1)
  assert(fb() == 1) -- fb now reads fa's upvalue
  assert(debug.upvalueid(fb, 1) == debug.upvalueid(fa, 1))
  debug.setupvalue(fa, 1, 77)
  assert(fb() == 77) -- writes are visible through the join
  assert(b == 2)     -- original variable untouched
end

-- each closure instantiation gets fresh upvalues
do
  local function make()
    local n = 0
    return function() n = n + 1; return n end
  end
  local c1, c2 = make(), make()
  assert(c1() == 1)
  assert(c1() == 2)
  assert(c2() == 1) -- independent counter
  assert(debug.upvalueid(c1, 1) ~= debug.upvalueid(c2, 1))
end

-- loop variables: each iteration closes over a distinct variable
do
  local fns = {}
  for i = 1, 3 do
    fns[i] = function() return i end
  end
  assert(fns[1]() == 1 and fns[2]() == 2 and fns[3]() == 3)
  assert(debug.upvalueid(fns[1], 1) ~= debug.upvalueid(fns[2], 1))
end

-- open vs closed upvalues: closures created in a live scope see live
-- updates; after scope exit the value is frozen at its last state
do
  local get, bump
  do
    local v = 100
    get = function() return v end
    bump = function() v = v + 1 end
  end
  assert(get() == 100)
  bump()
  assert(get() == 101) -- still shared after close
end

-- upvalues of C functions: stdlib functions without upvalues report none
do
  assert(debug.getupvalue(print, 1) == nil)
end

-- upvalues of C closures: gmatch iterators are C closures with upvalues;
-- names are reported as "" (C upvalues have no names)
do
  local it = string.gmatch("hello world", "%a+")
  local info = debug.getinfo(it, "u")
  assert(info.nups > 0)                 -- it really is a C closure
  local name, val = debug.getupvalue(it, 1)
  assert(name == "")                    -- C upvalue: empty name
  assert(debug.getupvalue(it, info.nups + 1) == nil) -- out of range
  -- the iterator still works after inspection
  assert(it() == "hello")
  assert(it() == "world")
end

-- setupvalue on a C closure writes the upvalue (empty name returned);
-- writing a collectable value exercises the C-closure write barrier
do
  local it = string.gmatch("abc", "%a")
  local n = debug.getinfo(it, "u").nups
  -- find an upvalue we can overwrite without breaking the iterator: use a
  -- scratch closure instead of a live one
  local scratch = string.gmatch("zzz", "z")
  assert(debug.setupvalue(scratch, 1, {"fresh table"}) == "")
  local _, v = debug.getupvalue(scratch, 1)
  assert(type(v) == "table" and v[1] == "fresh table")
  collectgarbage() -- barrier interaction: the new upvalue must survive GC
  local _, v2 = debug.getupvalue(scratch, 1)
  assert(type(v2) == "table" and v2[1] == "fresh table")
  -- untouched iterator is unaffected
  assert(it() == "a" and n > 0)
end

-- upvalueid works on C closures too: stable and distinct per closure
do
  local it1 = string.gmatch("x", "x")
  local it2 = string.gmatch("x", "x")
  local id1 = debug.upvalueid(it1, 1)
  assert(id1 == debug.upvalueid(it1, 1)) -- stable
  assert(id1 ~= debug.upvalueid(it2, 1)) -- distinct closures, distinct storage
end

-- nups via getinfo matches reality
do
  local u1, u2 = 1, 2
  local function f() return u1 + u2 end
  assert(debug.getinfo(f, "u").nups == 2)
  local function g() return 0 end
  assert(debug.getinfo(g, "u").nups == 0)
  assert(debug.getinfo(print, "u").nups == 0)
end

-- upvalue identity survives the closure being passed around and called
do
  local state = 0
  local function inc() state = state + 1; return state end
  local id_before = debug.upvalueid(inc, 1)
  local t = { fn = inc }
  assert(t.fn() == 1)
  assert(pcall(inc))
  assert(debug.upvalueid(inc, 1) == id_before)
  assert(state == 2)
end
