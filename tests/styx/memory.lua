-- persist and unpersist are memory-accounted: what they allocate is charged
-- to the state, and running out of memory at any allocation is a clean error
local styx = require "styx"
local H = require "harness"
local case = H.case

local perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
local back = {env = _ENV, yield = coroutine.yield}

-- a value that exercises every kind of object Styx handles today
local function sample(with_thread)
  local function counter()
    local n = 0
    return function() n = n + 1; return n end, function() return n end
  end
  local inc, get = counter()
  local co
  if with_thread then
    co = coroutine.create(function(a)
      local t = {a, "local"}
      coroutine.yield(t)
      return a
    end)
    coroutine.resume(co, 5)
  end
  local t = {1, 2.5, "string", ("long"):rep(20), inc = inc, get = get, co = co}
  t.self = t
  return setmetatable(t, {__index = {extra = true}})
end

case("unpersist is charged to the state", function()
  local big = {}
  for i = 1, 20000 do big[i] = "item" .. i end
  local blob = styx.persist(big)
  big = nil
  collectgarbage(); collectgarbage()
  local before = collectgarbage("count")
  local r = styx.unpersist(blob)
  local after = collectgarbage("count")
  assert(after - before > 300, "restored data accounted: " .. (after - before) .. "K")
  assert(#r == 20000)
end)

case("persist output is charged to the state", function()
  local t = {}
  for i = 1, 20000 do t[i] = i end
  collectgarbage(); collectgarbage()
  collectgarbage("stop")
  local before = collectgarbage("count")
  local blob = styx.persist(t)
  local after = collectgarbage("count")
  collectgarbage("restart")
  assert(#blob > 20000 * 8)
  assert(after - before > #blob / 1024, "output accounted")
end)

-- T.alloccount(n) makes allocation n+1 fail (debug builds only)
local function fails_cleanly_at_every_allocation(f)
  local n = 0
  while true do
    collectgarbage()
    T.alloccount(n)
    local ok, err = pcall(f)
    T.alloccount()
    if ok then return n end
    assert(tostring(err):find("not enough memory", 1, true),
           "allocation " .. n .. ": " .. tostring(err))
    H.checkheap()
    n = n + 1
    assert(n < 100000, "no end to allocations")
  end
end

if T then
  case("persist survives an allocation failure at every point", function()
    local v = sample(true)
    local expected = styx.persist(perms, v)
    local blob
    local n = fails_cleanly_at_every_allocation(function() blob = styx.persist(perms, v) end)
    assert(n > 0 and #blob == #expected)
  end)

  case("unpersist survives an allocation failure at every point", function()
    local blob = styx.persist(perms, sample(true))
    local r
    local n = fails_cleanly_at_every_allocation(function() r = styx.unpersist(back, blob) end)
    assert(n > 0)
    assert(r.get() == 0 and r.inc() == 1 and r.self == r and r.extra == true)
    assert(select(2, coroutine.resume(r.co)) == 5)
  end)

  -- the error from a failed allocation must reach the pcall of the coroutine
  -- that called unpersist, not a handler further out
  case("allocation failures in a coroutine's unpersist stay in that coroutine", function()
    local blob = styx.persist(perms, sample(true))
    local n = 0
    while true do
      collectgarbage()
      local co = coroutine.create(function()
        T.alloccount(n)
        local ok, err = pcall(styx.unpersist, back, blob)
        T.alloccount()
        return ok, err
      end)
      local resumed, ok, err = coroutine.resume(co)
      T.alloccount()
      assert(resumed, "allocation " .. n .. ": error escaped the coroutine: " .. tostring(ok))
      assert(coroutine.status(co) == "dead")
      H.checkheap()
      if ok then break end
      assert(tostring(err):find("not enough memory", 1, true),
             "allocation " .. n .. ": " .. tostring(err))
      n = n + 1
      assert(n < 100000, "no end to allocations")
    end
    assert(n > 0)
  end)

  case("unpersist under a memory limit", function()
    local big = {}
    for i = 1, 5000 do big[i] = {i, tostring(i)} end
    local blob = styx.persist(big)
    big = nil
    collectgarbage(); collectgarbage()
    T.totalmem(T.totalmem() + 20000)
    local ok, err = pcall(styx.unpersist, blob)
    T.totalmem(0)
    assert(not ok and tostring(err):find("not enough memory", 1, true), tostring(err))
    collectgarbage()
    H.checkheap()
    assert(#styx.unpersist(blob) == 5000)
  end)
end
