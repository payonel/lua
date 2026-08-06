-- tests/deep/sort.lua
-- table.sort on large arrays: exercises the quicksort paths that only
-- trigger above the randomized-pivot threshold (intervals > 100 slots),
-- which the core suite's small arrays never reach. Normal-use inputs:
-- random, sorted, reverse-sorted, and duplicate-heavy data.

local N = 2000

local function check_sorted(t, cmp)
  cmp = cmp or function(a, b) return a < b end
  for i = 1, #t - 1 do
    assert(not cmp(t[i + 1], t[i]), "not sorted at index " .. i)
  end
end

-- multiset equality: sorting must preserve the elements
local function counts(t)
  local c = {}
  for _, v in ipairs(t) do c[v] = (c[v] or 0) + 1 end
  return c
end
local function same_counts(a, b)
  for k, v in pairs(a) do assert(b[k] == v) end
  for k, v in pairs(b) do assert(a[k] == v) end
end

-- large random array, default comparator
do
  math.randomseed(20250805) -- deterministic run
  local t = {}
  for i = 1, N do t[i] = math.random(1, N * 10) end
  local before = counts(t)
  table.sort(t)
  check_sorted(t)
  same_counts(before, counts(t))
end

-- already sorted and reverse sorted large inputs
do
  local asc, desc = {}, {}
  for i = 1, N do
    asc[i] = i
    desc[i] = N - i + 1
  end
  table.sort(asc)
  check_sorted(asc)
  table.sort(desc)
  check_sorted(desc)
  for i = 1, N do assert(asc[i] == i and desc[i] == i) end
end

-- duplicate-heavy input (few distinct values across a large array)
do
  math.randomseed(99)
  local t = {}
  for i = 1, N do t[i] = math.random(1, 5) end
  local before = counts(t)
  table.sort(t)
  check_sorted(t)
  same_counts(before, counts(t))
end

-- large array with a custom comparator (descending)
do
  math.randomseed(7)
  local t = {}
  for i = 1, N do t[i] = math.random(1, N) end
  local ge = function(a, b) return a > b end
  local before = counts(t)
  table.sort(t, ge)
  check_sorted(t, ge)
  same_counts(before, counts(t))
end

-- large array of strings, default comparator
do
  math.randomseed(3)
  local t = {}
  for i = 1, N do
    t[i] = string.char(math.random(97, 122), math.random(97, 122),
                       math.random(97, 122))
  end
  table.sort(t)
  check_sorted(t)
end

-- sorting floats and mixed int/float numerics
do
  math.randomseed(11)
  local t = {}
  for i = 1, N do
    if i % 2 == 0 then t[i] = math.random(1, 1000)        -- integer
    else t[i] = math.random(1, 1000) + 0.5 end            -- float
  end
  table.sort(t)
  check_sorted(t)
end
