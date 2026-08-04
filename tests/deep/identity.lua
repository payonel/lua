-- tests/deep/identity.lua
-- Client-visible object identity and address reporting: tostring formats
-- and string.format("%p"). This is the script-side view of lua_topointer,
-- including on C functions. Stock Lua 5.5 behavior is the reference.

-- tostring formats carry the type name prefix
do
  assert(tostring(print):find("^function: ") ~= nil)
  assert(tostring(function() end):find("^function: ") ~= nil)
  assert(tostring({}):find("^table: ") ~= nil)
  assert(tostring(coroutine.create(function() end)):find("^thread: ") ~= nil)
end

-- %p: stable per object, distinct across objects
do
  local t = {}
  assert(string.format("%p", t) == string.format("%p", t))
  assert(string.format("%p", t) ~= string.format("%p", {}))
end

-- %p on C functions reports a real, stable, distinct address
-- (regression guard: lua_topointer on a light C function must return the
-- function address, not NULL)
do
  local p_print = string.format("%p", print)
  assert(p_print == string.format("%p", print))     -- stable
  assert(p_print ~= string.format("%p", type))      -- distinct functions differ
  assert(not p_print:find("null"))                  -- a real address
  assert(p_print:find("0x") ~= nil)
end

-- %p on Lua functions and threads
do
  local f = function() end
  local g = function() end
  assert(string.format("%p", f) == string.format("%p", f))
  assert(string.format("%p", f) ~= string.format("%p", g))
  local co = coroutine.create(function() end)
  assert(string.format("%p", co) == string.format("%p", co))
end

-- function values are first-class with reference identity
do
  local function f() end
  local g = f
  assert(f == g)
  assert(rawequal(f, g))
  local t = { [f] = "keyed" }
  assert(t[g] == "keyed") -- function as table key found via the same reference
end

-- distinct closures over distinct upvalues are distinct values
do
  local function make() local n = 0; return function() return n end end
  local c1, c2 = make(), make()
  assert(c1 ~= c2)
end

-- C functions compare equal to themselves across retrievals
do
  assert(print == print)
  assert(_G.print == print)
  local t = { p = print }
  assert(t.p == print)
end
