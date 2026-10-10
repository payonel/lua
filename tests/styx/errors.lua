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

case("C functions of the engine persist by name; others need the permanents table", function()
  -- a function of the engine is found again by its name, whatever the permanents
  assert(styx.unpersist({}, styx.persist(print)) == print)
  assert(styx.unpersist({}, styx.persist({f = string.format})).f == string.format)
  -- one with state in C (math.random's generator) has none
  fails("cannot persist a C function", styx.persist, math.random)
  fails("cannot persist a C function", styx.persist, {f = math.random})
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
  for tag = 13, 255 do
    fails("invalid persisted data", styx.unpersist, H.stream(string.char(tag)))
  end
end)

case("a stream cannot hand a script the main thread", function()
  fails("invalid persisted data", styx.unpersist, H.stream("\18"))
end)

case("dangling back references are rejected", function()
  -- a back reference to an id that was never defined
  fails("invalid persisted data", styx.unpersist, H.stream("\9" .. H.varint(99)))
end)

case("negative lengths are rejected", function()
  fails("invalid persisted data", styx.unpersist, H.stream("\5" .. H.varint(-1)))
end)

case("table sizes are checked", function()
  local function table_stream(asize, hsize, lenhint)
    return H.stream("\6" .. H.varint(0) .. H.varint(asize) .. H.varint(hsize) .. H.varint(lenhint) .. "\0\0\0")
  end
  assert(next(styx.unpersist(table_stream(0, 0, 0))) == nil)
  assert(next(styx.unpersist(table_stream(4, 8, 4))) == nil)
  fails("invalid persisted data", styx.unpersist, table_stream(-1, 0, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(0, 3, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(0, -4, 0))
  fails("invalid persisted data", styx.unpersist, table_stream(2, 0, 3))
  fails("invalid persisted data", styx.unpersist, table_stream(1 << 40, 0, 0))
  -- an object's hash is a 32-bit difference from the previous one's
  fails("invalid persisted data", styx.unpersist,
        H.stream("\6" .. H.varint(1 << 40) .. H.varint(0) .. H.varint(0) .. H.varint(0) .. "\0\0\0"))
  H.checkheap()
end)

case("the trailer is checked", function()
  local v = H.varint
  local empty_table = "\6" .. H.varint(0) .. v(0) .. v(0) .. v(0) .. "\0\0\0"
  assert(type(styx.unpersist(H.stream(empty_table))) == "table")
  -- no trailer, or a length past the start
  fails("invalid persisted data", styx.unpersist, H.header .. "\0")
  fails("invalid persisted data", styx.unpersist, H.header .. "\0" .. v(0) .. v(0) .. H.backvar(100))
  fails("invalid persisted data", styx.unpersist, H.header .. "\0" .. v(0) .. v(0) .. "\255")
  -- bytes between the value and the trailer
  fails("invalid persisted data", styx.unpersist, H.stream("\0\0"))
  -- marks: tables only, each once, all met
  assert(type(styx.unpersist(H.stream(empty_table, {1}))) == "table")
  fails("invalid persisted data", styx.unpersist, H.stream("\5" .. v(1) .. "x", {1}))
  fails("invalid persisted data", styx.unpersist, H.stream(empty_table, {1, 1}))
  fails("invalid persisted data", styx.unpersist, H.stream(empty_table, {0}))
  fails("invalid persisted data", styx.unpersist, H.stream(empty_table, {2}))
  fails("invalid persisted data", styx.unpersist, H.stream("\0", {1}))
  -- dead keys: a node of a table that is a dead key, given an object (a
  -- string, or any other object, here the table itself)
  local layout = "\6" .. H.varint(0) .. v(0) .. v(2) .. v(0) .. "\1" .. "\0" .. "\0" .. v(2) ..
                 "\2" .. "\5" .. v(1) .. "k" .. "\2" .. v(0) .. "\1" .. v(0)
  assert(type(styx.unpersist(H.stream(layout))) == "table")
  assert(type(styx.unpersist(H.stream(layout, nil, {{1, 1, 2}}))) == "table")
  assert(type(styx.unpersist(H.stream(layout, nil, {{1, 1, 1}}))) == "table")
  for _, bad in ipairs{{1, 0, 2}, {1, 2, 2}, {1, -1, 2}, {2, 1, 2}, {3, 1, 2}, {1, 1, 3}} do
    fails("invalid persisted data", styx.unpersist, H.stream(layout, nil, {bad}))
  end
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

case("there is no settings function", function()
  assert(styx.settings == nil)
end)
