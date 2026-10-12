-- tests/deep/metamethods.lua
-- Client-side metamethod/event surface: every metamethod observable from
-- scripts, selection rules, raw* bypasses, and metatable protection.
-- Stock Lua 5.5 behavior is the reference for every assertion here.

-- __index: function form, table form, and chains
do
  local t = setmetatable({}, { __index = function(_, k) return "fn:" .. k end })
  assert(t.foo == "fn:foo")

  local base = { inherited = "yes" }
  local mid = setmetatable({ own = 1 }, { __index = base })
  local leaf = setmetatable({}, { __index = mid })
  assert(leaf.own == 1)
  assert(leaf.inherited == "yes") -- two-level chain
  assert(leaf.missing == nil)

  -- present keys never consult __index
  local hits = 0
  local u = setmetatable({ present = 1 }, { __index = function() hits = hits + 1 end })
  local _ = u.present
  assert(hits == 0)
end

-- __newindex: function form intercepts only absent keys; table form
-- redirects the store; rawset bypasses
do
  local log = {}
  local t = setmetatable({ existing = 1 }, {
    __newindex = function(tt, k, v) log[#log + 1] = k; rawset(tt, k, v) end,
  })
  t.existing = 2 -- present key: direct store, no event
  assert(#log == 0 and t.existing == 2)
  t.fresh = 3
  assert(#log == 1 and log[1] == "fresh" and rawget(t, "fresh") == 3)

  local target = {}
  local proxy = setmetatable({}, { __newindex = target })
  proxy.a = 10
  assert(rawget(proxy, "a") == nil and target.a == 10)

  local sealed = setmetatable({}, { __newindex = function() error("sealed") end })
  rawset(sealed, "x", 1) -- bypasses the metamethod
  assert(sealed.x == 1)
end

do   -- bug since 5.4
  local oldmode = collectgarbage("generational")
  local parent = {}
  parent.__newindex = parent
  collectgarbage()
  local child = setmetatable({}, parent)
  child.__newindex = {x = "hello"}
  collectgarbage("step")
  assert(parent.__newindex.x == "hello")
  collectgarbage(oldmode)
end

-- __call: callable tables, arguments and returns
do
  local t = setmetatable({}, {
    __call = function(self, a, b) return self, a + b end,
  })
  local self, sum = t(3, 4)
  assert(self == t and sum == 7)
  -- non-callable value without __call raises
  assert(not pcall(function() local x = {}; x() end))
end

-- arithmetic metamethods: all operators, operand selection left-then-right
do
  local events = {}
  local mt = {}
  for _, ev in ipairs { "__add", "__sub", "__mul", "__div", "__mod", "__pow", "__idiv" } do
    mt[ev] = function(a, b) events[#events + 1] = ev; return 1000 end
  end
  mt.__unm = function(a) events[#events + 1] = "__unm"; return -1000 end
  local t = setmetatable({}, mt)

  assert(t + 1 == 1000 and t - 1 == 1000 and t * 2 == 1000 and t / 2 == 1000)
  assert(t % 2 == 1000 and t ^ 2 == 1000 and t // 2 == 1000)
  assert(-t == -1000)
  assert(#events == 8)

  -- right operand supplies the metamethod when the left is a number
  events = {}
  assert(2 + t == 1000)
  assert(events[1] == "__add")

  -- arithmetic on plain tables without metamethods raises
  assert(not pcall(function() return {} + 1 end))
end

-- bitwise metamethods
do
  local mt = {}
  local fired = {}
  for _, ev in ipairs { "__band", "__bor", "__bxor", "__shl", "__shr" } do
    mt[ev] = function() fired[#fired + 1] = ev; return 7 end
  end
  mt.__bnot = function() fired[#fired + 1] = "__bnot"; return 7 end
  local t = setmetatable({}, mt)
  assert(t & 1 == 7 and t | 1 == 7 and t ~ 1 == 7 and t << 1 == 7 and t >> 1 == 7 and ~t == 7)
  assert(#fired == 6)
end

-- __concat: fires when either operand is neither string nor number;
-- right-associativity determines evaluation order
do
  local t = setmetatable({}, { __concat = function(a, b) return "C" end })
  assert(t .. "x" == "C")
  assert("x" .. t == "C")
  assert(1 .. t == "C") -- numbers concat natively, so the table drives
  -- strings and numbers concat without events
  assert("a" .. 1 == "a1")
end

-- __len overrides # for tables; rawlen bypasses
do
  local t = setmetatable({ 1, 2, 3 }, { __len = function() return 42 end })
  assert(#t == 42)
  assert(rawlen(t) == 3)
  assert(rawlen("abc") == 3)
end

-- __eq: fires only for raw-unequal operands of the same kind;
-- result is coerced to boolean; rawequal bypasses
do
  local calls = 0
  local mt = { __eq = function(a, b) calls = calls + 1; return "truthy" end }
  local a = setmetatable({}, mt)
  local b = setmetatable({}, mt)
  assert(a == b)          -- metamethod result coerced to true
  assert(calls == 1)
  assert(a == a)          -- raw-equal: no event
  assert(calls == 1)
  assert(not rawequal(a, b))
  assert(calls == 1)      -- rawequal never fires events
  assert(not (a == 42))   -- different types: no event, plain false
  assert(calls == 1)

  local ne = setmetatable({}, { __eq = function() return nil end })
  local ne2 = setmetatable({}, getmetatable(ne))
  assert(not (ne == ne2)) -- nil coerced to false
end

-- __lt and __le
do
  local mt = {
    __lt = function(a, b) return a.v < b.v end,
    __le = function(a, b) return a.v <= b.v end,
  }
  local a = setmetatable({ v = 1 }, mt)
  local b = setmetatable({ v = 2 }, mt)
  assert(a < b)
  assert(not (b < a))
  assert(a <= b and a <= a)
  assert(b > a)  -- '>' is '<' with swapped operands
  assert(b >= a)
  -- comparing tables without metamethods raises
  assert(not pcall(function() return {} < {} end))
  -- mixed number/table comparison uses the table's metamethod... only if
  -- an operand has one; plain mixed comparison raises
  assert(not pcall(function() return {} < 1 end))
end

-- __tostring and __name
do
  local t = setmetatable({}, { __tostring = function(self) return "custom-repr" end })
  assert(tostring(t) == "custom-repr")

  -- __name replaces the type prefix in the default representation
  local named = setmetatable({}, { __name = "MyType" })
  assert(tostring(named):find("^MyType: ") ~= nil)

  -- __tostring returning a non-string raises
  local bad = setmetatable({}, { __tostring = function() return {} end })
  assert(not pcall(tostring, bad))
end

-- __metatable: protects both getmetatable and setmetatable
do
  local t = setmetatable({}, { __metatable = "locked" })
  assert(getmetatable(t) == "locked")
  local ok, err = pcall(setmetatable, t, {})
  assert(not ok)
  assert(err:find("protected metatable", 1, true))
end

-- getmetatable/setmetatable basics: setmetatable returns the table;
-- clearing with nil
do
  local t = {}
  local mt = {}
  assert(setmetatable(t, mt) == t)
  assert(getmetatable(t) == mt)
  setmetatable(t, nil)
  assert(getmetatable(t) == nil)
end

-- strings have a shared metatable with __index == string
do
  local mt = getmetatable("")
  assert(type(mt) == "table")
  assert(mt.__index == string)
  assert(("abc"):upper() == "ABC") -- method syntax rides on it
  -- other primitive types have no metatable by default
  assert(getmetatable(0) == nil)
  assert(getmetatable(true) == nil)
  assert(getmetatable(nil) == nil)
end

-- __gc: finalizers run when the object becomes garbage; a finalized
-- object can be resurrected for the finalizer's duration
do
  local order = {}
  do
    local a = setmetatable({ id = "first" }, { __gc = function(self) order[#order + 1] = self.id end })
    local b = setmetatable({ id = "second" }, { __gc = function(self) order[#order + 1] = self.id end })
  end
  collectgarbage("collect")
  collectgarbage("collect")
  assert(#order == 2)
  -- finalizers run in reverse creation order
  assert(order[1] == "second" and order[2] == "first")

  -- __gc must be present when setmetatable is called to mark the object
  local late = {}
  local mt = {}
  setmetatable(late, mt)
  mt.__gc = function() error("never marked") end -- added too late: ignored
  late = nil
  collectgarbage("collect")
  collectgarbage("collect")
end

-- __mode: weak keys and weak values
do
  local wv = setmetatable({}, { __mode = "v" })
  wv.k = {}
  local wk = setmetatable({}, { __mode = "k" })
  wk[{}] = "orphan-key"
  local anchor = {}
  wk[anchor] = "anchored"
  collectgarbage("collect")
  assert(wv.k == nil)
  assert(wk[anchor] == "anchored")
  local n = 0
  for _ in pairs(wk) do n = n + 1 end
  assert(n == 1) -- the orphaned key entry was dropped
end

-- metamethods stored on the metatable are looked up rawly (no __index
-- chasing on the metatable itself)
do
  local grand = { __add = function() return "grand" end }
  local mt = setmetatable({}, { __index = grand })
  local t = setmetatable({}, mt)
  -- __add is not raw-present in mt, so arithmetic raises despite the chain
  assert(not pcall(function() return t + 1 end))
end
