-- tests/deep/tbc.lua
-- Client-side to-be-closed (<close>) variable surface: __close timing and
-- ordering, error paths, interaction with break/return/goto, coroutines.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

local function closer(log, name)
  return setmetatable({}, {
    __close = function(self, err)
      log[#log + 1] = { name = name, err = err }
    end,
  })
end

-- __close runs at scope exit, in reverse declaration order
do
  local log = {}
  do
    local a <close> = closer(log, "a")
    local b <close> = closer(log, "b")
    assert(#log == 0) -- nothing closed yet
  end
  assert(#log == 2)
  assert(log[1].name == "b" and log[1].err == nil)
  assert(log[2].name == "a" and log[2].err == nil)
end

-- __close runs on return, before control reaches the caller's next line
do
  local log = {}
  local function f()
    local x <close> = closer(log, "x")
    return "ret"
  end
  assert(f() == "ret")
  assert(#log == 1 and log[1].name == "x")
end

-- __close runs on break and on goto out of the scope
do
  local log = {}
  for i = 1, 3 do
    local x <close> = closer(log, "iter" .. i)
    if i == 2 then break end
  end
  assert(#log == 2) -- closed once per completed/broken iteration
  assert(log[1].name == "iter1" and log[2].name == "iter2")

  local log2 = {}
  do
    local y <close> = closer(log2, "y")
    goto out
  end
  ::out::
  assert(#log2 == 1 and log2[1].name == "y")
end

-- on error, __close receives the error object and still runs in reverse
do
  local log = {}
  local ok, err = pcall(function()
    local a <close> = closer(log, "a")
    local b <close> = closer(log, "b")
    error("scope error")
  end)
  assert(not ok and err:find("scope error"))
  assert(#log == 2)
  assert(log[1].name == "b")
  assert(type(log[1].err) == "string" and log[1].err:find("scope error"))
  assert(log[2].name == "a")
  assert(type(log[2].err) == "string" and log[2].err:find("scope error"))
end

-- nil and false are permitted as to-be-closed values (nothing to close)
do
  local ok = pcall(function()
    local a <close> = nil
    local b <close> = false
    return true
  end)
  assert(ok)
end

-- a non-closable value raises at declaration time
do
  local ok, err = pcall(load("local x <close> = 42"))
  assert(not ok)
  assert(err:find("non%-closable value"))
  -- a table without __close is also non-closable
  local ok2, err2 = pcall(load("local x <close> = {}"))
  assert(not ok2)
  assert(err2:find("non%-closable value"))
end

-- an error inside __close propagates to the caller
do
  local ok, err = pcall(function()
    local x <close> = setmetatable({}, {
      __close = function() error("close failed") end,
    })
  end)
  assert(not ok)
  assert(err:find("close failed"))
end

-- error in one __close does not prevent the remaining closes
do
  local log = {}
  local ok, err = pcall(function()
    local a <close> = closer(log, "a")
    local b <close> = setmetatable({}, {
      __close = function() error("b close failed") end,
    })
  end)
  assert(not ok and err:find("b close failed"))
  assert(#log == 1 and log[1].name == "a") -- a was still closed
end

-- coroutine.close runs pending __close handlers of a suspended coroutine
do
  local log = {}
  local co = coroutine.create(function()
    local x <close> = closer(log, "x")
    coroutine.yield()
    return "unreached"
  end)
  assert(coroutine.resume(co))
  assert(#log == 0) -- still suspended, not closed
  assert(coroutine.close(co) == true)
  assert(#log == 1 and log[1].name == "x")
  assert(coroutine.status(co) == "dead")
end

-- a coroutine dying from an error keeps its pending tbc variables (the
-- stack survives for post-mortem inspection); coroutine.close then runs
-- them with the original error and returns false + that error
do
  local log = {}
  local co = coroutine.create(function()
    local x <close> = closer(log, "x")
    error("coroutine error")
  end)
  local ok, err = coroutine.resume(co)
  assert(not ok and err:find("coroutine error"))
  assert(#log == 0) -- not closed by the death itself
  local cok, cerr = coroutine.close(co)
  assert(cok == false and cerr:find("coroutine error"))
  assert(#log == 1 and log[1].name == "x")
  assert(type(log[1].err) == "string" and log[1].err:find("coroutine error"))
end

-- __close error surfaces through coroutine.close as false + error
do
  local co = coroutine.create(function()
    local x <close> = setmetatable({}, {
      __close = function() error("close boom") end,
    })
    coroutine.yield()
  end)
  assert(coroutine.resume(co))
  local ok, err = coroutine.close(co)
  assert(ok == false)
  assert(err:find("close boom"))
end

-- tbc value is still usable inside its scope; __close gets the object
do
  local seen
  do
    local handle <close> = setmetatable({ open = true }, {
      __close = function(self) seen = self; self.open = false end,
    })
    assert(handle.open == true)
  end
  assert(seen ~= nil and seen.open == false)
end
