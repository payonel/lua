-- string.format stopped at a value's '__tostring' keeps its state in a box
-- (a luaL_Box, kind "box"): where the item begins in the format string
-- (FMT_ITEM) and the argument it is at (FMT_ARG), as two of the box's
-- integers. 'formatk', the continuation that reads the box, must bound both
-- before it uses FMT_ITEM to index the format string and FMT_ARG as a stack
-- slot; the other continuations (gsub, find) check theirs. These craft a
-- stopped format's stream with each out of range: going on must fail cleanly.
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
for _, name in ipairs{"pcall", "xpcall", "error", "select", "tostring", "tonumber", "ipairs", "pairs", "next",
                      "type", "setmetatable", "getmetatable", "rawget", "rawset", "assert", "print"} do
  perms[_G[name]] = "base." .. name
end
local ncb = 0
local function permcb(f) ncb = ncb + 1; perms[f] = "cb" .. ncb; return f end

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

-- a format stopped in a '__tostring': find its box (the only luaL_Box, kind
-- "box" with five integers) and parse its integer section
local function frozen_format()
  for k = 1, 120 do
    local n = 0
    local mt = {__tostring = function() local x = 0 for _ = 1, 5 do x = x + 1 end return "V" end}
    perms[mt] = "mt" .. k
    local co = coroutine.create(function()
      local o = setmetatable({}, mt)
      return string.format("[%s]", o)
    end)
    debug.setbudget(co, 3, permcb(function() n = n + 1; if n == k then debug.freeze() end end))
    assert(coroutine.resume(co))
    if debug.isfrozen(co) then
      local blob = styx.persist(perms, co)
      local i = 1
      while true do
        i = blob:find("box", i, true)
        if not i then break end
        if blob:byte(i - 1) == 0x06 then -- var(3): the name "box"
          local ok, box = pcall(function()
            local p = i + 3
            local _; _, p = readvar(blob, p) -- objsize
            _, p = readvar(blob, p)          -- user values
            local nints; nints, p = readvar(blob, p)
            assert(nints == 5)               -- a luaL_Box persists five integers
            local ints_pos = p
            for _ = 1, 5 do local x; x, p = readvar(blob, p) end
            return {ints_pos = ints_pos, rest = p}
          end)
          if ok then return blob, box end
        end
        i = i + 1
      end
    end
  end
  error("no frozen format box was produced")
end

local FMT_ITEM, FMT_ARG = 1, 2 -- the integers, 1-based (box->ints[0], [1])

-- the box with integer 'which' set to 'v', the rest of the stream kept
local function with_int(blob, box, which, v)
  local p = box.ints_pos
  local enc = {}
  for j = 1, 5 do
    local x; x, p = readvar(blob, p)
    enc[j] = writevar(j == which and v or x)
  end
  return blob:sub(1, box.ints_pos - 1) .. table.concat(enc) .. blob:sub(box.rest)
end

local function goes_on_cleanly(blob)
  local r = styx.unpersist(H.invert(perms), blob)
  local ok, err = debug.thaw(r)
  return ok, err
end

local function refused(which, v)
  local blob, box = frozen_format()
  local ok, err = goes_on_cleanly(with_int(blob, box, which, v))
  assert(not ok and tostring(err):find("invalid state of a stopped format", 1, true),
         "a stopped format with a bad integer must be refused: " .. tostring(ok) .. " " .. tostring(err))
  H.checkheap()
end

case("a stopped format still goes on when its box is sound", function()
  local blob, box = frozen_format()
  assert(box.ints_pos > 0)
  local ok, err = goes_on_cleanly(blob)
  assert(ok, "a sound stopped format thaws: " .. tostring(err))
  H.checkheap()
end)

case("a format item index past the format string is refused", function()
  refused(FMT_ITEM, 1000000)
end)

case("a format argument index out of range is refused", function()
  refused(FMT_ARG, -5)
end)
