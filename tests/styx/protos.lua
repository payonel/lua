-- function prototypes: closures of one prototype share it after restore,
-- so the prototype is written once and restored once
local styx = require "styx"
local H = require "harness"
local case = H.case

-- a prototype that is large on its own (200 distinct string constants),
-- so that sharing it is visible against each closure's own small cost
local factory_source
do
  local k = {}
  for i = 1, 200 do k[i] = string.format("%q", "constant number " .. i) end
  factory_source = "return function(n) return function() return n, {" ..
                   table.concat(k, ", ") .. "} end end"
end

local function make_factory()
  return assert(load(factory_source))()
end

case("closures of one prototype write it once", function()
  local factory = make_factory()
  local one = #styx.persist({factory(1)})
  local many = {}
  for i = 1, 20 do many[i] = factory(i) end
  local all = #styx.persist(many)
  assert(all < one * 2, "20 closures: " .. all .. " bytes, one: " .. one)
end)

case("closures of one prototype keep their own upvalues", function()
  local factory = make_factory()
  local r = H.roundtrip({factory(1), factory(2), factory(3)})
  assert(select(1, r[1]()) == 1 and select(1, r[2]()) == 2 and select(1, r[3]()) == 3)
end)

case("restoring many closures of one prototype charges it once", function()
  local factory = make_factory()
  local blob_one = styx.persist({factory(1)})
  local fs = {}
  for i = 1, 50 do fs[i] = factory(i) end
  local blob_many = styx.persist(fs)
  collectgarbage(); collectgarbage()
  local m0 = collectgarbage("count")
  local one = styx.unpersist(blob_one)
  local m1 = collectgarbage("count")
  local many = styx.unpersist(blob_many)
  local m2 = collectgarbage("count")
  assert(one and many)
  assert(m2 - m1 < (m1 - m0) * 5,
         string.format("50 closures: %.1fK, one: %.1fK", m2 - m1, m1 - m0))
end)

case("closures of different prototypes stay separate", function()
  local function a() return "a" end
  local function b() return "b" end
  local r = H.roundtrip({a, b, a})
  assert(r[1]() == "a" and r[2]() == "b" and r[3] == r[1])
end)

case("a prototype shared between a coroutine and a closure", function()
  local function make()
    local function step(x) coroutine.yield(x); return x end
    return coroutine.create(function() return step(1) + step(2) end), step
  end
  local co, step = make()
  coroutine.resume(co)
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
  local back = {env = _ENV, yield = coroutine.yield}
  local r = H.roundtrip({co, step}, perms, back)
  assert(select(2, coroutine.resume(r[1])) == 2)
  assert(select(2, coroutine.resume(r[1])) == 3)
end)

case("a nested prototype is written once with its parent", function()
  local factory = make_factory()       -- its prototype nests the inner one
  local inner = factory(1)
  local alone = #styx.persist({inner})
  local both = #styx.persist({factory, inner})
  local parent_only = #styx.persist({factory})
  assert(both < parent_only + alone / 2,
         string.format("both: %d, parent: %d, inner: %d", both, parent_only, alone))
  local r = H.roundtrip({factory, inner})
  assert(select(1, r[2]()) == 1 and select(1, r[1](7)()) == 7)
end)

case("rejects truncation of a stream with shared prototypes", function()
  local factory = make_factory()
  H.rejects_truncation(styx.persist({factory(1), factory(2)}))
end)
