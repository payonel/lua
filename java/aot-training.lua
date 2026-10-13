-- the training run of the AOT cache (Makefile: build/lua.aot): a little of
-- every library and of the compiler, so that the classes a program uses are
-- in the cache; it reads no file of the tree and leaves none behind

local n = 0

-- the compiler: most of the syntax, text and binary chunks
local src = [==[
  local a <const>, b = 10, ...
  local t = {1, 2.5, "x", [10] = true, k = {nested = {}}, f = function() end}
  local function f(x, ...)
    local s = 0
    for i = 1, select("#", ...) do s = s + (select(i, ...)) end
    return x // 3, x % 3, x / 2, x ^ 2, x & 5 | 2 ~ 1, x << 2 >> 1, ~x, -x, s
  end
  local r = {f(b or 7, 1, 2, 3)}
  for i = 10, 1, -3 do r[#r + 1] = i end
  for x = 0.5, 2.0, 0.25 do r[#r + 1] = x end
  local i = 0
  ::top:: i = i + 1
  if i < 5 then goto top end
  while i > 0 do i = i - 1 end
  repeat i = i + 1 until i >= 3
  local s = "a" .. 1 .. "b" .. 2.0
  return #r, s, a > 1 and not (a == 2) or a ~= 3, t.k.nested, t[10]
]==]
local chunk = assert(load(src, "=train"))
n = n + chunk(4)
local bin = string.dump(chunk)
n = n + assert(load(bin, "=train", "b"))(4)
n = n + assert(load(string.dump(chunk, true), "=train", "b"))(4)
assert(not load("x = = 1"))

-- strings
local parts = {}
for i = 1, 300 do
  parts[i] = string.format("%d:%s:%5.2f:%x:%g:%q", i, tostring(i), i / 3, i, i * 1e10, "q\n" .. i)
end
local s = table.concat(parts, ",")
for w in s:gmatch("%d+") do n = n + #w end
s = s:gsub("(%d)(%d)", "%2%1"):gsub("%a", {q = "Q"}):gsub("%s", function(c) return "_" end)
n = n + #s:upper():lower():rep(2, "-"):reverse():sub(5, -5)
n = n + (s:find("Q", 1, true) or 0) + (s:find("(%d+):(%x+)") or 0)
n = n + #(s:match("^(%d+)") or "") + s:byte(1) + #string.char(65, 66, 67)
local packed = string.pack("<i4 d s2 z B", 1, 2.5, "str", "zs", 255)
n = n + select("#", string.unpack("<i4 d s2 z B", packed)) + string.packsize("i4d")
n = n + #tostring(1e100) + #tostring(-0.0) + #tostring(2^63) + #tostring(math.mininteger)
n = n + tonumber("0x1p4") + tonumber("  12  ") + tonumber("z", 36) + (tonumber("1e") or 0)
n = n + #utf8.char(72, 228, 8364, 128512) + utf8.len("häll€") + utf8.codepoint("€")
for _, c in utf8.codes("aé€") do n = n + c end
n = n + (utf8.offset("aé€", 3) or 0)

-- tables
local t = {}
for i = 1, 500 do t[i] = (i * 7919) % 501 end
table.sort(t)
table.sort(t, function(a, b) return a > b end)
table.insert(t, 1, 0); table.insert(t, -1); table.remove(t, 1); table.remove(t)
n = n + #table.move(t, 1, 10, 5, {}) + select("#", table.unpack(t, 1, 20)) + table.pack(1, 2, 3).n
local h = {}
for i = 1, 300 do h["k" .. i] = i; h[i + 0.5] = i end
for k, v in pairs(h) do n = n + v end
for i, v in ipairs(t) do if i > 10 then break end n = n + v end
n = n + next({1}) + rawlen(t) + (rawget(h, "k1") or 0) + (rawequal(h, h) and 1 or 0)

-- metatables
local mt = {}
mt.__index = function(_, k) return k end
mt.__newindex = function(o, k, v) rawset(o, k, v) end
mt.__call = function(_, x) return x end
mt.__add = function(a, b) return 1 end
mt.__eq = function(a, b) return true end
mt.__lt = function(a, b) return true end
mt.__le = function(a, b) return true end
mt.__concat = function(a, b) return "c" end
mt.__len = function() return 3 end
mt.__unm = function() return 0 end
mt.__tostring = function() return "P" end
mt.__name = "P"
local P, Q = setmetatable({}, mt), setmetatable({}, mt)
P.x = 1
n = n + #P.foo + P(1) + (P + Q) + #P + -P + #(P .. Q) + #tostring(P)
n = n + ((P == Q) and 1 or 0) + ((P < Q) and 1 or 0) + ((P <= Q) and 1 or 0)
do
  local c <close> = setmetatable({}, {__close = function() n = n + 1 end})
end
local weak = setmetatable({}, {__mode = "k"})
weak[{}] = 1
setmetatable({}, {__gc = function() n = n + 1 end})

-- functions, coroutines, errors
local function counter()
  local c = 0
  return function(d) c = c + d; return c end
end
local cnt = counter()
for i = 1, 2000 do n = n + cnt(1) % 7 end
local co = coroutine.wrap(function(a)
  for i = 1, 10 do a = coroutine.yield(a + i) end
  return a
end)
for i = 1, 5 do n = n + co(i) end
local c2 = coroutine.create(function() coroutine.yield(coroutine.isyieldable(), coroutine.running()) end)
assert(coroutine.resume(c2)); n = n + #coroutine.status(c2); coroutine.close(c2)
n = n + select("#", pcall(error, {})) + select("#", pcall(error, "x", 2))
n = n + #select(2, pcall(function() local z = nil; return z.y end))
n = n + #select(2, pcall(function() return 1 + {} end))
n = n + #select(2, xpcall(function() error("e") end, debug.traceback))
n = n + #select(2, pcall(string.rep))

-- math
math.randomseed(42)
n = n + math.floor(3.5) + math.ceil(3.5) + math.sqrt(16) + math.abs(-2) + math.fmod(7, 3)
n = n + math.max(1, 5, 3) + math.min(1, 5, 3) + math.exp(0) + math.log(8, 2) + math.sin(0) + math.cos(0)
n = n + (math.tointeger(3.0) or 0) + #math.type(1) + (math.ult(1, -1) and 1 or 0) + math.random(10)
n = n + select(2, math.modf(3.5)) + math.pi + (math.huge > 0 and 1 or 0)

-- os and io
n = n + os.time() % 2 + os.time({year = 2000, month = 1, day = 1}) % 2 + os.clock() * 0
n = n + #os.date("%Y-%m-%d %H:%M:%S") + #os.date("!%c", 0) + os.date("*t").year % 2
n = n + os.difftime(2, 1) + #(os.getenv("HOME") or "")
local f = io.tmpfile()
f:setvbuf("full")
f:write("12 3.5\nline\n", 42, "\n")
f:seek("set")
n = n + f:read("n") + f:read("n") + #f:read("L") + #f:read("l") + #f:read("a")
f:close()
local tmp = os.tmpname()
local fh = assert(io.open(tmp, "w")); fh:write("a\nb\n"); fh:close()
for l in io.lines(tmp) do n = n + #l end
fh = assert(io.open(tmp)); for l in fh:lines("L") do n = n + #l end; fh:close()
os.remove(tmp)
io.write("") io.stdout:flush()

-- packages and the debug library
package.preload.trainmod = function(name) return {name = name} end
n = n + #require("trainmod").name + #package.path // 1
n = n + debug.getinfo(1, "l").currentline + #debug.traceback("t")
n = n + #(debug.getlocal(1, 1) or "") + #(debug.getupvalue(cnt, 1) or "")
debug.sethook(function() n = n + 1 end, "", 100)
for i = 1, 1000 do n = n + i % 2 end
debug.sethook()

-- the collector
collectgarbage("generational"); collectgarbage(); collectgarbage("incremental")
collectgarbage("step"); n = n + collectgarbage("count") * 0

print("train", n)
