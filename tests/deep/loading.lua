-- tests/deep/loading.lua
-- Client-side chunk loading: streaming readers (load with a function),
-- binary chunk round trips through string.dump, stripped dumps, modes,
-- and load environments. Stock Lua 5.5 behavior is the reference.

-- reader function: chunk delivered in several pieces
do
  local pieces = {"return ", "40", " + ", "2"}
  local i = 0
  local f = assert(load(function()
    i = i + 1
    return pieces[i]
  end, "=pieces"))
  assert(f() == 42)
  assert(i == #pieces + 1) -- reader was called until it returned nil
end

-- reader function: an empty string (like nil) signals end of chunk
do
  local pieces = {"return 6 * 7", "", "this is never read ((("}
  local i = 0
  local f = assert(load(function()
    i = i + 1
    return pieces[i]
  end))
  assert(f() == 42)
  assert(i == 2) -- reading stopped at the empty string
end

-- reader function: one byte at a time
do
  local src = "local acc = 0; for i = 1, 10 do acc = acc + i end; return acc"
  local pos = 0
  local f = assert(load(function()
    pos = pos + 1
    if pos <= #src then return src:sub(pos, pos) end
  end, "=bytes"))
  assert(f() == 55)
end

-- binary round trip: dump and reload preserves behavior
do
  local function orig(a, b)
    local t = {msg = "hi", pi = 3.5, n = 42, ok = true}
    local function inner(x) return x * t.n end
    return inner(a) + b, t.msg
  end
  local d = string.dump(orig)
  assert(type(d) == "string" and #d > 0)
  local copy = assert(load(d))
  local n1, s1 = orig(2, 1)
  local n2, s2 = copy(2, 1)
  assert(n1 == n2 and s1 == s2)
end

-- binary chunks carry constants of every type and nested prototypes
do
  local function deep()
    local function lvl2()
      local function lvl3() return "bottom" end
      return lvl3() .. "-up"
    end
    return lvl2(), nil, false, 2.5, 7, "str"
  end
  local copy = assert(load(string.dump(deep)))
  local a, b, c, d2, e, f2 = copy()
  assert(a == "bottom-up" and b == nil and c == false)
  assert(d2 == 2.5 and e == 7 and f2 == "str")
end

-- binary chunk fed through a streaming reader in odd-sized pieces
do
  local d = string.dump(function() return "streamed" end)
  local pos = 0
  local f = assert(load(function()
    if pos >= #d then return nil end
    local step = 7 -- deliberately unaligned with any header field
    local piece = d:sub(pos + 1, pos + step)
    pos = pos + step
    return piece
  end, "=streamed", "b"))
  assert(f() == "streamed")
end

-- upvalues survive a dump/reload (fresh, unjoined upvalues)
do
  local function make()
    local count = 0
    return function() count = count + 1; return count end
  end
  local copy = assert(load(string.dump(make)))()
  assert(copy() == 1)
  assert(copy() == 2)
end

-- stripped dumps still load and run, with debug info gone
do
  local function f() return 99 end
  local d = string.dump(f, true)
  local g = assert(load(d))
  assert(g() == 99)
  local info = debug.getinfo(g, "S")
  assert(info.what == "main" or info.what == "Lua")
  assert(#d <= #string.dump(f)) -- stripping does not grow the dump
end

-- mode strings: "t" rejects binary, "b" rejects text
do
  local d = string.dump(function() end)
  local f, err = load(d, "=chunk", "t")
  assert(f == nil and err:find("binary"))
  local g, err2 = load("return 1", "=chunk", "b")
  assert(g == nil and err2:find("text"))
  assert(load(d, "=chunk", "bt") ~= nil) -- explicit both accepts either
  assert(load("return 1", "=chunk", "bt") ~= nil)
end

-- load with an explicit environment
do
  local env = {x = 11}
  local f = assert(load("return x", "=envchunk", "t", env))
  assert(f() == 11)
  local name, e = debug.getupvalue(f, 1)
  assert(name == "_ENV" and e == env)
end

-- a chunk loaded from a dump of a chunk loaded from source: two trips
do
  local f1 = assert(load("return 'twice'"))
  local f2 = assert(load(string.dump(f1)))
  local f3 = assert(load(string.dump(f2)))
  assert(f3() == "twice")
end

-- the pieces the reader function of 'load' gives are not kept once read:
-- only the one being read is
do
  local n = 0
  local mem = {}
  local piece = string.rep("x", 100000) .. "\n"
  local f = load(function()
    n = n + 1
    if n > 20 then return nil end
    collectgarbage()
    mem[n] = collectgarbage("count")
    return "--" .. n .. piece -- (a comment, a new string each time)
  end)
  assert(f and n == 21)
  assert(mem[20] - mem[3] < 400, mem[20] - mem[3])
end

-- 'global' is a reserved word only in the test build (T), as in stock's
-- test build; elsewhere it is a name, and it starts a declaration only at
-- the start of a statement, followed by a name, '<', '*' or 'function'
-- (the gate has no T in either build: there only what both builds share
-- is checked)
do
  local src = [[
    global = 1
    local t = {global = 2}
    t.global = t.global + global
    local global = 3
    global = global + 1
    return global, t.global, _ENV.global
  ]]
  local decl = [[
    global x, y = 1, 2
    global function gf () return x + y end
    global <const> z
    do global * end
    return gf()
  ]]
  local calls = "global(1) global.x = 2 global[1] = 3 global:m() global 's' global {} return global"
  local env = {}
  if T then
    for _, s in ipairs{src, calls, "global\n= 3", "local function global () end",
                       "return function (global) end"} do
      local f, msg = load(s)
      assert(not f and string.find(msg, "expected near"), msg)
    end
  elseif not GATE then
    local a, b, c = assert(load(src, "=src", "t", env))()
    assert(a == 4 and b == 3 and c == 1 and env["global"] == 1)
    assert(load("global\n= 3 return global", "=g", "t", env)() == 3)
    assert(load("local function global () return 5 end return global()")() == 5)
    assert(load("return function (global) return global end")()(6) == 6)
    assert(load(calls))
  end
  assert(load(decl, "=decl", "t", env)() == 3 and env.gf and env.x == 1)
  for _, s in ipairs{"global", "global ;", "global = = 1", "global function"} do
    assert(not load(s))
  end
end
