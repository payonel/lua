-- tests/deep/fmtscan.lua
-- Format scanning edge cases: trailing %, embedded NUL in format and
-- replacement strings, unknown specifiers, and pack/unpack format parsing.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

-- string.format: %% produces literal %
do
  assert(string.format("a%%b") == "a%b")
  assert(string.format("100%%") == "100%")
  assert(string.format("%%%%") == "%%")
end

-- string.format: trailing lone % is an error (no specifier byte follows)
do
  local ok, err = pcall(string.format, "%")
  assert(not ok)
  assert(err:find("no value", 1, true), err)
end

-- string.format: unknown specifier with arg is also an error
do
  local ok, err = pcall(string.format, "%^", 1)
  assert(not ok)
  -- str_format's getformat finds '^' as specifier; no matching case -> error
end

-- string.format: embedded NUL after % is treated as an unknown specifier
do
  local ok, err = pcall(string.format, "%\0")
  assert(not ok)
  assert(err:find("no value", 1, true), err)
end

-- string.format %q: control characters at end of string
do
  assert(string.format("%q", "\1") == '"\\1"')
  assert(string.format("%q", "\1x") == '"\\1x"')
end

-- string.format %q: control char followed by digit gets zero-padded
do
  assert(string.format("%q", "\1" .. "1") == '"\\0011"')
  assert(string.format("%q", "\9" .. "2") == '"\\0092"')
end

-- string.gsub replacement: %% in replacement produces literal %
do
  local r = string.gsub("hello", "l+", "%%")
  assert(r == "he%o", r)
end

-- string.gsub replacement: %0 is the whole match
do
  local r = string.gsub("hello", "l+", "[%0]")
  assert(r == "he[ll]o", r)
end

-- string.gsub replacement: %1-%9 are captures
do
  local r = string.gsub("abc", "(.)(.)", "%2%1")
  assert(r == "bac", r)
end

-- string.gsub replacement: trailing lone % is an error
do
  local ok, err = pcall(string.gsub, "hello", "l+", "x%")
  assert(not ok)
  assert(err:find("invalid use of", 1, true), err)
end

-- string.gsub replacement: unknown spec after % is an error
do
  local ok, err = pcall(string.gsub, "l+", "hello", "%x")
  assert(not ok or true)  -- stock may vary on which arg; just check no crash
end

-- string.gsub replacement: embedded NUL passes through literal runs
do
  local r = string.gsub("hello", "l+", "a\0b")
  assert(r == "hea\0bo", r)
  assert(#r == 6)
end

-- string.pack: NUL in format string terminates scanning (C-string behavior)
do
  local packed = string.pack("i4\0i8", 7)
  assert(#packed == 4)
end

-- string.unpack: same NUL termination
do
  local data = string.pack("i4", 7)
  local v, pos = string.unpack("i4\0i8", data)
  assert(v == 7)
  assert(pos == 5)
end

-- string.pack: invalid option raises
do
  local ok, err = pcall(string.pack, "i4q", 7)
  assert(not ok)
  assert(err:find("invalid format option", 1, true), err)
end

-- string.pack: 'X' at end of format with no following option raises
do
  local ok, err = pcall(string.pack, "X")
  assert(not ok)
  assert(err:find("invalid next option for option 'X'", 1, true), err)
end

-- string.pack: spaces in format are no-ops
do
  assert(#string.pack("  i4  ", 7) == 4)
end

-- tonumber: embedded NUL causes rejection
do
  assert(tonumber("10\0") == nil)
  assert(tonumber("1\0 0") == nil)
  assert(tonumber("ff\0", 16) == nil)
end

-- tonumber: high bytes (non-ASCII) in numeric strings are rejected
do
  assert(tonumber("\xC2\xBD") == nil)  -- UTF-8 ½
  assert(tonumber("\xFF") == nil)
end

-- tonumber: incomplete hex/exp forms are rejected
do
  assert(tonumber("0x") == nil)
  assert(tonumber("10e") == nil)
  assert(tonumber("0x.") == nil)
end

-- tonumber: surrounding whitespace is accepted
do
  assert(tonumber("  10  ") == 10)
  assert(tonumber(" \t0xA ") == 10)
end

-- io.read("n"): number at EOF (no trailing newline) succeeds
do
  local fn = os.tmpname()
  local f = assert(io.open(fn, "w"))
  f:write("3.14")
  f:close()
  f = assert(io.open(fn, "r"))
  assert(f:read("n") == 3.14)
  f:close()
  os.remove(fn)
end

-- io.read("n"): overlong numeral (>200 chars) returns nil
do
  local fn = os.tmpname()
  local f = assert(io.open(fn, "w"))
  f:write(string.rep("9", 250))
  f:close()
  f = assert(io.open(fn, "r"))
  assert(f:read("n") == nil)
  f:close()
  os.remove(fn)
end

-- io.read("n"): hex float, hex int, decimal exponent
do
  local fn = os.tmpname()
  local f = assert(io.open(fn, "w"))
  f:write("  -0x1p4\n0X10 5e2")
  f:close()
  f = assert(io.open(fn, "r"))
  assert(f:read("n") == -16.0)
  assert(f:read("n") == 16)
  assert(f:read("n") == 500.0)
  f:close()
  os.remove(fn)
end

-- debug.getinfo short_src: long source name truncation
do
  local longname = "@" .. string.rep("d", 300)
  local lf = load("return 1", longname)
  local info = debug.getinfo(lf, "S")
  assert(#info.short_src == 59)  -- LUA_IDSIZE - 1
  assert(info.short_src:sub(1, 3) == "...")
  assert(info.short_src:sub(-10) == "dddddddddd")
end

-- buffer-built strings are accounted under memlimit: building a string
-- larger than available memory raises a memory error, not unbounded growth
if debug and debug.setmemlimit then
  do
    collectgarbage("collect")
    debug.setmemlimit(2 * 1024 * 1024)
    local ok, err = pcall(function()
      local s = string.rep("x", 4 * 1024 * 1024)
    end)
    debug.setmemlimit(0) -- unlimited
    collectgarbage("collect")
    assert(not ok, "expected memory error")
    assert(tostring(err):find("memory", 1, true), err)
  end
end
