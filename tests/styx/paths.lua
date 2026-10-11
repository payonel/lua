-- errors say where: persist errors end with the path from the root to the
-- value that failed; unpersist errors give the byte offset in the stream
local styx = require "styx"
local H = require "harness"
local case = H.case

local function error_of(f, ...)
  local ok, err = pcall(f, ...)
  assert(not ok, "expected an error")
  return tostring(err)
end

local function has(err, text)
  assert(err:find(text, 1, true), "expected '" .. text .. "' in: " .. err)
end

case("the root value", function()
  has(error_of(styx.persist, io.stdout), "(at root)")
end)

case("string keys that are names", function()
  has(error_of(styx.persist, {config = {output = io.stdout}}), "(at root.config.output)")
end)

case("integer keys", function()
  has(error_of(styx.persist, {1, 2, io.stdout}), "(at root[3])")
  has(error_of(styx.persist, {[-7] = {io.stdout}}), "(at root[-7][1])")
end)

case("other keys", function()
  has(error_of(styx.persist, {["a b"] = io.stdout}), '(at root["a b"])')
  has(error_of(styx.persist, {[2.5] = io.stdout}), "(at root[2.5])")
  has(error_of(styx.persist, {[true] = io.stdout}), "(at root[true])")
  has(error_of(styx.persist, {[{}] = io.stdout}), "(at root[<table>])")
end)

case("a key that cannot be persisted", function()
  has(error_of(styx.persist, {list = {[io.stdout] = 1}}), "(at root.list<key userdata>)")
end)

case("metatables", function()
  local t = setmetatable({}, {__index = {f = math.random}})
  has(error_of(styx.persist, {obj = t}), "cannot persist a C function (at root.obj<metatable>.__index.f)")
end)

case("upvalues by name", function()
  local function make()
    local handle = io.stdout
    return function() return handle end
  end
  has(error_of(styx.persist, {fn = make()}), "(at root.fn<upvalue handle>)")
end)

case("thread stack slots", function()
  local co = coroutine.create(function()
    local file = io.stdout
    coroutine.yield()
    return file
  end)
  coroutine.resume(co)
  has(error_of(styx.persist, {[_ENV] = "env", [coroutine.yield] = "y"}, {co = co}), "(at root.co<stack ")
end)

case("__persist refusals", function()
  local t = setmetatable({}, {__persist = false})
  has(error_of(styx.persist, {a = {b = t}}), "its __persist is false (at root.a.b)")
  local f = setmetatable({}, {__persist = function() end})
  has(error_of(styx.persist, {a = {b = f}}), "__persist must be a boolean (at root.a.b)")
end)

case("deep paths", function()
  local t = {}
  local cur = t
  for i = 1, 5 do cur["k" .. i] = {}; cur = cur["k" .. i] end
  cur.bad = io.stdout
  has(error_of(styx.persist, t), "(at root.k1.k2.k3.k4.k5.bad)")
end)

case("the path does not change other error text", function()
  local err = error_of(styx.persist, {x = math.random})
  has(err, "cannot persist a C function (at root.x)")
end)

case("unpersist errors give the byte offset", function()
  local blob = styx.persist({1, 2, 3})
  has(error_of(styx.unpersist, blob:sub(1, 20)), "invalid persisted data (at byte ")
  has(error_of(styx.unpersist, "garbage"), "invalid persisted data (at byte 0)")
end)
