-- Lua closures: upvalues (closed), sharing, recursion, permanents
local styx = require "styx"
local function roundtrip(v, p1, p2) return styx.unpersist(p2 or {}, styx.persist(p1 or {}, v)) end

local function counter()
  local n = 0
  return function() n = n + 1; return n end, function() return n end
end
local inc, get = counter()
inc(); inc()
local r = roundtrip({inc, get})
local rinc, rget = r[1], r[2]
assert(rget() == 2)
rinc()
assert(rget() == 3)       -- the restored closures share one upvalue
assert(get() == 2)        -- and are independent of the originals

local function make_fact()
  local function fact(n) if n <= 1 then return 1 end return n * fact(n - 1) end
  return fact
end
assert(roundtrip(make_fact())(5) == 120)

-- a C function reachable from a closure is restored through the permanents
local function make_printer()
  local p = print
  return function() return p end
end
local restored = roundtrip(make_printer(), {[print] = "print"}, {print = tostring})
assert(restored() == tostring)
