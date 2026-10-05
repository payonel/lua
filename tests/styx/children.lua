-- persisting a child coroutine on its own: what it captured from outside
-- stays connected through the permanents table, which works on values
-- (tables, functions, threads); a captured variable is shared with the
-- outside by keeping it in a table that is a permanent
local styx = require "styx"
local H = require "harness"
local case = H.case

local base_perms = {[_ENV] = "env", [coroutine.yield] = "yield"}
local base_back = {env = _ENV, yield = coroutine.yield}

local function with(extra_perms, extra_back)
  local p, b = {}, {}
  for k, v in pairs(base_perms) do p[k] = v end
  for k, v in pairs(base_back) do b[k] = v end
  for k, v in pairs(extra_perms) do p[k] = v end
  for k, v in pairs(extra_back) do b[k] = v end
  return p, b
end

-- a child that captures an outer table and an outer callback (a lambda)
local function make_child(config, callback)
  return coroutine.create(function()
    while true do
      local msg = coroutine.yield()
      callback(config.prefix .. msg)
    end
  end)
end

case("a captured outer table restores as the permanent supplied", function()
  local config = {prefix = "old:"}
  local log = {}
  local function make_log(l) return function(m) l[#l + 1] = m end end
  local co = make_child(config, make_log(log))
  coroutine.resume(co)
  local other = {prefix = "new:"}
  local perms, back = with({[config] = "config", [log] = "log"}, {config = other, log = log})
  local r = styx.unpersist(back, styx.persist(perms, co))
  assert(r ~= co and coroutine.status(r) == "suspended")
  assert(coroutine.resume(r, "x"))
  assert(log[1] == "new:x", tostring(log[1]))
  other.prefix = "changed:"
  assert(coroutine.resume(r, "y"))
  assert(log[2] == "changed:y", "the restored child reads the supplied table itself")
  assert(config.prefix == "old:", "the original table is untouched")
end)

case("a captured callback (a lambda) is a permanent", function()
  local config = {prefix = "p:"}
  local log1, log2 = {}, {}
  local cb1 = function(s) log1[#log1 + 1] = s end
  local co = make_child(config, cb1)
  coroutine.resume(co)
  local cb2 = function(s) log2[#log2 + 1] = s end
  local perms, back = with({[cb1] = "callback", [config] = "config"}, {callback = cb2, config = config})
  local r = styx.unpersist(back, styx.persist(perms, co))
  coroutine.resume(r, "hello")
  coroutine.resume(r, "again")
  assert(#log1 == 0 and log2[1] == "p:hello" and log2[2] == "p:again")
  config.prefix = "q:"
  coroutine.resume(r, "shared")
  assert(log2[3] == "q:shared", "the restored child sees the supplied table itself")
end)

case("without permanents the captured values are copied", function()
  local config = {prefix = "copy:"}
  local log = {}
  local function make_log(l) return function(s) l[#l + 1] = s end end
  local co = make_child(config, make_log(log))
  coroutine.resume(co)
  local perms, back = with({}, {})
  local r = styx.unpersist(back, styx.persist(perms, co))
  config.prefix = "later:"
  coroutine.resume(r, "m")
  assert(#log == 0, "the restored child writes to its own copy of the log")
end)

case("a variable of the running scope: a value copy, or shared through a box", function()
  local count = 0
  local box = {count = 0}
  local co = coroutine.create(function()
    while true do
      coroutine.yield()
      count = count + 1
      box.count = box.count + 1
    end
  end)
  coroutine.resume(co)
  local perms, back = with({[box] = "box"}, {box = box})
  local r = styx.unpersist(back, styx.persist(perms, co))
  coroutine.resume(r)
  coroutine.resume(r)
  assert(count == 0, "the variable itself was copied")
  assert(box.count == 2, "the boxed value is shared through the permanent")
end)

case("a grandchild coroutine with permanents", function()
  local log = {}
  local cb = function(s) log[#log + 1] = s end
  local function make_parent(callback)
    local child = coroutine.create(function()
      while true do callback("child:" .. tostring(coroutine.yield())) end
    end)
    coroutine.resume(child)
    return coroutine.create(function()
      while true do
        local v = coroutine.yield()
        coroutine.resume(child, v)
        callback("parent:" .. v)
      end
    end)
  end
  local parent = make_parent(cb)
  coroutine.resume(parent)
  local log2 = {}
  local cb2 = function(s) log2[#log2 + 1] = s end
  local perms, back = with({[cb] = "cb", [tostring] = "tostring"}, {cb = cb2, tostring = tostring})
  local r = styx.unpersist(back, styx.persist(perms, parent))
  coroutine.resume(r, 7)
  assert(#log == 0)
  assert(log2[1] == "child:7" and log2[2] == "parent:7")
end)

case("the same permanent reached from a child and from outside it", function()
  local shared = {n = 0}
  local co = coroutine.create(function(t) while true do coroutine.yield(); t.n = t.n + 1 end end)
  coroutine.resume(co, shared)
  local perms, back = with({[shared] = "shared"}, {shared = shared})
  local r = styx.unpersist(back, styx.persist(perms, {co = co, shared = shared}))
  assert(r.shared == shared)
  coroutine.resume(r.co)
  assert(shared.n == 1)
end)
