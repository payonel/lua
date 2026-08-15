-- tests/deep/errors.lua
-- Client-side error/throw surface: pcall/xpcall/error/assert, error object
-- types and identity, error levels, message handlers, nested and deep
-- protected-call level matching, propagation through plain frames, rethrow
-- identity, and coroutine error propagation.
-- Stock Lua 5.5 behavior is the reference for every assertion here.
--
-- This suite deliberately pins the behavior that the engine's error-handling
-- mechanism (the protected-call frame matching in luaD_*) must preserve
-- regardless of how errors are represented internally.

-- pcall: success returns true + all results; args are forwarded
do
  local ok, a, b, c = pcall(function(x, y) return x, y, x + y end, 10, 20)
  assert(ok and a == 10 and b == 20 and c == 30)

  -- pcall of a value that returns nothing
  local ok2 = pcall(function() end)
  assert(ok2 == true)
end

-- pcall: an error returns false + the error object
do
  local ok, err = pcall(function() error("boom") end)
  assert(ok == false)
  assert(type(err) == "string" and err:find("boom"))
end

-- error object TYPES are preserved (identity for reference types)
do
  -- string message: position prepended at default level 1
  local ok, err = pcall(function() error("msg") end)
  assert(not ok and err:find(":%d+: msg$"))

  -- table object: passed by identity, NO position added
  local obj = { code = 7 }
  local ok2, err2 = pcall(function() error(obj) end)
  assert(not ok2 and rawequal(err2, obj) and err2.code == 7)

  -- number object: preserved as-is, no position
  local ok3, err3 = pcall(function() error(42) end)
  assert(not ok3 and err3 == 42)

  -- a nil error object is normalized to the "<no error object>" message;
  -- false is NOT normalized (only nil is) and is preserved by identity
  local ok4, err4 = pcall(function() error(nil) end)
  assert(not ok4 and err4 == "<no error object>")
  local ok5, err5 = pcall(function() error(false) end)
  assert(not ok5 and err5 == false)

  -- error() with no argument behaves like error(nil)
  local ok6, err6 = pcall(function() error() end)
  assert(not ok6 and err6 == "<no error object>")
end

-- error levels: position control via the level argument
do
  -- level 0: no position information, message used verbatim
  local ok, err = pcall(function() error("nowhere", 0) end)
  assert(not ok and err == "nowhere")

  -- level 1 (default): points at the function calling error
  local ok1, err1 = pcall(function() error("here") end)
  assert(not ok1 and err1:find(":%d+: here$"))

  -- level 2: points at the caller of the function that called error
  local function raiser() error("blame caller", 2) end
  local ok2, err2 = pcall(function() raiser() end)
  assert(not ok2 and err2:find(":%d+: blame caller$"))
end

-- assert: returns all arguments unchanged when the first is truthy
do
  local a, b, c = assert(1, "two", 3)
  assert(a == 1 and b == "two" and c == 3)
  assert(assert(true) == true)
  -- a false/nil first value with any other type still passes it through
  assert(assert(0) == 0) -- 0 is truthy in Lua
end

-- assert: failure uses the default message or the provided one
do
  local ok, err = pcall(function() assert(false) end)
  assert(not ok and err:find("assertion failed!$"))

  -- custom string message routes through error() -> position prepended
  local ok2, err2 = pcall(function() assert(nil, "custom") end)
  assert(not ok2 and err2:find(":%d+: custom$"))

  -- non-string assert message is preserved by identity, no position
  local m = { reason = "bad" }
  local ok3, err3 = pcall(function() assert(false, m) end)
  assert(not ok3 and rawequal(err3, m))

  local ok4, err4 = pcall(function() assert(nil, 99) end)
  assert(not ok4 and err4 == 99)
end

-- pcall of a non-callable value fails without crashing
do
  local ok, err = pcall(42)
  assert(not ok and err:find("attempt to call"))
end

-- nested pcall: an error is caught by the NEAREST enclosing pcall only;
-- the outer protected call observes success
do
  local outer_ok, outer_val = pcall(function()
    local inner_ok, inner_err = pcall(function() error("inner") end)
    assert(not inner_ok and inner_err:find("inner"))
    return "outer-ok"
  end)
  assert(outer_ok and outer_val == "outer-ok")
end

-- error propagates through several plain (non-protected) frames up to the
-- single enclosing pcall
do
  local function h() error("deep") end
  local function g() h() end
  local function f() g() end
  local ok, err = pcall(f)
  assert(not ok and err:find("deep"))
end

-- deep protected nesting: each level catches its child's error and re-raises
-- its own marker; the chain unwinds exactly one protected level at a time
do
  local function deep(n)
    if n == 0 then error("bottom") end
    local ok, err = pcall(deep, n - 1)
    assert(not ok)
    -- the child either hit bottom (n==1) or re-raised the previous marker
    assert(err:find("bottom") or err:find("mark:" .. (n - 1) .. "$"))
    error("mark:" .. n)
  end
  local ok, err = pcall(deep, 25)
  assert(not ok)
  assert(err:find("mark:25$")) -- top pcall sees the outermost re-raise
end

-- rethrow preserves error-object identity across pcall boundaries
do
  local original = setmetatable({}, { __tostring = function() return "E" end })
  local ok, err = pcall(function()
    local caught_ok, caught = pcall(function() error(original) end)
    assert(not caught_ok and rawequal(caught, original))
    error(caught) -- rethrow the very same object
  end)
  assert(not ok and rawequal(err, original))
end

-- xpcall: the message handler runs at the error point and transforms the
-- error; xpcall returns false + the handler's result; extra args reach f
do
  local ok, transformed = xpcall(function(a, b)
    assert(a == 1 and b == 2)
    error("raw")
  end, function(e)
    return "handled:" .. e
  end, 1, 2)
  assert(ok == false)
  assert(transformed:find("handled:") and transformed:find("raw"))
end

-- xpcall: handler can build a traceback (it runs before the stack unwinds)
do
  local ok, tb = xpcall(function() error("traced") end, debug.traceback)
  assert(ok == false)
  assert(type(tb) == "string")
  assert(tb:find("traced") and tb:find("stack traceback"))
end

-- xpcall: a handler may return a non-string object by identity
do
  local marker = { handled = true }
  local ok, res = xpcall(function() error("x") end, function() return marker end)
  assert(ok == false and rawequal(res, marker))
end

-- xpcall: an error INSIDE the message handler yields the error-in-error
-- handling result (a plain string), not an infinite recursion
do
  local ok, err = xpcall(function() error("first") end, function()
    error("handler blew up")
  end)
  assert(ok == false)
  assert(type(err) == "string" and err:find("error in error handling"))
end

-- coroutine error propagation: an error unwinds the coroutine to the resume
-- boundary (false + message), leaving it dead; it does not reach the caller
-- as a raised error
do
  local co = coroutine.create(function()
    error("co failure")
  end)
  local ok, err = coroutine.resume(co)
  assert(not ok and err:find("co failure"))
  assert(coroutine.status(co) == "dead")
end

-- pcall inside a coroutine catches locally; the coroutine keeps running and
-- can still yield afterwards (the protected frame lives on the thread stack)
do
  local co = coroutine.create(function()
    local ok, err = pcall(function() error("local") end)
    assert(not ok and err:find("local"))
    coroutine.yield("survived")
    return "done"
  end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "survived")
  ok, v = coroutine.resume(co)
  assert(ok and v == "done")
end

-- an error thrown after crossing a yield is caught by a pcall that itself
-- spans the yield
do
  local co = coroutine.create(function()
    local ok, err = pcall(function()
      coroutine.yield("before-error")
      error("after-resume")
    end)
    assert(not ok and err:find("after-resume", 1, true))
    return "recovered"
  end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "before-error")
  ok, v = coroutine.resume(co)
  assert(ok and v == "recovered")
end
