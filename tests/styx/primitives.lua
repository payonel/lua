-- nil, booleans and numbers survive persist/unpersist exactly
local styx = require "styx"
local H = require "harness"
local case = H.case

local function roundtrip(v)
  return styx.unpersist(styx.persist(v))
end

case("nil and booleans", function()
  assert(roundtrip(nil) == nil)
  assert(roundtrip(true) == true)
  assert(roundtrip(false) == false)
end)

case("integers keep value and subtype", function()
  for _, i in ipairs{0, 1, -1, 42, 255, 256, -256, 1 << 31, 1 << 32, -(1 << 53),
                     math.maxinteger, math.mininteger, math.maxinteger - 1} do
    local r = roundtrip(i)
    assert(r == i and math.type(r) == "integer", tostring(i))
  end
end)

case("floats keep value and subtype", function()
  for _, f in ipairs{0.0, 1.0, 3.5, -2.25, 0.1, 1e300, -1e-300, 2^53, 2^63,
                     math.huge, -math.huge, math.pi, 4.9e-324, 2.2250738585072014e-308} do
    local r = roundtrip(f)
    assert(r == f and math.type(r) == "float", tostring(f))
  end
end)

case("integral floats stay floats", function()
  local r = roundtrip(3.0)
  assert(math.type(r) == "float" and r == 3)
end)

case("negative zero keeps its sign", function()
  local nz = roundtrip(-0.0)
  assert(nz == 0 and 1 / nz == -math.huge)
  local pz = roundtrip(0.0)
  assert(1 / pz == math.huge)
end)

case("nan stays nan", function()
  local nan = roundtrip(0/0)
  assert(nan ~= nan)
end)

case("primitives as the only content of a table", function()
  local t = {nil, true, false, 1, 1.5, -0.0}
  H.roundtrip(t)
end)

case("integers take as few bytes as they need", function()
  local header = #styx.persist(nil) - 1
  local function payload(i) return #styx.persist(i) - header - 1 end
  assert(payload(0) == 1 and payload(-1) == 1 and payload(63) == 1 and payload(-64) == 1)
  assert(payload(64) == 2 and payload(8191) == 2)
  assert(payload(math.maxinteger) == 10 and payload(math.mininteger) == 10)
  assert(#styx.persist(("x"):rep(50)) == header + 1 + 1 + 50)
end)

case("streams start with the Styx header", function()
  local blob = styx.persist(1)
  assert(blob:sub(1, 4) == "Styx")
end)

case("persisting is deterministic for primitives", function()
  assert(styx.persist(12345) == styx.persist(12345))
  assert(styx.persist(-0.0) ~= styx.persist(0.0))
end)
