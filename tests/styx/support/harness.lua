-- Shared test harness for the Styx scripts (see docs/styx.md, ADR-14).
-- The host (styx_tests.cpp) runs every script once per GC mode, sets
-- STYX_MODE and STYX_VERBOSE, and reads STYX_RESULT afterwards.
local styx = require "styx"

local H = {}

STYX_RESULT = STYX_RESULT or {cases = 0, failures = 0}
local result = STYX_RESULT

-- engine invariants: the debug build's test library walks the whole heap
function H.checkheap()
  if T then T.checkmemory() end
end

function H.case(name, fn)
  result.cases = result.cases + 1
  H.checkheap()
  local ok, err = xpcall(fn, debug.traceback)
  if ok then
    collectgarbage()
    H.checkheap()
  end
  if ok then
    if STYX_VERBOSE then print("    " .. name .. " ... OK") end
  else
    result.failures = result.failures + 1
    print("    " .. name .. " ... FAIL [" .. tostring(STYX_MODE) .. "]")
    print("      " .. tostring(err):gsub("\n", "\n      "))
  end
end

-- inverse of a permanents table: object -> key becomes key -> object
function H.invert(t)
  local r = {}
  for k, v in pairs(t) do r[v] = k end
  return r
end

local collectable = {table = true, ["function"] = true, thread = true, userdata = true}

-- 'a' and 'b' are the same value graph: same shape, same primitive values,
-- and the same sharing (a bijection between their objects)
local function same(a, b, st, where)
  local ta = type(a)
  if ta ~= type(b) then
    return false, where .. ": " .. ta .. " vs " .. type(b)
  end
  if ta == "number" then
    if math.type(a) ~= math.type(b) then return false, where .. ": number subtype" end
    if a ~= a then
      return b ~= b, where .. ": nan"
    end
    if a ~= b or (a == 0 and 1 / a ~= 1 / b) then
      return false, where .. ": " .. tostring(a) .. " vs " .. tostring(b)
    end
    return true
  end
  if not collectable[ta] then
    return a == b, where .. ": " .. tostring(a) .. " vs " .. tostring(b)
  end
  if st.map[a] ~= nil then
    return st.map[a] == b, where .. ": sharing differs"
  end
  if st.rev[b] ~= nil then
    return false, where .. ": two objects restored as one"
  end
  st.map[a], st.rev[b] = b, a

  if ta == "table" then
    local ok, err = same(getmetatable(a), getmetatable(b), st, where .. "<mt>")
    if not ok then return false, err end
    local na, nb = 0, 0
    for _ in next, b do nb = nb + 1 end
    local pending = {}
    for k, v in next, a do
      na = na + 1
      local kb = k
      if collectable[type(k)] then
        kb = st.map[k]
        if kb == nil then pending[#pending + 1] = k end
      end
      if kb ~= nil then
        if rawget(b, kb) == nil then return false, where .. ": missing key " .. tostring(k) end
        ok, err = same(v, rawget(b, kb), st, where .. "[" .. tostring(k) .. "]")
        if not ok then return false, err end
      end
    end
    for _, k in ipairs(pending) do
      local kb = st.map[k]
      if kb == nil then
        -- match an unmapped object key by structure
        for cand in next, b do
          if type(cand) == type(k) and st.rev[cand] == nil then
            local trial = {map = setmetatable({}, {__index = st.map}),
                           rev = setmetatable({}, {__index = st.rev}), uv = st.uv, ruv = st.ruv}
            if same(k, cand, trial, where .. "<key>") then
              for x, y in pairs(trial.map) do st.map[x] = y end
              for x, y in pairs(trial.rev) do st.rev[x] = y end
              kb = cand
              break
            end
          end
        end
        if kb == nil then return false, where .. ": no match for key " .. tostring(k) end
      end
      ok, err = same(a[k], rawget(b, kb), st, where .. "[" .. tostring(k) .. "]")
      if not ok then return false, err end
    end
    if na ~= nb then return false, where .. ": " .. na .. " entries vs " .. nb end
    return true
  elseif ta == "function" then
    local ia, ib = debug.getinfo(a, "Su"), debug.getinfo(b, "Su")
    if ia.what == "C" or ib.what == "C" then
      return false, where .. ": unmapped C function"
    end
    if string.dump(a) ~= string.dump(b) then
      return false, where .. ": different code"
    end
    for i = 1, ia.nups do
      local ida, idb = debug.upvalueid(a, i), debug.upvalueid(b, i)
      if st.uv[ida] ~= nil or st.ruv[idb] ~= nil then
        if st.uv[ida] ~= idb then return false, where .. ": upvalue " .. i .. " sharing differs" end
      else
        st.uv[ida], st.ruv[idb] = idb, ida
        local _, va = debug.getupvalue(a, i)
        local _, vb = debug.getupvalue(b, i)
        local ok, err = same(va, vb, st, where .. "<up" .. i .. ">")
        if not ok then return false, err end
      end
    end
    return true
  elseif ta == "thread" then
    if coroutine.status(a) ~= coroutine.status(b) then
      return false, where .. ": status " .. coroutine.status(a) .. " vs " .. coroutine.status(b)
    end
    local level = 0
    while true do
      local fa, fb = debug.getinfo(a, level, "fl"), debug.getinfo(b, level, "fl")
      if (fa == nil) ~= (fb == nil) then return false, where .. ": frame count" end
      if fa == nil then break end
      if fa.currentline ~= fb.currentline then return false, where .. ": line at level " .. level end
      local ok, err = same(fa.func, fb.func, st, where .. "<frame" .. level .. ">")
      if not ok then return false, err end
      local i = 1
      while true do
        local na, va = debug.getlocal(a, level, i)
        local nb, vb = debug.getlocal(b, level, i)
        if na ~= nb then return false, where .. ": local " .. i .. " at level " .. level end
        if na == nil then break end
        ok, err = same(va, vb, st, where .. "<frame" .. level .. "." .. na .. ">")
        if not ok then return false, err end
        i = i + 1
      end
      level = level + 1
    end
    return true
  end
  return a == b, where .. ": userdata"
end

-- the restored graph matches the original; 'perms' maps original objects to
-- keys and 'back' maps keys to the objects restored in their place
function H.same(a, b, perms, back)
  local st = {map = {}, rev = {}, uv = {}, ruv = {}}
  for obj, key in pairs(perms or {}) do
    local to = (back or {})[key]
    if to ~= nil then st.map[obj], st.rev[to] = to, obj end
  end
  return same(a, b, st, "root")
end

-- persist and unpersist 'v', checking the heap after each step, the shape
-- of the result, and that persisting the result gives a stream of the same
-- size (persist and unpersist agree on every object)
function H.roundtrip(v, perms, back)
  perms = perms or {}
  back = back or H.invert(perms)
  local blob = styx.persist(perms, v)
  H.checkheap()
  local r = styx.unpersist(back, blob)
  H.checkheap()
  local ok, err = H.same(v, r, perms, back)
  assert(ok, err)
  local again = styx.persist(H.invert(back), r)
  assert(#again == #blob, "re-persisted size " .. #again .. " vs " .. #blob)
  return r, blob
end

-- the stream's variable-length integer: zigzag, 7 bits per byte
function H.varint(n)
  local u = (n << 1) ~ (n >> 63)
  local bytes = {}
  repeat
    local b = u & 0x7F
    u = u >> 7
    if u ~= 0 then b = b | 0x80 end
    bytes[#bytes + 1] = string.char(b)
  until u == 0
  return table.concat(bytes)
end

-- a length as the stream's last bytes: unsigned, 7 bits per byte, written
-- backward (the trailer's length, read from the end)
function H.backvar(n)
  local bytes = {}
  repeat
    local b = n & 0x7F
    n = n >> 7
    if n ~= 0 then b = b | 0x80 end
    table.insert(bytes, 1, string.char(b))
  until n == 0
  return table.concat(bytes)
end

-- the start of every stream of this state: name and version, then the seed
H.header = styx.persist(nil):sub(1, 13)

-- a hand-made stream: 'body' is the value; 'marked' lists reference ids to
-- mark for finalization, 'dead' lists {table id, node, key id} triples
function H.stream(body, marked, dead)
  marked, dead = marked or {}, dead or {}
  local t = {H.varint(#marked)}
  for _ in ipairs(marked) do t[#t + 1] = H.varint(0) end -- (their kinds: tables)
  for _, id in ipairs(marked) do t[#t + 1] = H.varint(id) end
  for _ in ipairs(marked) do t[#t + 1] = H.varint(0) end -- (their owners: the kernel's)
  for _ in ipairs(marked) do t[#t + 1] = H.varint(0) end -- (and none of them due)
  t[#t + 1] = H.varint(#dead)
  for _, d in ipairs(dead) do
    t[#t + 1] = H.varint(d[1]) .. H.varint(d[2]) .. H.varint(d[3])
  end
  local trailer = table.concat(t)
  return H.header .. body .. trailer .. H.backvar(#trailer)
end

-- every proper prefix of a valid stream is rejected with an error
function H.rejects_truncation(blob, back)
  for n = 0, #blob - 1 do
    local ok = pcall(styx.unpersist, back or {}, blob:sub(1, n))
    assert(not ok, "prefix of length " .. n .. " was accepted")
    if n % 64 == 0 then collectgarbage("step") end
  end
  H.checkheap()
end

return H
