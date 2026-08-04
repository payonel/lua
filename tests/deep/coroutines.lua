-- tests/deep/coroutines.lua
-- Client-side coroutine surface: lifecycle, value passing, wrap, close,
-- running/isyieldable, error propagation, nesting.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

-- lifecycle: suspended -> running -> suspended -> dead
do
  local co = coroutine.create(function(a)
    assert(coroutine.status(coroutine.running()) == "running")
    local b = coroutine.yield(a + 1)
    return b * 2
  end)
  assert(type(co) == "thread")
  assert(coroutine.status(co) == "suspended")

  local ok, v = coroutine.resume(co, 10)
  assert(ok and v == 11)
  assert(coroutine.status(co) == "suspended")

  ok, v = coroutine.resume(co, 21)
  assert(ok and v == 42)
  assert(coroutine.status(co) == "dead")

  -- resuming a dead coroutine fails without raising
  ok, v = coroutine.resume(co)
  assert(not ok)
  assert(v:find("cannot resume dead coroutine", 1, true))
end

-- value passing: first resume args are function params; later resume
-- args are yield results; yield args are resume results; return values
-- are the final resume results
do
  local co = coroutine.create(function(a, b)
    assert(a == 1 and b == 2)
    local c, d = coroutine.yield(a + b, a - b)
    assert(c == 30 and d == 40)
    local e = coroutine.yield()
    assert(e == 50)
    return "done", c + d
  end)
  local ok, s, t = coroutine.resume(co, 1, 2)
  assert(ok and s == 3 and t == -1)
  ok, s, t = coroutine.resume(co, 30, 40)
  assert(ok and s == nil and t == nil)
  ok, s, t = coroutine.resume(co, 50)
  assert(ok and s == "done" and t == 70)
end

-- running and isyieldable
do
  local main, ismain = coroutine.running()
  assert(type(main) == "thread" and ismain == true)
  assert(not coroutine.isyieldable())

  local co
  co = coroutine.create(function()
    local self, im = coroutine.running()
    assert(self == co and im == false)
    assert(coroutine.isyieldable())
  end)
  assert(coroutine.resume(co))
end

-- status "normal": a coroutine that has resumed another
do
  local outer, inner
  inner = coroutine.create(function()
    assert(coroutine.status(outer) == "normal")
    assert(coroutine.status(inner) == "running")
  end)
  outer = coroutine.create(function()
    local ok, err = coroutine.resume(inner)
    assert(ok, err)
  end)
  assert(coroutine.resume(outer))
end

-- errors inside a coroutine: resume returns false + message; status dead
do
  local co = coroutine.create(function() error("inner failure") end)
  local ok, err = coroutine.resume(co)
  assert(not ok)
  assert(err:find("inner failure", 1, true))
  assert(coroutine.status(co) == "dead")
end

-- pcall inside a coroutine catches; yield works across pcall
do
  local co = coroutine.create(function()
    local ok, err = pcall(function() error("caught") end)
    assert(not ok and err:find("caught"))
    local ok2, v = pcall(function() return coroutine.yield("mid") end)
    assert(ok2 and v == "back")
    return "end"
  end)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "mid")
  ok, v = coroutine.resume(co, "back")
  assert(ok and v == "end")
end

-- wrap: returns values directly, re-raises errors in the caller
do
  local gen = coroutine.wrap(function(a)
    local b = coroutine.yield(a * 2)
    return b + 1
  end)
  assert(gen(5) == 10)
  assert(gen(6) == 7)

  local boom = coroutine.wrap(function() error("wrapped error") end)
  local ok, err = pcall(boom)
  assert(not ok and err:find("wrapped error"))
  -- after the error the wrapped coroutine is dead; calling again raises
  assert(not pcall(boom))
end

-- close: suspended coroutine closes cleanly; dead close is fine;
-- a closed coroutine cannot be resumed
do
  local co = coroutine.create(function()
    coroutine.yield()
    return "never reached"
  end)
  assert(coroutine.resume(co))
  assert(coroutine.close(co) == true)
  assert(coroutine.status(co) == "dead")
  local ok, err = coroutine.resume(co)
  assert(not ok and err:find("dead", 1, true))

  local finished = coroutine.create(function() return 1 end)
  assert(coroutine.resume(finished))
  assert(coroutine.close(finished) == true)

  -- fresh, never-resumed coroutine can be closed too
  local fresh = coroutine.create(function() end)
  assert(coroutine.close(fresh) == true)
end

-- close on a coroutine that died with an error returns false + the error
do
  local co = coroutine.create(function() error("died here") end)
  local ok = coroutine.resume(co)
  assert(not ok)
  local cok, cerr = coroutine.close(co)
  assert(cok == false)
  assert(cerr:find("died here", 1, true))
  -- a second close reports a clean close
  assert(coroutine.close(co) == true)
end

-- closing the running coroutine (or main) raises
do
  assert(not pcall(coroutine.close, coroutine.running()))
  local co
  co = coroutine.create(function()
    local ok = pcall(coroutine.close, co)
    assert(not ok)
  end)
  assert(coroutine.resume(co))
end

-- nested coroutines: yields bind to the innermost coroutine
do
  local inner = coroutine.create(function()
    coroutine.yield("from-inner")
    return "inner-done"
  end)
  local outer = coroutine.create(function()
    local ok, v = coroutine.resume(inner)
    assert(ok and v == "from-inner")
    coroutine.yield("from-outer")
    ok, v = coroutine.resume(inner)
    assert(ok and v == "inner-done")
    return "outer-done"
  end)
  local ok, v = coroutine.resume(outer)
  assert(ok and v == "from-outer")
  ok, v = coroutine.resume(outer)
  assert(ok and v == "outer-done")
end

-- yield across iterator callbacks (for-in over a wrapped generator)
do
  local function range(n)
    return coroutine.wrap(function()
      for i = 1, n do coroutine.yield(i) end
    end)
  end
  local sum = 0
  for i in range(5) do sum = sum + i end
  assert(sum == 15)
end

-- a suspended coroutine's stack is inspectable but isolated: locals do
-- not leak between threads
do
  local co = coroutine.create(function()
    local secret = "thread-local"
    coroutine.yield()
    return secret
  end)
  assert(coroutine.resume(co))
  assert(rawget(_G, "secret") == nil)
  local ok, v = coroutine.resume(co)
  assert(ok and v == "thread-local")
end
