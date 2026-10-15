# Engine freeze-readiness inventory (branch lua-c++, HEAD 963b6f8d)

Scope: engine only (ldo, lvm, lapi, lgc, lstate, ldebug, lfunc, ltm, lzio, lundump, llex, lparser, lstring, ltable, lobject, lmem). Libraries are not covered. Read-only audit, nothing was built or run, so every status below comes from reading the code. Corrected on 2026-10-12 where the engine has changed since (a freeze where a call cannot freeze, hooks, `__gc`, `__close` at host calls and in the closes of threads, readers, the budget callback's errors): those statements describe lua-c++ at 040585f0 and cite its lines; the other line numbers are 963b6f8d's.

## How the freeze work is built (what "READY" is judged against)

- A budget callback runs only at two kinds of point: a Lua-frame instruction (`luaG_traceexec` -> `budgetpoint`, ldebug.cpp:1198-1227) and `lua_checkpointk` in C code (lapi.cpp:1635). `luaE_spend`/`luaE_spent` (lstate.h:641-654) only count and never run the callback. A freeze is `luaD_freeze` (ldo.cpp:432), a `throw LuaError{LUA_FREEZE}`. No protected call catches it. It ends at `endresume` (ldo.cpp:1393), the host's `lua_callk`/`lua_pcallk` (`hostfrozen`), or the host's close of a thread (`lua_closethreadk` with no `from`, `closerun`). A freeze that destroys the chain (`killchain`) unwinds the same way, and the threads it passes die.
- A Lua frame stopped at a budget point gets CIST_FROZE. `unroll` (ldo.cpp:997) then does `ci->savedpc--` for it. A Lua frame stopped inside a metamethod or call is finished by `luaV_finishOp` (lvm.cpp:995), as for a yield. A C frame is finished by `finishCcall` (ldo.cpp:962) through its `k`/`ctx`.
- "Cannot freeze" is the counter `L->nfz`. A `lua_freeze` from the budget callback (ldo.cpp:1945) leaves the callback; when `nfz > 0` on the thread that spent the budget or on any resumer up the chain, or a resume up the chain has no continuation, `luaD_budget` (ldo.cpp:489) finds it (`chaintop`, ldo.cpp:403) and destroys the whole budgeted chain instead of freezing (`killchain`, ldo.cpp:471, error "cannot freeze"). `lua_nofreeze` (ldo.cpp:1958) and `debug.canfreeze` tell the callback beforehand. `nfz` is raised by `lua_callk`/`lua_pcallk` with `k == nullptr` (lapi.cpp:1777, 2001), by `luaT_callTM`/`luaT_callTMres` outside Lua code and outside an armed API call (ltm.cpp:131, 156), by `callclosemethod` when `yy == 0` (lfunc.cpp:151), by `GCTM` around a firmware object's `__gc` (lgc.cpp:1270) and by a plain `lua_closethread` (`luaE_closethreadplain`, lstate.cpp:386); `luaD_hook` no longer raises it. `luaD_rawrunprotected` restores it on catch (ldo.cpp:156).
- Status meanings used below. READY: freezable and resumable, or no user code at all. NOT READY: the path runs user code but cannot freeze. Every such row is guarded by `nfz` (or an equivalent), so a freeze there destroys the chain instead of freezing (`killchain`), and it is not memory-unsafe. For most of these, stock Lua cannot yield there either. UNCLEAR: could not establish it from reading, reason given.
- `dothecall`/`DoTheCall` (lgc.cpp) is how `GCTM` calls a firmware object's `__gc`, as in stock; `RunFinalizer` and the finalizer thread are gone (see F).

## A. Metamethods from VM instructions (Lua frame is `L->ci`)

All of these reach `luaT_callTM`/`luaT_callTMres`. With `isLuacode(L->ci)` those call `luaD_call` (ltm.cpp:125-126, 150-151), which has no nfz++. A freeze inside the metamethod unwinds to the landing and `unroll` re-enters the Lua frame via `luaV_finishOp`.

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| A1 | index get | `luaV_finishget` (`__index` function) | lvm.cpp:319 (ops GETTABUP 1349, GETTABLE 1393, GETI 1428, GETFIELD 1454, SELF 1654) | metamethod from VM | `luaT_callTMres` -> `luaD_call` ltm.cpp:151 | `luaV_finishOp` lvm.cpp:1014-1021 copies result | READY | `__index` table chains loop with no call. |
| A2 | index set | `luaV_finishset` (`__newindex`) | lvm.cpp:382 (SETTABUP 1480, SETTABLE 1533, SETI 1573, SETFIELD 1599) | metamethod from VM | `luaT_callTM` -> `luaD_call` ltm.cpp:126 | `luaV_finishOp` default case, nothing to finish (lvm.cpp:1063) | READY | |
| A3 | arith/bitwise | `luaT_trybinTM`, `trybinassocTM`, `trybiniTM` -> `callbinTM` | lvm.cpp:2177, 2190, 2203 (OP_MMBIN, MMBINI, MMBINK); ltm.cpp:174 | metamethod from VM | `luaT_callTMres` -> `luaD_call` | `luaV_finishOp` lvm.cpp:1004-1010 | READY | |
| A4 | unary | `luaT_trybinTM` for UNM/BNOT | lvm.cpp:2224, 2241 | metamethod from VM | `luaD_call` | `luaV_finishOp` lvm.cpp:1011-1021 | READY | |
| A5 | length | `luaV_objlen` (`__len`) | lvm.cpp:2260 -> lvm.cpp:880 | metamethod from VM | `luaD_call` | `luaV_finishOp` OP_LEN | READY | |
| A6 | concat | `luaV_concat` -> `luaT_tryconcatTM` (`__concat`) | lvm.cpp:2269 -> lvm.cpp:804 -> ltm.cpp:212 | metamethod from VM | `luaD_call` | `luaV_finishOp` OP_CONCAT lvm.cpp:1038-1047 re-enters `luaV_concat` (may freeze again) | READY | Same call also runs from `finishOp` itself (lvm.cpp:1045), still a Lua frame. |
| A7 | equality | `luaV_equalobj` (`__eq`) | lvm.cpp:2309 -> lvm.cpp:734 | metamethod from VM | `luaD_call` | `luaV_finishOp` OP_EQ lvm.cpp:1029-1037 | READY | OP_EQK/EQI cannot call anything. |
| A8 | order | `luaT_callorderTM`, `luaT_callorderiTM` (`__lt`, `__le`) | lvm.cpp:2337 (LT), 2366 (LE), 2433-2523 (LTI, LEI, GTI, GEI); via `luaV_lessthan` 577 and `luaV_lessequal` 602 | metamethod from VM | `luaD_call` | `luaV_finishOp` lvm.cpp:1023-1037 | READY | |
| A9 | `__call` | `tryfuncTM` in `luaD_precall`/`luaD_pretailcall`/`ccall` | ldo.cpp:554 (called at 743, 787); OP_CALL lvm.cpp:3105, OP_TAILCALL 3140 | call of a non-function | none: only looks up and shifts the stack, the metamethod then runs as an ordinary callee | the callee is a normal frame | READY | `__call` is not run inside a nested C call. |
| A10 | Lua->Lua call | OP_CALL / OP_TAILCALL / returns | lvm.cpp:3105-3153 | explicit call | `luaD_precall` reuses the same `luaV_execute` C frame | all state in CallInfo/pc | READY | Lua->C: the C function's own `k` (library). |
| A11 | generic for | OP_TFORPREP (merged with first TFORCALL) and OP_TFORCALL | lvm.cpp:3307, 3331 | explicit call from VM | `luaD_call` | `luaV_finishOp` default; `savedpc` already points at TFORLOOP (lvm.cpp:3306, 3330) | READY | `luaF_newtbcupval` (2826) only looks up `__close`. |
| A12 | `__close` | OP_CLOSE -> `luaF_close(...,1)` | lvm.cpp:2284 -> lfunc.cpp:268 -> `callclosemethod` lfunc.cpp:143 | `__close` | `luaD_call` | `luaV_finishOp` OP_CLOSE lvm.cpp:1048 repeats the instruction | READY | `poptbclist` runs before the call, so the repeat only closes what is left. |
| A13 | `__close` | OP_RETURN with k -> `luaF_close(...,1)` | lvm.cpp:3182 | `__close` | `luaD_call` | `luaV_finishOp` OP_RETURN lvm.cpp:1053 | READY | |

## B. The same metamethods through the C API

Calls with a continuation arm the frame (`armk`, lapi.cpp:895: sets `k`, `ctx`, `apiop`, CIST_APIK, plus CIST_NYCALL when the thread cannot yield). `luaT_callTM*` then uses `luaD_call` or `luaD_callnoyield`, and a freeze unwinds to the landing. `finishCcall` -> `finishapik` (ldo.cpp:883) then finishes the op by `apiop` and calls `k`. Without `k`, `armk` returns false and `luaT_callTM*` takes the nfz++ branch.

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| B1 | get | `lua_gettablek`, `lua_getik`, `lua_getfieldk`, `lua_getfieldik` | lapi.cpp:940-942 (auxgetstr), 993-995 (auxgettable), 1038-1040 (auxgeti) | API metamethod | `luaV_finishget` -> `luaT_callTMres` -> `luaD_call` (or `luaD_callnoyield` + NYCALL) | `armk` + `finishapik` APIK_OVERKEY/APIK_PUSH, `finishCcall` k | READY | |
| B2 | get, no k | `lua_gettable`, `lua_getfield`, `lua_geti`, `lua_getglobal`, `lua_getfieldi`, `lua_getglobali` | same auxget* with `k=nullptr` (lapi.cpp:1003, 1010, 1051, 966-971, 1283) | API metamethod | `luaT_callTMres` ltm.cpp:156-158 | none. Quote: `L->nfz++; luaD_callnoyield(L, func, 1); L->nfz--;` | NOT READY | Safe: a freeze there destroys the chain. Includes `lua_getglobal` on a global table with `__index` (strict-mode style). Same non-yieldability as stock. |
| B3 | set | `lua_settablek`, `lua_setik`, `lua_setfieldk`, `lua_setfieldik` | lapi.cpp:1220-1222 (auxsetstr), 1259-1261 (auxsettable), 1326-1328 (auxseti) | API metamethod | `luaV_finishset` -> `luaT_callTM` | `armk` + `finishapik` APIK_POP2/APIK_POP1 | READY | |
| B4 | set, no k | `lua_settable`, `lua_setfield`, `lua_seti`, `lua_setglobal`, `lua_setfieldi`, `lua_setglobali` | lapi.cpp:1269, 1276, 1336, 1230-1238, 1290 | API metamethod | `luaT_callTM` ltm.cpp:131-133 | none. Quote: `L->nfz++; luaD_callnoyield(L, func, 0); L->nfz--;` | NOT READY | Safe: a freeze there destroys the chain. |
| B5 | compare | `lua_comparek` (`__eq`, `__lt`, `__le`) | lapi.cpp:1752-1768 -> `lua_compare` 396-402 | API metamethod | `luaV_equalobj`/`lessthan`/`lessequal` -> `luaT_callTMres` | `armk(APIK_BOOL)` only when a metamethod may run (`mayrun`), `finishapik` converts the result to a boolean | READY | |
| B6 | compare, no k | `lua_compare` | lapi.cpp:385-410 | API metamethod | same | none (nfz++ branch) | NOT READY | Safe: a freeze there destroys the chain. |
| B7 | arith | `lua_arithk` | lapi.cpp:1779-1784 -> `luaO_arith` lobject.cpp:267 -> `luaT_trybinTM` | API metamethod | `luaT_callTMres` | `armk(APIK_ARITH)`, `finishapik` | READY | |
| B8 | arith, no k | `lua_arith` | lapi.cpp:368-383 | API metamethod | same | none | NOT READY | Safe: a freeze there destroys the chain. |
| B9 | concat | `lua_concatk` | lapi.cpp:1786-1793 -> `luaV_concat` | API metamethod | `luaT_tryconcatTM` | `armk(APIK_CONCAT)` + `apifirst`; `finishapik` default case continues `luaV_concat` (ldo.cpp:914) | READY | Re-sets NYCALL in `finishapik` (ldo.cpp:912) when the thread cannot yield. |
| B10 | concat, no k | `lua_concat` | lapi.cpp:2078-2093 (calls `luaV_concat` at 2084) | API metamethod | same | none | NOT READY | Safe: a freeze there destroys the chain. |
| B11 | length | `lua_lenk` | lapi.cpp:1770-1777 -> `lua_len` 2095-2101 -> `luaV_objlen` | API metamethod | `luaT_callTMres` | `armk(APIK_PUSH)` only if `mayrun` | READY | |
| B12 | length, no k | `lua_len` | lapi.cpp:2099 | API metamethod | same | none | NOT READY | Safe: a freeze there destroys the chain. |
| B13 | no user code | `lua_rawlessthan`, `lua_plaingetrange`, `lua_plainmove`, `lua_tolstring`/`toistring` (`luaO_tostring`), `lua_pushfstring %s`, `lua_next`, `lua_rawequal` | lapi.cpp:416-423, 1397-1443 (`plainseti` -> `luaV_finishset` 1392), 482-519, 727-800, 2051 | none | none | n/a | READY | `lua_rawlessthan` only compares number/number and string/string. The plain-table bulk functions only assert "no metatable" (lapi.cpp:1401, 1428-1429), so `luaV_finishset` there never reaches a metamethod. In a release build a metatable would hit the nfz++ branch, not unsafe. |

## C. Explicit calls and coroutine entry

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| C1 | host call | `lua_callk`, `ci == &L->base_ci` | lapi.cpp:1666-1679 (`CallS`/`f_call` 1580 -> `luaD_callnoyield` 1582) | explicit call | `luaD_rawrunprotected` + `luaD_callnoyield` | landing: `hostfrozen(FZ_CALL)` lapi.cpp:1595; `lua_thawk` -> `Thaw` -> `unroll` | READY | Errors are rethrown as before (1679). |
| C2 | C call with k, yieldable | `lua_callk` | lapi.cpp:1689-1692 | explicit call | `luaD_call` | `ci->k`/`ctx`; `finishCcall` | READY | |
| C3 | C call with k, not yieldable | `lua_callk` | lapi.cpp:1695-1697 | explicit call | `luaD_callnoyield` with CIST_NYCALL set | `k`; `finishCcall` clears NYCALL (ldo.cpp:977); a thaw counts again | READY | |
| C4 | C call, no k | `lua_callk` / `lua_call` | lapi.cpp:1683-1685. Quote: `L->nfz++; luaD_callnoyield(L, func, nresults); L->nfz--;` | explicit call | `luaD_callnoyield` | none | NOT READY | Safe: a freeze there destroys the chain. This is every library `lua_call` without a continuation. |
| C5 | host pcall | `lua_pcallk`, base_ci | lapi.cpp:1820-1831 | explicit call | `luaD_pcall` (returns LUA_FREEZE untouched, ldo.cpp:1497) | `hostfrozen(FZ_PCALL)`, `frozenerrfunc`; `lua_thawk` | READY | |
| C6 | pcall with k, yieldable | `lua_pcallk` | lapi.cpp:1859-1873 | explicit call | `luaD_call` with CIST_YPCALL | `finishpcallk` (ldo.cpp:856), `precover` | READY | |
| C7 | pcall with k, not yieldable | `lua_pcallk` | lapi.cpp:1840-1857 | explicit call | `luaD_pcall`; on LUA_FREEZE sets CIST_YPCALL and CIST_NYCALL, then rethrows (1856) | `precover` uses `framecalls` for NYCALL frames (ldo.cpp:1118) | READY | The pcall becomes a recoverable one after the freeze. |
| C8 | pcall, no k | `lua_pcallk` / `lua_pcall` | lapi.cpp:1836-1838. Quote: `L->nfz++; status = luaD_pcall(L, &c, c.func, func); L->nfz--;` | explicit call | `luaD_pcall` | none | NOT READY | Safe: a freeze there destroys the chain. |
| C9 | coroutine entry | `lua_resumek` -> `resume` -> `ccall` | ldo.cpp:1255-1291, 1064 | coroutine entry | `luaD_rawrunprotected(Resume)` + `precover` | `endresume` (ldo.cpp:1140): freeze of this coroutine ends here; a freeze of a coroutine below goes on in `from` via `frozenchild` and `thaw()` (ldo.cpp:1178) | READY | Needs `k != nullptr`. |
| C10 | resume without k | `lua_resume` | ldo.cpp:1245-1248. Quote: `return lua_resumek(L, from, nargs, 0, nullptr);` | coroutine entry | as C9 | `L->resumedk = 0`, so a freeze of a coroutine below it would pass through this resume: `luaD_budget` destroys the chain instead (`chaintop`, LUA_NOFREEZE_RESUME) | NOT READY | Safe: a freeze through it destroys the chain. The coroutine itself freezes only as the top of its chain (its resumer has no budget). |
| C11 | thaw | `lua_thawk`, `thaw`, `endresume`, `endhostcall` | ldo.cpp:1296-1338, 1178-1241 | thaw | `Thaw` -> `unroll`; the child is thawed first, then the parent's `k` is called with the result (ldo.cpp:1186-1187) | READY by construction | READY | |
| C12 | continuations | `finishCcall`, `resume()` | ldo.cpp:985, 1083, 1187 | continuation call | C `k` (library or host code) | n/a | READY | `k` is C. User code under it is judged on its own path. |
| C13 | error recovery | `precover` -> `Unroll` | ldo.cpp:1112-1127 | resume after error | `luaD_rawrunprotected(Unroll)` | `findpcall`, `finishpcallk` | READY | |
| C14 | no user code | `luaD_rawrunprotected` users `LuaOpen`, `PushBuff`; `lua_yieldk` | lstate.cpp:378, lobject.cpp:725, ldo.cpp:1400 | n/a | none | n/a | READY | |

## D. Hooks

`luaD_hook` (ldo.cpp:328) raises no `nfz`: it marks the frame CIST_HOOKED and keeps in it what finishing the hook needs after a thaw (`hookev`, `hookdone`, `hooktop`, `hookcitop`, `hookdelta`, `hookres`). The hook function is a host C function (`lua_Hook`); the debug library's hook calls Lua through `lua_callhook` (ldo.cpp:1977), which can freeze (it cannot yield). After a thaw, `unroll` -> `finishhooked` (ldo.cpp:1184) finishes the hook (`luaD_finishhook`) and what it interrupted, by event. A host hook that calls Lua with a plain call raises `nfz` there (C4): a freeze destroys the chain. Hooks do not run at all while a budget callback runs (`L->l_G->budgetL == nullptr` check in `luaD_hook`).

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| D1 | call hook (Lua fn) | `luaD_hookcall` -> `luaD_hook` | ldo.cpp:509 (from lvm `tracecall`, `luaG_tracecall` ldebug.cpp:1188) | hook | `(*hook)(L,&ar)` ldo.cpp:361 | CIST_HOOKED, `hookev` HE_CALL (the function starts again, CIST_FROZE) or HE_CALLVA (in its VARARGPREP); `finishhooked` | READY | |
| D2 | call hook (C fn) | `precallC` -> `luaD_hook` | ldo.cpp:689 | hook | same | `hookev` HE_CALLC: `finishhooked` calls the C function | READY | |
| D3 | return hook | `rethook` -> `luaD_hook` | ldo.cpp:534 (from `luaD_poscall` 640 and `moveresults` 618) | hook | same | `hookev` HE_RET / HE_RETTBC: `finishhooked` moves the results and returns | READY | |
| D4 | count hook | `luaG_traceexec` -> `luaD_hook` | ldebug.cpp:1245 | hook | same | `hookev` HE_INSTR, `hookdone` HD_COUNT: the instruction is traced again (CIST_FROZE) without counting it again (a hook can still yield via CIST_HOOKYIELD, which is a yield, not a freeze) | READY | |
| D5 | line hook | `luaG_traceexec` -> `luaD_hook` | ldebug.cpp:1255 | hook | same | `hookev` HE_INSTR, `hookdone` HD_LINE, as D4 | READY | |

## E. `__close` outside the VM instructions (A12 and A13 are the VM ones)

`callclosemethod` (lfunc.cpp:135): `yy == 1` -> `luaD_call` (freezable). `yy == 2` -> `luaD_callnoyield` (freezable, not yieldable). `yy == 0` -> `nfz++` + `luaD_callnoyield` (lfunc.cpp:151). Which `yy` each caller passes decides the status below.

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| E1 | C function returning with tbc | `moveresults` -> `luaF_close(...,1)` | ldo.cpp:613 | `__close` | `luaD_call` | CIST_CLSRET (set at 612); `finishCcall` ldo.cpp:966 redoes `luaD_poscall` | READY | Also reached from OP_TAILCALL C results (lvm.cpp:3154). |
| E2 | pcall error recovery | `finishpcallk` -> `luaF_close(...,1)` | ldo.cpp:869 | `__close` | `luaD_call` | itself re-entered by `finishCcall` (ldo.cpp:980); CIST_RECST kept | READY | Where a coroutine's pcall catches an error, including after a freeze (NYCALL pcall). |
| E3 | `lua_settop` | `luaF_close(...,0)` | lapi.cpp:226. Quote: `newtop = luaF_close(L, newtop, CLOSEKTOP, 0);` | `__close` | `callclosemethod` nfz++ + `luaD_callnoyield` | none | NOT READY | Safe: a freeze there destroys the chain. Not yieldable in stock either. `lua_settopk` is the freezable form. |
| E4 | `lua_closeslot` | `luaF_close(...,0)` | lapi.cpp:238 | `__close` | same | none | NOT READY | Safe: a freeze there destroys the chain. `lua_closeslotk` is the freezable form. |
| E5 | `luaD_pcall` error path | `luaD_closeprotected` -> `luaF_close(...,0)` (`closepaux` ldo.cpp:1451) | ldo.cpp:1503 (non-k pcall errors, parser errors, a firmware object's `__gc`) | `__close` | same | none | NOT READY | Safe: a freeze there destroys the chain. A host `lua_pcall` that fails closes with `luaD_hostclose` (ldo.cpp:1577, `yy` 2): a `__close` can freeze there (frozen in the close, FZ_HOSTCLOSE); so can one in the close of a `lua_pcallk` with a continuation where the thread cannot yield (`yy` 2). |
| E6 | host call failing after a thaw | `endhostcall` -> `luaD_hostclose` (a `lua_pcall`), `luaD_closeprotected` (a `lua_callk`) | ldo.cpp:1695, 1717 | `__close` | `yy` 2 (`lua_pcall`), as E5 (`lua_callk`) | a `lua_pcall`: frozen in the close (FZ_HOSTCLOSE), a thaw finishes it (`hostcloserun`) | READY (`lua_pcall`) / NOT READY (`lua_callk`) | A `lua_callk`: safe, a freeze there destroys the chain. |
| E7 | thread reset | `lua_closethreadk` (ldo.cpp:1736: coroutine.close, wrap error close, the host's `lua_closethread` / `lua_resetthread` with no `from`), `lua_closeself` (ldo.cpp:1766); `luaE_closethreadplain` (lstate.cpp:386: a `lua_closethread` from a C function with no continuation) | lstate.cpp:322 (called from lstate.cpp:338, 2473 in lapi.cpp) | `__close` | `luaD_closeprotected`, `yy` 2 (`CloseRun`); the plain form under `nfz` | the thread is frozen in the close (`closerun`): after the thaw the caller's `k` gets the status, or the host's thaw finishes the close; a coroutine closing itself goes on with the close | READY (`lua_closethreadk`, `lua_closeself`) / NOT READY (plain) | The plain form: safe, a freeze there destroys the chain. Includes self-close through `luaD_throwbaselevel` (lstate.cpp:340). |
| E8 | unprotected error | `luaD_throw` with no handler -> `luaE_resetthread` | ldo.cpp:105 | `__close` | same | none | NOT READY | Safe: a freeze there destroys the chain. |
| E9 | state close | `close_state` -> `luaD_closeprotected(L,1,LUA_OK)` | lstate.cpp:267 (from `lua_close` 388) | `__close` | same | none | NOT READY | `lua_close` is a host call with nothing to land on. |
| E10 | failing `__gc` | a firmware object's: `GCTM` -> `luaD_pcall` error path (E5); a user object's: `gcdrain` -> `lua_pcallk` with a continuation (E2) | lgc.cpp:1248, 1333 | `__close` | as E5 / as E2 | none / as E2 | NOT READY (firmware) / READY (user) | A firmware object's `__gc` runs with the budget lifted: nothing in it asks to freeze. The error of a `__gc` is a warning. (The finalizer thread and its abandoned finalizer are gone.) |

## F. Finalizers (`__gc`) and the collector

Mechanism: a dead object with a `__gc` goes where its owner says (`separatetobefnz`). A firmware or host object (registered by code with no budget) goes to `tobefnz`, and the collector calls its `__gc` at once, as stock does: `GCTM` (lgc.cpp:1248), on the thread that collects, with that thread's budget lifted (`luaE_setbeff(L, nullptr)`) and `nfz` raised, through `luaD_pcall` (`DoTheCall`). There is no budget point inside it, so nothing asks to freeze there and no collector C code ever has to be unwound. A user object (registered by budgeted code) goes to the queue of its budget group, and a thread of the group calls those `__gc` with `gcdrain` (lgc.cpp:1340: each a `lua_pcallk` with continuation `gcdrain_cont`, under `lua_freezeonly`, so it can freeze but not yield): at a drain point (`drainpoint`, ldebug.cpp:1341: an instruction boundary where the chain can freeze; the frame is marked CIST_FROZE and its instruction runs after the thaw) and before `collectgarbage` returns (`drainafter`, lbaselib.cpp:258, continuation `drain_cont`), each time only when the call finds room (`luaD_checkminstack`); else they wait for the next point. An object whose group is gone is dead without its `__gc`.

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| F1 | finalizer call | `GCTM` -> `luaD_pcall` (firmware object); `gcdrain` -> `lua_pcallk` (user object) | lgc.cpp:1248, 1333 | `__gc` | `luaD_callnoyield` with the budget lifted and `nfz` raised (firmware); a call with a continuation that can freeze, not yield (user) | firmware: none needed (nothing asks to freeze); user: `gcdrain_cont`, and the drain point (CIST_FROZE) or `drain_cont` under it | READY | Errors become warnings (`luaE_warnerror`), as in stock. |
| F2 | incremental state machine | `singlestep` GCScallfin | lgc.cpp:1843-1846 | `__gc` | `GCTM` | as F1 | READY | Needs `!g->gcemergency`. |
| F3 | generational cycle end | `finishgencycle` -> `callallpendingfinalizers` | lgc.cpp:1439 -> 1120-1125 | `__gc` | `GCTM` | as F1 | READY | |
| F4 | state close | `luaC_freeallobjects` -> `callallpendingfinalizers` | lgc.cpp:1696 (from `close_state`, lstate.cpp:269) | `__gc` | `GCTM` | as F1 (firmware objects): nothing asks to freeze | READY | The `__gc` waiting for budget groups are not called: their queues are freed with the groups (lgc.cpp:2213); a frozen thread's `__close` are dropped. (The finalizer thread, whose frozen finalizer left later ones unrun, is gone.) |
| F5 | step | `luaC_step` -> `incstep`/`youngcollection`/`singlestep` | lgc.cpp:1912, 1885, 1477 | collector step | finalizers only through F2/F3 | as F1 | READY | |
| F6 | full collection | `luaC_fullgc` -> `fullinc`/`fullgen` -> `luaC_runtilstate` | lgc.cpp:1962, 1946-1955, 1870; called from `lua_gc(LUA_GCCOLLECT)` lapi.cpp:1960 | collector | finalizers through F2 | as F1 | READY | `collectgarbage` inside a firmware object's `__gc` does nothing (`GCSTPGC`, as stock); inside a user object's `__gc` it collects, and the `__gc` it queues wait (no drain nests: `allowhook` is off). |
| F7 | step via API | `lua_gc(LUA_GCSTEP)` | lapi.cpp:1991 | collector step | as F5 | as F1 | READY | |
| F8 | emergency GC | `luaM_permit_` -> `luaC_fullgc(L,1)` | lmem.cpp:45 | collector | none: `gcemergency` suppresses finalizers (lgc.cpp:1438, 1843) | n/a | READY | |

Every `luaC_checkGC`/`luaC_step` call site (all lead to F5, so F1-F3 apply). No `luaC_condGC` exists in this tree.

| File | Function | Line |
|---|---|---|
| lvm.cpp | `op_newtable` (OP_NEWTABLE) | 1627 |
| lvm.cpp | `op_concat` (OP_CONCAT) | 2273 |
| lvm.cpp | `op_closure` (OP_CLOSURE) | 2652 |
| lapi.cpp | `lua_tolstring` | 495 |
| lapi.cpp | `lua_toistring` | 516 |
| lapi.cpp | `lua_pushlstring` | 671 |
| lapi.cpp | `lua_pushstring` | 688 |
| lapi.cpp | `lua_pushistringref` | 698 |
| lapi.cpp | `lua_pushistring` | 708 |
| lapi.cpp | `lua_pushistringext` | 718 |
| lapi.cpp | `lua_pushfstringi` | 806 |
| lapi.cpp | `lua_pushcclosure` | 837 |
| lapi.cpp | `lua_createtable` | 1134 |
| lapi.cpp | `lua_loadi` | 1884 |
| lapi.cpp | `lua_gc` (LUA_GCSTEP) | 1991 |
| lapi.cpp | `lua_concat` | 2085 |
| lapi.cpp | `lua_newuserobject` | 2149 |
| lapi.cpp | `lua_newuserdatauv` | 2169 |
| lstate.cpp | `lua_newthread` | 285 |
| ltm.cpp | `createvarargtab` (via `luaT_adjustvarargs`) | 318 |
| ldebug.cpp | `luaG_runerror` | 1117 |
| ldo.cpp | `finishapik` (concat) | 916 |
| llex.cpp | `anchorstr` | 166 |
| lparser.cpp | `close_func` | 893 |

`luaC_fullgc` is also called from `lua_gc(LUA_GCCOLLECT)` lapi.cpp:1960, and as the emergency collection in lmem.cpp:45. `lstring`, `ltable`, `lobject`, `lundump` and `lfunc` have no GC trigger call of their own.

## G. Readers (`lua_Reader` -> `load`)

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| G1 | reader | `luaZ_fill` -> `z->reader->read(L)`, reached from the lexer `nextchar` (llex.cpp:36), `luaU_undump` (lundump.cpp:53 `luaZ_read`, 85 `luaZ_getaddr`, 96 `luaZ_fill`), and `f_parser`'s first read (ldo.cpp:1548). Entered by `lua_loadi` lapi.cpp:1886 -> `luaD_protectedparser` ldo.cpp:1593. | lzio.cpp:25. Quote: `std::optional<byte_view> chunk = z->reader->read(L);` | reader | host `lua_Reader`; `load`'s reader function, with a budget through `lua_pcallkeep` with a continuation (`FunctionReader`, lload.cpp:427), without one through a plain `lua_call` | with a budget: the load stops in the reader call or at a token's check point (`lua_checkpointk`), its box keeps it, and it goes on in `load_cont` after the thaw | READY | Without a budget nothing asks to freeze. A host reader that calls Lua with a plain call raises `nfz` (C4): a freeze there destroys the chain. See G2. |
| G2 | parser protected call | `luaD_protectedparser` | ldo.cpp:1608. Quote: `status = luaD_pcall(L, &p, L->top, L->errfunc);` | reader | `luaD_pcall` | on LUA_FREEZE (ldo.cpp:2286-2306) the stopped load goes to its box (`luaZ_hold`, the parser's arrays, buffer and anchors), or the parse is abandoned and what it left in the stack is cleared; then the freeze goes on (`luaD_throw`) | READY | |

## H. Message handlers

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| H1 | handler call | `luaG_errormsg` (from `luaG_runerror`, `lua_error`, `luaG_typeerror`, `luaD_growstack`, ...) | ldebug.cpp:1084-1096; `lua_error` lapi.cpp:2036-2046 | message handler | `luaD_callnoyield(L, L->top - 2, 1)` with CIST_ERRH set on `L->ci` | `unroll` ldo.cpp:1002-1006: clears CIST_ERRH and calls `luaG_throwerror` (ldebug.cpp:1101) | READY | Handler stays non-yieldable. Tested by libcases per commit 963b6f8d. |
| H2 | handler call at base level | `luaG_errormsg` when `L->ci == &L->base_ci` | ldebug.cpp:1093. Quote: `L->ci->callstatus \|= CIST_ERRH;` ; ldo.cpp:1000. Quote: `while ((ci = L->ci) != &L->base_ci)` | message handler | same | `unroll` stops at `base_ci`, so ERRH on `base_ci` is never examined | UNCLEAR | Reachable from a host `lua_pcall(L, 0, 0, msgh)` whose function slot is not callable: `precall` -> `tryfuncTM` -> `luaG_callerror` -> `luaG_runerror` -> `luaG_errormsg` with `L->ci == base_ci` (ldo.cpp:555-556, 743, 787). If the handler is Lua and freezes inside, the thaw finishes the handler, `unroll` leaves the loop without rethrowing, and `endhostcall` would see a normal end. I read the code but did not run this case. Same shape for any API error raised at the base level inside a pcall with a handler. |
| H3 | stale CIST_ERRH | `luaG_errormsg` when the handler itself throws | ldebug.cpp:1094-1095 (the clear on 1095 is skipped by the throw) | message handler | same | `unroll` ldo.cpp:1002 would test the stale bit | UNCLEAR | When the error is raised while `L->ci` is a C frame that is also the recover point (e.g. `xpcall` of a non-callable in a yieldable coroutine, with a failing handler), that frame keeps ERRH and CIST_YPCALL. `precover` then runs `unroll`, which would call `luaG_throwerror` before `finishCcall`. This changes the status or object delivered to `finishpcallk` only in a handler-fails-inside-the-pcall-frame case. Needs a test. I did not run it. |

## I. Budget callback, freeze and thaw

| # | Area | Function | Call site | Kind | Entry to user code | Resume mechanism | Status | Note |
|---|---|---|---|---|---|---|---|---|
| I1 | budget point in Lua code | `luaG_traceexec` -> `budgetpoint` -> `luaD_budget` | ldebug.cpp:1226-1227, 1198-1207; lvm.cpp:2714 | budget callback | `(*g->budgetf)(L)` ldo.cpp:414 (HOST callback; the debug library runs a Lua function on `lua_budgetthread`) | CIST_FROZE on the Lua frame (ldebug.cpp:1205), `unroll` `savedpc--` (ldo.cpp:1011-1012), `luaV_execute` clears the flag (lvm.cpp:2702) | READY | An error in the callback destroys the whole budgeted chain (`killchain`), as a freeze that cannot be does; the budget refills either way (the Guard in `luaD_budget`). |
| I2 | budget point in C code | `lua_checkpointk` | lapi.cpp:1635-1646 | budget callback | `luaD_budget` | `ci->k`/`ctx`; `luaD_freeze`; `finishCcall` calls `k` with LUA_YIELD | READY | |
| I3 | freeze request | `lua_freeze`, `luaD_exitcallback`, `luaD_budget`, `luaD_freeze` | ldo.cpp:1945, 596, 489 | freeze | none: `lua_freeze` leaves the callback; `luaD_budget` checks `nfz` and the `resumedk` chain (`chaintop`) and freezes the top of the chain or destroys the chain (`killchain`) | n/a | READY | |
| I4 | thaw | `lua_thaw`, `lua_thawk` | ldo.cpp:1340, 1296 | thaw | see C11 | see C11 | READY | |
| I5 | set budget | `lua_setbudget`, `setbudgetmask` | ldebug.cpp:188-206, 173 | config | none | n/a | READY | |
| I6 | counting | `luaE_spend`, `luaE_spent` | lstate.h:641-654; ldo.cpp:474, 685, 978, 1283; lapi.cpp:1665, 1819 | counting | none (never runs the callback) | n/a | READY | |

## J. Host callbacks (HOST, not judged)

| # | Callback | Where the engine calls it | Note |
|---|---|---|---|
| J1 | panic function | ldo.cpp:118 (`g->panic(L)` in `luaD_throw` with no handler) | HOST |
| J2 | warn function | lstate.cpp:398-406 `luaE_warning`; called from `lua_warningi` lapi.cpp:2134 and `luaE_warnerror` (lstate.cpp:511; from `GCTM` and `drainfinish`, the error of a `__gc`) | HOST |
| J3 | allocator `permit` | lmem.cpp:39, 46, 91, 98, 217 and others | HOST. An allocation failure can trigger the emergency GC (F8), with no finalizers. |
| J4 | `lua_UserObject` destructor | when the userdata is freed by the sweep or `luaC_freeallobjects` `deletelist` (lgc.cpp:1699); object created at lapi.cpp:2138 | HOST |
| J5 | `lua_Writer` | lapi.cpp:1927 (`luaU_dump`) | HOST |
| J6 | `lua_Reader`, `lua_Hook`, budget function | see G1, D, I1 | HOST |
| J7 | `lua_CFunction` / continuation `k` | ldo.cpp:692 (`precallC`), 985, 1083, 1187 | HOST or library. The C function itself is not Lua user code. |

## Summary

Counts (rows A, B, C, D, E, F, G, H, I; J is HOST and not counted):

E6, E7 and E10 are split (a READY form and a NOT READY one) and count in both.

- READY: 58
- NOT READY: 17 (all safe: a freeze there destroys the chain, through `nfz` or a resume without a continuation, or nothing there asks to freeze; none found that runs user code without a guard)
- UNCLEAR: 2
- Total rows: 74

NOT READY rows: B2, B4, B6, B8, B10, B12 (API metamethods without a continuation); C4, C8, C10 (`lua_callk`/`lua_pcallk` without `k`, `lua_resume` without `k`); E3-E5, E8, E9 (`__close` run by plain `lua_settop`, `lua_closeslot`, the pcall error path, an unprotected error, `lua_close`) and the plain forms of E6, E7, E10 (a host `lua_callk` failing after a thaw, a plain `lua_closethread`, a firmware object's failing `__gc`). (D1-D5, the hooks, and G1, the reader functions, were here at 963b6f8d; they can freeze now.)

UNCLEAR rows: H2 (CIST_ERRH set on `base_ci` is never examined by `unroll`), H3 (stale CIST_ERRH left when the handler itself throws).
