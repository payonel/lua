-- debug hooks on persisted coroutines (debug.sethook): the hook function,
-- its mask, its count and where the count stands are restored, so the
-- restored coroutine produces the same hook events as the original
local styx = require "styx"
local H = require "harness"
local case = H.case

local function make_hook(log)
  return function(event, line) log[#log + 1] = event .. ":" .. tostring(line) end
end

local function body()
  local a = 1
  coroutine.yield()
  local function f(x) return x + 1 end
  a = f(a)
  a = a + 2
  coroutine.yield()
  for i = 1, 3 do a = a + i end
  return a
end

-- persists a hooked coroutine and runs original and copy to the end,
-- comparing their hook events and results
local function compare(mask, count, steps)
  local log1, log2 = {}, {}
  local co = coroutine.create(body)
  debug.sethook(co, make_hook(log1), mask, count)
  for _ = 1, steps do coroutine.resume(co) end
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [log1] = "log"}
  local back = {env = _ENV, yield = coroutine.yield, log = log2}
  local r = styx.unpersist(back, styx.persist(perms, co))
  local f1, m1, c1 = debug.gethook(co)
  local f2, m2, c2 = debug.gethook(r)
  assert(type(f2) == "function" and f2 ~= f1 and m1 == m2 and c1 == c2,
         "gethook: " .. tostring(m2) .. " " .. tostring(c2))
  local mark = #log1
  local results1, results2 = {}, {}
  while coroutine.status(co) ~= "dead" do results1[#results1 + 1] = select(2, coroutine.resume(co)) end
  while coroutine.status(r) ~= "dead" do results2[#results2 + 1] = select(2, coroutine.resume(r)) end
  assert(#results1 == #results2 and results1[#results1] == results2[#results2])
  local after = table.move(log1, mark + 1, #log1, 1, {})
  assert(#after == #log2, "events: " .. #after .. " vs " .. #log2)
  for i = 1, #after do assert(after[i] == log2[i], i .. ": " .. after[i] .. " vs " .. log2[i]) end
  assert(#log2 > 0, "the restored hook ran")
end

case("a line hook", function() compare("l", 0, 1) end)
case("call and return hooks", function() compare("cr", 0, 1) end)
case("a count hook persisted mid-count", function() compare("", 3, 1) end)
case("a count hook with lines, after two resumes", function() compare("l", 2, 2) end)

case("no hook stays no hook", function()
  local co = coroutine.create(body)
  coroutine.resume(co)
  local r = styx.unpersist({env = _ENV, yield = coroutine.yield},
                            styx.persist({[_ENV] = "env", [coroutine.yield] = "yield"}, co))
  assert(debug.gethook(r) == nil)
end)

case("a hook turned off stays off", function()
  local co = coroutine.create(body)
  debug.sethook(co, function() end, "l")
  debug.sethook(co)
  coroutine.resume(co)
  local r = styx.unpersist({env = _ENV, yield = coroutine.yield},
                            styx.persist({[_ENV] = "env", [coroutine.yield] = "yield"}, co))
  assert(debug.gethook(r) == nil)
end)

case("the hook function is shared like any value", function()
  local log = {}
  local hook = make_hook(log)
  local a = coroutine.create(body)
  local b = coroutine.create(body)
  debug.sethook(a, hook, "l")
  debug.sethook(b, hook, "l")
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [log] = "log"}
  local back = {env = _ENV, yield = coroutine.yield, log = {}}
  local r = styx.unpersist(back, styx.persist(perms, {a, b}))
  assert(debug.gethook(r[1]) == debug.gethook(r[2]))
end)
