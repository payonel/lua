-- coroutines: unstarted, suspended in a loop, with locals on the stack
local styx = require "styx"
local perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
local back = {env = _ENV, yield = coroutine.yield}
local function roundtrip(co) return styx.unpersist(back, styx.persist(perms, co)) end

local co = coroutine.create(function(a, b)
  local x = coroutine.yield(a + b)
  return x * 2
end)
local c1 = roundtrip(co)
assert(c1 ~= co and coroutine.status(c1) == "suspended")
assert(select(2, coroutine.resume(c1, 1, 2)) == 3)
assert(select(2, coroutine.resume(c1, 21)) == 42)
assert(coroutine.status(c1) == "dead")
assert(coroutine.status(co) == "suspended")   -- the original is untouched

local gen = coroutine.create(function()
  for i = 1, 3 do coroutine.yield(i) end
  return "end"
end)
coroutine.resume(gen)
coroutine.resume(gen)
local g2 = roundtrip(gen)
assert(select(2, coroutine.resume(g2)) == 3)
assert(select(2, coroutine.resume(g2)) == "end")
assert(coroutine.status(g2) == "dead")

local dead = coroutine.create(function() end)
coroutine.resume(dead)
assert(coroutine.status(roundtrip(dead)) == "dead")
