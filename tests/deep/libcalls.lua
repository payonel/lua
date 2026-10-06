-- tests/deep/libcalls.lua
-- The cases of 'libcases.lua' (code the standard library calls back) give
-- the results of stock Lua 5.5, printed here as values (error positions
-- left out).

local function ser(v)
  local t = type(v)
  if t == "string" then
    v = v:gsub("^[^\n:]*:%d+: ", "") -- (no positions)
    return string.format("%q", v)
  elseif t == "number" then
    if math.type(v) == "integer" then return tostring(v) end
    return string.format("%.17g", v)
  elseif t == "boolean" or t == "nil" then
    return tostring(v)
  elseif t == "table" then
    local parts = {}
    local n = v.n or #v
    for i = 1, n do parts[#parts + 1] = ser(v[i]) end
    if v.n then parts[#parts + 1] = "n = " .. v.n end
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return "<" .. t .. ">"
end

local expected = {
  ["tostring __tostring"] =
    "{true, \"ts\", n = 2}",
  ["tostring __tostring, not a string"] =
    "{true, false, \"'__tostring' must return a string\", n = 3}",
  ["ipairs __index"] =
    "{true, 100, n = 2}",
  ["ipairs __index, an error"] =
    "{true, false, \"at 3\", n = 3}",
  ["gsub with a function"] =
    "{true, \"HELLO5 WORLD5 from LUA3\", 4, n = 3}",
  ["gsub with a function, captures and positions"] =
    "{true, \"v1=k1@1-6, v2=k2@8-13\", 2, n = 3}",
  ["gsub with a table __index"] =
    "{true, \"AbCAbC\", 6, n = 3}",
  ["gsub, anchored and limited"] =
    "{true, {\"baa\", 1, \"ccaa\", 2, \"-x-y-z-\", 4, \"same\", 1}, n = 2}",
  ["gsub, a bad replacement value"] =
    "{true, false, \"invalid replacement value (a table)\", n = 3}",
  ["gsub, an error in the function"] =
    "{true, false, \"at b\", n = 3}",
  ["format %s with __tostring"] =
    "{true, \"[obj|       obj|obj   |ob|42|plain]\", n = 2}",
  ["format %s, a long result and a bad one"] =
    "{true, {\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\", true, \"bad argument #2 to 'string.format' (string contains zeros)\", \"bad argument #3 to 'string.format' (no value)\"}, n = 2}",
  ["format %q of nil and booleans with __tostring"] =
    "{true, \"NIL YES NO \\\"s\\\"\", n = 2}",
  ["string arithmetic, the other operand's metamethod"] =
    "{true, {\"added\", \"idiv\", false, \"attempt to mul a 'string' with a 'table'\"}, n = 2}",
  ["insert through metamethods"] =
    "{true, {\"5,10,15,20,30,40\", \"w4 r4 w5 r3 w4 r2 w3 w2 r5 w6 r4 w5 r3 w4 r2 w3 r1 w2 w1\"}, n = 2}",
  ["insert errors"] =
    "{true, {\"bad argument #2 to 'table.insert' (position out of bounds)\", \"wrong number of arguments to 'insert'\", \"object length is not an integer\"}, n = 2}",
  ["insert into a long plain table"] =
    "{true, {302, 0, 1, \"mid\", 149, 300}, n = 2}",
  ["remove through metamethods"] =
    "{true, {5, 1, 3, \"nil\", \"2,4\", \"r5 w5 r1 r2 w1 r3 w2 r4 w3 w4 r2 r3 w2 w3 r3 w3\"}, n = 2}",
  ["remove, edges and errors"] =
    "{true, {\"nil\", \"nil\", \"bad argument #2 to 'table.remove' (position out of bounds)\", 1, 199, 2}, n = 2}",
  ["move through metamethods, up and down"] =
    "{true, {\"1,2,3,4,5\", \"1,1,2,3,4\", \"r1 w2 r2 w3 r3 w4 r4 w5 r5 w6 r4 w5 r3 w4 r2 w3 r1 w2\"}, n = 2}",
  ["move between tables that are equal by __eq"] =
    "{true, {\"0,1,2\", \"0,1,2\"}, n = 2}",
  ["move, long plain and errors"] =
    "{true, {502, 1, 500, 1, 1, 499, \"bad argument #4 to 'table.move' (destination wrap around)\", \"bad argument #3 to 'table.move' (too many elements to move)\"}, n = 2}",
  ["concat through metamethods"] =
    "{true, {\"a-2-c-4.5\", \"2c\", \"c, 4.5\", \"\", \"r1 r2 r3 r4 r2 r3 r3 r4\"}, n = 2}",
  ["concat errors"] =
    "{true, {\"invalid value (table) at index 2 in table for 'concat'\", \"bad argument #2 to 'table.concat' (string expected, got table)\"}, n = 2}",
  ["concat of a long plain table"] =
    "{true, {799, \"1,2,3,4,5,6,0,1,2,3,\", \",4,5,6,0,1\", \"56012345601\", \"\"}, n = 2}",
  ["unpack through metamethods"] =
    "{true, {{1, 2, 3, n = 3}, {2, 3, nil, n = 3}, {n = 0}, \"r1 r2 r3 r2 r3 r4\"}, n = 2}",
  ["unpack, long and errors"] =
    "{true, {250, 2, 500, 11, \"too many results to unpack\", \"too many results to unpack\"}, n = 2}",
  ["sort with a comparator: the comparisons made"] =
    "{true, {\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\", 54, \"14<5 4<5 3<5 8<5 5<10 5<12 5<11 5<13 5<0 1<5 9<5 5<6 5<15 5<7 5<2 9<5 5<2 2<4 0<2 3<2 2<1 3<2 2<1 4<3 1<0 14<7 13<7 14<13 15<13 13<10 6<13 8<13 9<13 11<13 12<13 15<13 13<12 14<15 12<7 8<7 12<8 10<8 8<9 8<11 8<6 10<8 8<6 6<7 12<11 9<11 10<11 11<11 11<10 10<9\"}, n = 2}",
  ["sort by __lt"] =
    "{true, \"1,2,3,4,5,6,7,8,9\", n = 2}",
  ["sort through metamethods"] =
    "{true, {\"a,b,c,d,e\", \"r1 r5 w1 w5 r3 r1 r5 w3 w5 r3 r4 w3 w4 r2 r3 r4 r3 w4 w4 r1 r3 w1 w3 r2 r1 r3\"}, n = 2}",
  ["sort errors"] =
    "{true, {false, \"invalid order function for sorting\", \"attempt to compare string with number\", \"in cmp\", \"bad argument #2 to 'table.sort' (function expected, got number)\"}, n = 2}",
  ["sort of a long plain table"] =
    "{true, {true, 28, 972, \"Z37\", \"A26\"}, n = 2}",
  ["os.time through metamethods"] =
    "{true, {0, \"year=2001 month=3 day=4 hour=1 min=0 sec=0 yday=63 wday=1 isdst=false\", \"ryear rmonth rday rhour rmin rsec risdst wyear wmonth wday whour wmin wsec wyday wwday wisdst\"}, n = 2}",
  ["os.time errors through metamethods"] =
    "{true, {\"field 'year' missing in date table\", \"field 'month' is not an integer\", \"field 'hour' is out-of-bound\", \"field 'day' is not an integer\"}, n = 2}",
  ["require: preload, loaded and the loader's result"] =
    "{true, {1, \":preload:\", true, true, \":preload:\", \"set by c\", \"libcases.a|:preload: b\"}, n = 2}",
  ["require: searchers that are Lua functions"] =
    "{true, {\"module libcases.found data3\", \"data3\", false, \"module 'libcases.missing' not found:\\\
\\9\\\
\\9no luck in s1\\\
\\00942\", \"s1 libcases.found;s2 libcases.found;s3 libcases.found;s1 libcases.missing;s2 libcases.missing;s3 libcases.missing\"}, n = 2}",
  ["require through metamethods of package tables"] =
    "{true, {\"P\", \":preload:\", \"module 'libcases.none' not found:\\\
\\9no field package.preload['libcases.none']\\\
\\9no file './libcases/none.nolua'\\\
\\9no file './libcases/none.noso'\\\
\\9no file './libcases.noso'\", \"module 'libcases.nowhere' not found:\\\
\\9no field package.preload['libcases.nowhere']\\\
\\9no file './libcases/nowhere.nolua'\\\
\\9no file './libcases/nowhere.noso'\\\
\\9no file './libcases.noso'\", \"L?libcases.p L=libcases.p L?libcases.none P?libcases.none pkg.searchers pkg.path pkg.cpath pkg.cpath\"}, n = 2}",
  ["require errors"] =
    "{true, {\"'package.searchers' must be a table\", \"bad argument #1 to 'require' (string expected, got no value)\", \"bad argument #1 to 'require' (string expected, got table)\"}, n = 2}",
}

local dir = debug.getinfo(1, "S").source:match("^@(.*[/\\])") or ""
local cases = dofile(dir .. "libcases.lua")
local seen = 0
for _, c in ipairs(cases) do
  local co = coroutine.create(c.f)
  local r = table.pack(coroutine.resume(co, function() end))
  local got = ser(r)
  assert(expected[c.name], c.name .. ": no expected result")
  assert(got == expected[c.name],
         c.name .. ":\n  got      " .. got .. "\n  expected " .. expected[c.name])
  seen = seen + 1
end
local n = 0
for _ in pairs(expected) do n = n + 1 end
assert(seen == n)
