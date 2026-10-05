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

case("userdata are not supported yet", function()
  fails("persisting a userdata value is not supported yet", styx.persist, io.stdout)
  fails("persisting a userdata value is not supported yet", styx.persist, {file = io.stdout})
end)

case("C functions need the permanents table", function()
  fails("cannot persist a C function", styx.persist, print)
  fails("cannot persist a C function", styx.persist, {f = string.format})
end)

case("the running thread cannot be persisted", function()
  fails("cannot persist a running thread", styx.persist, (coroutine.running()))
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

case("a stream cannot hand a script the main thread", function()
  local header = styx.persist(nil):sub(1, -2)
  fails("invalid persisted data", styx.unpersist, header .. "\18")
end)

case("dangling back references are rejected", function()
  -- a back reference to an id that was never defined
  local header = styx.persist(nil):sub(1, -2)
  fails("invalid persisted data", styx.unpersist, header .. "\9" .. H.varint(99))
end)

case("negative lengths are rejected", function()
  local header = styx.persist(nil):sub(1, -2)
  fails("invalid persisted data", styx.unpersist, header .. "\5" .. H.varint(-1))
end)

case("table sizes are checked", function()
  local header = styx.persist(nil):sub(1, -2)
  local function table_stream(asize, hsize, lenhint)
    return header .. "\6" .. H.varint(asize) .. H.varint(hsize) .. H.varint(lenhint) .. "\0\0"
  end
  assert(next(styx.unpersist(table_stream(0, 0, 0))) == nil)
  assert(next(styx.unpersist(table_stream(4, 8, 4))) == nil)
  fails("invalid persisted data", styx.unpersist, table_stream(-1, 0, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(0, 3, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(0, -4, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(2, 0, 3))
  fails("invalid persisted data", styx.unpersist, table_stream(1 << 40, 0, 0))
  H.checkheap()
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
  for _ = 1, 10 do fails("not supported yet", styx.persist, t) end
end)

case("settings is not implemented yet", function()
  fails("not implemented", styx.settings, "path")
end)
