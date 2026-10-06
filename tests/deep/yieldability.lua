-- tests/deep/yieldability.lua
-- Where code called back by the standard library may yield: what stock
-- Lua 5.5 allows and refuses. Library functions that call back into Lua
-- (metamethods, comparators, replacement functions, loaders, readers)
-- keep these rules whatever else they do in their middle.

-- runs 'f' in a coroutine; inside, 'probe' records whether the code
-- calling it can yield, and tries to yield
local function check(name, f, expect)
  local seen = {}
  local function probe()
    seen[#seen + 1] = coroutine.isyieldable()
    local ok, err = pcall(coroutine.yield, "probe")
    if not ok then seen.err = err end
    return ok
  end
  local co = coroutine.create(f)
  local ok, err = coroutine.resume(co, probe)
  local yielded = ok and coroutine.status(co) == "suspended"
  assert(ok, name .. ": " .. tostring(err))
  assert(#seen > 0, name .. ": probe not called")
  if expect then
    assert(seen[1] == true and yielded, name .. ": expected a yieldable call")
    -- (it goes on to its end)
    while coroutine.status(co) == "suspended" do assert(coroutine.resume(co)) end
  else
    for i = 1, #seen do assert(seen[i] == false, name .. ": expected not yieldable") end
    assert(not yielded, name .. ": yielded")
    assert(seen.err == "attempt to yield across a C-call boundary", name .. ": " .. tostring(seen.err))
  end
end

local function mt(fields) return setmetatable({}, fields) end

-- yieldable: metamethods called by the VM, and the library functions
-- that stock Lua calls with continuations
check("__index from Lua", function(probe)
  local t = mt{__index = function() probe() return 1 end}
  return t.x
end, true)
check("__lt from Lua", function(probe)
  local a = mt{__lt = function() probe() return true end}
  return a < a
end, true)
check("pcall", function(probe) pcall(probe) end, true)
check("xpcall", function(probe) xpcall(probe, print) end, true)
check("pairs __pairs", function(probe)
  local t = mt{__pairs = function(t) probe() return next, {}, nil end}
  for _ in pairs(t) do end
end, true)

-- not yieldable: everything else the library calls back
check("tostring __tostring", function(probe)
  return tostring(mt{__tostring = function() probe() return "x" end})
end, false)
check("ipairs __index", function(probe)
  local t = mt{__index = function(_, i) if i < 3 then probe() return i end end}
  for _ in ipairs(t) do end
end, false)
check("string.format %s", function(probe)
  return string.format("%s", mt{__tostring = function() probe() return "x" end})
end, false)
check("string.gsub function", function(probe)
  return string.gsub("ab", ".", function(c) probe() return c end)
end, false)
check("string.gsub table __index", function(probe)
  return string.gsub("ab", ".", mt{__index = function(_, c) probe() return c end})
end, false)
check("string arithmetic, the other operand's __add", function(probe)
  local s = mt{__add = function() probe() return 1 end}
  return "10" + s -- (the string's __add, from the library, calls it)
end, false)
check("table.sort comparator", function(probe)
  table.sort({3, 2, 1}, function(a, b) probe() return a < b end)
end, false)
check("table.sort __lt", function(probe)
  local m = {__lt = function(a, b) probe() return a.v < b.v end}
  local t = {setmetatable({v = 2}, m), setmetatable({v = 1}, m)}
  table.sort(t)
end, false)
check("table.insert __newindex", function(probe)
  local t = mt{__newindex = function(t, k, v) probe() rawset(t, k, v) end}
  table.insert(t, "x")
end, false)
check("table.insert __len", function(probe)
  local t = mt{__len = function() probe() return 0 end}
  table.insert(t, "x")
end, false)
check("table.remove __index", function(probe)
  local t = mt{__index = function() probe() return "v" end, __len = function() return 2 end}
  table.remove(t)
end, false)
check("table.concat __index", function(probe)
  local t = mt{__index = function(_, i) probe() return "s" end, __len = function() return 2 end}
  return table.concat(t)
end, false)
check("table.unpack __index", function(probe)
  local t = mt{__index = function(_, i) probe() return i end}
  return table.unpack(t, 1, 2)
end, false)
check("table.move __index", function(probe)
  local t = mt{__index = function(_, i) probe() return i end}
  return table.move(t, 1, 2, 1, {})
end, false)
check("os.time __index", function(probe)
  local t = mt{__index = function(_, k)
    probe()
    return ({year = 2000, month = 1, day = 1})[k]
  end}
  return os.time(t)
end, false)
check("require loader", function(probe)
  package.preload["yieldability_mod"] = function() probe() return true end
  require("yieldability_mod")
  package.preload["yieldability_mod"], package.loaded["yieldability_mod"] = nil, nil
end, false)
check("load reader", function(probe)
  local parts = {"return ", "1"}
  local i = 0
  return load(function() probe() i = i + 1 return parts[i] end)
end, false)
check("xpcall message handler", function(probe)
  xpcall(error, function(m) probe() return m end, "x")
end, false)
check("__close by coroutine.close", function(probe)
  local c = coroutine.create(function()
    local x <close> = mt{__close = function() probe() end}
    coroutine.yield()
  end)
  coroutine.resume(c)
  coroutine.close(c)
end, false)
check("__close at scope end", function(probe)
  do local x <close> = mt{__close = function() probe() end} end
end, true)
check("__close on error in pcall", function(probe)
  pcall(function()
    local x <close> = mt{__close = function() probe() end}
    error("e")
  end)
end, true)
check("__gc", function(probe)
  setmetatable({}, {__gc = function() probe() end})
  collectgarbage()
end, false)
check("hook", function(probe)
  local n = 0
  debug.sethook(function() n = n + 1 if n == 1 then probe() end end, "", 100)
  for i = 1, 300 do end
  debug.sethook()
end, false)
