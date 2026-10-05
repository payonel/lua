-- strings and tables: nesting, shared references, cycles, metatables
local styx = require "styx"
local function roundtrip(v) return styx.unpersist(styx.persist(v)) end

assert(roundtrip("") == "")
assert(roundtrip("hello") == "hello")
assert(roundtrip("a\0b") == "a\0b")
assert(roundtrip(("x"):rep(100000)) == ("x"):rep(100000))

local shared = {1, 2, 3}
local t = {a = "x", b = shared, c = shared, [10] = 3.5, nested = {deep = {true, false}}}
t.self = t
setmetatable(t, {__index = function() return 1 end})

local r = roundtrip(t)
assert(r ~= t and r.a == "x")
assert(r.b == r.c and r.b ~= shared and #r.b == 3)
assert(r.self == r and r[10] == 3.5)
assert(r.nested.deep[1] == true and r.nested.deep[2] == false)
assert(getmetatable(r).__index(r, "q") == 1)

-- a string shared between a key and a value stays shared
local s = ("shared"):rep(10)
local r2 = roundtrip({[s] = s})
assert(r2[s] == s)
