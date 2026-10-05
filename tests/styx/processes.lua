-- persist in this process, unpersist in a separate interpreter process:
-- the two runtimes share nothing but the persisted bytes (ADR-11)
local styx = require "styx"
local H = require "harness"
local case = H.case

local lua = "../../lua"

local function write_file(name, contents)
  local f = assert(io.open(name, "wb"))
  f:write(contents)
  f:close()
end

-- runs 'code' in a new interpreter with 'blob' and 'styx' in scope; returns
-- what it wrote to stdout
local function run_elsewhere(blob, code)
  local blobfile, script = os.tmpname(), os.tmpname()
  write_file(blobfile, blob)
  write_file(script, "local f = io.open(arg[1], 'rb') local blob = f:read('a') f:close()\n" ..
                     "local styx = require 'styx'\n" .. code)
  local p = assert(io.popen(lua .. " " .. script .. " " .. blobfile, "r"))
  local out = p:read("a")
  p:close()
  os.remove(blobfile)
  os.remove(script)
  return out
end

case("tables restored in another process", function()
  local t = {1, 2, 3, name = "remote"}
  t.self = t
  local out = run_elsewhere(styx.persist(t),
    "local t = styx.unpersist(blob) io.write(#t, t.name, tostring(t.self == t))")
  assert(out == "3remotetrue", out)
end)

case("a coroutine resumed in another process with its own permanents", function()
  local function make()
    local co = coroutine.create(function(n)
      local out = io.write
      while true do n = n + coroutine.yield(n); out(n, ";") end
    end)
    return co
  end
  local co = make()
  coroutine.resume(co, 2)
  local blob = styx.persist({[_ENV] = "env", [coroutine.yield] = "yield", [io.write] = "write"}, co)
  local out = run_elsewhere(blob,
    "local co = styx.unpersist({env = _ENV, yield = coroutine.yield, " ..
    "write = function(...) io.write('[', table.concat({...}), ']') end}, blob) " ..
    "coroutine.resume(co, 10) coroutine.resume(co, 100)")
  assert(out == "[12;][112;]", out)
end)

case("a closure with shared upvalues restored in another process", function()
  local function counter()
    local n = 40
    return function() n = n + 1; return n end, function() return n end
  end
  local inc, get = counter()
  local out = run_elsewhere(styx.persist({inc, get}),
    "local f = styx.unpersist(blob) f[1]() f[1]() io.write(f[2]())")
  assert(out == "42", out)
end)
