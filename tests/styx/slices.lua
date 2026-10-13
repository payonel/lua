-- a library function stopped between the slices of its work on a long
-- string keeps the bytes it made in a box (a luaL_Box, kind "box") and its
-- arguments on the stack; going on checks that they still describe that
-- work, and a stream crafted to say otherwise is refused
local styx = require "styx"
local H = require "harness"
local case = H.case

local perms = {[_G] = "_G"}
for _, name in ipairs{"string", "table", "math", "os", "io", "coroutine", "debug", "utf8", "package"} do
  local lib = _G[name]
  perms[lib] = "lib:" .. name
  for k, v in pairs(lib) do
    if type(v) == "function" then perms[v] = name .. "." .. k end
  end
end
local ncb = 0
local function permcb(f) ncb = ncb + 1; perms[f] = "cb" .. ncb; return f end

local function writevar(v)
  local u = (v >= 0) and (v << 1) or (((-v - 1) << 1) | 1)
  local out = {}
  repeat
    local c = u & 0x7f
    u = u >> 7
    if u ~= 0 then c = c | 0x80 end
    out[#out + 1] = string.char(c)
  until u == 0
  return table.concat(out)
end

local function readvar(b, p)
  local u, shift = 0, 0
  while true do
    local c = b:byte(p); p = p + 1
    u = u | ((c & 0x7f) << shift)
    if (c & 0x80) == 0 then break end
    shift = shift + 7
  end
  if (u & 1) == 1 then return -((u >> 1) + 1), p else return (u >> 1), p end
end

local TAG_INTEGER = "\3"

local function frozen(work, arg)
  local co = coroutine.create(work)
  local n = 0
  debug.setbudget(co, 1000, permcb(function(user_co, spender)
    if debug.getinfo(spender, 0, "f").func ~= work then
      n = n + 1
      if n == 3 then debug.freeze() end
    end
  end))
  assert(coroutine.resume(co, arg))
  assert(debug.isfrozen(co))
  return styx.persist(perms, co)
end

local function goes_on(blob)
  local r = styx.unpersist(H.invert(perms), blob)
  return debug.thaw(r)
end

local function with_int(blob, from, to)
  local old = TAG_INTEGER .. writevar(from)
  local i = blob:find(old, 1, true)
  assert(i and not blob:find(old, i + 1, true), "the integer once in the stream")
  return blob:sub(1, i - 1) .. TAG_INTEGER .. writevar(to) .. blob:sub(i + #old)
end

local function refused(blob)
  local ok, err = goes_on(blob)
  assert(not ok and tostring(err):find("invalid state of a stopped call", 1, true),
         "a crafted stopped call must be refused: " .. tostring(ok) .. " " .. tostring(err))
  H.checkheap()
end

local REP = 777777
local function rep(x) local s = string.rep("ab", x * 3, ",") return #s, s:sub(-4) end

case("a rep stopped between slices goes on", function()
  local ok, len, tail = goes_on(frozen(rep, REP // 3))
  assert(ok and len == 3 * REP - 1 and tail == "b,ab", tostring(ok) .. " " .. tostring(len))
  H.checkheap()
end)

for _, v in ipairs{2, 0, -5} do
  case("a rep stopped between slices, with a count of " .. v .. ", is refused", function()
    refused(with_int(frozen(rep, REP // 3), REP, v))
  end)
end

local function with_boxint(blob, which, v)
  local i = 1
  while true do
    i = blob:find("box", i, true)
    assert(i, "a box in the stream")
    if blob:byte(i - 1) == 0x06 then
      local ok, p = pcall(function()
        local p = i + 3
        local _, n
        _, p = readvar(blob, p)
        _, p = readvar(blob, p)
        n, p = readvar(blob, p)
        assert(n == 5)
        return p
      end)
      if ok then
        local q = p
        local enc = {}
        for j = 1, 5 do
          local x; x, q = readvar(blob, q)
          enc[j] = writevar(j == which and v or x)
        end
        return blob:sub(1, p - 1) .. table.concat(enc) .. blob:sub(q)
      end
    end
    i = i + 1
  end
end

local M = 3 << 20
local function fmt(s) local r = string.format("<%s>", s) return #r, r:sub(-3) end
local function concat(s) local r = table.concat({"a", s, "b"}, ",") return #r, r:sub(-4) end
local function ulen(s) return utf8.len(s) end
local function uoff(s) return utf8.offset(s, 0, #s) end
local function tonum(s) return tonumber(s, 10) end
local function upper(s) local r = s:upper() return #r, r:sub(-2) end
local inputs = {
  {fmt, string.rep("f", M), {M + 2, "ff>"}, 3, "invalid state of a stopped format"},
  {concat, string.rep("c", M), {M + 4, "cc,b"}, 2},
  {ulen, string.rep("u", M), {M}, 1},
  {uoff, "a" .. string.rep("\x80", M), {1}, 1},
  {tonum, string.rep("1", M), {tonumber(string.rep("1", M), 10)}, 1},
  {upper, string.rep("p", M), {M, "PP"}},
  {function(s) return #string.rep(s, 2.0 ^ 21) end, "q", {1 << 21}},
  {function(s) return #string.rep(s, "2097152", ",") end, "q", {(1 << 22) - 1}},
}
for k, c in ipairs(inputs) do
  local work, arg, expected, int, msg = c[1], c[2], c[3], c[4], c[5]
  case("long work " .. k .. " stopped between slices goes on", function()
    local r = table.pack(goes_on(frozen(work, arg)))
    assert(r[1], tostring(r[2]))
    for j, v in ipairs(expected) do assert(r[j + 1] == v, k .. ": " .. tostring(r[j + 1])) end
    H.checkheap()
  end)
  if int then
    case("long work " .. k .. " stopped with a crafted box is refused", function()
      local ok, err = goes_on(with_boxint(frozen(work, arg), int, 1 << 40))
      assert(not ok and tostring(err):find(msg or "invalid state of a stopped call", 1, true),
             k .. ": " .. tostring(ok) .. " " .. tostring(err))
      H.checkheap()
    end)
  end
end
