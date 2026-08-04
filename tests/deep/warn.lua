-- tests/deep/warn.lua
-- Client-visible warn() surface. Emission goes to stderr and cannot be
-- captured from a pure script, so this pins down the callable contract:
-- signature, control messages, and error behavior.
-- Stock Lua 5.5 behavior is the reference.

assert(type(warn) == "function")

-- control messages are accepted silently (no error, no output)
warn("@off")
warn("@on")
warn("@off")

-- unknown control messages are ignored without error by the standard
-- lauxlib handler. Debug/test builds (ltests, detected via the T global)
-- install a strict handler that deliberately aborts on unknown controls,
-- so this stock contract can only be asserted without it.
if rawget(_G, "T") == nil then
  warn("@no-such-control")
end

-- with warnings off, emitting does not error (and produces no output)
warn("deep-test warning, should not be visible")

-- multi-argument form: all pieces must be strings; concatenated as one message
warn("part1 ", "part2 ", "part3 (warnings are off; nothing should print)")

-- numbers coerce to strings (stock luaL_checkstring behavior);
-- non-coercible arguments raise
do
  assert(pcall(warn, 42)) -- coerces, no error (warnings are off)
  local ok, err = pcall(warn, "a", {}, "b")
  assert(not ok)
  assert(err:find("string expected", 1, true))
end

-- zero arguments raise (at least one string is required)
do
  assert(not pcall(warn))
end

-- leave warnings in the default state (off)
warn("@off")
