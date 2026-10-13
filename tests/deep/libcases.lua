-- tests/deep/libcases.lua
-- Code that the standard library calls back (metamethods, replacement
-- functions, comparators...), with the results stock Lua 5.5 gives:
-- 'libcalls.lua' checks them, and 'freeze.lua' runs them frozen at every
-- budget point. Each case gets 'busy', which its callbacks call.

local cases = {}
local function case(name, f) cases[#cases + 1] = {name = name, f = f} end
local function mt(fields) return setmetatable({}, fields) end

-- the base library
case("tostring __tostring", function(busy)
  return tostring(mt{__tostring = function() busy() return "ts" end})
end)
case("tostring __tostring, not a string", function(busy)
  return pcall(tostring, mt{__tostring = function() busy() return {} end})
end)
case("ipairs __index", function(busy)
  local t = mt{__index = function(_, i) busy() if i <= 4 then return i * i end end}
  local s = 0
  for i, v in ipairs(t) do s = s + i * v end
  return s
end)
case("ipairs __index, an error", function(busy)
  local t = mt{__index = function(_, i) busy() if i == 3 then error("at 3", 0) end return i end}
  return pcall(function() for _ in ipairs(t) do end end)
end)

-- the string library
case("gsub with a function", function(busy)
  return string.gsub("hello world from lua", "(%w+)", function(w)
    busy()
    if w == "from" then return nil end -- (keeps the match)
    return w:upper() .. #w
  end)
end)
case("gsub with a function, captures and positions", function(busy)
  return string.gsub("k1=v1, k2=v2", "()(%w+)=(%w+)()", function(p1, k, v, p2)
    busy()
    return v .. "=" .. k .. "@" .. p1 .. "-" .. p2
  end)
end)
case("gsub with a table __index", function(busy)
  local t = setmetatable({a = "A"}, {__index = function(_, k) busy() return k == "c" and "C" or false end})
  return string.gsub("abcabc", "%w", t)
end)
case("gsub, anchored and limited", function(busy)
  local r = {}
  r[1], r[2] = string.gsub("aaa", "^a", function() busy() return "b" end)
  r[3], r[4] = string.gsub("aaaa", "a", function() busy() return "c" end, 2)
  r[5], r[6] = string.gsub("xyz", "", function() busy() return "-" end)
  r[7], r[8] = string.gsub("same", "%w+", function() busy() return false end)
  return r
end)
case("gsub, a bad replacement value", function(busy)
  return pcall(string.gsub, "ab", ".", function() busy() return {} end)
end)
case("gsub, an error in the function", function(busy)
  return pcall(string.gsub, "abc", ".", function(c) busy() if c == "b" then error("at b", 0) end end)
end)
case("format %s with __tostring", function(busy)
  local o = mt{__tostring = function() busy() return "obj" end}
  return string.format("[%s|%10s|%-6s|%.2s|%d|%s]", o, o, o, o, 42, "plain")
end)
case("format %s, a long result and a bad one", function(busy)
  local long = mt{__tostring = function() busy() return ("x"):rep(150) end}
  local r = {string.format("%5s%s", long, long)}
  r[2] = pcall(string.format, "%s", mt{__tostring = function() busy() return 1 end})
  r[3] = select(2, pcall(string.format, "%10s", mt{__tostring = function() busy() return "a\0b" end}))
  r[4] = select(2, pcall(string.format, "%s %s", mt{__tostring = function() busy() return "x" end}))
  return r
end)
case("format %q of nil and booleans with __tostring", function(busy)
  local mn, mb = debug.getmetatable(nil), debug.getmetatable(true)
  debug.setmetatable(nil, {__tostring = function() busy() return "NIL" end})
  debug.setmetatable(true, {__tostring = function(b) busy() return b and "YES" or "NO" end})
  local r = string.format("%q %q %q %q", nil, true, false, "s")
  debug.setmetatable(nil, mn)
  debug.setmetatable(true, mb)
  return r
end)
case("string arithmetic, the other operand's metamethod", function(busy)
  local o = mt{__add = function(a, b) busy() return "added" end,
               __idiv = function(a, b) busy() return "idiv" end}
  return {"10" + o, "3" // o, pcall(function() return "x" * {} end)}
end)
case("rep of long results", function(busy)
  busy()
  local a = string.rep("x", 600000)
  local b = string.rep("abc", 100000, ", ")
  local u = string.rep("y", 300000) .. "z"
  local c = string.rep(u, 2, "|")
  local d = string.rep("", 300000, "-")
  return {#a, a == string.rep(string.rep("x", 600), 1000), #b, b:sub(262140, 262150),
          b == string.rep("abc, ", 99999) .. "abc", #c, c:sub(300000, 300003), c:sub(-2),
          #d, d == string.rep("--", 149999) .. "-", string.rep(u, 1) == u}
end)
case("rep of long results, with counts that are a float and a string", function(busy)
  busy()
  local a = string.rep("a", 2^19)
  local b = string.rep("bc", "200000", "-")
  return {#a, a:sub(-2), #b, b:sub(-4)}
end)
case("upper, lower, reverse and format of long strings", function(busy)
  busy()
  local s = string.rep("aB\n\"\\\1z\200", 40000)
  local u = s:upper()
  local f = string.format("%q|%s|%d", s, s, 7)
  local lit = string.format(string.rep("ab%%", 100000))
  return {#u, {u:byte(1, 16)}, {s:lower():byte(-8, -1)}, {s:reverse():byte(1, 9)}, #f, {f:byte(1, 12)},
          {f:byte(-12, -1)}, #lit, lit:sub(-6)}
end)
case("find, utf8, tonumber and concat on long strings", function(busy)
  busy()
  local s = string.rep("x", 600000)
  local u = string.rep("x€ü", 100000)
  local c = table.concat({"a", s, "b"}, ",")
  local c2 = table.concat({"p", "q"}, s)
  return {tostring(s:find("y", 1, true)), (s .. "y"):find("y", 1, true), utf8.len(u), utf8.offset(u, 150000),
          utf8.offset(u, -150000), tonumber(string.rep("1", 300000), 2),
          tonumber(string.rep(" ", 300000) .. "ff", 16), #c, c:sub(-4), #c2, c2:sub(1, 2)}
end)

-- the table library

-- an object over 't' through '__index', '__newindex' and '__len', which
-- log what they do
local function proxy(t, busy, log)
  return setmetatable({}, {
    __index = function(_, k) busy() if log then log[#log + 1] = "r" .. k end return t[k] end,
    __newindex = function(_, k, v) busy() if log then log[#log + 1] = "w" .. k end t[k] = v end,
    __len = function() busy() return #t end,
  })
end

case("insert through metamethods", function(busy)
  local t, log = {10, 20, 30}, {}
  local p = proxy(t, busy, log)
  table.insert(p, 40)
  table.insert(p, 2, 15)
  table.insert(p, 1, 5)
  return {table.concat(t, ","), table.concat(log, " ")}
end)
case("insert errors", function(busy)
  local p = proxy({1, 2}, busy)
  return {select(2, pcall(table.insert, p, 5, "x")), select(2, pcall(table.insert, p, 1, 2, 3)),
          select(2, pcall(table.insert, setmetatable({}, {__len = function() busy() return 1.5 end}), 1))}
end)
case("insert into a long plain table", function(busy)
  local t = {}
  for i = 1, 300 do t[i] = i end
  busy()
  table.insert(t, 1, 0)
  table.insert(t, 150, "mid")
  return {#t, t[1], t[2], t[150], t[151], t[302]}
end)
case("remove through metamethods", function(busy)
  local t, log = {1, 2, 3, 4, 5}, {}
  local p = proxy(t, busy, log)
  local a = table.remove(p)
  local b = table.remove(p, 1)
  local c = table.remove(p, 2)
  local d = table.remove(p, #t + 1)
  return {a, b, c, tostring(d), table.concat(t, ","), table.concat(log, " ")}
end)
case("remove, edges and errors", function(busy)
  local p = proxy({}, busy)
  local r = {tostring(table.remove(p)), tostring(table.remove(p, 0))}
  r[3] = select(2, pcall(table.remove, proxy({1, 2}, busy), 7))
  local t = {}
  for i = 1, 200 do t[i] = i end
  r[4] = table.remove(t, 1)
  r[5] = #t
  r[6] = t[1]
  return r
end)
case("move through metamethods, up and down", function(busy)
  local log = {}
  local src = proxy({1, 2, 3, 4, 5}, busy, log)
  local t = {}
  local dst = proxy(t, busy, log)
  table.move(src, 1, 5, 2, dst)
  local same = {1, 2, 3, 4, 5}
  local p = proxy(same, busy, log)
  table.move(p, 1, 4, 2) -- (overlapping, same table: down)
  return {table.concat(t, ",", 2, 6), table.concat(same, ","), table.concat(log, " ")}
end)
case("move between tables that are equal by __eq", function(busy)
  local eq = function() busy() return true end
  local a = setmetatable({1, 2, 3}, {__eq = eq})
  local b = setmetatable({0, 0, 0}, {__eq = eq})
  table.move(a, 1, 2, 2, b) -- (equal by __eq: moves down)
  local c = setmetatable({0, 0, 0}, {__eq = function() busy() return false end})
  table.move(a, 1, 2, 2, c)
  return {table.concat(b, ","), table.concat(c, ",")}
end)
case("move, long plain and errors", function(busy)
  local a = {}
  for i = 1, 500 do a[i] = i end
  busy()
  local b = table.move(a, 1, 500, 3, {})
  table.move(a, 1, 499, 2) -- (down, in place)
  return {#b, b[3], b[502], a[1], a[2], a[500],
          select(2, pcall(table.move, a, 1, math.maxinteger, 2)),
          select(2, pcall(table.move, a, -1, math.maxinteger, 2))}
end)
case("concat through metamethods", function(busy)
  local log = {}
  local p = proxy({"a", 2, "c", 4.5}, busy, log)
  return {table.concat(p, "-"), table.concat(p, "", 2, 3), table.concat(p, ", ", 3),
          table.concat(p, "x", 3, 2), table.concat(log, " ")}
end)
case("concat errors", function(busy)
  local p = proxy({"a", {}, "c"}, busy)
  return {select(2, pcall(table.concat, p)), select(2, pcall(table.concat, proxy({"a"}, busy), {}))}
end)
case("concat of a long plain table", function(busy)
  local t = {}
  for i = 1, 400 do t[i] = i % 7 end
  busy()
  local s = table.concat(t, ",")
  return {#s, s:sub(1, 20), s:sub(-10), table.concat(t, "", 390), table.concat({}, "x")}
end)
case("unpack through metamethods", function(busy)
  local log = {}
  local p = proxy({1, 2, 3, nil, 5}, busy, log)
  return {table.pack(table.unpack(p)), table.pack(table.unpack(p, 2, 4)),
          table.pack(table.unpack(p, 3, 2)), table.concat(log, " ")}
end)
case("unpack, long and errors", function(busy)
  local t = {}
  for i = 1, 250 do t[i] = i * 2 end
  busy()
  local r = table.pack(table.unpack(t))
  return {r.n, r[1], r[250], select("#", table.unpack(t, 10, 20)),
          select(2, pcall(table.unpack, t, 1, 1e8)),
          select(2, pcall(table.unpack, t, math.mininteger, math.maxinteger))}
end)
case("sort with a comparator: the comparisons made", function(busy)
  local t = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0, 13, 11, 12, 10, 15, 14}
  local log = {}
  table.sort(t, function(a, b) busy() log[#log + 1] = a .. "<" .. b return a < b end)
  return {table.concat(t, ","), #log, table.concat(log, " ")}
end)
case("sort by __lt", function(busy)
  local m = {__lt = function(a, b) busy() return a.v < b.v end}
  local t = {}
  for i, v in ipairs{4, 1, 3, 9, 2, 8, 5, 7, 6} do t[i] = setmetatable({v = v}, m) end
  table.sort(t)
  local r = {}
  for i = 1, #t do r[i] = t[i].v end
  return table.concat(r, ",")
end)
case("sort through metamethods", function(busy)
  local raw, log = {"d", "b", "e", "a", "c"}, {}
  table.sort(proxy(raw, busy, log))
  return {table.concat(raw, ","), table.concat(log, " ")}
end)
case("sort errors", function(busy)
  local t = {}
  for i = 1, 20 do t[i] = 21 - i end
  local r = {pcall(table.sort, t, function(a, b) busy() return true end)}
  r[3] = select(2, pcall(table.sort, {1, "x", 2}))
  r[4] = select(2, pcall(table.sort, {3, 2, 1}, function(a, b) busy() error("in cmp", 0) end))
  r[5] = select(2, pcall(table.sort, {1, 2}, 3))
  return r
end)
case("sort of a long plain table", function(busy)
  local t = {}
  local x = 7
  for i = 1, 90 do x = (x * 31 + 11) % 1000 t[i] = x end
  busy()
  table.sort(t)
  local ok = true
  for i = 2, #t do ok = ok and t[i - 1] <= t[i] end
  local s = {}
  for i = 1, 60 do s[i] = string.char(65 + (i * 7) % 26) .. i end
  table.sort(s, function(a, b) return a > b end)
  return {ok, t[1], t[90], s[1], s[60]}
end)

-- the os library

case("os.time through metamethods", function(busy)
  local raw, log = {year = 2000, month = 14, day = 31, hour = 25, isdst = false}, {}
  local p = setmetatable({}, {
    __index = function(_, k) busy() log[#log + 1] = "r" .. k return raw[k] end,
    __newindex = function(_, k, v) busy() log[#log + 1] = "w" .. k raw[k] = v end,
  })
  local t = os.time(p)
  local plain = os.time{year = 2000, month = 14, day = 31, hour = 25, isdst = false}
  local fields = {}
  for _, k in ipairs{"year", "month", "day", "hour", "min", "sec", "yday", "wday", "isdst"} do
    fields[#fields + 1] = k .. "=" .. tostring(raw[k])
  end
  return {t - plain, table.concat(fields, " "), table.concat(log, " ")}
end)
case("os.time errors through metamethods", function(busy)
  local function date(fields)
    return setmetatable({}, {__index = function(_, k) busy() return fields[k] end})
  end
  return {select(2, pcall(os.time, date{month = 1, day = 1})),
          select(2, pcall(os.time, date{year = 2000, month = "x", day = 1})),
          select(2, pcall(os.time, date{year = 2000, month = 1, day = 1, hour = 2^40})),
          select(2, pcall(os.time, date{year = 2000, month = 1, day = 1.5}))}
end)

-- the package library

-- runs 'f' with 'package' fields replaced, and restores them
local function withpackage(fields, f)
  local saved = {}
  for k, v in pairs(fields) do saved[k] = package[k] package[k] = v end
  local r = table.pack(pcall(f))
  for k in pairs(fields) do package[k] = saved[k] end
  assert(r[1], r[2])
  return table.unpack(r, 2, r.n)
end

case("require: preload, loaded and the loader's result", function(busy)
  local log = {}
  package.preload["libcases.a"] = function(name, data) busy() log[#log + 1] = name .. "|" .. data return {v = 1} end
  package.preload["libcases.b"] = function() busy() log[#log + 1] = "b" end -- (returns nothing)
  package.preload["libcases.c"] = function(name)
    busy()
    package.loaded[name] = "set by c" -- (and returns nil)
  end
  local a1, d1 = require("libcases.a")
  local a2 = require("libcases.a") -- (loaded)
  local b, db = require("libcases.b")
  local c = require("libcases.c")
  for _, n in ipairs{"a", "b", "c"} do
    package.preload["libcases." .. n], package.loaded["libcases." .. n] = nil, nil
  end
  return {a1.v, d1, a1 == a2, b, db, c, table.concat(log, " ")}
end)
case("require: searchers that are Lua functions", function(busy)
  local log = {}
  local searchers = {
    function(name) busy() log[#log + 1] = "s1 " .. name return "\n\tno luck in s1" end,
    function(name) busy() log[#log + 1] = "s2 " .. name return nil end,
    function(name) busy() log[#log + 1] = "s3 " .. name
      if name == "libcases.found" then
        return function(n, d) busy() return "module " .. n .. " " .. d end, "data3"
      end
      return 42 -- (neither a function nor a string)
    end,
  }
  return withpackage({searchers = searchers}, function()
    local m, d = require("libcases.found")
    local ok, err = pcall(require, "libcases.missing")
    package.loaded["libcases.found"] = nil
    return {m, d, ok, err, table.concat(log, ";")}
  end)
end)
case("require through metamethods of package tables", function(busy)
  local log = {}
  local loaded = package.loaded
  local preload = package.preload
  preload["libcases.p"] = function() busy() return "P" end
  -- LOADED and PRELOAD through __index and __newindex
  local mtl = {__index = function(t, k) busy() log[#log + 1] = "L?" .. k end,
               __newindex = function(t, k, v) busy() log[#log + 1] = "L=" .. k rawset(t, k, v) end}
  local mtp = {__index = function(t, k) busy() log[#log + 1] = "P?" .. k end}
  setmetatable(loaded, mtl)
  setmetatable(preload, mtp)
  local r = {require("libcases.p")}
  r[#r + 1] = withpackage({path = "./?.nolua", cpath = "./?.noso"}, function()
    return select(2, pcall(require, "libcases.none"))
  end)
  setmetatable(loaded, nil)
  setmetatable(preload, nil)
  loaded["libcases.p"], preload["libcases.p"] = nil, nil
  -- 'package.searchers' and 'package.path' through __index of 'package'
  local searchers, path, cpath = package.searchers, package.path, package.cpath
  package.searchers, package.path, package.cpath = nil, nil, nil
  setmetatable(package, {__index = function(t, k)
    busy()
    log[#log + 1] = "pkg." .. k
    if k == "searchers" then return searchers elseif k == "path" then return "./?.nolua"
    elseif k == "cpath" then return "./?.noso" end
  end})
  r[#r + 1] = select(2, pcall(require, "libcases.nowhere"))
  setmetatable(package, nil)
  package.searchers, package.path, package.cpath = searchers, path, cpath
  r[#r + 1] = table.concat(log, " ")
  return r
end)
case("require errors", function(busy)
  busy()
  return withpackage({searchers = 42}, function()
    return {select(2, pcall(require, "libcases.x")), select(2, pcall(require)),
            select(2, pcall(require, {}))}
  end)
end)

-- message handlers (xpcall)

case("xpcall: the handler of an error()", function(busy)
  local function h(m) busy() return "handled: " .. (type(m) == "table" and "a table" or tostring(m)) end
  return {xpcall(error, h, "plain", 0)}, {xpcall(error, h, {})}, {xpcall(error, h)}
end)
case("xpcall: the handler of a runtime error sees the erroring frame", function(busy)
  local function h(m)
    busy()
    local info = debug.getinfo(2, "Sl")
    return (m:gsub("^[^:]*:%d+: ", "")) .. " @" .. info.what .. ":" .. tostring(info.currentline > 0)
  end
  local function bad(t) return t.x.y end
  return {xpcall(bad, h, {})}, {xpcall(function() return 1 + nil end, h)}
end)
case("xpcall: a handler returns nothing, or several values", function(busy)
  local r1 = {xpcall(error, function() busy() end, "x")}
  local r2 = {xpcall(error, function() busy() return "a", "b" end, "x")}
  return r1, r2
end)
case("xpcall: errors in the handler", function(busy)
  local n = 0
  local r1 = {xpcall(error, function(m) busy() n = n + 1 if n < 3 then error("again " .. n, 0) end return "end " .. n end, "x")}
  local r2 = {xpcall(error, function(m) busy() error(m) end, "y")}
  return r1, n, (r2[2]:gsub("^[^:]*:%d+: ", ""))
end)
case("xpcall: nested, and '__close' after the handler", function(busy)
  local log = {}
  local function inner()
    local x <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "close " .. tostring(e) end})
    error("inner", 0)
  end
  local r = {xpcall(inner, function(m)
    busy()
    log[#log + 1] = "handler " .. m
    local ok, v = xpcall(error, function(m2) busy() return "nested " .. m2 end, "deep", 0)
    log[#log + 1] = v
    return "outer " .. m
  end)}
  return r, table.concat(log, "; ")
end)

-- __close handlers run after an error in a protected call, in a call that
-- cannot yield (here, in a sort comparator)
case("pcall in a call that cannot yield: variables closed after an error", function(busy)
  local log = {}
  local function body()
    local x <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "x:" .. tostring(e) end})
    local y <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "y:" .. tostring(e) error("in y", 0) end})
    error("body", 0)
  end
  local n = 0
  table.sort({3, 1, 2}, function(a, b)
    n = n + 1
    if n == 1 then
      local ok, err = pcall(body)
      log[#log + 1] = tostring(ok) .. ":" .. tostring(err)
      local ok2, err2 = xpcall(body, function(m) busy() return "h:" .. tostring(m) end)
      log[#log + 1] = tostring(ok2) .. ":" .. tostring(err2)
      -- handlers that fail in turn (the call stays one that cannot yield)
      local function again()
        local a <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "ga:" .. tostring(e) error("ea", 0) end})
        local b <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "gb:" .. tostring(e) error("eb", 0) end})
        local c <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "gc:" .. tostring(e) error("ec", 0) end})
        return 1
      end
      local ok3, err3 = pcall(again)
      log[#log + 1] = tostring(ok3) .. ":" .. tostring(err3) .. ":" .. tostring(coroutine.isyieldable())
    end
    return a < b
  end)
  return table.concat(log, ",")
end)

-- closing coroutines
case("coroutine.close and coroutine.wrap: variables closed", function(busy)
  local log = {}
  local function mk(fail)
    return coroutine.create(function()
      local a <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "a:" .. tostring(e) end})
      local b <close> = setmetatable({}, {__close = function(_, e)
        busy()
        log[#log + 1] = "b:" .. tostring(e)
        if fail then error("b failed", 0) end
      end})
      coroutine.yield(1)
    end)
  end
  local c1 = mk(false)
  coroutine.resume(c1)
  local r1 = {coroutine.close(c1)}
  local c2 = mk(true)
  coroutine.resume(c2)
  local r2 = {coroutine.close(c2)}
  local w = coroutine.wrap(function()
    local x <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "w:" .. tostring(e) end})
    error("wrap err", 0)
  end)
  local r3 = {pcall(w)}
  return r1, r2, r3, table.concat(log, ",")
end)

case("coroutine.close of the running coroutine", function(busy)
  local log = {}
  local co
  co = coroutine.create(function()
    local a <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "a:" .. tostring(e) end})
    local b <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "b:" .. tostring(e) end})
    coroutine.close(co)
    log[#log + 1] = "not reached"
  end)
  local r = {coroutine.resume(co)}
  local co2 = coroutine.wrap(function()
    local c <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "c:" .. tostring(e) error("in c", 0) end})
    local d <close> = setmetatable({}, {__close = function(_, e) busy() log[#log + 1] = "d:" .. tostring(e) end})
    coroutine.close(coroutine.running())
  end)
  local r2 = {pcall(co2)}
  return r, r2, table.concat(log, ","), coroutine.status(co)
end)

-- debug hooks (the hook function runs with the hooked function at level 2)
case("debug hooks: call and return events", function(busy)
  local log = {}
  local function hook(ev) busy() log[#log + 1] = ev .. ":" .. debug.getinfo(2, "S").what end
  local function v(...) return select("#", ...) end
  local function t(x) return v(x, x) end
  debug.sethook(hook, "cr")
  local a = t(1)
  local b = math.abs(-3)
  local c = string.rep("ab", 2)
  debug.sethook()
  return a, b, c, table.concat(log, ",")
end)
case("debug hooks: a return hook after the variables of the function are closed", function(busy)
  local log = {}
  local function f(...)
    local v <close> = setmetatable({}, {__close = function() busy() log[#log + 1] = "closed" end})
    return "r1", "r2", ...
  end
  debug.sethook(function(ev) busy() log[#log + 1] = ev end, "r")
  local a, b, c = f("x")
  debug.sethook()
  return a, b, c, table.concat(log, ",")
end)
case("debug hooks: line and count events", function(busy)
  local lines, counts = 0, 0
  local function f()
    local s = 0
    for i = 1, 3 do s = s + i end
    return s
  end
  debug.sethook(function(ev) busy() if ev == "line" then lines = lines + 1 else counts = counts + 1 end end, "l", 4)
  local r = f()
  debug.sethook()
  return r, lines, counts
end)
case("debug hooks: return after closing variables, and varargs", function(busy)
  local log = {}
  local function w(...)
    local x <close> = setmetatable({}, {__close = function() busy() log[#log + 1] = "closed" end})
    return ...
  end
  debug.sethook(function(ev) busy() log[#log + 1] = ev end, "cr")
  local a, b, c = w(1, 2, 3)
  debug.sethook()
  return a, b, c, table.concat(log, ",")
end)
case("debug hooks: call and return hooks on yield and resume", function(busy)
  local log = {}
  local co = coroutine.create(function(x)
    local y = coroutine.yield(x + 1)
    return y * 2
  end)
  debug.sethook(co, function(ev) busy() log[#log + 1] = ev end, "cr")
  local r1 = {coroutine.resume(co, 1)}
  local r2 = {coroutine.resume(co, 10)}
  return r1, r2, table.concat(log, ",")
end)
case("debug hooks: an error in a hook function", function(busy)
  local n = 0
  debug.sethook(function(ev) busy() n = n + 1 if n == 3 then error("hook failed", 0) end end, "c")
  local ok, err = pcall(function() local x = math.abs(1) local y = math.abs(2) return x + y end)
  debug.sethook()
  return ok, err, n
end)

-- load with a reader function
case("load with a reader function", function(busy)
  local parts = {"local a = ", "40 ", "+ 2 ", "return a, ", "...", nil}
  local i = 0
  local f = load(function() busy() i = i + 1 return parts[i] end)
  local r = {f("va")}
  return r, i
end)
case("load with a reader function: chunk name, mode and environment", function(busy)
  local i = 0
  local pieces = {"return x", " + y", nil}
  local env = {x = 1, y = 2}
  local f = load(function() busy() i = i + 1 return pieces[i] end, "=name", "t", env)
  local bad = {load(function() busy() return "return +" end, "=bad", "t")}
  local binary = {load(function() busy() return "return 1" end, "=bin", "b")}
  return f(), i, bad[1], bad[2], binary[1], binary[2]
end)
case("load with a reader function: a binary chunk in pieces", function(busy)
  local dumped = string.dump(function(a, b) return a * b + 1 end)
  local pos = 0
  local f = load(function()
    busy()
    if pos >= #dumped then return nil end
    pos = pos + 3
    return dumped:sub(pos - 2, pos)
  end, "=dump", "b")
  return f(6, 7), pos >= #dumped and pos < #dumped + 3
end)
case("load with a reader function: errors", function(busy)
  local n = 0
  local r1 = {load(function() busy() n = n + 1 if n == 2 then error("reader failed", 0) end return "x = " end)}
  local r2 = {load(function() busy() return {} end)}
  local once = false
  local r3 = {load(function() busy() if once then return nil end once = true return 42 end, "=num")}
  return r1[1], r1[2], r2[1], r2[2], r3[1], r3[2]
end)

-- patterns that backtrack (their matcher has check points)
case("patterns that backtrack: find, match, gmatch, gsub", function(busy)
  local subj = string.rep("a", 12) .. "b" .. string.rep("ab", 4) .. "(x(y)z)"
  local t = {}
  for a, b in subj:gmatch("(a-)(b)") do t[#t + 1] = #a .. b end
  return {subj:find("a-a-a-b")}, {subj:find("((a-)(a*)b)(%b())", 3)}, {subj:match("^(a+)(.-)%f[%(](.-)$")},
         {subj:match("(a?a?a?b)()(ab)%3%3")}, table.concat(t, ","), {subj:gsub("(a*)(b?)", "%2%1", 5)},
         {subj:gsub("a-b", function(m) busy() return #m end)}
end)
case("patterns that backtrack: errors", function(busy)
  busy()
  return {pcall(string.match, string.rep("a", 12) .. "b", "a-a-a-b%2")},
         {pcall(string.find, string.rep("a", 205), string.rep("a?", 205) .. "$")}
end)

-- the math library
case("math.min and math.max with '__lt'", function(busy)
  local mt = {__lt = function(a, b) busy() return a.v < b.v end}
  local function o(v) return setmetatable({v = v}, mt) end
  return math.min(o(3), o(1), o(2)).v, math.max(o(3), o(9), o(2)).v, math.min(4, 2, 8), math.max(1.5, 2),
         pcall(math.min, o(1), 5)
end)

return cases
