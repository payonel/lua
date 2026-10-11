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
  local inc2 = counter()   -- same prototype as 'inc'
  local co, get_local
  if with_thread then
    co = coroutine.create(function(a)
      local t = {a, "local"}
      local closing <close> = setmetatable({}, {__close = function() end})
      coroutine.yield(function() return t end)
      return a
    end)
    debug.sethook(co, function() end, "l", 3)
    local _, open = coroutine.resume(co, 5)
    get_local = open
  end
  local layout = {}
  for i = 1, 40 do layout["k" .. i] = i end
  for i = 1, 40, 3 do layout["k" .. i] = nil end
  local t = {1, 2.5, "string", ("long"):rep(20), inc = inc, get = get, co = co,
             open = get_local, inc2 = inc2, layout = layout}
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
  assert(#blob > 20000)
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
    collectgarbage(); collectgarbage() -- deleted keys become dead keys, as in each attempt
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
    assert(r.inc2() == 1 and r.get() == 1)
    assert(r.open()[1] == 5)
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

  -- the trailer's parts: tables marked for finalization, dead keys
  local function marked_sample(log)
    local mt = {__gc = function(o) log[#log + 1] = o.name end}
    local v = {}
    for i = 1, 4 do v[i] = setmetatable({name = "m" .. i}, mt) end
    local t = {}
    for i = 1, 30 do t["d" .. i] = i end
    local gone = {}
    for i = 1, 30, 2 do t["d" .. i] = nil; gone[#gone + 1] = "d" .. i end
    collectgarbage(); collectgarbage() -- the deleted keys become dead keys
    v.t, v.gone = t, gone
    return v
  end

  case("persist of marked tables and dead keys survives an allocation failure at every point", function()
    local log = {}
    local v = marked_sample(log)
    local expected = styx.persist({[log] = "log"}, v)
    local blob
    local n = fails_cleanly_at_every_allocation(function() blob = styx.persist({[log] = "log"}, v) end)
    assert(n > 0 and blob == expected)
  end)

  case("unpersist of marked tables and dead keys survives an allocation failure at every point", function()
    local log = {}
    local v = marked_sample(log)
    local blob = styx.persist({[log] = "log"}, v)
    local r
    local n = fails_cleanly_at_every_allocation(function() r = styx.unpersist({log = log}, blob) end)
    assert(n > 0)
    collectgarbage(); collectgarbage()
    assert(#log == 0, "failed attempts ran finalizers: " .. table.concat(log, " "))
    for i, k in ipairs(r.gone) do
      assert(pcall(next, r.t, k), "next from a deleted key")
      assert(next(r.t, k) == next(v.t, v.gone[i]))
    end
    v, r = nil, nil
    collectgarbage(); collectgarbage()
    assert(#log == 8, table.concat(log, " "))
  end)

  case("object keys, kept in place or rebuilt, survive an allocation failure at every point", function()
    local objs, t = {}, {}
    for i = 1, 30 do objs[i] = (i % 2 == 0) and {} or function() end; t[objs[i]] = i end
    for i = 1, 30, 4 do t[objs[i]] = nil end
    collectgarbage(); collectgarbage()
    local perm, other = {}, {}
    local mixed = {[perm] = "perm"}   -- 'perm' comes back as 'other': rebuilt
    for i = 1, 10 do mixed[{}] = i end
    local blob = styx.persist({[perm] = "perm"}, {t = t, objs = objs, mixed = mixed})
    local r
    local n = fails_cleanly_at_every_allocation(function() r = styx.unpersist({perm = other}, blob) end)
    assert(n > 0)
    for i = 1, 30 do if i % 4 ~= 1 then assert(r.t[r.objs[i]] == i) end end
    assert(r.mixed[other] == "perm")
  end)

  case("an error with a path survives an allocation failure at every point", function()
    local value = {a = {b = {c = io.stdout}}, list = {1, 2, 3}}
    local n = 0
    while true do
      collectgarbage()
      T.alloccount(n)
      local ok, err = pcall(styx.persist, value)
      T.alloccount()
      assert(not ok)
      H.checkheap()
      err = tostring(err)
      if err:find("(at root.a.b.c)", 1, true) then break end
      assert(err:find("not enough memory", 1, true), "allocation " .. n .. ": " .. err)
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
