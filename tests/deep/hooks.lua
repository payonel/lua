-- tests/deep/hooks.lua
-- Client-side debug hook surface: sethook/gethook, every event type,
-- per-coroutine hooks, and state inspection from inside a hook.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

local debug = require "debug"

-- no hook by default, on the main thread and on fresh coroutines
do
  assert(debug.gethook() == nil)
  local co = coroutine.create(function() end)
  assert(debug.gethook(co) == nil)
end

-- gethook reports (function, mask, count); mask is normalized to "c","r","l" order
do
  local function h() end
  debug.sethook(h, "lc", 5)
  local f, mask, count = debug.gethook()
  assert(f == h and mask == "cl" and count == 5)
  debug.sethook(h, "rl")
  f, mask, count = debug.gethook()
  assert(f == h and mask == "rl" and count == 0)
  debug.sethook() -- clear
  assert(debug.gethook() == nil)
end

-- line events: fire per executed line with the line number
do
  local lines = {}
  local function hook(event, line)
    assert(event == "line")
    assert(type(line) == "number" and line > 0)
    lines[#lines + 1] = line
  end
  local function f()
    local a = 1
    local b = 2
    return a + b
  end
  debug.sethook(hook, "l")
  f()
  debug.sethook()
  -- at minimum the three body lines of f plus this chunk's own lines fired
  assert(#lines >= 3)
end

-- call and return events, filtered to a known function
do
  local events = {}
  local target
  local function hook(event)
    local info = debug.getinfo(2, "f")
    if info and info.func == target then
      events[#events + 1] = event
    end
  end
  local function f() return 1 end
  target = f
  debug.sethook(hook, "cr")
  f()
  debug.sethook()
  assert(#events == 2)
  assert(events[1] == "call")
  assert(events[2] == "return")
end

-- tail calls produce a "tail call" event (under the call mask) and no
-- extra return event for the tail-called frame's caller
do
  local seen = {}
  local function hook(event)
    seen[event] = (seen[event] or 0) + 1
  end
  local function g() return 1 end
  local function f() return g() end -- tail call
  debug.sethook(hook, "c")
  assert(f() == 1)
  debug.sethook()
  assert(seen["tail call"] and seen["tail call"] >= 1)
end

-- count events: fire every N instructions, event name is "count"
do
  local n = 0
  debug.sethook(function(event)
    assert(event == "count")
    n = n + 1
  end, "", 100)
  local f, mask, count = debug.gethook()
  assert(mask == "" and count == 100)
  local x = 0
  for i = 1, 5000 do x = x + i end
  debug.sethook()
  assert(n > 0)
  assert(x == 12502500)
end

-- hooks are per-thread: a coroutine's hook does not affect the main
-- thread and vice versa
do
  local co_events = 0
  local co = coroutine.create(function()
    local a = 1
    local b = 2
    coroutine.yield()
    local c = a + b
    return c
  end)
  debug.sethook(co, function() co_events = co_events + 1 end, "l")
  local f, mask = debug.gethook(co)
  assert(mask == "l")
  assert(debug.gethook() == nil) -- main thread unaffected

  assert(coroutine.resume(co))
  assert(co_events > 0)
  local mark = co_events

  -- main-thread execution does not fire the coroutine's hook
  local x = 0
  for i = 1, 100 do x = x + i end
  assert(co_events == mark)

  -- resuming fires it again
  assert(coroutine.resume(co))
  assert(co_events > mark)
  assert(coroutine.status(co) == "dead")
end

-- full state inspection from inside a line hook: getinfo, getlocal, and
-- upvalue access all work at the hook point (the "hibernation window")
do
  local captured = nil
  local upv = 77
  local function victim(p, q)
    local sum = p + q + upv
    return sum
  end
  local function hook(event, line)
    local info = debug.getinfo(2, "nSluf")
    if info.func == victim then
      local c = { line = line, what = info.what, nparams = info.nparams, nups = info.nups }
      local n1, v1 = debug.getlocal(2, 1)
      local n2, v2 = debug.getlocal(2, 2)
      c.p_name, c.p_val = n1, v1
      c.q_name, c.q_val = n2, v2
      local un, uv = debug.getupvalue(info.func, 1)
      c.up_name, c.up_val = un, uv
      captured = captured or c
    end
  end
  debug.sethook(hook, "l")
  assert(victim(10, 20) == 107)
  debug.sethook()
  assert(captured ~= nil)
  assert(captured.what == "Lua")
  assert(captured.nparams == 2)
  assert(captured.nups == 1)
  assert(captured.p_name == "p" and captured.p_val == 10)
  assert(captured.q_name == "q" and captured.q_val == 20)
  assert(captured.up_name == "upv" and captured.up_val == 77)
end

-- a hook can modify locals in the running frame via setlocal
do
  local function hook(event, line)
    local info = debug.getinfo(2, "S")
    if info and info.what == "Lua" then
      local name, val = debug.getlocal(2, 1)
      if name == "marker" and val == 1 then
        assert(debug.setlocal(2, 1, 2) == "marker")
      end
    end
  end
  local function f()
    local marker = 1
    local pad = 0 -- extra line so the hook sees marker == 1
    return marker
  end
  debug.sethook(hook, "l")
  local r = f()
  debug.sethook()
  assert(r == 2)
end

-- errors inside a pcall'd function still run hooks; hook state survives
do
  local fired = false
  debug.sethook(function() fired = true end, "c")
  local ok, err = pcall(function() error("boom") end)
  debug.sethook()
  assert(not ok and err:find("boom"))
  assert(fired)
end
