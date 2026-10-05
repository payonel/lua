-- the permanents table: objects written as keys and resolved on unpersist
local styx = require "styx"
local H = require "harness"
local case = H.case

case("a permanent restores to exactly the object supplied on unpersist", function()
  local original, target = {}, {}
  local r = styx.unpersist({obj = target}, styx.persist({[original] = "obj"}, {original}))
  assert(rawequal(r[1], target))
end)

case("string keys", function()
  local blob = styx.persist({[print] = "print"}, {print})
  local r = styx.unpersist({print = tostring}, blob)
  assert(r[1] == tostring)
end)

case("number, float and boolean keys", function()
  for _, key in ipairs{1, -5, 2.5, true, false, math.maxinteger} do
    local blob = styx.persist({[print] = key}, {print})
    local r = styx.unpersist({[key] = type}, blob)
    assert(r[1] == type, tostring(key))
  end
end)

case("tables are permanents too", function()
  local registry = {name = "original"}
  local other = {name = "replacement"}
  local t = {ref = registry, again = registry}
  local r = styx.unpersist({reg = other}, styx.persist({[registry] = "reg"}, t))
  assert(r.ref == other and r.again == other)
end)

case("a permanent table is written as its key, not its contents", function()
  local big = {}
  for i = 1, 1000 do big[i] = i end
  local with = #styx.persist({[big] = "big"}, {big})
  local without = #styx.persist({big})
  assert(with < 100 and without > 1000)
end)

case("a permanent referenced many times resolves to one object", function()
  local t = {print, print, {print}}
  local r = styx.unpersist({p = tostring}, styx.persist({[print] = "p"}, t))
  assert(r[1] == tostring and r[2] == tostring and r[3][1] == tostring)
end)

case("coroutines and strings can be permanents", function()
  local co = coroutine.create(function() end)
  local other = coroutine.create(function() end)
  local s = ("long string "):rep(10)
  local r = styx.unpersist({co = other, s = "short"},
                            styx.persist({[co] = "co", [s] = "s"}, {co, s}))
  assert(r[1] == other and r[2] == "short")
end)

case("the root value itself can be a permanent", function()
  local r = styx.unpersist({x = tostring}, styx.persist({[print] = "x"}, print))
  assert(r == tostring)
end)

case("unknown key on unpersist is an error naming the key", function()
  local blob = styx.persist({[print] = "my.print"}, print)
  local ok, err = pcall(styx.unpersist, {}, blob)
  assert(not ok and err:find("my.print", 1, true))
end)

case("wrong type on unpersist is an error", function()
  local blob = styx.persist({[print] = "print"}, print)
  local ok, err = pcall(styx.unpersist, {print = 5}, blob)
  assert(not ok and err:find("function expected, got number", 1, true))
end)

case("a standard library table as a permanent", function()
  local function make() return function(s) return string.upper(s) end end
  local perms = {[_ENV] = "env"}
  local r = styx.unpersist({env = {string = {upper = function(s) return "UP:" .. s end}}},
                            styx.persist(perms, make()))
  assert(r("x") == "UP:x")
end)

case("permanents do not change the caller's tables", function()
  local perms = {[print] = "print"}
  local back = {print = print}
  styx.unpersist(back, styx.persist(perms, {print}))
  assert(perms[print] == "print" and back.print == print)
  local n = 0
  for _ in pairs(perms) do n = n + 1 end
  assert(n == 1)
end)

-- variables as permanents: perms[debug.upvalueid(f, i)] = key on persist;
-- on unpersist back[key] is a function whose first upvalue is the variable
-- the restored closures then share

local function counter()
  local n = 0
  return function() n = n + 1; return n end, function() return n end
end

case("a captured variable restores as the variable supplied", function()
  local inc, get = counter()
  inc()
  local x = 100
  local blob = styx.persist({[debug.upvalueid(inc, 1)] = "count"}, {inc, get})
  local r = styx.unpersist({count = function() return x end}, blob)
  assert(r[2]() == 100, "the restored closures read the supplied variable")
  r[1]()
  assert(x == 101, "and write it")
  x = 7
  assert(r[2]() == 7)
  assert(get() == 1, "the original variable is untouched")
end)

case("a variable of the running scope stays connected", function()
  local count = 0
  local co = coroutine.create(function()
    while true do coroutine.yield(); count = count + 1 end
  end)
  coroutine.resume(co)
  local fn = debug.getinfo(co, 1, "f").func
  local idx
  for i = 1, debug.getinfo(fn, "u").nups do
    if debug.getupvalue(fn, i) == "count" then idx = i end
  end
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [debug.upvalueid(fn, idx)] = "count"}
  local back = {env = _ENV, yield = coroutine.yield, count = function() return count end}
  local r = styx.unpersist(back, styx.persist(perms, co))
  coroutine.resume(r)
  coroutine.resume(r)
  assert(count == 2, "the restored coroutine updates this scope's variable")
end)

case("a missing or unusable variable permanent is an error", function()
  local inc = counter()
  local blob = styx.persist({[debug.upvalueid(inc, 1)] = "count"}, inc)
  local ok, err = pcall(styx.unpersist, {}, blob)
  assert(not ok and err:find("count", 1, true), err)
  ok, err = pcall(styx.unpersist, {count = 5}, blob)
  assert(not ok and err:find("function", 1, true), err)
  ok, err = pcall(styx.unpersist, {count = function() end}, blob)
  assert(not ok and err:find("function", 1, true), err)
end)
