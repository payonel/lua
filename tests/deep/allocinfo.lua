-- tests/deep/allocinfo.lua
-- Client-visible allocation information: collectgarbage("count") and
-- collector control. Stock Lua 5.5 behavior is the reference.

-- count reflects allocation and collection
do
  collectgarbage("collect")
  local before = collectgarbage("count")
  assert(type(before) == "number" and before > 0)

  local t = {}
  for i = 1, 1000 do t[i] = { i, i + 1 } end
  local grown = collectgarbage("count")
  assert(grown > before)

  t = nil
  collectgarbage("collect")
  local shrunk = collectgarbage("count")
  assert(shrunk < grown)
end

-- count tracks allocation precisely enough to see a known-size payload,
-- and keeps tracking while the collector is stopped. The payload is built
-- as many distinct interned strings, which are fully accounted.
do
  collectgarbage("collect")
  collectgarbage("stop")
  local before = collectgarbage("count")
  local strs = {}
  for i = 1, 100 do
    strs[i] = string.rep("x", 1024) .. i -- 100 distinct ~1KB interned strings
  end
  local after = collectgarbage("count")
  assert(after - before >= 90) -- at least ~90 of the ~100 KB payload
  strs = nil
  collectgarbage("restart")
  collectgarbage("collect")
  assert(collectgarbage("count") < after)
end

-- a large buffer-built string (string.rep result) is fully accounted:
-- its payload registers in collectgarbage("count") and is credited
-- back when the string is collected
do
  collectgarbage("collect")
  local before = collectgarbage("count")
  local s = string.rep("x", 1024 * 1024) -- 1MB, buffer-built
  local after = collectgarbage("count")
  assert(#s == 1024 * 1024)
  assert(after - before >= 1024) -- the full 1024 KB payload is counted
  s = nil
  collectgarbage("collect")
  assert(collectgarbage("count") - before < 16) -- payload credited back
end

-- stop/restart/isrunning round trip
do
  assert(collectgarbage("isrunning"))
  collectgarbage("stop")
  assert(not collectgarbage("isrunning"))
  collectgarbage("stop") -- idempotent
  assert(not collectgarbage("isrunning"))
  collectgarbage("restart")
  assert(collectgarbage("isrunning"))
end

-- step returns a boolean (whether it finished a collection cycle)
do
  local r = collectgarbage("step")
  assert(type(r) == "boolean")
end

-- default option is "collect"; plain call returns 0
do
  assert(collectgarbage() == 0)
  assert(collectgarbage("collect") == 0)
end

-- invalid options raise
do
  local ok, err = pcall(collectgarbage, "no-such-option")
  assert(not ok)
  assert(err:find("invalid option", 1, true))
end

-- garbage actually becomes collectable: finalizer proves reclamation
do
  local finalized = false
  do
    local victim = setmetatable({}, { __gc = function() finalized = true end })
  end
  collectgarbage("collect")
  collectgarbage("collect") -- finalizers may need a second cycle
  assert(finalized)
end

-- weak tables drop entries under collection (allocation is reclaimable)
do
  local weak = setmetatable({}, { __mode = "v" })
  weak[1] = {}
  collectgarbage("collect")
  assert(weak[1] == nil)
end
