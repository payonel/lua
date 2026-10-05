-- malformed input and values that cannot be persisted raise errors
local styx = require "styx"
local H = require "harness"
local case = H.case

local function fails(pattern, f, ...)
  local ok, err = pcall(f, ...)
  assert(not ok, "expected an error")
  if pattern then
    assert(tostring(err):find(pattern, 1, true), "unexpected error: " .. tostring(err))
  end
end

case("userdata cannot be persisted", function()
  fails("cannot persist a userdata value", styx.persist, io.stdout)
  fails("cannot persist a userdata value", styx.persist, {file = io.stdout})
end)

case("C functions need the permanents table", function()
  fails("cannot persist a C function", styx.persist, print)
  fails("cannot persist a C function", styx.persist, {f = string.format})
end)

case("the running thread cannot be persisted", function()
  fails("cannot persist a running thread", styx.persist, (coroutine.running()))
end)

case("a thread in an error state cannot be persisted", function()
  local co = coroutine.create(function() error("boom") end)
  coroutine.resume(co)
  fails("cannot persist a thread with an error status", styx.persist, co)
end)

case("argument checking", function()
  fails(nil, styx.persist)
  fails(nil, styx.unpersist)
  fails(nil, styx.unpersist, 42)
  fails(nil, styx.persist, 1, 7)       -- perms must be a table
  fails(nil, styx.unpersist, 1, "x")
end)

case("the permanents table is optional on both sides", function()
  assert(styx.unpersist({}, styx.persist({}, 7)) == 7)
  assert(styx.unpersist(styx.persist(7)) == 7)
end)

case("garbage input is rejected", function()
  fails("invalid persisted data", styx.unpersist, "")
  fails("invalid persisted data", styx.unpersist, "garbage")
  fails("invalid persisted data", styx.unpersist, "Styx")
  fails("invalid persisted data", styx.unpersist, ("\0"):rep(64))
end)

case("truncated and trailing bytes are rejected", function()
  local good = styx.persist(7)
  fails("invalid persisted data", styx.unpersist, good:sub(1, -2))
  fails("invalid persisted data", styx.unpersist, good .. "x")
end)

case("unknown tags are rejected", function()
  local header = styx.persist(nil):sub(1, -2)
  for tag = 13, 255 do
    fails("invalid persisted data", styx.unpersist, header .. string.char(tag))
  end
end)

case("dangling back references are rejected", function()
  -- a back reference to an id that was never defined
  local header = styx.persist(nil):sub(1, -2)
  fails("invalid persisted data", styx.unpersist, header .. "\9" .. string.pack("<i8", 99))
end)

case("negative lengths are rejected", function()
  local header = styx.persist(nil):sub(1, -2)
  fails("invalid persisted data", styx.unpersist, header .. "\5" .. string.pack("<i8", -1))
end)

case("errors leave nothing half-built behind", function()
  for _ = 1, 20 do
    pcall(styx.persist, {1, 2, {3, io.stdout}})
    pcall(styx.unpersist, styx.persist({1, 2, {3}}):sub(1, -3))
  end
  collectgarbage()
  H.checkheap()
end)

case("an error inside a deep graph does not leak the stack", function()
  local t = {}
  local cur = t
  for _ = 1, 500 do cur.n = {}; cur = cur.n end
  cur.bad = io.stdout
  for _ = 1, 10 do fails("cannot persist", styx.persist, t) end
end)

case("nesting past the depth limit is an error, not a crash", function()
  local t = {}
  local cur = t
  for _ = 1, 20000 do cur.n = {}; cur = cur.n end
  fails("nested too deeply", styx.persist, t)
end)

case("settings is not implemented yet", function()
  fails("not implemented", styx.settings, "path")
end)
