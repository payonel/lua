-- A suspended coroutine is persisted; on unpersist the permanents table maps
-- "print" to a local function. After resuming, that local function (not the
-- real print) must be called and receive the printed text.
local styx = require "styx"
local H = require "harness"

H.case("resumed coroutine calls the print supplied on unpersist", function()
  local captured = {}
  local function capture(...)
    captured[#captured + 1] = table.concat({...}, "\t")
  end

  local co = coroutine.create(function()
    local print = print
    coroutine.yield()
    print("hello from the resumed coroutine")
  end)
  assert(coroutine.resume(co))
  assert(coroutine.status(co) == "suspended")

  local blob = styx.persist(
    {[_ENV] = "_ENV", [print] = "print", [coroutine.yield] = "coroutine.yield"}, co)

  local restored = styx.unpersist(
    {_ENV = _ENV, print = capture, ["coroutine.yield"] = coroutine.yield}, blob)
  assert(type(restored) == "thread" and restored ~= co)
  assert(coroutine.status(restored) == "suspended")

  assert(coroutine.resume(restored))
  assert(coroutine.status(restored) == "dead")
  assert(#captured == 1)
  assert(captured[1] == "hello from the resumed coroutine")
end)
