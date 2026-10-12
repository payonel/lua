-- tests/deep/patterns.lua
-- Pattern matching edge cases. Stock Lua 5.5 behavior is the reference for
-- every assertion here (the script API must match stock, whatever the C++
-- API does).

-- A back-reference to a position capture ('()'): stock compares the capture
-- length as an unsigned size, so a position capture (length CAP_POSITION,
-- a negative marker) never matches as a back-reference -- the match fails,
-- rather than "succeeding" with a negative length that walks the subject
-- backward or reads it out of bounds.
do
  assert(string.find("abc", "()%1") == nil)
  assert(string.find("abc", "()%1x") == nil)
  assert(("xx"):find("()%1") == nil)
  assert(string.match("abc", "()%1") == nil)
  -- a longer subject: the failed back-reference must not read past either end
  assert(string.find(("a"):rep(20), "()%1") == nil)
  assert(string.find("abcdef", "b()%1") == nil)
  assert(select("#", string.find("abc", "()%1")) == 1) -- just nil, no captures
end

-- Ordinary back-references still work (the fix only changed the comparison).
do
  assert(select(2, string.find("aa", "(a)%1")) == 2)
  local s, e, c = string.find("abcabc", "(abc)%1")
  assert(s == 1 and e == 6 and c == "abc")
  assert(string.match("abab", "(ab)%1") == "ab")
  assert(select(2, string.find("hello hello", "(%a+) %1")) == 11)
  assert(string.find("hello world", "(%a+) %1") == nil) -- no repeat: no match
  local out, n = string.gsub("hello hello there", "(%a+) %1", "<%1>")
  assert(out == "<hello> there" and n == 1)
end

-- Position captures themselves (not back-referenced) are unaffected.
do
  assert(string.match("abc", "()b()") == 2)
  assert(select(3, string.find("hello", "l()")) == 4) -- position capture after 'l'
  local a, b = string.match("abc", "()b()")
  assert(a == 2 and b == 3)
end
