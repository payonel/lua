-- malformed input and values that cannot be persisted raise errors
local styx = require "styx"

local ok, err = pcall(styx.persist, io.stdout)
assert(not ok and err:find("cannot persist a userdata value", 1, true))

ok, err = pcall(styx.persist, print)
assert(not ok and err:find("cannot persist a C function", 1, true))

ok, err = pcall(styx.persist, (coroutine.running()))
assert(not ok and err:find("cannot persist a running thread", 1, true))

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

-- permanents: unknown key and wrong type are reported
local blob = styx.persist({[print] = "print"}, print)
ok, err = pcall(styx.unpersist, {}, blob)
assert(not ok and err:find("print", 1, true))
ok, err = pcall(styx.unpersist, {print = 5}, blob)
assert(not ok and err:find("function expected, got number", 1, true))
