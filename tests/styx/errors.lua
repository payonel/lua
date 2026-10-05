-- malformed input and values that cannot be persisted yet raise errors
local styx = require "styx"

local ok, err = pcall(styx.persist, {})
assert(not ok and err:find("cannot persist a table value", 1, true))

assert(not pcall(styx.persist))
assert(not pcall(styx.unpersist))
assert(not pcall(styx.unpersist, 42))
assert(not pcall(styx.unpersist, ""))
assert(not pcall(styx.unpersist, "garbage"))

local good = styx.persist(7)
assert(not pcall(styx.unpersist, good:sub(1, -2)))
assert(not pcall(styx.unpersist, good .. "x"))

-- the optional permanents table comes first and must be a table
assert(styx.unpersist({}, styx.persist({}, 7)) == 7)
assert(not pcall(styx.persist, 1, 7))
