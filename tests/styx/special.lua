-- __persist: a metatable field that lets a table control how it is
-- persisted (true: literally; false: never; a function: called with the
-- value, it returns a closure that rebuilds the value on unpersist)
local styx = require "styx"
local H = require "harness"
local case = H.case

local perms = {[_ENV] = "env"}
local back = {env = _ENV}

local function fails(pattern, f, ...)
  local ok, err = pcall(f, ...)
  assert(not ok, "expected an error")
  assert(tostring(err):find(pattern, 1, true), "unexpected error: " .. tostring(err))
end

case("__persist = true persists a table literally", function()
  local t = setmetatable({1, 2, x = "y"}, {__persist = true})
  local r = H.roundtrip(t)
  assert(r[2] == 2 and r.x == "y" and getmetatable(r).__persist == true)
end)

case("__persist = false forbids persisting a table", function()
  local t = setmetatable({}, {__persist = false})
  fails("cannot persist a table: its __persist is false", styx.persist, t)
  fails("cannot persist a table: its __persist is false", styx.persist, {inner = t})
end)

local function make_handle_class()
  local Handle = {}
  Handle.__index = Handle
  -- a handle persists only its name; restoring reopens it by name
  Handle.__persist = function(h)
    local name = h.name
    return function() return setmetatable({name = name, reopened = true}, Handle) end
  end
  return Handle
end

case("a __persist function persists a restore closure instead", function()
  local Handle = make_handle_class()
  local h = setmetatable({name = "log", buffer = ("x"):rep(1000)}, Handle)
  local blob = styx.persist(perms, h)
  assert(#blob < 1000, "the table's contents are not persisted")
  local r = styx.unpersist(back, blob)
  assert(r.name == "log" and r.reopened == true and r.buffer == nil)
  assert(getmetatable(r) == getmetatable(r))
end)

case("a value restored through __persist keeps its sharing", function()
  local Handle = make_handle_class()
  local h = setmetatable({name = "shared"}, Handle)
  local r = styx.unpersist(back, styx.persist(perms, {h, h, {h}}))
  assert(r[1] == r[2] and r[3][1] == r[1] and r[1].reopened)
end)

case("the __persist function receives the value being persisted", function()
  local seen
  local function make_mt(record)
    return {__persist = function(v) record(v); return function() return {} end end}
  end
  local t = setmetatable({}, make_mt(function(v) seen = v end))
  styx.persist(perms, t)
  assert(seen == t)
end)

case("__persist must be a boolean or a function", function()
  local t = setmetatable({}, {__persist = 42})
  fails("__persist must be a boolean or a function", styx.persist, t)
end)

case("a __persist function must return a function", function()
  local t = setmetatable({}, {__persist = function() return "not a function" end})
  fails("__persist must return a function", styx.persist, t)
end)

case("the restore closure must return a value of the original type", function()
  local function make_mt()
    return {__persist = function() return function() return 42 end end}
  end
  local blob = styx.persist(perms, setmetatable({}, make_mt()))
  fails("__persist restore function returned number, table expected", styx.unpersist, back, blob)
end)

case("errors in a __persist function propagate", function()
  local t = setmetatable({}, {__persist = function() error("refused") end})
  fails("refused", styx.persist, t)
end)

case("__persist is read raw from the metatable", function()
  local mt = setmetatable({}, {__index = {__persist = false}})
  local t = setmetatable({v = 1}, mt)
  assert(H.roundtrip(t).v == 1)
end)

case("userdata are permanents too", function()
  local r = styx.unpersist({out = io.stderr}, styx.persist({[io.stdout] = "out"}, {io.stdout}))
  assert(r[1] == io.stderr)
end)

case("userdata are not supported yet (except as permanents)", function()
  fails("userdata value", styx.persist, io.stdout)
end)

-- not supported yet (docs/styx.md): the restore closure reaching the value
-- it rebuilds; today this is a clean error on unpersist
case("a restore closure that refers to its own value is an error, not a crash", function()
  local function make_mt()
    return {__persist = function(t) return function() return t end end}
  end
  local t = setmetatable({}, make_mt())
  local blob = styx.persist(perms, t)
  fails("invalid persisted data", styx.unpersist, back, blob)
  H.checkheap()
end)

case("rejects truncation of a stream with special and literal values", function()
  local Handle = make_handle_class()
  local blob = styx.persist(perms, {setmetatable({name = "n"}, Handle),
                                     setmetatable({1, 2}, {__persist = true})})
  H.rejects_truncation(blob, back)
end)
