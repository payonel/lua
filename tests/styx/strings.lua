-- strings: contents, sizes, sharing
local styx = require "styx"
local H = require "harness"
local case = H.case

local function roundtrip(v) return styx.unpersist(styx.persist(v)) end

case("empty and short strings", function()
  assert(roundtrip("") == "")
  assert(roundtrip("hello") == "hello")
  assert(roundtrip("a\0b") == "a\0b")
end)

case("every byte value", function()
  local t = {}
  for i = 0, 255 do t[#t + 1] = string.char(i) end
  local s = table.concat(t)
  assert(roundtrip(s) == s)
end)

case("short/long string boundary", function()
  for n = 30, 50 do
    local s = ("y"):rep(n)
    assert(roundtrip(s) == s)
  end
end)

case("long strings", function()
  local s = ("x"):rep(100000)
  assert(roundtrip(s) == s)
  local s2 = ("0123456789"):rep(50000)
  assert(roundtrip(s2) == s2)
end)

case("strings with utf8 content", function()
  local s = "olá \u{1F600} \u{10FFFF}"
  assert(roundtrip(s) == s)
  assert(utf8.len(roundtrip(s)) == utf8.len(s))
end)

case("restored strings are usable as table keys", function()
  local key = ("k"):rep(64)
  local r = roundtrip(key)
  local t = {[key] = 1}
  assert(t[r] == 1)
end)

case("a string repeated in a table is written once", function()
  local s = ("shared"):rep(100)
  local one = #styx.persist({s})
  local many = #styx.persist({s, s, s, s, s, s, s, s})
  assert(many < one + 8 * 16, "repeat references are small")
end)

case("a string shared between a key and a value", function()
  local s = ("shared"):rep(10)
  local r = roundtrip({[s] = s})
  assert(r[s] == s)
end)
