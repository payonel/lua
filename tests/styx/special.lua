-- __persist: a metatable field that lets a table control how it is
-- persisted: true (or absent), literally; false, never. Nothing else: a
-- function would be user code that the persist runs (and the closure it
-- returned, user code that the unpersist runs), outside any budget
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

case("__persist must be a boolean: a function is refused, and is not called", function()
  local called = false
  local t = setmetatable({}, {__persist = function() called = true return function() return {} end end})
  fails("__persist must be a boolean", styx.persist, t)
  fails("__persist must be a boolean", styx.persist, {inner = t})
  assert(not called)
  fails("__persist must be a boolean", styx.persist, setmetatable({}, {__persist = 42}))
  fails("__persist must be a boolean", styx.persist, setmetatable({}, {__persist = "yes"}))
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

case("rejects truncation of a stream with literal values", function()
  local blob = styx.persist(perms, {setmetatable({name = "n"}, {__persist = true}),
                                     setmetatable({1, 2}, {__persist = true})})
  H.rejects_truncation(blob, back)
end)
