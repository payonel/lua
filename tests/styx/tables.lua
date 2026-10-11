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

case("no depth limit: a 200000-deep chain persists and restores", function()
  local t = {}
  local cur = t
  for i = 1, 200000 do cur.next = {i = i}; cur = cur.next end
  local r = styx.unpersist(styx.persist(t))
  local n = 0
  cur = r.next
  while cur do n = n + 1; assert(cur.i == n); cur = cur.next end
  assert(n == 200000)
end)

case("no depth limit: deeply nested closures", function()
  local function make(depth)
    local f = function() return 0 end
    for i = 1, depth do
      local g, k = f, i
      f = function() return g() + k end
    end
    return f
  end
  local f = make(5000)
  local r = styx.unpersist(styx.persist(f))
  assert(r() == f())
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

-- tables with holes have several borders; the one '#' finds depends on the
-- table's layout, so a restored table must keep the original layout
local function holey_tables()
  local list = {}
  list[#list + 1] = {1, nil, 3}
  list[#list + 1] = {1, 2, nil, 4, nil, nil, 7}
  local t = {}
  for i = 1, 20 do t[i] = i end
  t[5], t[17] = nil, nil
  list[#list + 1] = t
  t = {}
  t[1], t[2], t[4] = 1, 2, 4
  list[#list + 1] = t
  t = {n = 1, 10, 20, 30}
  t[2] = nil
  list[#list + 1] = t
  t = {}
  for i = 1, 100 do t[i] = i end
  for i = 1, 100, 3 do t[i] = nil end
  list[#list + 1] = t
  t = {}
  for i = 1, 64 do t[i] = i end
  for i = 33, 64 do t[i] = nil end   -- array part larger than its contents
  list[#list + 1] = t
  return list
end

case("the length of tables with holes is unchanged", function()
  for i, t in ipairs(holey_tables()) do
    local before = #t
    local r = roundtrip(t)
    assert(#r == before, "table " .. i .. ": #" .. #r .. " vs " .. before)
  end
end)

case("a table with holes behaves the same when it grows", function()
  for i, t in ipairs(holey_tables()) do
    local r = roundtrip(t)
    t[#t + 1] = "x"; r[#r + 1] = "x"
    assert(#r == #t, "table " .. i)
  end
end)

if T then
  case("array size, hash size and length hint are restored", function()
    local list = holey_tables()
    local big = {}
    for i = 1, 1000 do big["k" .. i] = i end
    for i = 1, 900 do big["k" .. i] = nil end   -- hash part larger than its contents
    list[#list + 1] = big
    list[#list + 1] = {}
    for i, t in ipairs(list) do
      local r = roundtrip(t)
      local a1, h1, l1 = T.querytab(t)
      local a2, h2, l2 = T.querytab(r)
      assert(a1 == a2 and h1 == h2 and l1 == l2,
             string.format("table %d: %d/%d/%d vs %d/%d/%d", i, a1, h1, l1, a2, h2, l2))
    end
  end)
end

-- order: a restored table iterates as the original (the same hash seed in
-- one state; keys that hash by content)
local function churned(n, seed)
  math.randomseed(seed)
  local t = {}
  for i = 1, n do t["key" .. math.random(n * 10)] = i end
  for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end
  for i = 1, n // 4 do t["new" .. math.random(n * 10)] = i end
  t[1.5], t[true], t[-7] = "f", "b", "n"
  return t
end

local function order(t)
  local o = {}
  for k in pairs(t) do o[#o + 1] = tostring(k) end
  return table.concat(o, " ")
end

case("a churned table iterates in the same order after restore", function()
  for seed = 1, 100 do
    local t = churned(64, seed)
    local r = roundtrip(t)
    assert(order(r) == order(t), "seed " .. seed)
  end
end)

case("deleted keys keep the order (after a collection)", function()
  for seed = 1, 30 do
    local t = churned(64, seed)
    for k in pairs(t) do if math.random() < 0.5 then t[k] = nil end end
    collectgarbage(); collectgarbage()
    local r = roundtrip(t)
    assert(order(r) == order(t), "seed " .. seed)
    r["added"] = true
    assert(r.added and r["key" .. 1] == t["key" .. 1])
  end
end)

case("a coroutine suspended in a pairs loop neither skips nor repeats", function()
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [pairs] = "pairs", [next] = "next"}
  local back = {env = _ENV, yield = coroutine.yield, pairs = pairs, next = next}
  for seed = 1, 100 do
    local t = churned(64, seed)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    local co = coroutine.create(function(tt)
      local seen = {}
      for k in pairs(tt) do seen[#seen + 1] = k; coroutine.yield() end
      return seen
    end)
    coroutine.resume(co, t)
    for _ = 1, n // 2 do coroutine.resume(co) end
    local r = styx.unpersist(back, styx.persist(perms, co))
    local ok, seen
    repeat ok, seen = coroutine.resume(r); assert(ok, seen) until coroutine.status(r) == "dead"
    local distinct = {}
    for _, k in ipairs(seen) do assert(not distinct[k], "repeated " .. tostring(k)); distinct[k] = true end
    assert(#seen == n, "seed " .. seed .. ": " .. #seen .. " of " .. n)
  end
end)

-- a key removed from a table becomes a dead key at the next collection;
-- 'next' still continues from it (ADR-29)
case("next from a deleted key, dead after a collection, continues as before", function()
  for seed = 1, 30 do
    local t = churned(64, seed)
    local keys = {}
    for k in pairs(t) do if type(k) == "string" then keys[#keys + 1] = k end end
    local gone = {}
    for i = 1, #keys, 3 do t[keys[i]] = nil; gone[#gone + 1] = keys[i] end
    collectgarbage(); collectgarbage()
    local r = roundtrip({t = t, gone = gone})
    for i, k in ipairs(r.gone) do
      local ok, nk = pcall(next, r.t, k)
      assert(ok, "seed " .. seed .. ": " .. tostring(nk))
      assert(nk == next(t, gone[i]), "seed " .. seed)
    end
  end
end)

case("a coroutine that clears entries as it iterates resumes where it was", function()
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [pairs] = "pairs", [next] = "next"}
  local back = {env = _ENV, yield = coroutine.yield, pairs = pairs, next = next}
  for seed = 1, 30 do
    local t = churned(64, seed)
    local function make(tt)
      return coroutine.create(function()
        local seen = {}
        for k in pairs(tt) do tt[k] = nil; seen[#seen + 1] = tostring(k); coroutine.yield() end
        return table.concat(seen, " ")
      end)
    end
    local co = make(t)
    for _ = 1, 10 do coroutine.resume(co) end
    collectgarbage(); collectgarbage()   -- the cleared keys become dead keys
    local r = styx.unpersist(back, styx.persist(perms, co))
    local function finish(c)
      local ok, seen
      repeat ok, seen = coroutine.resume(c); assert(ok, seen) until coroutine.status(c) == "dead"
      return seen
    end
    assert(finish(r) == finish(co), "seed " .. seed)
  end
end)

case("a dead key whose string is not persisted needs nothing", function()
  local t = {}
  for i = 1, 20 do t["only" .. i] = i end
  for i = 1, 20, 2 do t["only" .. i] = nil end
  collectgarbage(); collectgarbage()
  local r = roundtrip(t)
  assert(order(r) == order(t))
end)

-- object keys (tables, functions, threads) hash by the hash each object was
-- given when it was created; persisted with the object, it puts the
-- restored object in the same slot (ADR-30)
local function object_keyed(n, seed)
  math.randomseed(seed)
  local t, names = {}, {}
  local function add(i)
    local r = math.random(3)
    local k = (r == 1) and {} or (r == 2) and function() return i end
              or coroutine.create(function() end)
    t[k] = i
    names[k] = "o" .. i
  end
  for i = 1, n do add(i) end
  for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end
  for i = n + 1, n + n // 4 do add(i) end
  collectgarbage(); collectgarbage() -- deleted keys become dead keys, as they would
  return t, names
end

local function named_order(t, names)
  local o = {}
  for k in pairs(t) do o[#o + 1] = names[k] or tostring(k) end
  return table.concat(o, " ")
end

case("tables, functions and threads as keys iterate in the same order", function()
  for seed = 1, 50 do
    local t, names = object_keyed(64, seed)
    local r = roundtrip({t = t, names = names})
    assert(named_order(r.t, r.names) == named_order(t, names), "seed " .. seed)
    for k, v in pairs(t) do assert(r.t[k] == nil) end -- the restored keys are new objects
  end
end)

case("deleted object keys keep the order, and next continues from them", function()
  for seed = 1, 30 do
    local t, names = object_keyed(64, seed)
    local gone = {}
    for k in pairs(t) do if math.random() < 0.3 then t[k] = nil; gone[#gone + 1] = k end end
    collectgarbage(); collectgarbage()   -- the deleted keys become dead keys
    local r = roundtrip({t = t, names = names, gone = gone})
    assert(named_order(r.t, r.names) == named_order(t, names), "seed " .. seed)
    for i, k in ipairs(r.gone) do
      local ok, nk = pcall(next, r.t, k)
      assert(ok, "seed " .. seed .. ": " .. tostring(nk))
      local ok2, ok_nk = pcall(next, t, gone[i])
      assert(ok2 and names[ok_nk] == r.names[nk], "seed " .. seed)
    end
  end
end)

case("a coroutine suspended in a pairs loop over object keys neither skips nor repeats", function()
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [pairs] = "pairs", [next] = "next"}
  local back = H.invert(perms)
  for seed = 1, 50 do
    local t, names = object_keyed(64, seed)
    local co = coroutine.create(function()
      local seen = {}
      for k in pairs(t) do seen[#seen + 1] = names[k]; coroutine.yield() end
      return table.concat(seen, " ")
    end)
    for _ = 1, 20 do coroutine.resume(co) end
    local r = styx.unpersist(back, styx.persist(perms, co))
    local function finish(c)
      local ok, seen
      repeat ok, seen = coroutine.resume(c); assert(ok, seen) until coroutine.status(c) == "dead"
      return seen
    end
    assert(finish(r) == finish(co), "seed " .. seed)
  end
end)

case("a loop that clears object keys as it goes resumes where it was", function()
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [pairs] = "pairs", [next] = "next"}
  local back = H.invert(perms)
  for seed = 1, 30 do
    local t, names = object_keyed(64, seed)
    local co = coroutine.create(function()
      local seen = {}
      for k in pairs(t) do t[k] = nil; seen[#seen + 1] = names[k]; coroutine.yield() end
      return table.concat(seen, " ")
    end)
    for _ = 1, 10 do coroutine.resume(co) end
    collectgarbage(); collectgarbage()
    local r = styx.unpersist(back, styx.persist(perms, co))
    local function finish(c)
      local ok, seen
      repeat ok, seen = coroutine.resume(c); assert(ok, seen) until coroutine.status(c) == "dead"
      return seen
    end
    assert(finish(r) == finish(co), "seed " .. seed)
  end
end)

case("strings, numbers and objects mixed as keys keep the order", function()
  for seed = 1, 30 do
    local t, names = object_keyed(32, seed)
    for i = 1, 40 do t["s" .. math.random(200)] = i; t[math.random(1000) + 0.5] = i end
    t[true] = 1
    local r = roundtrip({t = t, names = names})
    assert(named_order(r.t, r.names) == named_order(t, names), "seed " .. seed)
  end
end)

case("weak-keyed tables keep the order of their live keys", function()
  local t, names = object_keyed(48, 3)
  local weak = setmetatable({}, {__mode = "k"})
  for k, v in pairs(t) do weak[k] = v end
  local r = roundtrip({t = t, names = names, weak = weak})
  assert(named_order(r.weak, r.names) == named_order(weak, names))
end)

case("keys that are permanents restored as the same objects keep the order", function()
  local objs = {}
  for i = 1, 40 do objs[i] = (i % 2 == 0) and {} or function() end end
  local t, names = {}, {}
  for i = 1, 40 do t[objs[i]] = i; names[objs[i]] = "p" .. i end
  for i = 1, 40, 3 do t[objs[i]] = nil end
  local perms = {}
  for i = 1, 40 do perms[objs[i]] = "p" .. i end
  local r = styx.unpersist(H.invert(perms), styx.persist(perms, t))
  assert(named_order(r, names) == named_order(t, names))
end)

case("keys whose objects the restoring side supplies differently: rebuilt, contents kept", function()
  local objs = {}
  for i = 1, 40 do objs[i] = {} end
  local t = {}
  for i = 1, 40 do t[objs[i]] = i end
  t.name = "x"
  local perms, back, others = {}, {}, {}
  for i = 1, 40 do perms[objs[i]] = i; others[i] = {}; back[i] = others[i] end
  local r = styx.unpersist(back, styx.persist(perms, t))
  local n = 0
  for k, v in pairs(r) do n = n + 1 end
  assert(n == 41 and r.name == "x")
  for i = 1, 40 do assert(r[others[i]] == i) end
  for i = 41, 200 do r[{}] = i end   -- inserts and rehash work
  for i = 1, 40 do assert(r[others[i]] == i) end
end)

case("a key the restoring side supplies as another object: the table is rebuilt, contents kept", function()
  local special = {}
  local t = {[special] = "s"}
  for i = 1, 20 do t[{}] = i end
  local other = {"made"} -- (the restoring side's object for that name: another hash)
  local r = styx.unpersist({k = other}, styx.persist({[special] = "k"}, t))
  local n = 0
  for k in pairs(r) do n = n + 1 end
  assert(n == 21 and r[other] == "s")
end)

case("object keys: lookups through the restored keys", function()
  local ks = {}
  local t = {}
  for i = 1, 30 do ks[i] = {i = i}; t[ks[i]] = i end
  local r = roundtrip({t = t, ks = ks})
  for i = 1, 30 do assert(r.t[r.ks[i]] == i) end
end)

case("rejects truncation of a table stream", function()
  local blob = styx.persist({1, "two", {3}, x = {y = "z"}})
  H.rejects_truncation(blob)
end)
