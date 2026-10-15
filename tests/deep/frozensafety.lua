-- tests/deep/frozensafety.lua
-- Whatever the firmware does with a frozen thread, a thread handle or a user
-- function, the VM never crashes, corrupts memory or hangs (spec 2.10). The
-- firmware here is this file: it freezes user programs of many shapes at many
-- points, then does things to them in a pseudo-random order, each in a
-- protected call: what the operations answer does not matter, only that
-- the VM goes on (these tests are for a debug build, and for valgrind).
-- Stock Lua 5.5 has none of this (the file returns at once there).

local debug = require "debug"
if not debug.setbudget then return end

local random = math.random

local function nop() end

-- the user programs: each gets a table where it leaves handles (inner
-- coroutines, functions) for the firmware to abuse
local programs = {}
local function program(name, f) programs[#programs + 1] = {name = name, f = f} end

program("a loop with locals", function(h)
  local t = {}
  for i = 1, 60 do t[i] = i * i end
  h.fn = function(x) return x + 1 end
  return #t
end)

program("nested coroutines", function(h)
  local inner = coroutine.create(function(a)
    local b = coroutine.yield(a + 1)
    for i = 1, 20 do b = b + i end
    return b
  end)
  h.inner = inner
  local w = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end return "done" end)
  h.wrap = w
  local _, v = coroutine.resume(inner, 1)
  local _, r = coroutine.resume(inner, v)
  return r, w(), w(), w(), w()
end)

program("protected calls with closing variables", function(h)
  local function body()
    local x <close> = setmetatable({}, {__close = function() for i = 1, 10 do end end})
    for i = 1, 10 do end
    error("e")
  end
  h.body = body
  local ok1 = pcall(body)
  local ok2 = xpcall(body, function(m) for i = 1, 10 do end return m end)
  return ok1, ok2
end)

program("library callbacks", function(h)
  local t = {5, 3, 9, 1, 7, 2, 8}
  table.sort(t, function(a, b) for i = 1, 5 do end return a < b end)
  local g = string.gsub("abcdef", "%w", function(c) for i = 1, 5 do end return c:upper() end)
  local mt = setmetatable({}, {__index = function(_, k) for i = 1, 5 do end return k end,
                                __tostring = function() for i = 1, 5 do end return "T" end})
  h.obj = mt
  return table.concat(t, ","), g, mt[1], tostring(mt)
end)

program("hooks and loads", function(h)
  local n = 0
  debug.sethook(function() n = n + 1 for i = 1, 3 do end end, "crl", 5)
  local f = load(function() for i = 1, 5 do end return nil end)
  local parts, i = {"return 1 +", " 2"}, 0
  local g = load(function() i = i + 1 for k = 1, 3 do end return parts[i] end)
  for k = 1, 20 do end
  debug.sethook()
  return f, g and g(), n
end)

program("closing coroutines", function(h)
  local co = coroutine.create(function()
    local x <close> = setmetatable({}, {__close = function() for i = 1, 8 do end end})
    coroutine.yield(1)
  end)
  h.co = co
  coroutine.resume(co)
  local ok = coroutine.close(co)
  local self = coroutine.wrap(function()
    local y <close> = setmetatable({}, {__close = function() for i = 1, 8 do end end})
    coroutine.close(coroutine.running())
  end)
  pcall(self)
  return ok
end)

program("waiting finalizers", function(h)
  for i = 1, 5 do
    setmetatable({}, {__gc = function() for k = 1, 10 do end end})
  end
  collectgarbage()
  return 1
end)

-- the firmware's things to do with a frozen program and its handles
local function handles(co, h)
  local list = {co}
  for _, v in pairs(h) do
    if type(v) == "thread" then list[#list + 1] = v end
  end
  return list
end

local ops = {}
local function op(name, f) ops[#ops + 1] = {name = name, f = f} end

op("resume", function(c) return coroutine.resume(c, 1, 2) end)
op("close", function(c) return coroutine.close(c) end)
op("status", function(c) return coroutine.status(c) end)
op("isyieldable", function(c) return coroutine.isyieldable(c) end)
op("thaw", function(c) return debug.thaw(c) end)
op("isfrozen", function(c) return debug.isfrozen(c) end)
op("traceback", function(c) return debug.traceback(c, "m", random(0, 4)) end)
op("getinfo", function(c)
  for level = 0, 4 do debug.getinfo(c, level, "nSltufrL") end
end)
op("getlocal", function(c)
  for level = 0, 3 do for i = 1, 6 do debug.getlocal(c, level, i) end end
end)
op("setlocal", function(c)
  -- (a value of the same type: the interpreter reads loop variables and the
  -- like as what they are, as in stock Lua, and a table here is a new one)
  for level = 0, 3 do
    for i = 1, 4 do
      local name, v = debug.getlocal(c, level, i)
      if name and name:sub(1, 1) ~= "(" then -- (not the interpreter's own: loop states, temporaries)
        if type(v) == "table" then v = {} end
        debug.setlocal(c, level, i, v)
      end
    end
  end
end)
op("sethook", function(c) debug.sethook(c, nop, "crl", random(0, 3)) end)
op("clearhook", function(c) debug.sethook(c) end)
op("gethook", function(c) return debug.gethook(c) end)
op("setbudget", function(c) debug.setbudget(c, random(1, 40), nop) end)
op("clearbudget", function(c) debug.setbudget(c, 0) end)
op("getbudget", function(c) return debug.getbudget(c) end)
op("collect", function() collectgarbage() end)
op("step", function() collectgarbage("step", 10) end)
op("upvalues", function(c)
  local info = debug.getinfo(c, 1, "f")
  local f = info and info.func
  if f then
    -- (the same value back: the programs share their upvalues with this file)
    for i = 1, 5 do local n, v = debug.getupvalue(f, i) if n then debug.setupvalue(f, i, v) end end
  end
end)
op("call a user function", function(c, h)
  for _, v in pairs(h) do
    if type(v) == "function" then pcall(v, 1, 2) end
  end
end)
op("a user object", function(c, h)
  local o = h.obj
  if o then pcall(function() return o[1], tostring(o) end) end
end)
op("a wrapped coroutine", function(c, h)
  if h.wrap then pcall(h.wrap) end
end)
op("resume from another coroutine", function(c)
  local k = coroutine.create(function() return coroutine.resume(c) end)
  return coroutine.resume(k)
end)
op("close a handle", function(c, h)
  local list = handles(c, h)
  return coroutine.close(list[random(1, #list)])
end)

local function freezeat(n)
  local calls = 0
  return function()
    calls = calls + 1
    if calls == n then debug.freeze() end
  end
end

local frozen, killed, ran = 0, 0, 0

for _, seed in ipairs{20261009, 7, 123456789} do
math.randomseed(seed)
for _, prog in ipairs(programs) do
  for _, at in ipairs{1, 2, 3, 5, 8, 13, 21, 34, 55} do
    local h = {}
    local co = coroutine.create(prog.f)
    debug.setbudget(co, 3, freezeat(at))
    local ok = coroutine.resume(co, h)
    if debug.isfrozen(co) then frozen = frozen + 1 end
    for step = 1, 25 do
      local o = ops[random(1, #ops)]
      local list = handles(co, h)
      local target = list[random(1, #list)]
      pcall(o.f, target, h)
      ran = ran + 1
    end
    -- whatever happened, the VM is usable, and what is frozen can be let go
    pcall(debug.setbudget, co, 0)
    local guard = 0
    while debug.isfrozen(co) and guard < 1000 do
      guard = guard + 1
      if not pcall(debug.thaw, co) then break end
    end
    collectgarbage()
    h, co = nil, nil
    collectgarbage()
  end
end
end

assert(frozen > 30 and ran > 1000, frozen .. " " .. ran)

-- the traceback of another thread names the functions of its frames, not
-- what the stack of the calling thread has at the same place
do
  local co = coroutine.create(function() local x = coroutine.yield(1) end)
  coroutine.resume(co)
  local base = debug.traceback(co)
  local f1, f2, f3, f4, f5, f6, f7, f8 = string.format, string.rep, print, 1, 2, 3, 4, 5
  assert(debug.traceback(co) == base)
  local g = function()
    local a, b, c, d, e, f, g, h = print, tostring, string.format, string.rep, 3, 4, 5, 6
    return debug.traceback(co)
  end
  assert(g() == base)
end
