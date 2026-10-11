-- tests/deep/budgetgroups.lua
-- The budget belongs to a thread (a budget group), not to the VM
-- ('debug.setbudget([co,] n, f)'): threads created by a budgeted thread
-- belong to its group; a resume puts the resumer's group in effect for the
-- run and the thread goes back to its own afterwards; kernel threads have
-- none. Stock Lua 5.5 has none of this (the file returns at once there).

local debug = require "debug"
if not debug.setbudget then return end

local main = coroutine.running()

-- a thread made while a budget is set belongs to its group; one made
-- before, or after the budget is removed, does not
do
  local before = coroutine.create(function() end)
  debug.setbudget(main, 3, function() end)
  local child = coroutine.create(function() for i = 1, 100 do end end)
  local n, f, left, root = debug.getbudget(child)
  assert(n == 3 and type(f) == "function" and root == main)
  assert(debug.getbudget(before) == nil)
  debug.setbudget(main, 0)
  local after = coroutine.create(function() end)
  assert(debug.getbudget(after) == nil)
  assert(debug.getbudget(child) == 3) -- it keeps its group
  assert(debug.getbudget(main) == nil)
end

-- 'debug.setbudget' and 'debug.getbudget' need a thread: the kernel sets
-- the budget of another thread
do
  assert(not pcall(debug.setbudget, 2, function() end))
  assert(not pcall(debug.getbudget))
  debug.setbudget(main, 2, function() end)
  assert(debug.getbudget(main) == 2)
  debug.setbudget(main, 0)
  assert(debug.getbudget(main) == nil)
end

-- the threads of a group share one counter
do
  local U = coroutine.create(function()
    local c = coroutine.create(function() for i = 1, 1000 do end end)
    assert(coroutine.resume(c))
  end)
  debug.setbudget(U, 1 << 40, function() end)
  local _, _, left0 = debug.getbudget(U)
  assert(coroutine.resume(U))
  local _, _, left1 = debug.getbudget(U)
  assert(left0 - left1 > 1000) -- the child's steps are in the same count
end

-- a thread of the kernel (no budget) resumed by a budgeted thread runs
-- under that thread's group for the run, and with none afterwards
do
  local T = coroutine.create(function()
    for i = 1, 50 do end
    coroutine.yield()
    for i = 1, 50 do end
  end)
  local seen = {}
  local U = coroutine.create(function() assert(coroutine.resume(T)) end)
  debug.setbudget(U, 10, function(_, t) seen[t] = (seen[t] or 0) + 1 end)
  assert(coroutine.resume(U))
  local during = seen[T]
  assert(during and during > 0) -- it was counted while U resumed it
  assert(debug.getbudget(T) == nil) -- but it never owned a budget
  assert(coroutine.resume(T)) -- the kernel resumes it: no budget now
  assert(seen[T] == during)
end

-- a thread made by a budgeted thread and resumed by the kernel directly
-- (a task) is budgeted by its own group
do
  local seen, task = {}, nil
  local U = coroutine.create(function()
    task = coroutine.create(function() for i = 1, 50 do end end)
  end)
  debug.setbudget(U, 10, function(_, t) seen[t] = (seen[t] or 0) + 1 end)
  assert(coroutine.resume(U))
  assert(debug.getbudget(task) == 10)
  assert(coroutine.resume(task)) -- resumed by the main thread, which has none
  assert(seen[task] and seen[task] > 0)
end

-- the callback gets the spending thread
do
  local spenders = {}
  local U = coroutine.create(function()
    local c = coroutine.create(function() for i = 1, 30 do end end)
    coroutine.resume(c)
    spenders.c = c
  end)
  debug.setbudget(U, 5, function(_, t) spenders[t] = true end)
  assert(coroutine.resume(U))
  assert(spenders[U] and spenders[spenders.c])
end

-- the root is a weak reference: gone when that thread is collected
do
  local function make()
    local task
    local U = coroutine.create(function()
      task = coroutine.create(function() end)
    end)
    debug.setbudget(U, 100, function() end)
    assert(coroutine.resume(U))
    local _, _, _, root = debug.getbudget(task)
    assert(root == U)
    return task
  end
  local task = make()
  collectgarbage(); collectgarbage()
  local n, _, _, root = debug.getbudget(task)
  assert(n == 100 and root == nil) -- the group lives on in the task
end

-- also when the thread was given its budget again since (the same group,
-- updated), and when the group dies in the same collection as its root
do
  local function make()
    local task
    local U = coroutine.create(function()
      task = coroutine.create(function() end)
    end)
    debug.setbudget(U, 100, function() end)
    assert(coroutine.resume(U))
    debug.setbudget(U, 50, function() end) -- the group of U, updated
    assert(select(4, debug.getbudget(task)) == U)
    return task
  end
  local task = make()
  collectgarbage(); collectgarbage()
  local n, _, _, root = debug.getbudget(task)
  assert(n == 50 and root == nil)
  for i = 1, 20 do -- the root and its group dying together
    local c = coroutine.create(function() end)
    debug.setbudget(c, 10, function() end)
    debug.setbudget(c, 20, function() end)
  end
  collectgarbage(); collectgarbage()
end

-- a thread given its budget again updates its group: the limit and the
-- callback of every thread of the group, the threads it made too, from
-- any of them; the steps start anew (in the group's own callback, when it
-- ends); removing the budget of a thread leaves the others in the group
do
  local U = coroutine.create(function() end)
  debug.setbudget(U, 7, function() end)
  local old = coroutine.create(function() end) -- made by main: not in the group
  assert(debug.getbudget(old) == nil)
  local c1
  local V = coroutine.create(function()
    c1 = coroutine.create(function() end)
  end)
  local f11, f13, f17 = function() end, function() end, function() end
  debug.setbudget(V, 11, f11)
  assert(coroutine.resume(V))
  debug.setbudget(V, 13, f13)
  local n, f, left, root = debug.getbudget(c1)
  assert(n == 13 and f == f13 and left == 13 and root == V)
  assert(debug.getbudget(V) == 13)
  debug.setbudget(c1, 17, f17) -- (from any thread of the group)
  assert(debug.getbudget(V) == 17 and select(2, debug.getbudget(V)) == f17)
  -- in the group's callback: the new limit holds from its end
  local calls, steps = 0, {}
  local W = coroutine.create(function() for i = 1, 200 do end end)
  debug.setbudget(W, 50, function(user_co)
    calls = calls + 1
    if calls == 1 then debug.setbudget(user_co, 20, function() calls = calls + 1 end) end
  end)
  assert(coroutine.resume(W))
  assert(calls > 5 and debug.getbudget(W) == 20, calls)
  -- removing the budget of one thread of a group
  debug.setbudget(c1, 0)
  assert(debug.getbudget(c1) == nil and debug.getbudget(V) == 17)
end

-- the groups and their callbacks are collected like any object
do
  local before = collectgarbage("count")
  for i = 1, 200 do
    local U = coroutine.create(function() end)
    debug.setbudget(U, i, function() end)
  end
  collectgarbage(); collectgarbage()
  assert(collectgarbage("count") < before + 50) -- nothing piles up
end

-- setbudget is immediate: no thread runs without its budget once it is set
do
  -- on a thread that is running, resumed by the kernel
  local n = 0
  local T = coroutine.create(function()
    debug.setbudget(coroutine.running(), 1, function() n = n + 1 end)
    for i = 1, 50 do end
  end)
  assert(coroutine.resume(T))
  assert(n > 10)

  -- on a thread that is waiting for another it resumed (normal)
  local m = 0
  local U
  U = coroutine.create(function()
    local c = coroutine.create(function()
      debug.setbudget(U, 1, function() m = m + 1 end)
    end)
    assert(coroutine.resume(c))
    local before = m
    for i = 1, 50 do end -- U runs with its budget from here
    assert(m > before + 10)
  end)
  assert(coroutine.resume(U))

  -- on a suspended thread: at its next resume
  local k = 0
  local V = coroutine.create(function()
    coroutine.yield()
    for i = 1, 50 do end
  end)
  assert(coroutine.resume(V))
  debug.setbudget(V, 1, function() k = k + 1 end)
  assert(k == 0)
  assert(coroutine.resume(V))
  assert(k > 10)
end

-- many groups alive at once (the table of owner ids grows), each with its own
-- objects that wait for their '__gc'
do
  local cos, ran = {}, 0
  for i = 1, 100 do
    local c = coroutine.create(function()
      setmetatable({}, {__gc = function() ran = ran + 1 end})
      coroutine.yield()
      for _ = 1, 3 do end
    end)
    debug.setbudget(c, i + 1000, function() end)
    assert(coroutine.resume(c))
    cos[i] = c
  end
  for i = 1, 100 do assert(debug.getbudget(cos[i]) == i + 1000) end
  collectgarbage()
  assert(ran == 0) -- (each waits for a thread of its group)
  for i = 1, 100 do assert(coroutine.resume(cos[i])) end
  assert(ran == 100, ran)
  for i = 1, 100 do assert(debug.getbudget(cos[i]) == i + 1000) end
end
