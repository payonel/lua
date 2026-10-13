-- opcodes: rounds of workloads that together run the VM's opcodes, timing each (args: [rounds [scale]])
local rounds = math.tointeger(tonumber(arg and arg[1])) or 10
local scale = tonumber(arg and arg[2]) or 1

local clock = os.clock
local fmt = string.format
local setmetatable = setmetatable

local workloads = {}
local function workload (name, n, f)
  workloads[#workloads + 1] = {name = name, n = math.floor(n * scale), f = f}
end

workload("integer arithmetic", 2000000, function (n)
  local a, b, s = 3, 7, 0
  for i = 1, n do
    local x = i + 1
    x = x * 3 - b
    x = x % 1000 + a
    x = x // 3 - 2
    s = s + ((x & 0xff) ~ (x | 3))
    s = (s ~ i) & 0xffffff
    s = s + ((i << 2) >> 1) + (x << a) - (x >> a)
    s = s + (-x) + ~a + (s & b) + (s | b)
    s = s - x * b + i % b - i // b
    s = s + (i << 1) - (i >> 1) + ((s ~ 0x5a5) & 0xff) + (1 << (i & 7))
  end
  return s
end)

workload("float arithmetic", 2000000, function (n)
  local s, h = 0.0, 0.5
  for i = 1, n do
    local x = i / 4
    local y = x * 2.5 + h
    y = y - x / 2 + 1.5
    y = y + (i % 7) ^ 2 - x ^ 2 / (x * x)
    s = s + y // 3 + y % 3 - 0.25
    s = s + (-y) * 0.0 + 2.0 ^ 3 + h ^ y
  end
  for x = 0.5, n / 10, 1.0 do
    s = s + x * h
  end
  return s
end)

workload("comparisons and branches", 2000000, function (n)
  local s, t, f = 0, true, false
  local str = "abc"
  for i = 1, n do
    local a, b = i % 13, i % 7
    if a < b then s = s + 1 elseif a <= b then s = s + 2 else s = s + 3 end
    if a == b then s = s + 5 end
    if a ~= 3 then s = s + 1 end
    if a > 5 then s = s + 1 end
    if a >= 10 then s = s - 1 end
    if b < 2 then s = s + 4 end
    if b <= 4 then s = s + 1 end
    if str == "abc" then s = s + 1 end
    local c = a < b
    local d = not c
    local e = (a > 6) or f
    local g = c and t or nil
    if d then s = s + 1 end
    if e then s = s + 1 end
    if not g then s = s + 2 end
    local m = a > b and a or b
    s = s + m
  end
  local j = 0
  while j < n do
    j = j + 3
    if j % 2 == 0 then s = s + 1 else s = s - 1 end
  end
  repeat j = j - 7 until j < 0
  return s + j
end)

local function defglobal (v)
  global BENCH_NEW = v
  local r = BENCH_NEW
  BENCH_NEW = nil
  return r
end

workload("tables", 300000, function (n)
  local s = 0
  local keys = {"a", "b", "c", "d"}
  for i = 1, n do
    local t = {i, i + 1, i + 2, x = i, y = 2}
    t[4] = t[1] + t[2]
    t.z = t.x * t.y
    local k = keys[(i & 3) + 1]
    t[k] = i
    s = s + t[k] + t.z + #t + t[3]
    local j = (i & 7) + 1
    t[j] = j
    s = s + t[j]
    BENCH_GLOBAL = s & 0xffff
    s = s + BENCH_GLOBAL + defglobal(i & 3)
  end
  local arr = {}
  for i = 1, n do arr[i] = i end
  for i = 1, n do s = s + arr[i] end
  local h = {}
  for i = 1, n do h["k" .. (i & 1023)] = i end
  for i = 1, n do s = s + h["k" .. (i & 1023)] end
  return s
end)

local function fib (x)
  if x < 2 then return x end
  return fib(x - 1) + fib(x - 2)
end

local function none () end
local function one (x) return x end
local function two (x, y) return y, x end
local function count (x, acc)
  if x == 0 then return acc end
  return count(x - 1, acc + 1)
end

workload("calls and returns", 1000000, function (n)
  local s = 0
  for _ = 1, n // 250000 do s = s + fib(25) end
  for i = 1, n do
    none()
    s = s + one(i)
    local a, b = two(i, 1)
    s = s + a - b
  end
  for _ = 1, n // 1000 do s = s + count(1000, 0) end
  return s
end)

local function sum (...)
  local s = 0
  for i = 1, select("#", ...) do s = s + (select(i, ...)) end
  return s
end

local function pack2 (...) return ... end

local function named (...t)
  return t[1] + t[2] + t.n
end

workload("varargs", 300000, function (n)
  local s = 0
  for i = 1, n do
    s = s + sum(i, 1, 2)
    local a, b, c = pack2(i, 2, 3)
    s = s + a + b + c
    s = s + named(i, 4)
    local t = {pack2(1, 2, i)}
    s = s + #t
  end
  return s
end)

workload("closures and upvalues", 300000, function (n)
  local s = 0
  local counter = 0
  local function inc () counter = counter + 1; return counter end
  for i = 1, n do
    local v = i
    local f = function () v = v + 1; return v end
    s = s + f() + inc()
    do
      local w = i & 15
      local g = function () return w end
      s = s + g()
    end
  end
  for i = 1, n do
    local u = i
    local h = function () return u * 2 end
    if i % 3 == 0 then s = s + h() end
  end
  return s + counter
end)

local V = {}
V.__index = V
V.__add = function (a, b)
  if type(b) == "number" then return setmetatable({x = a.x + b}, V) end
  if type(a) == "number" then return setmetatable({x = a + b.x}, V) end
  return setmetatable({x = a.x + b.x}, V)
end
V.__mul = function (a, b) return setmetatable({x = a.x * b}, V) end
V.__unm = function (a) return setmetatable({x = -a.x}, V) end
V.__eq = function (a, b) return a.x == b.x end
V.__lt = function (a, b) return a.x < b.x end
V.__le = function (a, b) return a.x <= b.x end
V.__len = function (a) return a.x end
V.__concat = function (a, b) return "v" end
V.__call = function (a, y) return a.x + y end
function V.new (x) return setmetatable({x = x}, V) end
function V:get () return self.x end
function V:add (y) self.x = self.x + y; return self end

local Base = {base = 1}
local Mid = setmetatable({mid = 2}, {__index = Base})
local Proxy = setmetatable({}, {
  __index = function (t, k) return k * 2 end,
  __newindex = function (t, k, v) rawset(t, k, v) end,
})

workload("methods and metamethods", 100000, function (n)
  local s = 0
  local obj = setmetatable({}, {__index = Mid})
  for i = 1, n do
    local v, w = V.new(i), V.new(2)
    s = s + v:get() + v:add(1):get()
    local r = v + w
    r = r + 1
    r = 1 + r
    r = r * 2.5
    r = -r
    s = s + r.x
    if v == w then s = s + 1 end
    if v < w then s = s + 1 end
    if v <= w then s = s + 1 end
    s = s + #v + v(3) + #(v .. w)
    s = s + obj.base + obj.mid + Proxy[i & 7]
    Proxy[i & 63] = nil
  end
  return s
end)

workload("strings", 200000, function (n)
  local s = 0
  local parts = {"alpha", "beta", "gamma"}
  for i = 1, n do
    local a = parts[(i % 3) + 1]
    local str = a .. "-" .. i .. "/" .. a
    s = s + #str
    local b = str .. i * 2
    if b ~= str then s = s + 1 end
    s = s + #(i .. "") + #tostring(i)
  end
  return s
end)

workload("generic for", 6000, function (n)
  local s = 0
  local t = {}
  for i = 1, 100 do t[i] = i end
  local h = {}
  for i = 1, 100 do h["k" .. i] = i end
  local function range (m)
    return function (_, c)
      if c < m then return c + 1 end
    end, nil, 0
  end
  for _ = 1, n do
    for i, v in ipairs(t) do s = s + i + v end
    for k, v in pairs(h) do s = s + v end
    for k, v in next, t do s = s + v end
    for i in range(100) do s = s + i end
  end
  local closed = 0
  local mt = {__close = function () closed = closed + 1 end}
  for i = 1, n * 50 do
    local c <close> = setmetatable({}, mt)
    s = s + i
  end
  return s + closed
end)

local function fmtsum (v)
  if math.type(v) == "integer" then return fmt("%d", v) end
  return fmt("%.17g", v)
end

io.write(fmt("opcodes: %d rounds, scale %g\n", rounds, scale))
local times = {}
for r = 1, rounds do
  local total = 0
  for w, item in ipairs(workloads) do
    local t0 = clock()
    local result = item.f(item.n)
    local dt = clock() - t0
    times[w] = times[w] or {}
    times[w][r] = dt
    total = total + dt
    if r == 1 then item.result = result
    elseif item.result ~= result then error(item.name .. ": result differs in round " .. r) end
  end
  io.write(fmt("round %2d: %7.3f s\n", r, total))
  io.flush()
end

local function median (list)
  local c = {}
  for i = 1, #list do c[i] = list[i] end
  table.sort(c)
  local m = #c // 2
  if #c % 2 == 1 then return c[m + 1] end
  return (c[m] + c[m + 1]) / 2
end

io.write(fmt("%-26s %8s %8s %8s  %s\n", "workload", "first", "median", "last", "result"))
local first, med, last = 0, 0, 0
for w, item in ipairs(workloads) do
  local tw = times[w]
  first, med, last = first + tw[1], med + median(tw), last + tw[rounds]
  io.write(fmt("%-26s %8.3f %8.3f %8.3f  %s\n", item.name, tw[1], median(tw), tw[rounds], fmtsum(item.result)))
end
io.write(fmt("%-26s %8.3f %8.3f %8.3f\n", "total", first, med, last))
