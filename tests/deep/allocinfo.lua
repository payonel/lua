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
-- and keeps tracking while the collector is stopped
do
  collectgarbage("collect")
  collectgarbage("stop")
  local before = collectgarbage("count")
  local s = string.rep("x", 100 * 1024) -- ~100 KB in one string
  local after = collectgarbage("count")
  assert(after - before >= 100) -- at least the payload, in KB
  s = nil
  collectgarbage("restart")
  collectgarbage("collect")
  assert(collectgarbage("count") < after)
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
