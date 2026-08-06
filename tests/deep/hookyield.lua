-- tests/deep/hookyield.lua
-- Function-exit boundaries under instrumentation and suspension: return
-- hooks firing after to-be-closed variables close, and coroutines
-- suspended in the middle of closing TBC variables during a return.
-- (Yielding from inside a hook itself is a C-hook-only capability and is
-- covered by test_public_api.cpp.)
-- Stock Lua 5.5 behavior is the reference for every assertion here.

local debug = require "debug"

-- return hook fires after __close of a TBC variable, and multiple
-- results survive the closing
do
  local order = {}
  local function victim()
    local guard <close> = setmetatable({}, {
      __close = function() order[#order + 1] = "close" end,
    })
    return 1, 2, 3
  end
  local target_seen = false
  debug.sethook(function(ev)
    if ev == "return" then
      local info = debug.getinfo(2, "f")
      if info and info.func == victim then
        target_seen = true
        order[#order + 1] = "rethook"
      end
    end
  end, "r")
  local a, b, c = victim()
  debug.sethook()
  assert(a == 1 and b == 2 and c == 3)
  assert(target_seen)
  -- the variable closed before the return hook ran
  assert(order[1] == "close")
  assert(order[2] == "rethook")
end

-- a coroutine may yield from inside a __close metamethod that runs
-- while a function returns; the return completes after resuming
do
  local closed = false
  local co = coroutine.create(function()
    local function inner()
      local guard <close> = setmetatable({}, {
        __close = function()
          coroutine.yield("closing")
          closed = true
        end,
      })
      return "inner-result", 2
    end
    local r, n = inner()
    return r, n
  end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "closing")
  assert(not closed)
  assert(coroutine.status(co) == "suspended")
  local ok2, r, n = coroutine.resume(co)
  assert(ok2)
  assert(closed)
  assert(r == "inner-result" and n == 2) -- results survived the suspension
  assert(coroutine.status(co) == "dead")
end

-- same, but with all results wanted (multret tail position)
do
  local co = coroutine.create(function()
    local function inner()
      local guard <close> = setmetatable({}, {
        __close = function() coroutine.yield() end,
      })
      return "a", "b", "c"
    end
    return inner()
  end)
  assert(coroutine.resume(co)) -- runs until the yield inside __close
  local ok, x, y, z = coroutine.resume(co)
  assert(ok and x == "a" and y == "b" and z == "c")
  assert(coroutine.status(co) == "dead")
end

-- line hook active while a TBC-bearing function returns: hook events and
-- close order both hold under instrumentation
do
  local events = {}
  local function f()
    local t <close> = setmetatable({}, {
      __close = function() events[#events + 1] = "close" end,
    })
    events[#events + 1] = "body"
    return #events
  end
  debug.sethook(function() end, "l") -- any hook enables the instrumented path
  local n = f()
  debug.sethook()
  assert(n == 1)
  assert(events[1] == "body" and events[2] == "close")
end
