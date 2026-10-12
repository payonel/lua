-- tests/deep/finalizers.lua
-- Finalizers ('__gc') as stock Lua 5.5 runs them: when, in what order,
-- what they can do, and how their errors are reported. (Which thread runs
-- them is not pinned here: see freeze.lua.)

-- in reverse order of marking, when a collection finds them; a full
-- collection runs all of them before it returns (the collector stopped
-- meanwhile, so that only that collection finds them: no cycle of its own
-- finds some first, nor the one a restore ends with in the Styx gate)
do
  local order = {}
  collectgarbage("stop")
  for i = 1, 5 do
    setmetatable({}, {__gc = function() order[#order + 1] = i end})
  end
  collectgarbage()
  collectgarbage("restart")
  assert(table.concat(order, ",") == "5,4,3,2,1", table.concat(order, ","))
end

-- a finalizer runs once, gets its object, can make it live again
do
  local saved, calls = nil, 0
  setmetatable({tag = "x"}, {__gc = function(o) calls = calls + 1 saved = o end})
  collectgarbage()
  assert(calls == 1 and saved.tag == "x")
  saved = nil
  collectgarbage()
  collectgarbage()
  assert(calls == 1) -- (not again)
end

-- an object made by a finalizer, with a finalizer of its own, is
-- finalized by a later collection
do
  local log = {}
  setmetatable({}, {__gc = function()
    log[#log + 1] = "outer"
    setmetatable({}, {__gc = function() log[#log + 1] = "inner" end})
  end})
  collectgarbage()
  collectgarbage()
  assert(table.concat(log, " ") == "outer inner", table.concat(log, " "))
end

-- inside a finalizer: 'collectgarbage' fails (the collector is stopped),
-- errors do not leave it, and it cannot yield (a collection made in a
-- coroutine: from the main thread the message differs, see freeze.lua;
-- the program stops the collector meanwhile, so that only that collection
-- finds the object, as above)
do
  local inside = {}
  collectgarbage("stop")
  setmetatable({}, {__gc = function()
    inside.gc = table.pack(collectgarbage())
    inside.count = table.pack(collectgarbage("count"))
    inside.yieldable = coroutine.isyieldable()
    inside.err = select(2, pcall(coroutine.yield))
    error("finalizer error") -- (a warning, not an error here)
  end})
  local co = coroutine.wrap(function() return pcall(collectgarbage) end)
  assert(co())
  collectgarbage("restart")
  assert(inside.gc.n == 1 and inside.gc[1] == nil and inside.count.n == 1 and inside.count[1] == nil)
  assert(inside.yieldable == false and inside.err == "attempt to yield across a C-call boundary")
end

-- the warnings of failing finalizers, and their '__close' variables,
-- closed before the warning (a child interpreter shows them)
if io.popen and arg and arg[-1] then
  local script = [[
    warn("@allow")
    setmetatable({}, {__gc = function() error("plain") end})
    setmetatable({}, {__gc = function() error({}) end})
    setmetatable({}, {__gc = function()
      local x <close> = setmetatable({}, {__close = function(_, e)
        io.stderr:write("closing with ", tostring(e), "\n")
      end})
      error("with close", 0)
    end})
    setmetatable({}, {__gc = function()
      local x <close> = setmetatable({}, {__close = function() error("in close", 0) end})
      error("replaced", 0)
    end})
    collectgarbage()
    io.stderr:write("end\n")
  ]]
  -- (the script is one line for the shell; '@allow' lets test builds warn)
  local p = io.popen(string.format("%q -W -e %q 2>&1", arg[-1], script))
  local out = p:read("a")
  p:close()
  local expected = "Lua warning: error in __gc (in close)\n" ..
                   "closing with with close\n" ..
                   "Lua warning: error in __gc (with close)\n" ..
                   "Lua warning: error in __gc (error object is not a string)\n" ..
                   "Lua warning: error in __gc ((command line):1: plain)\n" ..
                   "end\n"
  assert(out == expected, out)
end
