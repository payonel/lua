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

-- 'debug.setbudget' and 'debug.getbudget' take the running thread when
-- they are given none
do
  debug.setbudget(2, function() end)
  assert(debug.getbudget() == 2 and debug.getbudget(main) == 2)
  debug.setbudget(0)
  assert(debug.getbudget() == nil)
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
  debug.setbudget(U, 10, function(t) seen[t] = (seen[t] or 0) + 1 end)
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
  debug.setbudget(U, 10, function(t) seen[t] = (seen[t] or 0) + 1 end)
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
  debug.setbudget(U, 5, function(t) spenders[t] = true end)
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

-- a thread given a new budget leaves the threads it already made in the
-- old group
do
  local U = coroutine.create(function() end)
  debug.setbudget(U, 7, function() end)
  local old = coroutine.create(function() end) -- made by main: not in the group
  assert(debug.getbudget(old) == nil)
  local c1
  local V = coroutine.create(function()
    c1 = coroutine.create(function() end)
  end)
  debug.setbudget(V, 11, function() end)
  assert(coroutine.resume(V))
  debug.setbudget(V, 13, function() end)
  assert(debug.getbudget(c1) == 11 and debug.getbudget(V) == 13)
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
