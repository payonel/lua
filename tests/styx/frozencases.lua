-- Programs for the C++ test 'frozen_engine_cases' (like tests/deep/libcases.lua:
-- each case is a function taking 'busy', run in a budgeted coroutine, frozen at
-- every budget point, its state persisted and restored in a new one): what the
-- engine itself runs in a frozen chain besides the library's callbacks. (The
-- collector is stopped where the order of the finalizers matters: the debt
-- of a restored state is not the original's, so a step of the automatic
-- collector comes at another point; see "Determinism".)
local cases = {}
local function case(name, f) cases[#cases + 1] = {name = name, f = f} end

case("__gc drained after a collection, frozen inside the __gc", function(busy)
  collectgarbage("stop")
  local log = {}
  do
    local mt = {__gc = function(o) for i = 1, 4 do busy() end log[#log + 1] = "gc" .. o.id end}
    for i = 1, 3 do setmetatable({id = i}, mt) end
  end
  collectgarbage()
  for i = 1, 3 do busy() end
  collectgarbage("restart")
  return table.concat(log, ",")
end)

case("__gc that is run by the budget points of a loop", function(busy)
  collectgarbage("stop")
  local log = {}
  local mt = {__gc = function(o) busy() log[#log + 1] = o.id end}
  for i = 1, 40 do
    setmetatable({id = i}, mt)
    local s = 0
    for j = 1, 30 do s = s + j end
  end
  for i = 1, 20 do local t = {} for j = 1, 100 do t[j] = j end end
  collectgarbage()
  collectgarbage()
  collectgarbage("restart")
  table.sort(log)
  return #log, log[1], log[#log]
end)

case("an error inside a drained __gc is a warning, and the program goes on", function(busy)
  collectgarbage("stop")
  local seen = {}
  local mt = {__gc = function(o) busy() seen[#seen + 1] = o.id error("in gc " .. o.id, 0) end}
  for i = 1, 2 do setmetatable({id = i}, mt) end
  collectgarbage()
  for i = 1, 5 do busy() end
  collectgarbage("restart")
  return #seen
end)

case("a message handler that freezes, then an error caught after the thaw", function(busy)
  local function handler(m) busy() busy() return m .. "!" end
  local ok, e = xpcall(function() busy() error("x", 0) end, handler)
  return ok, e
end)

case("to-be-closed variables closed by an error, freezing in the handlers", function(busy)
  local out = {}
  local function closer(n) return setmetatable({}, {__close = function(_, e) busy() out[#out + 1] = n .. tostring(e) end}) end
  local ok, e = pcall(function()
    local a <close> = closer("a")
    local b <close> = closer("b")
    busy()
    error("boom", 0)
  end)
  return ok, e, table.concat(out, ",")
end)

case("coroutine.close of a coroutine with variables to close", function(busy)
  local out = {}
  local co = coroutine.create(function()
    local v <close> = setmetatable({}, {__close = function() busy() busy() out[#out + 1] = "closed" end})
    coroutine.yield(1)
  end)
  coroutine.resume(co)
  local ok, e = coroutine.close(co)
  return ok, e, out[1], coroutine.status(co)
end)

case("a wrapped generator across the chain", function(busy)
  local gen = coroutine.wrap(function() for i = 1, 4 do busy() coroutine.yield(i * i) end return "done" end)
  local r = {}
  for i = 1, 5 do r[#r + 1] = gen() end
  return table.concat(r, ",")
end)

case("string.gmatch frozen in the middle of its loop", function(busy)
  local words = {}
  for w, n in ("alpha=1, beta=22, gamma=333"):gmatch("(%a+)=(%d+)") do
    busy()
    words[#words + 1] = w .. n
  end
  return table.concat(words, ";")
end)

case("table.sort and table.concat frozen with their boxes", function(busy)
  local t = {}
  for i = 1, 30 do t[i] = (i * 7) % 31 end
  table.sort(t, function(a, b) busy() return a > b end)
  return table.concat(t, ",", 1, 8)
end)

case("a hook function frozen at line events", function(busy)
  local lines = 0
  debug.sethook(function() busy() lines = lines + 1 end, "l")
  local s = 0
  for i = 1, 3 do
    s = s + i
  end
  debug.sethook()
  return lines > 0, s
end)

case("call and return hooks frozen, reading the values transferred", function(busy)
  local log = {}
  local function f(a, b, c) return a + b, c end
  debug.sethook(function(event)
    busy()
    local ar = debug.getinfo(2, "r")
    local t = {}
    for i = ar.ftransfer, ar.ftransfer + ar.ntransfer - 1 do
      local _, v = debug.getlocal(2, i)
      t[#t + 1] = tostring(v)
    end
    log[#log + 1] = event .. ":" .. table.concat(t, " ")
  end, "cr")
  local x, y = f(1, 2, "z")
  debug.sethook()
  return table.concat(log, ";"), x, y
end)

return cases
