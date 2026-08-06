-- tests/deep/numbers.lua
-- Client-side number parsing and conversion: hex floats, decimal fraction
-- and exponent scanning, locale decimal points, and number/string round
-- trips. Stock Lua 5.5 behavior is the reference for every assertion here.

-- hex float literals and tonumber: binary exponents
do
  assert(tonumber("0x1p-4") == 0.0625)
  assert(tonumber("0x1p4") == 16.0)
  assert(tonumber("0X1P4") == 16.0)     -- case-insensitive prefix/exponent
  assert(tonumber("0x1p+4") == 16.0)    -- explicit positive exponent
  assert(tonumber("0x.8") == 0.5)       -- fraction only
  assert(tonumber("0x8.") == 8.0)       -- trailing point
  assert(tonumber("0xA.8p1") == 21.0)   -- integer + fraction + exponent
  assert(tonumber("0xa.8P-1") == 5.25)
  assert(tonumber("  0x.8  ") == 0.5)   -- surrounding spaces accepted
end

-- hex float literals straight from the lexer
do
  assert(0x1p-4 == 0.0625)
  assert(0x.8 == 0.5)
  assert(0xA.8p1 == 21.0)
  assert(0x10 == 16)          -- plain hex integers stay integers
  assert(math.type(0x10) == "integer")
  assert(math.type(0x1p0) == "float")
end

-- decimal fraction scanning: zeros in the fraction and exponents
do
  assert(tonumber("1.500e-3") == 0.0015)
  assert(tonumber("0.000125") == 0.000125)
  assert(tonumber("1.000000000000") == 1.0)
  assert(tonumber("0.0000000000000001") == 1e-16)
  assert(tonumber("2.5E+2") == 250.0)
  assert(tonumber("2.5E2") == 250.0)
  assert(tonumber(".5") == 0.5)
  assert(tonumber("5.") == 5.0)
  assert(tonumber("1e-3") == 0.001)
end

-- non-numbers stay nil (sanity for the scanner paths above)
do
  assert(tonumber("0x") == nil)
  assert(tonumber("0xp4") == nil)
  assert(tonumber("1e") == nil)
  assert(tonumber("1.2.3") == nil)
  assert(tonumber("") == nil)
end

-- number -> string -> number round trips
do
  for _, n in ipairs({0.0625, 1e-16, 250.0, 21.0, 5.25, -0.0015}) do
    assert(tonumber(tostring(n)) == n)
  end
  assert(tostring(0.5):find("0%.5") ~= nil)
end

-- locale decimal point: parsing and formatting follow the locale
do
  local locales = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "pt_BR"}
  local set = nil
  for _, loc in ipairs(locales) do
    if os.setlocale(loc, "numeric") then set = loc; break end
  end
  if set then
    -- in a comma locale the comma is the decimal point ...
    assert(tonumber("0,5") == 0.5)
    assert(tostring(0.5):find(",") ~= nil)
    -- ... and formatted numbers round-trip through the locale point
    assert(tonumber(tostring(2.25)) == 2.25)
    os.setlocale("C", "numeric")
  end
  -- back in the C locale, standard behavior holds
  assert(tonumber("0.5") == 0.5)
  assert(tonumber("0,5") == nil)
end

-- long chunk names are truncated with "..." in error messages
do
  local longname = string.rep("x", 300)
  local f, err = load("this is not lua", longname)
  assert(f == nil)
  assert(err:find("%.%.%.") ~= nil)   -- name shortened with ...
  assert(#err < 300 + 100)            -- message is actually shortened

  -- @-style (file-like) source names truncate keeping the tail
  local f2, err2 = load("also not lua ((", "@" .. string.rep("d/", 150) .. "chunk.lua")
  assert(f2 == nil)
  assert(err2:find("%.%.%.") ~= nil)
  assert(err2:find("chunk%.lua") ~= nil) -- tail of the path is preserved
end
