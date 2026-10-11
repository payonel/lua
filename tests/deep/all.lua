#!../../lua
-- tests/deep/all.lua
-- Second test bucket: deep client-API surface tests.
--
-- This suite is deliberately separate from tests/all.lua: the core set is
-- used for performance-variance tracking and must not be disturbed. This
-- set goes beyond the upstream tests and pins down the client-visible
-- script API, which must remain literally identical to stock Lua 5.5
-- regardless of how the consumer (C++) API diverges.
--
-- Run from tests/:      ../lua deep/all.lua
-- Run from tests/deep/: ../../lua all.lua
-- Or via makefile:      make test-deep

local version = "Lua 5.5"
if _VERSION ~= version then
  io.stderr:write("deep suite is for ", version, ", not for ", _VERSION, "\n")
  os.exit(1)
end

local tests = {
  "hooks.lua",
  "hookyield.lua",
  "closures.lua",
  "debuglib.lua",
  "coroutines.lua",
  "tbc.lua",
  "errors.lua",
  "metamethods.lua",
  "allocinfo.lua",
  "identity.lua",
  "warn.lua",
  "numbers.lua",
  "loading.lua",
  "sort.lua",
  "fmtscan.lua",
  "yieldability.lua",
  "libcalls.lua",
  "finalizers.lua",
  "weak.lua",
  "freeze.lua",
  "budgetgroups.lua",
  "budgetgc.lua",
  "frozensafety.lua",
  "detached.lua",
}

-- locate our own directory so the runner works from any cwd
local dir = arg and arg[0] and arg[0]:match("^(.*[/\\])") or ""

print("\n\tStarting deep tests")

local start = os.clock()
-- (the freeze gate, 'make gate', runs these with a budget on the main
-- thread: the files that test the budget itself are not run there)
local ownbudget = {["freeze.lua"] = true, ["budgetgroups.lua"] = true, ["budgetgc.lua"] = true,
                   ["frozensafety.lua"] = true, ["detached.lua"] = true}
for _, name in ipairs(tests) do
  io.write("  ", name, " ... ")
  if GATE and ownbudget[name] then
    print("skipped (it sets budgets itself)")
  else
    dofile(dir .. name)
    print("OK")
  end
  collectgarbage()
end

print(string.format("\n\tOK - all deep tests passed (%.2fs)\n", os.clock() - start))
