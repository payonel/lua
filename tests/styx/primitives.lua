-- nil, booleans and numbers survive persist/unpersist exactly
local styx = require "styx"

local function roundtrip(v)
  return styx.unpersist(styx.persist(v))
end

assert(roundtrip(nil) == nil)
assert(roundtrip(true) == true)
assert(roundtrip(false) == false)

for _, i in ipairs{0, 1, -1, 42, math.maxinteger, math.mininteger} do
  local r = roundtrip(i)
  assert(r == i and math.type(r) == "integer")
end

for _, f in ipairs{0.0, 3.5, -2.25, 1e300, math.huge, -math.huge} do
  local r = roundtrip(f)
  assert(r == f and math.type(r) == "float")
end

local nz = roundtrip(-0.0)
assert(nz == 0 and 1 / nz == -math.huge)

local nan = roundtrip(0/0)
assert(nan ~= nan)
