# Repro report (HEAD 963b6f8d, linked against obj/release/liblua.a; nothing in /home/payo/code/lua modified)
Build: g++ -O2 -std=c++23 -fno-stack-protector -fno-common -march=native -I/home/payo/code/lua X.cpp obj/release/liblua.a
NOTE: the release build has assert() ENABLED (the F4 assertion below fires in it).

## H2 - REPRODUCED  (h2.cpp)
Host lua_pcall at base level, msgh = Lua function (loop of 10 iterations, returns 'H:'..msg..':'..sum), budget 1 step, freeze L at call `at`, then lua_thaw until not frozen.
- no freeze (at=0): status=2 (ERRRUN), value "H:attempt to call a number value:55"  (right)
- frozen in the handler (at=1..12), callee a number or nil: thaw returns status=0 (LUA_OK), stack has 3 slots, error silently turned into a normal return:
  `pcall number at= 1 frozen=1 thaws=1 status=0 top=3 top-val=H:attempt to call a number value:55`  (same for nil, every at in 1..12)
- callable that errors (error('boom')): NOT affected, status=2 after thaw, same as unfrozen (ci there is not base_ci).
Right behaviour: status 2 with handler result as error object, same as the unfrozen run.

## H3 - stale CIST_ERRH CONFIRMED (via instrumentation), but effect found is benign; no crash (h3.cpp, inst/)
Handler that errors (xpcall(5, function() error('H1') end) inside a coroutine, then a second error / a yield). Recursion of the handler ends in "error in error handling" (stock behaviour); the CIST_ERRH set on the xpcall ci is never cleared.
Instrumented COPY of ldo.cpp (inst/ldo_inst.cpp, prints in the unroll ERRH branch): with NO freeze at all, 3 of 4 programs print
`[INSTR] unroll saw stale/real CIST_ERRH, rethrowing` -> unroll rethrows via luaG_throwerror before finishCcall(finishpcallk).
Observable results are unchanged: `false|error in error handling|false|ok:...E2`, and the second error / yield-resume behave correctly (sweep of freeze points at=0..60 step 3, 4 programs, same output). Only difference: rethrown status is ERRRUN instead of ERRERR (not visible from Lua pcall results). valgrind -q on h2 and h3: no errors. So: bug in code (stale bit, spurious rethrow, needs no freeze), no wrong status/object/crash observed.
The base-level variant (handler error in host pcall of a number, h2 modes 3/4) leaves the stale bit on base_ci, which unroll never examines: status=5 and "error in error handling", same frozen or not.

## F4 - REPRODUCED, worse than suspected (f4.cpp)
5 objects with Lua __gc (20-iteration loop, then prints "gc i done"), budget 1 step set before lua_close, callback calls lua_freeze(L,L) on call `at` (the spender is the finalizer thread, "other").
- control (never freezes, at=1000000): all 5 run: gc 5..gc 1 done, exit 0 (stock-like).
- at=1:   no finalizer finishes, then `f4: lstate.cpp:273: close_state: Assertion `gettotalbytes(g) == cost::Global' failed.` -> abort (exit 134)
- at=70:  `gc 5 done` then freeze in 2nd, then the same assertion abort.
- at=130: gc 5, gc 4 done, then same abort.
So after the freeze the remaining finalizers never run (gc 4..1 missing) and lua_close aborts in close_state (memory not all freed: frozen finalizer thread/objects accounting); with NDEBUG it would presumably be a silent leak. No hang. Stock: all 5 run, clean close. Right behaviour: either run remaining finalizers (thaw/abandon the frozen one then continue) or at least abandon cleanly.
