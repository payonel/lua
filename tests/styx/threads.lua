-- coroutines: unstarted, suspended (one or more Lua frames), dead
local styx = require "styx"
local H = require "harness"
local case = H.case

local perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
local back = {env = _ENV, yield = coroutine.yield}
local function roundtrip(co) return H.roundtrip(co, perms, back) end

case("unstarted coroutine with arguments", function()
  local co = coroutine.create(function(a, b)
    local x = coroutine.yield(a + b)
    return x * 2
  end)
  local c1 = roundtrip(co)
  assert(c1 ~= co and coroutine.status(c1) == "suspended")
  assert(select(2, coroutine.resume(c1, 1, 2)) == 3)
  assert(select(2, coroutine.resume(c1, 21)) == 42)
  assert(coroutine.status(c1) == "dead")
  assert(coroutine.status(co) == "suspended")   -- the original is untouched
end)

case("suspended inside a loop", function()
  local gen = coroutine.create(function()
    for i = 1, 3 do coroutine.yield(i) end
    return "end"
  end)
  coroutine.resume(gen)
  coroutine.resume(gen)
  local g2 = roundtrip(gen)
  assert(select(2, coroutine.resume(g2)) == 3)
  assert(select(2, coroutine.resume(g2)) == "end")
  assert(coroutine.status(g2) == "dead")
end)

case("dead coroutine", function()
  local dead = coroutine.create(function() end)
  coroutine.resume(dead)
  local r = roundtrip(dead)
  assert(coroutine.status(r) == "dead")
  assert(not coroutine.resume(r))
end)

case("locals of several kinds on the stack", function()
  local co = coroutine.create(function()
    local i, f, s, t, b = 1, 2.5, "str", {k = "v"}, false
    coroutine.yield()
    return i, f, s, t.k, b
  end)
  coroutine.resume(co)
  local r = roundtrip(co)
  local _, i, f, s, k, b = coroutine.resume(r)
  assert(i == 1 and f == 2.5 and s == "str" and k == "v" and b == false)
end)

case("yield from nested Lua calls", function()
  local function make()
    local function inner(n) local got = coroutine.yield(n); return got + n end
    local function middle(n) return inner(n * 10) + 1 end
    return coroutine.create(function(n) return middle(n) + 100 end)
  end
  local co = make()
  assert(select(2, coroutine.resume(co, 2)) == 20)
  local r = roundtrip(co)
  assert(select(2, coroutine.resume(r, 5)) == 126)
end)

case("yield from deep recursion", function()
  local function make()
    local function rec(n)
      if n == 0 then return coroutine.yield("bottom") end
      return rec(n - 1) + 1
    end
    return coroutine.create(function() return rec(150) end)
  end
  local co = make()
  assert(select(2, coroutine.resume(co)) == "bottom")
  local r = roundtrip(co)
  assert(select(2, coroutine.resume(r, 0)) == 150)
end)

case("yielding several values and receiving several", function()
  local co = coroutine.create(function(...)
    local a, b, c = coroutine.yield(...)
    return c, b, a
  end)
  local _, x, y, z = coroutine.resume(co, 1, 2, 3)
  assert(x == 1 and y == 2 and z == 3)
  local r = roundtrip(co)
  local _, p, q, s = coroutine.resume(r, "a", "b", "c")
  assert(p == "c" and q == "b" and s == "a")
end)

case("vararg function suspended", function()
  local co = coroutine.create(function(...)
    coroutine.yield()
    return select("#", ...), ...
  end)
  coroutine.resume(co, "x", nil, "z")
  local r = roundtrip(co)
  local _, n, a, b, c = coroutine.resume(r)
  assert(n == 3 and a == "x" and b == nil and c == "z")
end)

case("many yields after restore", function()
  local co = coroutine.create(function()
    local sum = 0
    while true do sum = sum + coroutine.yield(sum) end
  end)
  coroutine.resume(co)
  local r = roundtrip(co)
  local last
  for i = 1, 100 do last = select(2, coroutine.resume(r, i)) end
  assert(last == 5050)
end)

case("a coroutine held in a table twice is restored once", function()
  local co = coroutine.create(function() coroutine.yield() end)
  coroutine.resume(co)
  local r = roundtrip({co, co, {co}})
  assert(r[1] == r[2] and r[3][1] == r[1])
end)

case("a coroutine whose stack refers back to its container", function()
  local holder = {}
  local co = coroutine.create(function(h) coroutine.yield(); return h.name end)
  holder.co, holder.name = co, "holder"
  coroutine.resume(co, holder)
  local r = roundtrip(holder)
  assert(select(2, coroutine.resume(r.co)) == "holder")
end)

case("a coroutine holding another suspended coroutine", function()
  local inner = coroutine.create(function() coroutine.yield("in1"); return "in2" end)
  coroutine.resume(inner)
  local outer = coroutine.create(function(c)
    coroutine.yield()
    return select(2, coroutine.resume(c))
  end)
  coroutine.resume(outer, inner)
  local r = roundtrip(outer)
  assert(select(2, coroutine.resume(r)) == "in2")
end)

case("a restored coroutine can be persisted again", function()
  local co = coroutine.create(function()
    for i = 1, 5 do coroutine.yield(i) end
  end)
  coroutine.resume(co)
  local r1 = roundtrip(co)
  coroutine.resume(r1)
  local r2 = roundtrip(r1)
  assert(select(2, coroutine.resume(r2)) == 3)
end)

case("errors raised after restore are catchable", function()
  local co = coroutine.create(function()
    coroutine.yield()
    error("after restore")
  end)
  coroutine.resume(co)
  local r = styx.unpersist({env = _ENV, yield = coroutine.yield, error = error},
                            styx.persist({[_ENV] = "env", [coroutine.yield] = "yield", [error] = "error"}, co))
  local ok, msg = coroutine.resume(r)
  assert(not ok and msg:find("after restore", 1, true))
  assert(coroutine.status(r) == "dead")
end)

-- a thread stream ends with its frames ('nframes' then, per frame, ten
-- 8-byte fields (the first being the stack slot of the frame's function) and
-- ten bytes of what a freeze keeps: no continuation, the C calls, how an API
-- call goes on, a hook function in progress; all zero here), its
-- to-be-closed list ('ntbc' then one 8-byte level each; none here), the
-- hooks allowed (one byte), the values a hook is given (two, one byte each
-- here: 0), the state of a frozen thread (one byte, 0: not frozen), its
-- hook (one byte, 0: no hook) and its groups and chain (four values: nil)
case("frames that do not match the stack are rejected", function()
  local co = coroutine.create(function(a) coroutine.yield(a) end)
  coroutine.resume(co, 1)
  local blob = styx.persist(perms, co)
  assert(styx.unpersist(back, blob))
  local tail = blob:sub(-3) -- the trailer (empty) and its length
  assert(tail == "\0\0\2", "layout: trailer")
  blob = blob:sub(1, -4)
  local frame_size = 10 * 8 + 10
  local frames_end = #blob - 4 - 1 - 1 - 2 - 1 - 8
  assert(string.unpack("<i8", blob, frames_end + 1) == 0, "layout: ntbc")
  assert(blob:byte(frames_end + 9) == 1, "layout: allowhook")
  assert(blob:byte(frames_end + 10) == 0 and blob:byte(frames_end + 11) == 0, "layout: transferred values")
  assert(blob:byte(frames_end + 12) == 0, "layout: frozen")
  assert(blob:byte(frames_end + 13) == 0, "layout: hook")
  local nframes = string.unpack("<i8", blob, frames_end - 2 * frame_size - 7)
  assert(nframes == 2, "layout: " .. nframes)
  local first = frames_end - 2 * frame_size
  for _, slot in ipairs{0, 2} do   -- the nil base slot; the argument
    local bad = blob:sub(1, first) .. string.pack("<i8", slot) .. blob:sub(first + 9) .. tail
    local ok, err = pcall(styx.unpersist, back, bad)
    assert(not ok and err:find("invalid persisted data", 1, true), tostring(err))
  end
  H.checkheap()
end)

-- a thread that died with an error keeps its frames, its error value and
-- its pending to-be-closed variables; the restored one behaves the same
local function compare_dead(make, perms_extra, back_extra)
  local function observe(co, log)
    local r = {coroutine.status(co), debug.traceback(co)}
    local ok, err = coroutine.resume(co)
    r[#r + 1] = tostring(ok) .. " " .. tostring(err)
    local cok, cerr = coroutine.close(co)
    r[#r + 1] = tostring(cok) .. " " .. tostring(cerr)
    r[#r + 1] = table.concat(log, ",")
    r[#r + 1] = coroutine.status(co)
    return r
  end
  local log1, log2 = {}, {}
  local original = make(log1)
  local p, b = {}, {}
  for k, v in pairs(perms) do p[k] = v end
  for k, v in pairs(back) do b[k] = v end
  p[log1] = "log"; b.log = log2
  for k, v in pairs(perms_extra or {}) do p[k] = v end
  for k, v in pairs(back_extra or {}) do b[k] = v end
  local restored = styx.unpersist(b, styx.persist(p, original))
  local a, c = observe(original, log1), observe(restored, log2)
  for i = 1, #a do
    assert(a[i] == c[i], "step " .. i .. ":\n" .. tostring(a[i]) .. "\nvs\n" .. tostring(c[i]))
  end
end

case("a thread dead after error() behaves the same after restore", function()
  compare_dead(function(log)
    local co = coroutine.create(function()
      local x <close> = setmetatable({}, {__close = function(_, e) log[#log + 1] = "closed:" .. tostring(e) end})
      local function inner() error("boom") end
      inner()
    end)
    assert(not coroutine.resume(co))
    return co
  end, {[error] = "error"}, {error = error})
end)

case("a thread dead after a runtime error behaves the same after restore", function()
  compare_dead(function(log)
    local co = coroutine.create(function() local t = nil; return t.x end)
    assert(not coroutine.resume(co))
    return co
  end)
end)

case("a thread dead with a table as error value keeps that value", function()
  local co = coroutine.create(function() error({code = 42}) end)
  coroutine.resume(co)
  local r = styx.unpersist({env = _ENV, yield = coroutine.yield, error = error},
                            styx.persist({[_ENV] = "env", [coroutine.yield] = "yield", [error] = "error"}, co))
  local ok, err = coroutine.close(r)
  assert(not ok and type(err) == "table" and err.code == 42)
end)

case("rejects truncation of a thread stream", function()
  local co = coroutine.create(function(a) local t = {a}; coroutine.yield(t); return a end)
  coroutine.resume(co, 3)
  H.rejects_truncation(styx.persist(perms, co), back)
end)
