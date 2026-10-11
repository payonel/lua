-- tests/deep/weak.lua
-- Weak tables with weak keys (ephemerons): what a collection keeps and what
-- it clears. An entry's value is kept while its key is, and only then, so
-- a chain of entries (each value the key of the next) is kept whole from a
-- key that is held, and goes whole when that key goes: in any order of the
-- entries in the table, across tables, through a weak table reachable only
-- from another one's value, and in both modes of the collector.

-- a permutation of 1..n (the same every run)
local function shuffled(n)
  local p, x = {}, 7
  for i = 1, n do p[i] = i end
  for i = n, 2, -1 do
    x = (x * 1103515245 + 12345) % 2147483648
    local j = x % i + 1
    p[i], p[j] = p[j], p[i]
  end
  return p
end

local function count(t)
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  return n
end

local function weak() return setmetatable({}, {__mode = "k"}) end

-- 'n' keys chained over the tables 'ts' (entry i in table i % #ts + 1),
-- put in a shuffled order; returns the first key
local function chain(n, ts)
  local keys = {}
  for i = 1, n do keys[i] = {} end
  for _, i in ipairs(shuffled(n - 1)) do
    ts[i % #ts + 1][keys[i]] = keys[i + 1]
  end
  return keys[1]
end

for _, mode in ipairs{"incremental", "generational"} do
  collectgarbage(mode)

  -- a chain in one table, held at its first key, then let go
  do
    local t = weak()
    local first = chain(3000, {t})
    collectgarbage()
    assert(count(t) == 2999, mode)
    first = nil
    collectgarbage()
    assert(count(t) == 0, mode)
  end

  -- a chain across three tables
  do
    local ts = {weak(), weak(), weak()}
    local first = chain(3000, ts)
    collectgarbage()
    assert(count(ts[1]) + count(ts[2]) + count(ts[3]) == 2999, mode)
    first = nil
    collectgarbage()
    assert(count(ts[1]) + count(ts[2]) + count(ts[3]) == 0, mode)
  end

  -- a weak table reachable only through another one's value, with a chain
  -- going back and forth between the two
  do
    local outer = weak()
    local key, inner = {}, weak()
    outer[key] = inner
    local keys = {}
    for i = 1, 2000 do keys[i] = {} end
    for _, i in ipairs(shuffled(1999)) do
      local t = (i % 2 == 0) and outer or inner
      t[keys[i]] = keys[i + 1]
    end
    outer[keys[2000]] = "end"
    local first = keys[1]
    keys = nil
    inner = nil
    collectgarbage()
    local held = outer[key]
    assert(held and count(outer) + count(held) == 1 + 2000, mode)
    held, first = nil, nil
    collectgarbage()
    assert(count(outer[key]) == 0 and count(outer) == 1, mode)
    key = nil
    collectgarbage()
    assert(count(outer) == 0, mode)
  end

  -- a value that is the key of its own entry, or of another entry
  do
    local t = weak()
    local a, b = {}, {}
    t[a] = a
    t[b] = a
    a = nil
    collectgarbage()
    assert(count(t) == 2 and t[t[b]] == t[b], mode)
    b = nil
    collectgarbage()
    assert(count(t) == 0, mode)
  end

  -- entries with values that are not objects, and keys that are not
  -- objects, are kept as they are
  do
    local t = weak()
    local k = {}
    t[k], t[1], t.s = "a string", {}, {}
    collectgarbage()
    assert(count(t) == 3 and t[k] == "a string", mode)
    k = nil
    collectgarbage()
    assert(count(t) == 2, mode)
  end
end
collectgarbage("incremental")
