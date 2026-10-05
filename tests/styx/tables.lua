-- tables: nesting, shared references, cycles, metatables, key kinds
local styx = require "styx"
local H = require "harness"
local case = H.case
local roundtrip = H.roundtrip

case("empty table", function()
  local r = roundtrip({})
  assert(next(r) == nil)
end)

case("sequence", function()
  local t = {}
  for i = 1, 1000 do t[i] = i * 2 end
  local r = roundtrip(t)
  assert(#r == 1000 and r[500] == 1000)
end)

case("hash part with every key kind", function()
  local t = {name = "n", [1.5] = "float", [true] = "yes", [false] = "no",
             [-7] = "negative", [math.maxinteger] = "max", [""] = "empty"}
  local r = roundtrip(t)
  assert(r[1.5] == "float" and r[true] == "yes" and r[false] == "no")
  assert(r[-7] == "negative" and r[math.maxinteger] == "max" and r[""] == "empty")
end)

case("float keys with integral values stay integer keys", function()
  local t = {}
  t[2.0] = "two"
  local r = roundtrip(t)
  assert(r[2] == "two" and math.type(next(r)) == "integer")
end)

case("mixed array and hash parts", function()
  local t = {10, 20, 30, x = 1, y = {1, 2}}
  local r = roundtrip(t)
  assert(#r == 3 and r[3] == 30 and r.y[2] == 2)
end)

case("nested tables", function()
  local t = {a = {b = {c = {d = {e = "deep"}}}}}
  assert(roundtrip(t).a.b.c.d.e == "deep")
end)

case("deep nesting below the depth limit", function()
  local t = {}
  local cur = t
  for _ = 1, 2000 do cur.next = {}; cur = cur.next end
  cur.last = true
  local r = roundtrip(t)
  for _ = 1, 2000 do r = r.next end
  assert(r.last == true)
end)

case("shared references stay shared", function()
  local shared = {1, 2, 3}
  local t = {a = shared, b = shared, list = {shared, shared}}
  local r = roundtrip(t)
  assert(r.a == r.b and r.list[1] == r.a and r.list[2] == r.a)
  assert(r.a ~= shared)
end)

case("cycles", function()
  local t = {}
  t.self = t
  local a, b = {}, {}
  a.b, b.a = b, a
  t.pair = a
  local r = roundtrip(t)
  assert(r.self == r and r.pair.b.a == r.pair)
end)

case("tables as keys", function()
  local k1, k2 = {"k1"}, {"k2"}
  local t = {[k1] = "one", [k2] = "two", keys = {k1, k2}}
  local r = roundtrip(t)
  assert(r[r.keys[1]] == "one" and r[r.keys[2]] == "two")
end)

case("a table that is its own key", function()
  local t = {}
  t[t] = t
  local r = roundtrip(t)
  assert(r[r] == r)
end)

case("metatables", function()
  local mt = {__index = function(_, k) return k .. "!" end}
  local t = setmetatable({}, mt)
  local r = roundtrip(t)
  assert(r.hi == "hi!")
end)

case("a shared metatable stays shared", function()
  local mt = {__index = {kind = "shared"}}
  local t = {setmetatable({}, mt), setmetatable({}, mt)}
  local r = roundtrip(t)
  assert(getmetatable(r[1]) == getmetatable(r[2]) and r[1].kind == "shared")
end)

case("a table that is its own metatable", function()
  local t = {__index = function() return "self" end}
  setmetatable(t, t)
  local r = roundtrip(t)
  assert(getmetatable(r) == r and r.anything == "self")
end)

-- closures made here have closed upvalues (open ones are not supported yet)
local function make_vector_class()
  local V = {}
  V.__index = V
  V.__add = function(a, b) return setmetatable({x = a.x + b.x}, V) end
  V.__eq = function(a, b) return a.x == b.x end
  V.__len = function(a) return a.x end
  V.__tostring = function(a) return "V(" .. a.x .. ")" end
  return V
end

case("metamethods work after restore", function()
  local V = make_vector_class()
  local r = roundtrip({setmetatable({x = 1}, V), setmetatable({x = 2}, V)},
                      {[_ENV] = "env"}, {env = _ENV})
  local s = r[1] + r[2]
  assert(s.x == 3 and #s == 3 and tostring(s) == "V(3)")
  assert(r[1] == setmetatable({x = 1}, getmetatable(r[1])))
end)

case("metatables with __gc are restored and finalize", function()
  local log = {}
  local function make_mt(l) return {__gc = function(o) l[#l + 1] = o.id end} end
  local mt = make_mt(log)
  local r = styx.unpersist({log = log}, styx.persist({[log] = "log"}, setmetatable({id = 7}, mt)))
  assert(r.id == 7)
  r = nil
  collectgarbage(); collectgarbage()
  assert(log[#log] == 7, "restored object was finalized")
end)

case("weak tables keep their mode", function()
  local t = setmetatable({}, {__mode = "k"})
  local r = roundtrip(t)
  assert(getmetatable(r).__mode == "k")
  r[{}] = true
  collectgarbage()
  assert(next(r) == nil, "restored weak table drops dead keys")
end)

case("rawequal and identity across the graph", function()
  local leaf = {}
  local t = {leaf, {leaf}, {{leaf}}}
  local r = roundtrip(t)
  assert(rawequal(r[1], r[2][1]) and rawequal(r[1], r[3][1][1]))
end)

case("restored tables are independent of the originals", function()
  local t = {v = 1}
  local r = roundtrip(t)
  r.v = 2
  assert(t.v == 1)
end)

case("rejects truncation of a table stream", function()
  local blob = styx.persist({1, "two", {3}, x = {y = "z"}})
  H.rejects_truncation(blob)
end)
