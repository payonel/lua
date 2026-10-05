-- to-be-closed variables in suspended coroutines: after restore they are
-- closed when the coroutine leaves their scope, finishes, errors or is
-- closed, exactly as in the original
local styx = require "styx"
local H = require "harness"
local case = H.case

-- a closable value that records its closing in 'log' (a permanent)
local function make_closable_class(log)
  return {__close = function(v, err) log[#log + 1] = v.name .. (err and (":" .. tostring(err)) or "") end}
end

local function roundtrip_with(log, v)
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [log] = "log"}
  local back = {env = _ENV, yield = coroutine.yield, log = log}
  return H.roundtrip(v, perms, back)
end

case("a to-be-closed variable is closed when the restored coroutine returns", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local res <close> = setmetatable({name = "res"}, C)
    coroutine.yield()
    return "done"
  end)
  coroutine.resume(co)
  local r = roundtrip_with(log, co)
  assert(#log == 0)
  assert(select(2, coroutine.resume(r)) == "done")
  assert(#log == 1 and log[1] == "res")
  assert(coroutine.status(co) == "suspended", "the original is untouched")
end)

case("closed in reverse order, across nested frames", function()
  local log = {}
  local C = make_closable_class(log)
  local function make()
    local function inner()
      local b <close> = setmetatable({name = "b"}, C)
      local c <close> = setmetatable({name = "c"}, C)
      coroutine.yield()
    end
    return coroutine.create(function()
      local a <close> = setmetatable({name = "a"}, C)
      inner()
      coroutine.yield()
    end)
  end
  local co = make()
  coroutine.resume(co)
  local r = roundtrip_with(log, co)
  coroutine.resume(r)
  assert(table.concat(log, ",") == "c,b")
  coroutine.resume(r)
  assert(table.concat(log, ",") == "c,b,a")
end)

case("coroutine.close closes the restored variables", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local x <close> = setmetatable({name = "x"}, C)
    local y <close> = setmetatable({name = "y"}, C)
    coroutine.yield()
  end)
  coroutine.resume(co)
  local r = roundtrip_with(log, co)
  assert(coroutine.close(r))
  assert(table.concat(log, ",") == "y,x")
end)

case("an error after restore: closing behaves as in the original", function()
  local function run(co)
    local log = co.log
    local ok, err = coroutine.resume(co.co)
    local closed = {coroutine.close(co.co)}
    return table.concat(log, ","), ok, err, closed[1], closed[2]
  end
  local function make(log)
    local C = make_closable_class(log)
    local co = coroutine.create(function()
      local x <close> = setmetatable({name = "x"}, C)
      coroutine.yield()
      error("boom", 0)
    end)
    coroutine.resume(co)
    return co
  end
  local log1, log2 = {}, {}
  local original = make(log1)
  local perms = {[_ENV] = "env", [coroutine.yield] = "yield", [log1] = "log", [error] = "error"}
  local back = {env = _ENV, yield = coroutine.yield, log = log2, error = error}
  local restored = styx.unpersist(back, styx.persist(perms, original))
  local a = {run({co = original, log = log1})}
  local b = {run({co = restored, log = log2})}
  for i = 1, 5 do
    assert(a[i] == b[i], "result " .. i .. ": " .. tostring(a[i]) .. " vs " .. tostring(b[i]))
  end
  assert(a[1] ~= "", "the variable was closed")
end)

case("false and nil to-be-closed values are not closed", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local a <close> = false
    local b <close> = nil
    local c <close> = setmetatable({name = "c"}, C)
    coroutine.yield()
  end)
  coroutine.resume(co)
  local r = roundtrip_with(log, co)
  coroutine.resume(r)
  assert(table.concat(log, ",") == "c")
end)

case("the original and the restored coroutine close separately", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local v <close> = setmetatable({name = "v"}, C)
    coroutine.yield()
  end)
  coroutine.resume(co)
  local r = roundtrip_with(log, co)
  coroutine.resume(co)
  coroutine.resume(r)
  assert(#log == 2 and log[1] == "v" and log[2] == "v")
end)

case("a restored coroutine with to-be-closed variables persists again", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local v <close> = setmetatable({name = "v"}, C)
    for i = 1, 3 do coroutine.yield(i) end
  end)
  coroutine.resume(co)
  local r1 = roundtrip_with(log, co)
  coroutine.resume(r1)
  local r2 = roundtrip_with(log, r1)
  coroutine.resume(r2)
  coroutine.resume(r2)
  assert(#log == 1)
end)

case("rejects truncation of a stream with to-be-closed variables", function()
  local log = {}
  local C = make_closable_class(log)
  local co = coroutine.create(function()
    local v <close> = setmetatable({name = "v"}, C)
    coroutine.yield()
  end)
  coroutine.resume(co)
  H.rejects_truncation(styx.persist({[_ENV] = "env", [coroutine.yield] = "yield", [log] = "log"}, co),
                       {env = _ENV, yield = coroutine.yield, log = log})
end)
