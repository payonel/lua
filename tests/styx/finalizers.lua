-- finalizers (ADR-28): a restored object is marked for finalization exactly
-- when the original was, whatever its metatable holds and whatever order
-- the walk meets objects in; the finalizers run in the original's order;
-- a failed unpersist runs no finalizer of what it built
local styx = require "styx"
local H = require "harness"
local case = H.case

-- finalizer calls are logged by the object's name
local function logger()
  local log = {}
  return log, function(o) log[#log + 1] = o.name end
end

local function collect()
  collectgarbage()
  collectgarbage()
end

local function restore(perms, v)
  return styx.unpersist(H.invert(perms), styx.persist(perms, v))
end

case("a class that lists its instances: every restored instance is finalized", function()
  local log, gc = logger()
  local Class = {__gc = gc}
  Class.__index = Class
  Class.instances = {}
  for i = 1, 3 do Class.instances[i] = setmetatable({name = "obj" .. i}, Class) end
  local r = restore({[log] = "log"}, Class)
  assert(getmetatable(r.instances[1]) == r and r.instances[2].__gc == r.__gc)
  r = nil
  collect()
  table.sort(log)
  assert(table.concat(log, " ") == "obj1 obj2 obj3", table.concat(log, " "))
end)

case("an instance restored before its class is finalized too", function()
  local log, gc = logger()
  local Class = {__gc = gc}
  local obj = setmetatable({name = "first"}, Class)
  Class.instance = obj
  local r = restore({[log] = "log"}, obj)
  r = nil
  collect()
  assert(table.concat(log, " ") == "first", table.concat(log, " "))
end)

case("an object already finalized (it kept itself alive) is not finalized again", function()
  local log, gc = logger()
  local keep = {}
  do
    setmetatable({name = "once"}, {__gc = function(o) gc(o); keep.o = o end})
  end
  collect()
  assert(#log == 1 and keep.o ~= nil)
  local r = restore({[log] = "log", [keep] = "keep"}, keep.o)
  assert(getmetatable(r).__gc ~= nil)
  keep.o, r = nil, nil
  collect()
  assert(#log == 1, "finalized again: " .. #log)
end)

case("__gc set after setmetatable: not finalized, as the original is not", function()
  local log, gc = logger()
  local mt = {}
  local o = setmetatable({name = "late"}, mt)
  mt.__gc = gc
  local r = restore({[log] = "log"}, o)
  assert(getmetatable(r).__gc ~= nil)
  o, r = nil, nil
  collect()
  assert(#log == 0, "finalized: " .. #log)
end)

case("marked, then __gc removed and set again: finalized, as the original is", function()
  local log, gc = logger()
  local mt = {__gc = gc}
  local o = setmetatable({name = "back"}, mt)
  mt.__gc = nil
  local r = restore({[log] = "log"}, o)
  assert(getmetatable(r).__gc == nil)
  getmetatable(r).__gc = gc
  mt.__gc = gc
  o, r = nil, nil
  collect()
  assert(#log == 2, "finalizer calls: " .. #log)
end)

case("marked with no metatable left: finalized once one is set", function()
  local log, gc = logger()
  local o = setmetatable({name = "bare"}, {__gc = gc})
  setmetatable(o, nil)
  local r = restore({[log] = "log"}, o)
  assert(getmetatable(r) == nil)
  -- a metatable with no __gc does not mark; __gc set later is called only
  -- for an object that was marked
  local later = {}
  setmetatable(r, later)
  setmetatable(o, later)
  later.__gc = gc
  o, r = nil, nil
  collect()
  assert(#log == 2, "finalizer calls: " .. #log)
end)

case("finalizers run in the original's order, not the walk's", function()
  local log, gc = logger()
  local mt = {__gc = gc}
  local objs = {}
  for i = 1, 6 do objs[i] = setmetatable({name = "o" .. i}, mt) end
  -- the walk meets them in an order unlike the one they were marked in
  local v = {objs[4], objs[1], objs[6], objs[2], {objs[5], {objs[3]}}}
  local perms = {[log] = "log"}
  local blob = styx.persist(perms, v)
  objs, v = nil, nil
  collect()
  local original = table.concat(log, " ")
  assert(original == "o6 o5 o4 o3 o2 o1", original)
  for i = #log, 1, -1 do log[i] = nil end
  local r = styx.unpersist(H.invert(perms), blob)
  r = nil
  collect()
  assert(table.concat(log, " ") == original, table.concat(log, " "))
end)

case("an object waiting for its finalizer is restored marked", function()
  local log, gc = logger()
  local perms = {[log] = "log"}
  local blob
  do
    -- 'b' is marked first, so 'a' is finalized first, while 'b' waits
    local b = setmetatable({name = "b"}, {__gc = gc})
    setmetatable({name = "a"}, {__gc = function(o) blob = styx.persist(perms, b); gc(o) end})
  end
  collect()
  assert(table.concat(log, " ") == "a b", table.concat(log, " "))
  local r = styx.unpersist(H.invert(perms), blob)
  r = nil
  collect()
  assert(table.concat(log, " ") == "a b b", table.concat(log, " "))
end)

case("permanents keep their own marks", function()
  local log, gc = logger()
  local p = setmetatable({name = "perm"}, {__gc = gc})
  local r = styx.unpersist({log = log, p = p}, styx.persist({[log] = "log", [p] = "p"}, {p}))
  assert(r[1] == p)
  r = nil
  collect()
  assert(#log == 0, table.concat(log, " "))
end)

case("an unpersist that fails runs no finalizer of what it built", function()
  local log, gc = logger()
  local Class = {__gc = gc}
  Class.__index = Class
  local v = {}
  for i = 1, 5 do v[i] = setmetatable({name = "x" .. i}, Class) end
  -- the restore fails after building the instances: a permanent the
  -- restoring side does not have
  local stop = function() end
  v.stop = stop
  local perms = {[log] = "log", [_ENV] = "env", [stop] = "stop"}
  local blob = styx.persist(perms, v)
  local back = H.invert(perms)
  back.stop = nil
  for _ = 1, 3 do
    local ok, err = pcall(styx.unpersist, back, blob)
    assert(not ok and tostring(err):find("stop"), tostring(err))
  end
  collect()
  assert(#log == 0, "finalizers of half-built objects ran: " .. table.concat(log, " "))
end)

case("streams with marked objects reject truncation", function()
  local log, gc = logger()
  local mt = {__gc = gc}
  local v = {setmetatable({name = "t1"}, mt), setmetatable({name = "t2"}, mt)}
  local perms = {[log] = "log"}
  H.rejects_truncation(styx.persist(perms, v), H.invert(perms))
  collect()
  assert(#log == 0, table.concat(log, " "))
end)

case("restored marks survive a round trip of their own", function()
  local log, gc = logger()
  local mt = {__gc = gc}
  local v = {}
  for i = 1, 4 do v[i] = setmetatable({name = "r" .. i}, mt) end
  local perms = {[log] = "log", [mt] = "mt"}
  local back = H.invert(perms)
  local r1 = styx.unpersist(back, styx.persist(perms, v))
  local r2 = styx.unpersist(back, styx.persist(perms, r1))
  v, r1 = nil, nil
  collect()
  table.sort(log)
  assert(table.concat(log, " ") == "r1 r1 r2 r2 r3 r3 r4 r4", table.concat(log, " "))
  for i = #log, 1, -1 do log[i] = nil end
  r2 = nil
  collect()
  assert(table.concat(log, " ") == "r4 r3 r2 r1", table.concat(log, " "))
end)
