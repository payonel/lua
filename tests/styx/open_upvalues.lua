-- open upvalues: closures over locals of functions that are still running.
-- An upvalue owned by a suspended coroutine stays open (shared with that
-- coroutine's stack) after restore; one owned by a thread that cannot be
-- persisted (running, or the main thread) is restored as its current value.
local styx = require "styx"
local H = require "harness"
local case = H.case

local perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
local back = {env = _ENV, yield = coroutine.yield}
local function roundtrip(v) return H.roundtrip(v, perms, back) end

case("a closure over a live local is restored with its current value", function()
  local n = 10
  local function get() return n end
  n = 11
  local r = roundtrip(get)
  assert(r() == 11)
  n = 12
  assert(r() == 11, "the restored closure does not follow the original local")
end)

case("closures sharing a live local share it after restore", function()
  local n = 0
  local function inc() n = n + 1; return n end
  local function get() return n end
  inc()
  local r = roundtrip({inc, get})
  r[1](); r[1]()
  assert(r[2]() == 3)
  assert(n == 1, "the original local is untouched")
end)

case("live locals of different functions stay separate", function()
  local a = "a"
  local function outer()
    local b = "b"
    return function() return a .. b end
  end
  local r = roundtrip(outer())
  assert(r() == "ab")
end)

case("a coroutine and a closure over its local stay connected", function()
  local co = coroutine.create(function()
    local n = 0
    local function inc() n = n + 1 end
    coroutine.yield(inc)
    return n
  end)
  local _, inc = coroutine.resume(co)
  local r = roundtrip({co = co, inc = inc})
  r.inc(); r.inc(); r.inc()
  assert(select(2, coroutine.resume(r.co)) == 3)
end)

case("a closure persisted alone brings the coroutine that owns its upvalue", function()
  local co = coroutine.create(function()
    local items = {}
    coroutine.yield(function(x) items[#items + 1] = x; return #items end)
    return #items
  end)
  local _, add = coroutine.resume(co)
  add("first")
  local radd = roundtrip(add)
  assert(radd("second") == 2)
end)

case("the coroutine sees writes made through the restored closure", function()
  local co = coroutine.create(function()
    local msg = "start"
    local function set(v) msg = v end
    local function get() return msg end
    coroutine.yield(set, get)
    return msg
  end)
  local _, set, get = coroutine.resume(co)
  local r = roundtrip({co, set, get})
  r[2]("changed")
  assert(r[3]() == "changed")
  assert(select(2, coroutine.resume(r[1])) == "changed")
end)

case("upvalues at several levels of one coroutine", function()
  local function make()
    local function inner(f)
      local y = 2
      coroutine.yield(f, function() return y end)
      return y
    end
    return coroutine.create(function()
      local x = 1
      local r = inner(function() x = x + 100; return x end)
      return x + r
    end)
  end
  local co = make()
  local _, fx, fy = coroutine.resume(co)
  local r = roundtrip({co, fx, fy})
  assert(r[2]() == 101 and r[3]() == 2)
  assert(select(2, coroutine.resume(r[1])) == 103)
end)

case("the coroutine finishing closes the upvalue; the closure keeps it", function()
  local co = coroutine.create(function()
    local n = 5
    coroutine.yield(function() n = n + 1; return n end)
    n = 50
  end)
  local _, f = coroutine.resume(co)
  local r = roundtrip({co, f})
  assert(r[2]() == 6)
  coroutine.resume(r[1])
  assert(coroutine.status(r[1]) == "dead")
  assert(r[2]() == 51)
  collectgarbage()
  assert(r[2]() == 52)
end)

case("a closure over a coroutine's local, held by another coroutine", function()
  local owner = coroutine.create(function()
    local v = "owner's"
    coroutine.yield(function() return v end)
  end)
  local _, get = coroutine.resume(owner)
  local other = coroutine.create(function(g) coroutine.yield(); return g() end)
  coroutine.resume(other, get)
  local r = roundtrip({other = other, owner = owner})
  assert(select(2, coroutine.resume(r.other)) == "owner's")
end)

case("a restored coroutine with open upvalues persists again", function()
  local co = coroutine.create(function()
    local n = 0
    while true do coroutine.yield(function() n = n + 1; return n end) end
  end)
  local _, f = coroutine.resume(co)
  local r1 = roundtrip({co, f})
  r1[2]()
  local r2 = roundtrip(r1)
  assert(r2[2]() == 2)
  local _, f2 = coroutine.resume(r2[1])
  assert(f2() == 3, "a new closure of the restored coroutine sees the same local")
end)

case("a closure over a local of the persisting coroutine is a snapshot", function()
  local co = coroutine.wrap(function()
    local n = 7
    local function get() return n end
    local blob = styx.persist(perms, get)
    n = 8
    return styx.unpersist(back, blob)
  end)
  assert(co()() == 7)
end)

case("rejects truncation of a stream with open upvalues", function()
  local co = coroutine.create(function()
    local n = 1
    coroutine.yield(function() return n end)
  end)
  local _, f = coroutine.resume(co)
  H.rejects_truncation(styx.persist(perms, {co, f}), back)
end)
