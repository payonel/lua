-- tests/deep/budgetgc.lua
-- '__gc' with budget groups: the owner of an object, the queues of the
-- groups, and who calls what. A '__gc' of an object that unbudgeted code
-- registered (a kernel or host object) is called by the collector at once,
-- with the budget lifted. A '__gc' of an object that budgeted code
-- registered (a user object) waits in the queue of its group, and a thread
-- of the group calls it, as ordinary user code, at the next point where it
-- can freeze, and before 'collectgarbage' returns. Stock Lua 5.5 has none
-- of this (the file returns at once there).

local debug = require "debug"
if not debug.setbudget then return end

local function deepeq(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then return a == b end
  for k, v in pairs(a) do if not deepeq(v, rawget(b, k)) then return false end end
  for k in pairs(b) do if rawget(a, k) == nil then return false end end
  return true
end

-- resume, thawing the coroutine when it freezes (a kernel would do this)
local function kresume(co, ...)
  local r = table.pack(coroutine.resume(co, ...))
  while debug.isfrozen(co) do r = table.pack(debug.thaw(co)) end
  return table.unpack(r, 1, r.n)
end

local function nop() end

-- a kernel object's '__gc' is called at once, by the collector, on the
-- thread that collects, with no budget points inside it
do
  local calls, seen = 0, {}
  local co = coroutine.create(function()
    collectgarbage()
    return "done"
  end)
  setmetatable({}, {__gc = function() -- (made here: by the kernel)
    seen.th = coroutine.running()
    local c0 = calls
    for i = 1, 200 do end
    seen.during = calls - c0
    seen.ran = true
  end})
  debug.setbudget(co, 1, function() calls = calls + 1 end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "done")
  assert(seen.ran and seen.th == co and seen.during == 0)
  assert(calls >= 3) -- (the budget counted outside it)
end

-- a user object's '__gc' is called before 'collectgarbage' returns, on the
-- thread of the group, in the reverse order of registration, with the budget
do
  local order, steps = {}, 0
  local co
  co = coroutine.create(function()
    for i = 1, 3 do
      setmetatable({}, {__gc = function()
        order[#order + 1] = i
        order.th = coroutine.running()
        for k = 1, 10 do end
      end})
    end
    collectgarbage()
    order[#order + 1] = "after"
    return "ok"
  end)
  debug.setbudget(co, 1, function() steps = steps + 1 end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "ok")
  assert(deepeq(order, {3, 2, 1, "after", th = co}), table.concat(order, ","))
  assert(steps > 30)
end

-- collected by the kernel, a user object waits for its group: its '__gc'
-- runs when a thread of the group is resumed, before it goes on
do
  local log = {}
  local function make() setmetatable({}, {__gc = function() log[#log + 1] = "gc" end}) end
  local co = coroutine.create(function()
    make()
    coroutine.yield("made")
    log[#log + 1] = "resumed"
    return "end"
  end)
  debug.setbudget(co, 1000, nop)
  assert(select(2, coroutine.resume(co)) == "made")
  collectgarbage() -- (the kernel collects: the object is dead, queued)
  collectgarbage()
  assert(#log == 0)
  assert(select(2, coroutine.resume(co)) == "end")
  assert(deepeq(log, {"gc", "resumed"}), table.concat(log, ","))
end

-- the group of the object, not the thread that dies: a thread of the
-- group other than the one that made the object calls its '__gc'
do
  local log = {}
  local maker = coroutine.create(function()
    setmetatable({}, {__gc = function() log[#log + 1] = coroutine.running() end})
  end)
  debug.setbudget(maker, 1000, nop)
  assert(coroutine.resume(maker))
  local other = coroutine.create(function() for i = 1, 10 do end return "x" end)
  -- (a thread of the maker's group, made while the group is in effect)
  local made
  local spawner = coroutine.create(function() made = coroutine.create(function() for i = 1, 10 do end return "x" end) end)
  debug.setbudget(spawner, 1000, nop)
  assert(coroutine.resume(spawner))
  local group = select(4, debug.getbudget(spawner))
  assert(group == spawner)
  collectgarbage()
  assert(#log == 0)
  assert(coroutine.resume(maker) == false) -- (dead)
  -- the maker's own group runs it when a thread of that group runs
  local sib = coroutine.create(function() end)
  debug.setbudget(sib, 1000, nop) -- (another group: does not)
  assert(coroutine.resume(sib))
  assert(#log == 0)
end

-- '__gc' of user objects, frozen at every budget point, do what they do
-- unfrozen (and unbudgeted, where the collector calls them at once)
do
  local function run(n)
    local log = {}
    local co = coroutine.create(function()
      for i = 1, 3 do
        setmetatable({}, {__gc = function()
          local s = 0
          for k = 1, 30 do s = s + k end
          log[#log + 1] = s .. ":" .. i
        end})
      end
      collectgarbage()
      log[#log + 1] = "end"
      return table.concat(log, ",")
    end)
    local freezes = 0
    if n then debug.setbudget(co, n, function() freezes = freezes + 1 debug.freeze() end) end
    local ok, r = kresume(co)
    assert(ok, r)
    return r, freezes
  end
  local plain = run(nil)
  assert(plain == "465:3,465:2,465:1,end", plain)
  for _, n in ipairs{1, 2, 5, 11} do
    local r, freezes = run(n)
    assert(r == plain, n .. ": " .. r)
    assert(freezes > 15, n .. ": " .. freezes .. " freezes")
  end
end

-- a chain dropped without being thawed, with '__gc' waiting, is just freed
do
  local called = false
  local weak = setmetatable({}, {__mode = "k"})
  do
    local co = coroutine.create(function()
      local o = setmetatable({}, {__gc = function() called = true end})
      weak[o] = true
      o = nil
      coroutine.yield()
    end)
    debug.setbudget(co, 1000, nop)
    coroutine.resume(co)
  end
  collectgarbage()
  collectgarbage()
  assert(not called)
  assert(next(weak) == nil)
end

-- ... also when a waiting object refers to a thread of the program: the
-- program's memory is released; while the kernel still holds the program,
-- its waiting objects stay, and their '__gc' are not called
do
  local function program(called)
    return function()
      local big = string.rep("x", 1 << 20) -- (1 MB of the program)
      local me = coroutine.running()
      setmetatable({me = me}, {__gc = function() called.v = true end})
      while true do big = big end
    end
  end
  local function run(keep)
    collectgarbage(); collectgarbage()
    local base = collectgarbage("count")
    collectgarbage("stop") -- (no collection while it runs)
    local called = {}
    local co = coroutine.create(program(called))
    debug.setbudget(co, 1000, function() debug.freeze() end)
    assert(coroutine.resume(co) and debug.isfrozen(co))
    if not keep then co = nil end
    collectgarbage("restart")
    for i = 1, 4 do collectgarbage() end
    assert(not called.v)
    return collectgarbage("count") - base, co
  end
  local kept, co = run(true)
  assert(kept > 1024 and debug.isfrozen(co))
  co = nil
  local dropped = run(false)
  assert(dropped < 256, dropped)
end

-- a waiting object is white between cycles, as swept objects are: one that
-- an earlier '__gc' stored in an old table (no barrier when it was stored,
-- black) is not freed under it (debug builds check the collector's invariant)
do
  local T = rawget(_G, "T")
  for _, mode in ipairs{"generational", "incremental"} do
    collectgarbage(mode)
    local G = {}
    collectgarbage(); collectgarbage() -- (G is old)
    local co = coroutine.create(function()
      do
        local B = setmetatable({tag = "B"}, {__gc = nop})
        local A = setmetatable({b = B}, {__gc = function(o) G.b = o.b end})
      end
      coroutine.yield()
      for i = 1, 5 do end -- (their '__gc' are called here)
      return "done"
    end)
    debug.setbudget(co, 1000000, nop)
    assert(coroutine.resume(co))
    collectgarbage("step") -- they die: they wait for the group
    assert(G.b == nil)
    assert(coroutine.resume(co))
    assert(G.b and G.b.tag == "B")
    if T then T.checkmemory() end
    for i = 1, 3 do collectgarbage("step") end
    for i = 1, 1000 do local t = {i, i, i} end
    collectgarbage()
    assert(G.b.tag == "B")
    if T then T.checkmemory() end
  end
  collectgarbage("incremental")
end

-- the owner of an object is the group in effect when it was registered: an
-- object whose group is gone is dead without its '__gc', and no new group
-- is given its owner id, however many groups come and go
do
  local called = false
  local holder
  do
    local co = coroutine.create(function()
      holder = setmetatable({}, {__gc = function() called = true end})
    end)
    debug.setbudget(co, 1000, nop)
    assert(coroutine.resume(co))
  end
  collectgarbage()
  assert(holder ~= nil and not called) -- (alive: the kernel holds it)
  local groups = 0
  for i = 1, 70000 do
    local co = coroutine.create(nop)
    debug.setbudget(co, 100, nop)
    groups = groups + 1
    if i % 4000 == 0 then collectgarbage() end
  end
  holder = nil
  collectgarbage()
  collectgarbage()
  -- (the groups of the threads resumed now call only their own)
  for i = 1, 20 do
    local co = coroutine.create(function() for k = 1, 10 do end end)
    debug.setbudget(co, 1, nop)
    assert(coroutine.resume(co))
  end
  collectgarbage()
  assert(not called)
end

-- a file handle of a dropped group (its '__gc' never runs) does not leak
-- its file: the data it wrote reaches the file
do
  local name = os.tmpname()
  do
    local co = coroutine.create(function()
      local f = assert(io.open(name, "w"))
      f:write("data") -- (never flushed or closed by the program)
    end)
    debug.setbudget(co, 1000, nop)
    assert(coroutine.resume(co))
  end
  collectgarbage()
  collectgarbage()
  local f = assert(io.open(name))
  assert(f:read("a") == "data")
  f:close()
  os.remove(name)
end

-- at most 65534 groups at once, with a clear error, raised on the caller
-- (a kernel coroutine here), not on the thread given the budget, which is
-- as it was; room again when they go
do
  local live = {}
  local ok, err, target
  local kernel = coroutine.wrap(function()
    for i = 1, 65540 do
      local co = coroutine.create(nop)
      ok, err = pcall(debug.setbudget, co, 100, nop)
      if not ok then target = co break end
      live[i] = co
    end
    return "the kernel goes on"
  end)
  assert(kernel() == "the kernel goes on")
  assert(not ok and tostring(err):find("too many budget groups"), tostring(err))
  assert(#live >= 65000 and #live <= 65534, #live)
  assert(coroutine.status(target) == "suspended" and debug.getbudget(target) == nil)
  live = nil
  collectgarbage()
  collectgarbage()
  local co = coroutine.create(nop)
  debug.setbudget(co, 100, nop)
  assert(debug.getbudget(co) == 100)
  assert(coroutine.resume(target)) -- (it runs, as it would have)
end

-- an error in a user '__gc' is a warning, as in stock, and the object is
-- not tried again (a child interpreter shows the warnings)
if io.popen and arg and arg[-1] then
  local script = [[
    warn("@allow")
    local co = coroutine.create(function()
      setmetatable({}, {__gc = function() error("in user gc", 0) end})
      setmetatable({}, {__gc = function() io.stderr:write("second\n") end})
      collectgarbage()
      io.stderr:write("end\n")
      collectgarbage()
    end)
    debug.setbudget(co, 1000, function() end)
    assert(coroutine.resume(co))
  ]]
  local p = io.popen(string.format("%q -W -e %q 2>&1", arg[-1], script))
  local out = p:read("a")
  p:close()
  assert(out == "second\nLua warning: error in __gc (in user gc)\nend\n", out)
end

-- closing the state with '__gc' waiting for their groups does not call them
if io.popen and arg and arg[-1] then
  local script = [[
    local co = coroutine.create(function()
      setmetatable({}, {__gc = function() io.stderr:write("user gc\n") end})
      coroutine.yield()
    end)
    setmetatable({}, {__gc = function() io.stderr:write("kernel gc\n") end})
    debug.setbudget(co, 1000, function() end)
    coroutine.resume(co)
    collectgarbage()
    io.stderr:write("end\n")
  ]]
  local p = io.popen(string.format("%q -e %q 2>&1", arg[-1], script))
  local out = p:read("a")
  p:close()
  assert(out == "kernel gc\nend\n" or out == "end\nkernel gc\n", out)
end
