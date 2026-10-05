-- Lua closures: code, closed upvalues, sharing, recursion, permanents.
-- Closures are made by factory functions that have returned, so their
-- upvalues are closed (open upvalues are not supported yet).
local styx = require "styx"
local H = require "harness"
local case = H.case
local roundtrip = H.roundtrip

local function counter()
  local n = 0
  return function() n = n + 1; return n end, function() return n end
end

case("function without upvalues", function()
  local f = roundtrip(function(a, b) return a * b + 1 end)
  assert(f(3, 4) == 13)
end)

case("closures share one upvalue", function()
  local inc, get = counter()
  inc(); inc()
  local r = roundtrip({inc, get})
  local rinc, rget = r[1], r[2]
  assert(rget() == 2)
  rinc()
  assert(rget() == 3)       -- the restored closures share one upvalue
  assert(get() == 2)        -- and are independent of the originals
end)

case("separate instances keep separate upvalues", function()
  local inc1, get1 = counter()
  local inc2, get2 = counter()
  inc1()
  local r = roundtrip({inc1, get1, inc2, get2})
  r[3](); r[3]()
  assert(r[2]() == 1 and r[4]() == 2)
end)

case("an upvalue shared by closures reached by different paths", function()
  local inc, get = counter()
  local r = roundtrip({a = {inc}, b = {{get}}})
  r.a[1]()
  assert(r.b[1][1]() == 1)
end)

case("recursive local function", function()
  local function make_fact()
    local function fact(n) if n <= 1 then return 1 end return n * fact(n - 1) end
    return fact
  end
  assert(roundtrip(make_fact())(10) == 3628800)
end)

case("mutually recursive closures", function()
  local function make()
    local even, odd
    function even(n) if n == 0 then return true end return odd(n - 1) end
    function odd(n) if n == 0 then return false end return even(n - 1) end
    return even, odd
  end
  local even, odd = make()
  local r = roundtrip({even, odd})
  assert(r[1](10) == true and r[2](7) == true)
end)

case("closures returning closures", function()
  local function adder(x) return function(y) return function(z) return x + y + z end end end
  local add1 = adder(1)
  local r = roundtrip(add1)
  assert(r(2)(3) == 6)
end)

case("upvalues holding tables and cycles", function()
  local function make()
    local state = {count = 0}
    local f = function() state.count = state.count + 1; return state end
    state.f = f
    return f
  end
  local r = roundtrip(make())
  local st = r()
  assert(st.count == 1 and st.f == r)
end)

case("a closure stored inside the table it captures", function()
  local function make()
    local t = {}
    t.get = function() return t end
    return t
  end
  local r = roundtrip(make())
  assert(r.get() == r)
end)

case("vararg functions", function()
  local f = roundtrip(function(...) return select("#", ...), ... end, {[_ENV] = "env"}, {env = _ENV})
  local n, a, b = f("x", nil)
  assert(n == 2 and a == "x" and b == nil)
end)

case("functions with many constants and nested prototypes", function()
  local function make()
    local k = {}
    for i = 1, 300 do k[i] = "c" .. i end
    return function(i)
      local function inner(x) return x .. "!" end
      return inner(k[i]), 1.5, 2^60, "literal"
    end
  end
  local r = roundtrip(make())
  local a, b, c, d = r(300)
  assert(a == "c300!" and b == 1.5 and c == 2^60 and d == "literal")
end)

case("debug information survives", function()
  local f = roundtrip(function() return debug.getinfo(1, "l").currentline end,
                     {[_ENV] = "env"}, {env = _ENV})
  local info = debug.getinfo(f, "S")
  assert(info.what == "Lua" and info.linedefined > 0)
  assert(f() > 0)
end)

case("_ENV through the permanents table", function()
  local function make() return function() return tostring(42) end end
  local r = roundtrip(make(), {[_ENV] = "env"}, {env = _ENV})
  assert(r() == "42")
end)

case("_ENV replaced by another table on restore", function()
  local function make() return function() return x end end
  local env = {x = "from other env"}
  local r = styx.unpersist({env = env}, styx.persist({[_ENV] = "env"}, make()))
  assert(r() == "from other env")
end)

case("a C function reachable from a closure goes through the permanents", function()
  local function make_printer()
    local p = print
    return function() return p end
  end
  local restored = styx.unpersist({print = tostring},
                                   styx.persist({[print] = "print"}, make_printer()))
  assert(restored() == tostring)
end)

case("two closures of one prototype work independently", function()
  local a1 = counter()
  local a2 = counter()
  local r = roundtrip({a1, a2})
  assert(r[1]() == 1 and r[1]() == 2 and r[2]() == 1)
end)

case("rejects truncation of a closure stream", function()
  local inc, get = counter()
  H.rejects_truncation(styx.persist({inc, get}))
end)
