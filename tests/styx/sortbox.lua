-- A frozen table.sort's state is persisted as a user object of the kind
-- "table.sort" (ADR-10, version 9): the integers of its state, little-endian
-- 8 bytes each. Its factory ('makesortbox' in ltablib.cpp) must refuse any
-- state the sort could not run safely. These craft a stopped sort's stream
-- with a pending stack at (and past) the limit and go on at a state that
-- pushes: the restore must fail cleanly, not overrun the state array.
local styx = require "styx"
local H = require "harness"
local case = H.case

-- the standard libraries by name (their functions are the host's permanents),
-- as in frozen.lua: enough that a frozen coroutine round trips
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

-- the layout of the state ('enum' in ltablib.cpp) and of its steps
local SO_PC, SO_NINTS, SO_N = 0, 11, 12
local S_SW2 = 19                  -- a step that pushes a pending interval
local SORT_MAXDEPTH, SORT_NINTS = 33, 145

-- integers in the stream: zigzag, 7 bits per byte (lstyx.cpp 'var'/'get_var')
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

local function slot_at(blob, state_start, k) return (string.unpack("<i8", blob, state_start + k * 8)) end

-- the "table.sort" user object: the kind name is var(10) and the bytes, then
-- objsize, user values, the integer count (0) and the byte count (all var),
-- then the state as 8-byte little-endian integers. The name also appears as a
-- permanent's key, so each candidate's fields are checked to be a sort box's
-- (the hash before the name is a variable-length integer, so it is not an
-- anchor).
local function find_box(blob)
  local i = 1
  while true do
    i = blob:find("table.sort", i, true)
    if not i then error("sort box not found in stream") end
    if blob:byte(i - 1) == 0x14 then
      local ok, box = pcall(function()
        local p = i + 10
        local _; _, p = readvar(blob, p) -- objsize
        _, p = readvar(blob, p)          -- user values
        local nints; nints, p = readvar(blob, p)
        local bytelen_pos = p
        local bytelen; bytelen, p = readvar(blob, p)
        local nslots = bytelen // 8
        assert(nints == 0 and bytelen % 8 == 0 and nslots >= 17 and nslots <= SORT_NINTS)
        assert(slot_at(blob, p, SO_NINTS) == nslots and slot_at(blob, p, 10) == 1) -- SO_BOXED
        return {bytelen_pos = bytelen_pos, bytelen = bytelen, state_start = p, nslots = nslots}
      end)
      if ok then return box end
    end
    i = i + 1
  end
end

-- a sort frozen at a budget point that has boxed its state: scan the budget
-- points until a freeze lands inside 'table.sort' and the stream holds its
-- box (how many steps that takes depends on the build, so the box, not just
-- the kind name, is what is looked for; 'first': the budget point to start at)
local function frozen_sort_blob(first)
  for k = first or 20, 200 do
    local n = 0
    local co = coroutine.create(function()
      local t = {}
      for i = 1, 60 do t[i] = (i * 37) % 101 end
      table.sort(t, function(a, b) local x = 0 for _ = 1, 2 do x = x + 1 end return a > b end)
      return t[1]
    end)
    debug.setbudget(co, 7, permcb(function() n = n + 1; if n == k then debug.freeze() end end))
    assert(coroutine.resume(co))
    if debug.isfrozen(co) then
      local blob = styx.persist(perms, co)
      if blob:find("table.sort", 1, true) and pcall(find_box, blob) then
        return blob, find_box(blob)
      end
    end
  end
  error("no frozen sort box was produced")
end

local function slot(blob, box, k) return (string.unpack("<i8", blob, box.state_start + k * 8)) end
local function setslot(blob, box, k, v)
  local off = box.state_start + k * 8
  return blob:sub(1, off - 1) .. string.pack("<i8", v) .. blob:sub(off + 8)
end

-- a sort box with its state replaced by 'slots' (indexed from 0): the byte
-- count grows with it, the rest of the stream is kept
local function with_state(blob, box, slots, n)
  local sb = {}
  for k = 0, n - 1 do sb[#sb + 1] = string.pack("<i8", slots[k] or 0) end
  sb = table.concat(sb)
  return blob:sub(1, box.bytelen_pos - 1) .. writevar(#sb) .. sb .. blob:sub(box.state_start + box.bytelen)
end

local function rejects(blob)
  local ok, err = pcall(styx.unpersist, H.invert(perms), blob)
  return not ok and tostring(err):find("invalid persisted data", 1, true) ~= nil, err
end

case("a legit frozen sort still persists and restores", function()
  local blob = frozen_sort_blob()
  local r = styx.unpersist(H.invert(perms), blob)
  assert(debug.isfrozen(r), "restored sort is frozen")
  while debug.isfrozen(r) do assert((debug.thaw(r))) end
  assert(coroutine.status(r) == "dead")
  H.checkheap()
end)

case("a pending stack at the limit, about to push, is refused", function()
  local blob, box = frozen_sort_blob()
  -- fill the pending stack to SORT_MAXDEPTH with the whole state array
  -- present (SO_NINTS == SORT_NINTS) and go on at a push: the next push would
  -- write past the end of the state array ('SortBox::s')
  local slots = {}
  for k = 0, SORT_NINTS - 1 do slots[k] = (k < box.nslots) and slot(blob, box, k) or 0 end
  for k = 13, SORT_NINTS - 1 do slots[k] = 0 end
  slots[SO_PC] = S_SW2
  slots[SO_NINTS] = SORT_NINTS
  slots[SO_N] = SORT_MAXDEPTH
  local ok, err = rejects(with_state(blob, box, slots, SORT_NINTS))
  assert(ok, "a sort box with n == SORT_MAXDEPTH must be refused: " .. tostring(err))
  H.checkheap()
end)

case("a pending count so large its check overflows is refused", function()
  local blob, box = frozen_sort_blob()
  -- a count whose '4 * n' overflows a signed 64-bit integer: the old check
  -- 'SO_PENDING + 4 * n > SO_NINTS' wrapped and let it through
  local bad = setslot(blob, box, SO_PC, S_SW2)
  bad = setslot(bad, box, SO_N, 1 << 62)
  local ok, err = rejects(bad)
  assert(ok, "a sort box with a huge n must be refused: " .. tostring(err))
  H.checkheap()
end)

case("a sort frozen early persists zeros past its pending entries", function()
  -- the first budget point inside the sort: nothing pushed or popped yet, so
  -- every slot of the pending stack past the live entries was never written
  -- by the sort; it must persist as 0 (not whatever the C stack held), for
  -- the same stream from the same program, run after run
  local blob, box = frozen_sort_blob(1)
  local live = 13 + 4 * slot(blob, box, SO_N) -- SO_PENDING + 4 * SO_N
  for k = live, box.nslots - 1 do
    local v = slot(blob, box, k)
    assert(v == 0, "pending slot " .. k .. " of " .. box.nslots .. " persisted as " .. v)
  end
  H.checkheap()
end)
