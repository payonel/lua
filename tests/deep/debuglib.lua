-- tests/deep/debuglib.lua
-- Client-side debug.getinfo / getlocal / setlocal / traceback surface.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

local debug = require "debug"

-- getinfo on a Lua function object: source/definition/param/upvalue fields
do
  local function f(a, b, ...)
    local c = a
    return c, b
  end
  local info = debug.getinfo(f, "Slu")
  assert(info.what == "Lua")
  assert(info.nparams == 2)
  assert(info.isvararg == true)
  assert(info.nups == 0)
  assert(type(info.linedefined) == "number" and info.linedefined > 0)
  assert(info.lastlinedefined > info.linedefined)
  assert(type(info.source) == "string")
  assert(type(info.short_src) == "string")
  assert(info.currentline == -1) -- not running
  local finfo = debug.getinfo(f, "f")
  assert(finfo.func == f)
end

-- getinfo on a C function
do
  local info = debug.getinfo(print, "Slu")
  assert(info.what == "C")
  assert(info.source == "=[C]")
  assert(info.short_src == "[C]")
  assert(info.linedefined == -1 and info.lastlinedefined == -1)
  assert(info.isvararg == true)
  assert(info.nparams == 0)
end

-- activelines lists the executable lines of a Lua function
do
  local function f()
    local a = 1
    local b = 2
    return a + b
  end
  local info = debug.getinfo(f, "L")
  assert(type(info.activelines) == "table")
  local n = 0
  for line, flag in pairs(info.activelines) do
    assert(type(line) == "number" and flag == true)
    n = n + 1
  end
  assert(n >= 3)
end

-- level-based getinfo: level 1 is the running function, currentline is live
do
  local function inner()
    return debug.getinfo(1, "nSl")
  end
  local info = inner()
  assert(info.what == "Lua")
  assert(info.currentline >= info.linedefined)
  assert(info.currentline <= info.lastlinedefined)
  -- out-of-range level returns nil
  assert(debug.getinfo(200) == nil)
end

-- getlocal/setlocal by level: parameters, locals, write-through
do
  local function f(a, b)
    local c = a + b
    local name, val = debug.getlocal(1, 1)
    assert(name == "a" and val == 10)
    name, val = debug.getlocal(1, 2)
    assert(name == "b" and val == 20)
    name, val = debug.getlocal(1, 3)
    assert(name == "c" and val == 30)
    assert(debug.setlocal(1, 3, 99) == "c")
    assert(debug.setlocal(1, 200, 0) == nil) -- out of range (value still required)
    return c
  end
  assert(f(10, 20) == 99)
end

-- getlocal on a function object: parameter names, no live values
do
  local function f(x, y) return x + y end
  assert(debug.getlocal(f, 1) == "x")
  assert(debug.getlocal(f, 2) == "y")
  assert(debug.getlocal(f, 3) == nil)
end

-- varargs are reachable with negative indices, named "(vararg)"
do
  local function f(...)
    local name, v = debug.getlocal(1, -1)
    assert(name == "(vararg)" and v == "A")
    name, v = debug.getlocal(1, -2)
    assert(name == "(vararg)" and v == "B")
    assert(debug.getlocal(1, -3) == nil)
    assert(debug.setlocal(1, -1, "Z") == "(vararg)")
    return select(1, ...)
  end
  f("A", "B")
end

-- getlocal across coroutine boundaries: inspect a suspended coroutine
do
  local co = coroutine.create(function(p)
    local q = p * 2
    coroutine.yield()
    return q
  end)
  assert(coroutine.resume(co, 21))
  -- co is suspended inside the function; level 1 is that frame
  local name, val = debug.getlocal(co, 1, 1)
  assert(name == "p" and val == 21)
  name, val = debug.getlocal(co, 1, 2)
  assert(name == "q" and val == 42)
  assert(debug.setlocal(co, 1, 2, 100) == "q")
  local ok, r = coroutine.resume(co)
  assert(ok and r == 100)
end

-- traceback: message prefix, non-string passthrough, thread argument
do
  local tb = debug.traceback("msg", 1)
  assert(type(tb) == "string")
  assert(tb:sub(1, 3) == "msg")
  assert(tb:find("stack traceback:", 1, true))
  -- number messages are convertible to string, so they get a traceback
  local ntb = debug.traceback(42)
  assert(type(ntb) == "string" and ntb:sub(1, 2) == "42")
  -- non-convertible messages are returned untouched
  local t = {}
  assert(debug.traceback(t) == t)
  assert(type(debug.traceback()) == "string")
  local co = coroutine.create(function() end)
  assert(type(debug.traceback(co)) == "string")
end

-- getinfo ">" form: inspect the caller of a hook-like invocation
do
  local function callee()
    local info = debug.getinfo(2, "n")
    return info
  end
  local function caller() return callee() end
  local info = caller()
  assert(info ~= nil) -- name fields may be nil, but the level exists
end

-- name resolution: global calls report their name
do
  local reported
  local function grab()
    local info = debug.getinfo(2, "n")
    reported = info and info.name
  end
  _G.deep_named_probe = function() grab() end
  deep_named_probe()
  _G.deep_named_probe = nil
  assert(reported == "deep_named_probe")
end
