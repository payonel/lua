-- starve: the longest stretch with no budget point in each operation on long strings and big tables (args: [MB [names...]])
local debug = require "debug"
assert(debug.setbudget, "needs the budget of this Lua")

local mb = tonumber(arg and arg[1]) or 16
local only = {}
for i = 2, arg and #arg or 0 do only[arg[i]] = true end
local S = math.floor(mb * (1 << 20))

local clock = os.clock
local fmt = string.format

local ops = {}
local function op (name, prep, f)
  ops[#ops + 1] = {name = name, prep = prep, f = f}
end

local function str (n, c) return string.rep(c or "x", n) end

op("string.rep, one byte", function () return S end,
   function (n) return #string.rep("x", n) end)
op("string.rep, with a separator", function () return S // 4 end,
   function (n) return #string.rep("abc", n, "|") end)
op("string.rep of a long string", function () return str(S // 2) end,
   function (s) return #string.rep(s, 2) end)
op("string.upper", function () return str(S) end,
   function (s) return #s:upper() end)
op("string.lower", function () return str(S, "X") end,
   function (s) return #s:lower() end)
op("string.reverse", function () return str(S) end,
   function (s) return #s:reverse() end)
op("string.format %s", function () return str(S) end,
   function (s) return #string.format("%s", s) end)
op("string.format %q", function () return str(S) end,
   function (s) return #string.format("%q", s) end)
op("string.sub", function () return str(S) end,
   function (s) return #s:sub(2) end)
op("string.gsub, the rest after a match", function () return str(S) end,
   function (s) return #s:gsub("^x", "y") end)
op("string.find, plain", function () return str(S) end,
   function (s) return s:find("y", 1, true) end)
op("string.pack s", function () return str(S) end,
   function (s) return #string.pack("s", s) end)
op("string.byte, a stack's worth", function () return str(900000) end,
   function (s) return select("#", s:byte(1, -1)) end)
op("utf8.len", function () return str(S) end,
   function (s) return utf8.len(s) end)
op("utf8.offset", function () return str(S) end,
   function (s) return utf8.offset(s, -1) end)
op("table.concat of two long strings", function () local s = str(S // 2) return {s, s} end,
   function (t) return #table.concat(t) end)
op("tonumber", function () return str(S, "1") end,
   function (s) return tonumber(s) end)
op("the .. operator", function () return str(S // 2) end,
   function (s) return #(s .. s) end)
op("the first hash of a long key", function () return str(S) end,
   function (s) local t = {} t[s] = true return t[s] end)
op("< of long strings", function () return {str(S), str(S - 1) .. "y"} end,
   function (t) return t[1] < t[2] end)
op("== of long strings", function () return {str(S), str(S - 1) .. "x"} end,
   function (t) return t[1] == t[2] end)
op("a hash part growing", function () return S // 64 end,
   function (n) local t = {} for i = 1, n do t[i * 2] = i end return #t end)

local function measure (o)
  local input = o.prep()
  collectgarbage()
  collectgarbage()
  local last, longest = 0, 0
  local co = coroutine.create(function (x)
    last = clock()
    local r = o.f(x)
    local now = clock()
    if now - last > longest then longest = now - last end
    return r
  end)
  debug.setbudget(co, 1000, function ()
    local now = clock()
    if now - last > longest then longest = now - last end
    last = now
  end)
  local t = clock()
  local ok, r = coroutine.resume(co, input)
  t = clock() - t
  assert(ok, r)
  return longest, t
end

print(fmt("%d MB", mb))
for _, o in ipairs(ops) do
  if next(only) == nil or only[o.name] then
    local longest, total = measure(o)
    print(fmt("%-40s %9.1f ms longest %9.1f ms in all", o.name, longest * 1000, total * 1000))
  end
end
