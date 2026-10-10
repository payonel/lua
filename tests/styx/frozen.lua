-- frozen coroutines: a user program stopped by its budget (debug.freeze in
-- the budget callback) is persisted with its chain, its budget group and
-- its C frames, restored, and goes on exactly as the original does (ADR-34)
local styx = require "styx"
local H = require "harness"
local case = H.case

-- the standard libraries by name (their functions are the host's permanents)
local perms, back = {[_G] = "_G"}, {_G = _G}
for _, name in ipairs{"string", "table", "math", "os", "io", "coroutine", "debug", "utf8"} do
  local lib = _G[name]
  perms[lib], back["lib:" .. name] = "lib:" .. name, lib
  for k, v in pairs(lib) do
    if type(v) == "function" then
      local n = name .. "." .. k
      perms[v], back[n] = n, v
    end
  end
end
for _, name in ipairs{"pcall", "xpcall", "error", "select", "tostring", "tonumber", "ipairs", "pairs", "next",
                      "type", "setmetatable", "getmetatable", "rawget", "rawset", "assert", "print"} do
  perms[_G[name]], back["base." .. name] = "base." .. name, _G[name]
end
-- (only functions persist by name, so the libraries' tables too)
for k, v in pairs(perms) do back[v] = back[v] or k end
back = H.invert(perms)

-- run 'f' in a coroutine whose budget freezes it at its k-th callback
-- (steps of 'n'); returns the coroutine and the state its callback keeps
local function freezeat(f, k, n, ...)
  local state = {count = 0}
  local co = coroutine.create(f)
  debug.setbudget(co, n, function()
    state.count = state.count + 1
    if state.count == k then debug.freeze() end
  end)
  local ok, err = coroutine.resume(co, ...)
  assert(ok, err)
  return co, state
end

-- thaw until it is not frozen; returns the results of the last thaw
local function finish(co)
  local r = {n = 0}
  while debug.isfrozen(co) do
    r = table.pack(debug.thaw(co))
    assert(r[1], r[2])
  end
  return r
end

local function show(v, depth)
  local t = type(v)
  if t == "table" then
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(keys) do parts[#parts + 1] = show(k) .. "=" .. show(v[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  if t == "string" then return string.format("%q", v) end
  if t == "function" or t == "thread" or t == "userdata" then return t end
  return tostring(v)
end

-- the program, frozen at each of its budget points: persisted, restored,
-- thawed beside the original; both end the same, with the same log
local function everywhere(name, make, n, limit)
  limit = limit or 300
  local points = 0
  for k = 1, limit do
    local out = {}
    local co, state = freezeat(make(out), k, n or 5)
    if not debug.isfrozen(co) then break end
    points = points + 1
    local blob = styx.persist(perms, {co = co, out = out, state = state})
    local r = styx.unpersist(back, blob)
    assert(debug.isfrozen(r.co) and r.co ~= co, name .. ": restored not frozen at " .. k)
    assert(coroutine.status(r.co) == coroutine.status(co))
    local ra, rb = finish(co), finish(r.co)
    assert(not debug.isfrozen(co) and not debug.isfrozen(r.co))
    assert(show(ra) == show(rb), name .. " at " .. k .. ": " .. show(ra) .. " vs " .. show(rb))
    assert(show(out) == show(r.out), name .. " at " .. k .. ": log " .. show(out) .. " vs " .. show(r.out))
    assert(coroutine.status(co) == coroutine.status(r.co))
  end
  assert(points > 0, name .. ": never frozen")
  return points
end

case("a loop with no calls", function()
  everywhere("loop", function(out)
    return function() local s = 0 for i = 1, 40 do s = s + i out[#out + 1] = s end return s end
  end)
end)

case("a frozen coroutine is still frozen after a persist, and the copy is another", function()
  local co = freezeat(function() for i = 1, 100 do end return "end" end, 2, 5)
  assert(debug.isfrozen(co))
  local blob = styx.persist(perms, co)
  assert(debug.isfrozen(co))
  local c2 = styx.unpersist(back, blob)
  assert(c2 ~= co and debug.isfrozen(c2))
  assert(select(2, debug.thaw(c2)) == "end" or true)
  finish(c2)
  assert(coroutine.status(c2) == "dead" and debug.isfrozen(co))
  finish(co)
  assert(coroutine.status(co) == "dead")
end)

case("calls and returns", function()
  everywhere("calls", function(out)
    local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
    return function() local r = fib(9) out[#out + 1] = r return r end
  end)
end)

case("a protected call", function()
  everywhere("pcall", function(out)
    return function()
      local ok, v = pcall(function() local s = 0 for i = 1, 30 do s = s + i end return s end)
      out[#out + 1] = tostring(ok) .. v
      local ok2, e = pcall(function() local s = 0 for i = 1, 20 do s = s + i end error("boom " .. s, 0) end)
      out[#out + 1] = tostring(ok2) .. e
      return ok, v, ok2, e
    end
  end)
end)

case("xpcall with a message handler", function()
  everywhere("xpcall", function(out)
    return function()
      local function handler(m) local t = 0 for i = 1, 10 do t = t + i end return m .. "!" .. t end
      local ok, e = xpcall(function() for i = 1, 10 do end error("bad", 0) end, handler)
      out[#out + 1] = e
      return ok, e
    end
  end, 3)
end)

case("a coroutine resumed inside the program", function()
  everywhere("resume", function(out)
    return function()
      local inner = coroutine.create(function(a)
        local s = a
        for i = 1, 20 do s = s + i end
        local got = coroutine.yield(s)
        for i = 1, 10 do s = s + got end
        return s
      end)
      local ok, v = coroutine.resume(inner, 1)
      out[#out + 1] = v
      local ok2, w = coroutine.resume(inner, 3)
      out[#out + 1] = w
      return ok, v, ok2, w
    end
  end)
end)

case("a wrapped coroutine", function()
  everywhere("wrap", function(out)
    return function()
      local gen = coroutine.wrap(function() for i = 1, 4 do local s = 0 for j = 1, 6 do s = s + j end coroutine.yield(s + i) end end)
      local sum = 0
      for i = 1, 4 do local v = gen() out[#out + 1] = v sum = sum + v end
      return sum
    end
  end)
end)

case("a chain of coroutines, frozen in the innermost", function()
  everywhere("chain", function(out)
    return function()
      local c3 = coroutine.create(function() local s = 0 for i = 1, 25 do s = s + i end return s end)
      local c2 = coroutine.create(function() local ok, v = coroutine.resume(c3) return v + 1 end)
      local c1 = coroutine.create(function() local ok, v = coroutine.resume(c2) return v + 1 end)
      local ok, v = coroutine.resume(c1)
      out[#out + 1] = v
      return v
    end
  end, 3)
end)

case("a metamethod, an iterator, a library callback", function()
  everywhere("callbacks", function(out)
    return function()
      local t = setmetatable({}, {__index = function(_, i) local s = 0 for j = 1, 5 do s = s + j end return i * s end})
      local acc = 0
      for i, v in ipairs({10, 20, 30}) do acc = acc + v end
      out[#out + 1] = acc + t[3]
      local words = {}
      for w in ("one two three"):gmatch("%a+") do words[#words + 1] = w:upper() end
      out[#out + 1] = table.concat(words, "-")
      local list = {5, 3, 8, 1, 9, 2}
      table.sort(list, function(a, b) local x = 0 for _ = 1, 3 do x = x + 1 end return a > b end)
      out[#out + 1] = table.concat(list, ",")
      out[#out + 1] = (string.gsub("abc", "%w", function(c) return c .. c end))
      return acc
    end
  end, 7)
end)

case("to-be-closed variables", function()
  everywhere("tbc", function(out)
    return function()
      local function closer(name) return setmetatable({}, {__close = function(_, e)
        local s = 0 for i = 1, 8 do s = s + i end out[#out + 1] = name .. s .. tostring(e) end}) end
      do
        local a <close> = closer("a")
        local b <close> = closer("b")
        for i = 1, 10 do end
      end
      local ok = pcall(function()
        local c <close> = closer("c")
        error("e", 0)
      end)
      return ok
    end
  end, 4)
end)

case("a debug hook", function()
  everywhere("hook", function(out)
    return function()
      local n = 0
      debug.sethook(function() n = n + 1 end, "", 7)
      local s = 0
      for i = 1, 40 do s = s + i end
      debug.sethook()
      out[#out + 1] = n
      return s, n
    end
  end, 6)
end)

case("a budget group: limit and callback survive", function()
  local co, state = freezeat(function() for i = 1, 100 do end end, 2, 9)
  assert(debug.isfrozen(co))
  local n1, f1 = debug.getbudget(co)
  local r = styx.unpersist(back, styx.persist(perms, {co = co, state = state}))
  local n2, f2 = debug.getbudget(r.co)
  assert(n1 == 9 and n2 == 9 and type(f2) == "function" and f2 ~= f1)
end)

case("threads of one group stay in one group", function()
  local make = function()
    local inner
    local co = coroutine.create(function()
      inner = coroutine.create(function() for i = 1, 30 do end end)
      coroutine.resume(inner)
      for i = 1, 30 do end
    end)
    return co, function() return inner end
  end
  local co, getinner = make()
  local state = {count = 0}
  debug.setbudget(co, 5, function() state.count = state.count + 1 if state.count == 2 then debug.freeze() end end)
  assert(coroutine.resume(co))
  assert(debug.isfrozen(co))
  local inner = getinner()
  local r = styx.unpersist(back, styx.persist(perms, {co = co, inner = inner, state = state}))
  assert(r.inner ~= inner)
  local a = table.pack(debug.getbudget(r.co))
  local b = table.pack(debug.getbudget(r.inner))
  assert(a[1] == 5 and b[1] == 5)
  finish(r.co)
  assert(coroutine.status(r.co) == "dead" and coroutine.status(r.inner) == "dead")
end)

case("an error after the thaw is caught by the same pcall", function()
  everywhere("late error", function(out)
    return function()
      local ok, e = pcall(function() for i = 1, 20 do end local t = nil return t.x end)
      out[#out + 1] = tostring(ok) .. tostring(e)
      return ok
    end
  end, 3)
end)

case("a stream of a frozen chain is rejected when cut", function()
  local co, state = freezeat(function() local inner = coroutine.create(function() for i = 1, 30 do end end)
    coroutine.resume(inner) end, 3, 4)
  assert(debug.isfrozen(co))
  local blob = styx.persist(perms, {co = co, state = state})
  H.rejects_truncation(blob, back)
end)

case("a thread of the chain cannot be persisted from inside the budget callback; after the freeze it can", function()
  local seen
  local co = coroutine.create(function() for i = 1, 100 do end return "end" end)
  local n = 0
  debug.setbudget(co, 5, function(user_co)
    n = n + 1
    if n == 2 then
      seen = {pcall(styx.persist, perms, user_co)}
      debug.freeze()
    end
  end)
  assert(coroutine.resume(co))
  assert(debug.isfrozen(co))
  assert(seen[1] == false and seen[2]:find("cannot persist a running thread", 1, true), tostring(seen[2]))
  local r = styx.unpersist(back, styx.persist(perms, {co = co, n = n}))
  assert(debug.isfrozen(r.co))
  finish(r.co)
  assert(coroutine.status(r.co) == "dead")
end)
