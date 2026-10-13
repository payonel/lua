# Freeze-readiness inventory: standard libraries and auxiliary library

Branch lua-c++ at 963b6f8d. Read-only audit; nothing built or modified.

## API facts (lua.h, lauxlib.h, lua_internal.h, lapi.cpp, ltm.cpp, lfunc.cpp, ldo.cpp)

- Public continuation variants that exist: lua_callk, lua_pcallk, lua_yieldk, lua_gettablek, lua_getfieldk (declared, `lua_getfieldik` internal form used), lua_getik, lua_settablek, lua_setfieldk, lua_setik, lua_comparek, lua_lenk, lua_arithk, lua_concatk.
- lua_resume has NO public continuation variant. The internal `lua_resumek(co, from, nargs, ctx, k)` and `lua_thawk(co, from, ctx, k)` (lua_internal.h) are what the libraries use.
- luaL_tolstring has none (it calls `luaL_toistringk(..., nullptr)`, a plain call). The k form is internal `luaL_toistringk(L, idx, ctx, k)` plus `luaL_tostringresult`.
- luaL_len has none public. Internal `luaL_lenk` + `luaL_lenresult` exist.
- luaL_callmeta and luaL_getmetafield: callmeta has none (plain lua_call); getmetafield is a raw get (no user code).
- Engine-side "cannot freeze" guard: a plain call (k == nullptr in lua_callk / lua_pcallk, a metamethod run from an API call without continuation, lua_closethread / lua_closeslot, hooks, the message handler's own call) does `L->nfz++`. A `lua_freeze` from the budget callback there does not freeze: the whole budgeted chain is destroyed (`luaD_budget` -> `killchain`, error "cannot freeze"; its threads are dead, and the call that resumed or called the chain's top fails with the error). A callback that should go on checks `lua_nofreeze` (or `debug.canfreeze`) first. So a NOT READY row below means "a freeze there destroys the chain, it does not corrupt"; it is not an unsafe unwind.
- `luaE_freezeonly(L)` (CIST_FZONLY): the function uses k variants, but what it calls stays non-yieldable (as stock Lua) and can only freeze. State is in the stack, in `ctx`, or in a `luaL_Box` userdata.

## Cross-cutting paths that are engine code (no per-function rows)

- Error raising (`lua_error`, `luaL_errori`, `luaL_argerrori`, `luaL_typeerrori`, `luaB_error`, `luaB_assert`, etc.): runs the xpcall/message handler via `luaG_errormsg` (ldebug.cpp:1084), which marks CIST_ERRH so a freeze can pass the handler (commit 963b6f8d). Engine-handled for every library function. READY (engine).
- GC finalizers (__gc, user code): any allocating API call or `lua_gc` can run them. They run on the finalizer thread `g->finth` (`GCTM` lgc.cpp:1095, `luaD_runfinalizer` ldo.cpp:465), which has its own protected frame. A freeze of the finalizer ends there (`FZ_RESUME`); a freeze request for the thread that collected is refused (`F->resumedk == 0`, `lua_freeze`). No library function needs a continuation for this. READY (engine).
- Registry and library-internal tables read with plain `lua_getfieldi` / `lua_setfieldi` / `lua_geti` (registry `_LOADED`, `_PRELOAD`, `_CLIBS`, `_HOOKKEY`, `_IO_input`, `_IO_output`, `LUA_RIDX_MAINTHREAD`, fresh result tables in db_getinfo / os.date / table.pack): these tables have no metatable by default, so no user code. See row X1 for the caveat.

## Table (grouped by file)

Status: READY = continuation set before the user-code call. NOT READY = plain call / no continuation (a freeze through it destroys the chain). UNCLEAR = see note.

### lbaselib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| B1 | print (luaB_print) | lbaselib.cpp:63 | 2 (__tostring, __name) | `luaL_toistringk(L, 1, 1, printk)` | `printk`, ctx = arg index i, string result on the stack top; `luaE_freezeonly` at :62 | READY | arg 1 |
| B2 | print (printk loop) | lbaselib.cpp:47 | 2 | `luaL_toistringk(L, i, i, printk)` | `printk`, ctx = i | READY | later args; output already written is host state, not VM state |
| B3 | tostring (luaB_tostring) | lbaselib.cpp:604 | 2 | `luaL_toistringk(L, 1, 0, tostringk)` | `tostringk`; `luaE_freezeonly` :603 | READY | |
| B4 | pairs (luaB_pairs) | lbaselib.cpp:352 | 2 (__pairs) | `lua_callk(L, 1, 4, 0, pairscont)` | `pairscont` (returns 4) | READY | not freezeonly: yieldable in a coroutine, as stock |
| B5 | ipairs iterator (ipairsaux) | lbaselib.cpp:373 | 2 (__index) | `lua_getik(L, 1, i, 0, ipairsauxk)` | `ipairsauxk`; `luaE_freezeonly` :372 | READY | |
| B6 | load with reader function (luaB_load / generic_reader) | lbaselib.cpp:455 (reached from :488 `lua_loadi`) | 3 (user reader) | `lua_call(L, 0, 1)` | none | NOT READY | `lua_call(L, 0, 1); /* call it */`. The parser's C frames are below the reader call, so it cannot be unwound; the plain call does `nfz++`, freeze is refused. Text-chunk `load` (:479) and `loadfile` (:428) read with C readers: no user code. |
| B7 | dofile (luaB_dofile) | lbaselib.cpp:508 | 1 | `lua_callk(L, 0, LUA_MULTRET, 0, dofilecont)` | `dofilecont` | READY | the file read (:506 `luaL_loadfilei`) is a C reader |
| B8 | pcall (luaB_pcall) | lbaselib.cpp:571 | 1 | `lua_pcallk(L, n, LUA_MULTRET, 0, 0, finishpcall)` | `finishpcall`, ctx 0 | READY | |
| B9 | xpcall (luaB_xpcall) | lbaselib.cpp:588 | 1, 7 (handler) | `lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishpcall)` | `finishpcall`, ctx 2 | READY | handler call is engine (CIST_ERRH) |
| B10 | collectgarbage (luaB_collectgarbage) | lbaselib.cpp:257-301 | 6 (__gc) | `lua_gc(L, o, ...)` (collect, step, gen, inc, ...) | none needed | READY (engine) | finalizers run on `g->finth`, see cross-cutting. "collect", "step", "incremental", "generational" can run them |
| B11 | warn (luaB_warn) | lbaselib.cpp:80-81 | 7 | `lua_warning(...)` | none | READY | calls the host-installed warn function (C, `lua_setwarnf`), not Lua code |

Pure engine: tonumber, error (handler path is engine), getmetatable (raw `luaL_getmetafieldi`), setmetatable, rawequal, rawlen, rawget, rawset, select, type, next (raw `lua_next`), assert, loadfile, load of a string chunk, luaopen_base.

### lcorolib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| C1 | coroutine.resume (luaB_coresume / auxresume) | lcorolib.cpp:69 | 1 | `lua_resumek(co, L, narg, 0, k)` | `coresume_cont` (re-derives co from index 1, status via `lua_resumenres`) | READY | |
| C2 | coroutine.wrap function (luaB_auxwrap / auxresume) | lcorolib.cpp:69, :132 | 1 | `lua_resumek(co, L, narg, 0, auxwrap_cont)` | `auxwrap_cont` (co from upvalue 1) | READY | |
| C3 | coroutine.wrap function, error path (auxwrap_end) | lcorolib.cpp:107 | 5 (__close) | `stat = lua_closethread(co, L);` | none | NOT READY | closes co's to-be-closed variables; `lfunc.cpp:145 callclosemethod` with `yy = 0` does `nfz++` + `luaD_callnoyield`, so freeze is refused. Continuation after it (`luaL_where`, `lua_concat`, `lua_error`) is plain C |
| C4 | coroutine.close (luaB_close), dead / suspended | lcorolib.cpp:226 | 5 | `status = lua_closethread(co, L);` | none | NOT READY | same mechanism as C3; this is the common case (closing a suspended coroutine with pending tbc variables runs user __close) |
| C5 | coroutine.close (luaB_close), closing itself | lcorolib.cpp:251 | 5 | `lua_closethread(co, L);` (co == L) | none | NOT READY | runs its own tbc closes, then `luaD_throwbaselevel`; never returns |

Pure engine: create, wrap's creation (luaB_cowrap), yield (`lua_yield` with no k, returns to the resumer; no user code), status, running, isyieldable, `lua_concat` of two strings (:116), `lua_geti` on the registry (:248).

### ldblib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| D1 | debug.sethook installs hookf; the hook runs (hookf) | ldblib.cpp:383 (install at :451) | 4 | `lua_call(L, 2, 0); /* call hook function */` | none | NOT READY | hooks are explicitly non-freezable: `ldo.cpp:359 L->nfz++ /* a hook cannot be frozen */`. By design, guarded |
| D2 | debug.debug (db_debug_from), run command | ldblib.cpp:527 | 1 | `lua_pcallk(L, 0, 0, 0, DD_RAN, db_debugk)` | `db_debugk`, ctx = step; `luaE_freezeonly` :525 | READY | |
| D3 | debug.debug, report error | ldblib.cpp:507 | 2 (__tostring) | `luaL_toistringk(L, -1, DD_PRINT, db_debugk)` | `db_debugk`, ctx = DD_PRINT; `luaE_freezeonly` :506 | READY | |
| D4 | debug.setbudget callback (budgetf) | ldblib.cpp:566 | 1, 7 | `lua_pcall(B, 1, 0, 0)` | none | UNCLEAR | The callback is user Lua code run on a private thread B at base level (host-style call), "with no hooks and no budget" (ldo.cpp:395-420). It is not in the chain `lua_freeze` can freeze; its own calls are not counted (`budgetnfz`). By design a plain call there is intended, but I cannot confirm that a freeze requested from the callback cannot reach B itself. Treat as engine-mediated, not a library function in the freeze chain |
| D5 | debug.thaw (db_thaw) | ldblib.cpp:668 | 1 | `lua_thawk(co, L, 0, thaw_cont)` | `thaw_cont` | READY | |

Pure engine: getregistry, getmetatable, setmetatable, getuservalue, setuservalue, getinfo (fresh table; `lua_setfieldi` on it), getlocal, setlocal, getupvalue, setupvalue, upvalueid, upvaluejoin, gethook, traceback (`luaL_tracebacki`: raw `lua_next` + `lua_rawequal`, no __tostring / __name call), getbudget, freeze (`lua_freeze` only sets a request), isfrozen, luaopen_debug. `db_sethook` itself only stores the function in `_HOOKKEY` (rawset) and calls `lua_sethook`.

### liolib.cpp

No row. Checked io.open / close / read / write / lines / flush / seek / setvbuf / popen / tmpfile / type / input / output, the file methods, `f_gc` and `f_close` (C close functions `io_fclose` / `io_pclose` are C). No `lua_call*`, `lua_pcall*`, `lua_resume`, `luaL_tolstring`, no table-field reads on user tables. Only registry accesses (liolib.cpp:138, :228, :256, :317). `__gc` / `__close` of files are C functions (engine).

### lmathlib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| M1 | math.min (math_min) | lmathlib.cpp:249 | 2 (__lt) | `if (lua_compare(L, i, imin, LUA_OPLT))` | none | NOT READY | no number check on arguments, so tables with `__lt` run user code. `lua_comparek` exists, unused |
| M2 | math.max (math_max) | lmathlib.cpp:264 | 2 (__lt) | `if (lua_compare(L, imax, i, LUA_OPLT))` | none | NOT READY | same |

Everything else in lmathlib (floor, ceil, sqrt, random, fmod, tointeger, ult, type, abs, etc.) uses numeric conversions only.

### loadlib.cpp (package, require)

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| P1 | require (ll_require): LOADED table read | loadlib.cpp:639 (first), :547 | 2 (__index on LOADED / registry) | `lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE, reqctx(REQ_LOADED,0), ll_requirek)`; `lua_getfieldik(L, 2, name, reqctx(REQ_GOT,0), ll_requirek)` | `ll_requirek`, ctx = step * 1 + searcher index * 8; state on the stack (name 1, LOADED 2, searchers 3, `luaL_Box` 4 with the error message); `luaE_freezeonly` :638 | READY | |
| P2 | require: `package.searchers` read | loadlib.cpp:555 | 2 | `lua_getfieldik(L, lua_upvalueindex(1), lt::searchers, reqctx(REQ_SEARCHERS,0), ll_requirek)` | `ll_requirek` | READY | |
| P3 | require: call a searcher | loadlib.cpp:623 | 1, 3 | `lua_callk(L, 1, 2, reqctx(REQ_SEARCHED, i), ll_requirek)` | `ll_requirek`, ctx has i; the message buffer saved with `luaL_buffsave` to the box at :622 | READY | |
| P4 | require: run the module loader | loadlib.cpp:575 | 1, 3 | `lua_callk(L, 2, 1, reqctx(REQ_LOADER,0), ll_requirek)` | `ll_requirek` | READY | |
| P5 | require: set / re-read / set true on LOADED | loadlib.cpp:592, :597, :604 | 2 (__newindex / __index) | `lua_setfieldik(..., REQ_SET ...)`, `lua_getfieldik(..., REQ_RESULT ...)`, `lua_setfieldik(..., REQ_DONE ...)` | `ll_requirek` | READY | |
| P6 | package.searchers[1] preload (searcher_preload / searcher_preloadk) | loadlib.cpp:505, :487 | 2 | `lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE, 1, searcher_preloadk)`, `lua_getfieldik(L, -1, name, 2, searcher_preloadk)` | `searcher_preloadk`; `luaE_freezeonly` :504 | READY | |
| P7 | searcher_Lua | loadlib.cpp:391 | 2 (reads `package.path`) | `lua_getfieldik(L, lua_upvalueindex(1), lt::path, 0, searcher_Luak)` | `searcher_Luak`; `luaE_freezeonly` :390 | READY | file load after it is a C reader |
| P8 | searcher_C | loadlib.cpp:440 | 2 (reads `package.cpath`) | `lua_getfieldik(..., lt::cpath, 0, searcher_Ck)` | `searcher_Ck`; `luaE_freezeonly` :439 | READY | `loadfunc` / `lsys_load` only dlopen + dlsym, never call the Lua opener |
| P9 | searcher_Croot | loadlib.cpp:477 | 2 | `lua_getfieldik(..., lt::cpath, 0, searcher_Crootk)` | `searcher_Crootk`; `luaE_freezeonly` :476 | READY | |
| P10 | package.loadlib (ll_loadlib), package.searchpath (ll_searchpath) | loadlib.cpp:243, :327 | none | | | n/a | pure engine: file existence checks, string building, dlopen |

`lib_gc` (`__gc` of loaded C libraries) is a C function (engine). `luaopen_package` / `setpath` are host-init code.

### loslib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| O1 | os.time with a table, reading fields (os_time_from) | loslib.cpp:311 | 2 (__index) | `lua_getfieldik(L, 1, key, step, os_timek)` | `os_timek`, ctx = step 0..6; values accumulate on the stack; `luaE_freezeonly` :379 | READY | |
| O2 | os.time, writing normalized fields back | loslib.cpp:352 | 2 (__newindex) | `lua_setfieldik(L, 1, timewrite[w], TIME_NREAD + w + 1, os_timek)` | `os_timek`, ctx = 7 + w + 1 | READY | |
| O3 | os.exit(code, true) (os_exit) | loslib.cpp:427 | 5, 6 | `lua_close(L);` | none | NOT READY | closes the state: `close_state` (lstate.cpp:259) runs `luaD_closeprotected` (tbc __close, `yy = 0`, not freezable) and every pending __gc via `luaC_freeallobjects`, then `exit(status)`. Terminal: the state is destroyed, nothing to continue. Only the `true` form |

Pure engine: os.date (builds a fresh table with `lua_setfieldi`; reads no user table), clock, difftime, getenv, remove, rename, tmpname, setlocale, execute, exit(code) without close.

### lstrlib.cpp

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| S1 | string.gsub, function replacement (add_value) | lstrlib.cpp:1106 | 1 | `lua_callk(L, nargs, 1, 0, gsubk)` | `gsubk`; state in a `luaL_Box` at stack index 5 (s, e, n, changed, saved buffer via `luaL_buffsave`); `luaE_freezeonly` :1205 | READY | |
| S2 | string.gsub, table replacement with a metatable | lstrlib.cpp:1122 | 2 (__index) | `lua_gettablek(L, 3, 0, gsubk)` | `gsubk`, same box | READY | |
| S3 | string.gsub, table replacement without a metatable | lstrlib.cpp:1114 | 2 | `lua_gettable(L, 3)` | none, guarded by `box == nullptr` (set only when `lua_isplaintable`, i.e. no metatable, :1203) | READY | calls no user code |
| S4 | string metamethods __add, __sub, ... (arith_*, trymt) | lstrlib.cpp:324 | 2 (second operand's metamethod) | `lua_callk(L, 2, 1, 0, arithk)` | `arithk` (returns 1); `luaE_freezeonly` :323 | READY | |
| S5 | same, numeric path | lstrlib.cpp:330 | 2 | `lua_arith(L, op)` | none | READY | both operands were just converted to numbers (`tonum`): no metamethod possible |
| S6 | string.format "%s" | lstrlib.cpp:1940 | 2 (__tostring, __name) | `luaL_toistringk(L, arg, 0, formatk)` | `formatk`; state (item offset, arg, buffer) in `luaL_Box` after the args (`formatsave` :1765, `luaE_freezeonly` :1772) | READY | `luaL_getmetafieldi(__tostring)` is checked first; no metafield means `luaL_tostringdefault` (pure) |
| S7 | string.format "%q" of nil / boolean | lstrlib.cpp:1924 | 2 | `luaL_toistringk(L, arg, 0, formatk)` | `formatk` | READY | |
| S8 | string.format, `addliteral` for nil / boolean | lstrlib.cpp:1451 | 2 | `luaL_toistring(L, arg)` (plain, k = nullptr) | none | READY (unreachable with user code) | reached only when `formatsave` found no `__tostring` field (:1920-1923 branch), so the plain call cannot run a metamethod. Fragile: depends on that precondition |

Pure engine: len, sub, upper, lower, rep, reverse, byte, char, find, match, gmatch (state in userdata), pack, unpack, packsize, dump (`lua_dump` with a C writer), string metatable `__index = string`.

### ltablib.cpp

The whole library follows one design (comment at :82): `luaE_freezeonly`, `luaL_lenk` / `lua_getik` / `lua_setik` / `lua_comparek` with continuations, and a fast path (`lua_isplaintable`, `lua_plainmove`, `lua_plaingetrange`) for tables with no metatable. Budget check points use `lua_checkpointk`.

| # | Lib.function (C fn) | Call site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| T1 | table.insert (tinsert) length | ltablib.cpp:210-212 -> :73 | 2 (__len) | `aux_getnk` -> `luaL_lenk(L, n, 0, tinsertk_len)` -> `lua_lenk` (lauxlib.cpp:1103) | `tinsertk_len`; `luaE_freezeonly` :211 | READY | |
| T2 | table.insert, shifting up | ltablib.cpp:134 (get), :137 (set) | 2 (__index, __newindex) | `lua_getik(L, 1, i-1, i, tinsertk_get)`, `lua_setik(L, 1, i, i, tinsertk_set)` | `tinsertk_get`, `tinsertk_set`, `tinsertk_loop` (check point); ctx = i | READY | |
| T3 | table.insert, final store | ltablib.cpp:124, :198 | 2 | `lua_setik(L, 1, pos, 0, tinsertk_done)` | `tinsertk_done` | READY | |
| T4 | table.remove (tremove) length | ltablib.cpp:314-316 | 2 | `luaL_lenk(..., tremovek_len)` | `tremovek_len`; `luaE_freezeonly` :314 | READY | |
| T5 | table.remove, first read, shifting, clearing | ltablib.cpp:300, :246, :249, :236 | 2 | `lua_getik(..., tremovek_first)`, `lua_getik(..., tremovek_get)`, `lua_setik(..., tremovek_set)`, `lua_setik(..., tremovek_done)` | the k functions named; ctx = pos | READY | |
| T6 | table.move (tmove) | ltablib.cpp:369, :372 | 2 | `lua_getik(L, 1, f+i, i, tmovek_upset / downset)`, `lua_setik(L, tt, t+i, i, tmovek_upnext / downnext)` | six `tmovek_*` functions; ctx = i; `luaE_freezeonly` :408 | READY | |
| T7 | table.move, table equality | ltablib.cpp:418 | 2 (__eq) | `lua_comparek(L, 1, tt, LUA_OPEQ, 0, tmovek_eq)` | `tmovek_eq` | READY | |
| T8 | table.concat (tconcat) length | ltablib.cpp:574-577 | 2 | `luaL_lenk(..., tconcatk_len)` | `tconcatk_len`; `luaE_freezeonly` :574 | READY | |
| T9 | table.concat, element reads | ltablib.cpp:487 | 2 | `lua_getik(L, 1, i, i, tconcatk_get)` | `tconcatk_get` / `tconcatk_loop`; buffer saved by `luaL_buffsave` into a box at stack index 5 | READY | box exists whenever the table has a metatable (:540-545 area) |
| T10 | table.unpack (tunpack) length and reads | ltablib.cpp:684 (freezeonly), :620 | 2 | `luaL_lenk(..., tunpackk_len)`, `lua_getik(L, 1, i+count, 0, tunpackk_got)` | `tunpackk_len`, `tunpackk_got`, `tunpackk_loop`; first and last index kept on the stack | READY | |
| T11 | table.sort (sort) length | ltablib.cpp:1039-1041 | 2 | `luaL_lenk(..., sortk_len)` | `sortk_len`; `luaE_freezeonly` :1039 | READY | |
| T12 | table.sort, element reads / writes | ltablib.cpp:822 (get), :829 (set) | 2 | `lua_getik(L, 1, n, 0, sortk)`, `lua_setik(L, 1, n, 0, sortk)` | `sortk`; state in an `int64` userdata at stack index 3 (`sort_box`), saved before each call (`SORT_SAVE`, program counter `SO_PC`) | READY | |
| T13 | table.sort, comparison | ltablib.cpp:777 (`__lt`), :783 (function) | 1, 2 | `lua_comparek(L, a, b, LUA_OPLT, 0, sortk)`, `lua_callk(L, 2, 1, 0, sortk)` | `sortk`, same box | READY | |

`table.pack` (`lua_seti` ltablib.cpp:591, `lua_setfieldi` :593 on a table it just created: no metatable, no user code) and `table.create` are pure engine.

### lutf8lib.cpp

No row. char, codepoint, len, offset, codes, charpattern use string/integer conversions only. No metamethod-running API calls (grep: none).

### lauxlib.cpp (auxiliary library; rows for the helpers themselves)

| # | Function | Site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| A1 | luaL_toistringk / luaL_tolstring / luaL_toistring | lauxlib.cpp:1129 (k form), :1192, :1198 | 2 | `lua_callk(L, 1, 1, ctx, k)` | k supplied by the caller. `luaL_toistring` and `luaL_tolstring` pass `k = nullptr` (:1194), i.e. a plain, non-freezable call | READY (k form) / NOT READY (plain forms) | All guest-reachable library callers use the k form (print, tostring, format, debug.debug). Plain `luaL_toistring` only at lstrlib.cpp:1451 (S8). `luaL_tolstring` only at lua.cpp:535 (HOST) |
| A2 | luaL_len | lauxlib.cpp:1094-1097 | 2 (__len) | `lua_len(L, idx);` | none (`luaL_lenk` :1101 is the k form) | NOT READY (no library caller) | only caller is lua.cpp:245 (HOST). Library code uses `luaL_lenk` |
| A3 | luaL_callmeta / luaL_callmetai | lauxlib.cpp:1090 | 2 | `lua_call(L, 1, 1);` | none | NOT READY (no library caller) | only caller lua.cpp:125 (HOST msghandler) |
| A4 | luaL_requiref / luaL_requirefi | lauxlib.cpp:1331 (+ :1325 getfield, :1303 setfield, :1339 setglobal) | 1, 2 | `lua_call(L, 1, 1);`, `lua_getfieldi(L, -1, modname)`, `lua_setglobali(L, modname)` | none | NOT READY (init only) | `openf` is a C opener; `_LOADED` and `_G` could carry user metatables. Called only from `luaL_openselectedlibs` (linit.cpp:51), HOST init |
| A5 | luaL_dofile / luaL_dostring | lauxlib.cpp:1591, :1599 | 1 | `lua_pcall(L, 0, LUA_MULTRET, 0)` | none | NOT READY (no library caller) | host helpers; the standard libraries do not call them |
| A6 | luaL_getsubtable / luaL_getsubtablei | lauxlib.cpp:1295, :1303 | 2 | `lua_getfieldi(L, idx, fname)`, `lua_setfieldi(L, idx, fname)` | none | READY for the registry callers | library callers pass the registry or `package` (loadlib.cpp:687, :698, :701; ldblib.cpp:438; linit.cpp:46) |
| A7 | luaL_traceback / luaL_tracebacki | lauxlib.cpp:79 (`pushglobalfuncname`), :40-57 (`findfield`) | 2 | `lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE)`, then raw `lua_next` / `lua_rawequal` | none | READY | no `__tostring` / `__name` call; `luaL_argerrori` goes through this too |
| A8 | luaL_loadfilex, luaL_loadbufferx, luaL_loadstring | lauxlib.cpp:996, :1040 | 3 | `lua_loadi(...)` with a C reader | none | READY | readers are C (`getF`, string reader); no user code |
| A9 | luaL_error family | lauxlib.cpp:311 | 7 | `lua_error(L)` | none | READY (engine) | message handler, see cross-cutting |

Pure engine: luaL_getmetafield, luaL_newmetatable, luaL_setmetatable, luaL_checkudata, luaL_testudata, luaL_check* / opt*, luaL_Buffer functions and `luaL_Box` (no metamethods), luaL_ref / unref, luaL_fileresult, luaL_execresult, luaL_where, luaL_gsub, luaL_setfuncs, luaL_newlib, luaL_makeseed, luaL_newstate, luaL_checkversion, warn functions (host output only).

### linit.cpp (HOST init)

| # | Function | Site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| I1 | luaL_openselectedlibs / luaL_openlibs | linit.cpp:51, :57 | 1, 2 | `luaL_requiref(...)` (see A4), `lua_setfield(L, -2, name)` on the new PRELOAD table | none | HOST | runs at state creation, before any script |

### lua.cpp (standalone interpreter, HOST program, not part of the library)

| # | Function | Site | Kind | Exact call | Continuation | Status | Note |
|---|---|---|---|---|---|---|---|
| H1 | docall (all chunks) | lua.cpp:150 | 1, 7 | `lua_pcall(L, narg, nres, base)` | none | HOST | host call at base level: handled by the engine as `FZ_PCALL` (lapi.cpp:1824) |
| H2 | msghandler | lua.cpp:125 | 2 | `luaL_callmetai(L, 1, lt::mm_tostring)` | none | HOST | |
| H3 | lstop / laction signal hook | lua.cpp:49, :63 | 4 | `lua_sethook(...)` | none | HOST | |
| H4 | dolibrary, createargtable, pushargs | lua.cpp:184, :225, :232, :243-245 | 2 | `lua_setglobali`, `lua_getglobali`, `luaL_len(L, -1)` | none | HOST | globals / `arg` table before / after the script |
| H5 | REPL prompt | lua.cpp:529-535 | 2 | `lua_getglobali(L, _PROMPT)` then `luaL_tolstring(L, -1)` | none | HOST | |
| H6 | REPL print results | lua.cpp:663-665 | 1 | `lua_pcall(L, n, 0, 0)` | none | HOST | |
| H7 | pmain start | lua.cpp:771 | 1 | `lua_pcall(L, 2, 1, 0)` | none | HOST | |

### Cross-cutting caveat

| # | Item | Site | Status | Note |
|---|---|---|---|---|
| X1 | Plain `lua_getfieldi` / `lua_setfieldi` / `lua_geti` on the registry and on library-internal tables (`_LOADED`, `_PRELOAD`, `_CLIBS`, `_HOOKKEY`, `_IO_input`, `_IO_output`, `LUA_RIDX_MAINTHREAD`) | ldblib.cpp:371, :470, :564, :610; liolib.cpp:138, :228, :256, :317; loadlib.cpp:194; lauxlib.cpp:79; lcorolib.cpp:248 | UNCLEAR | `debug.getregistry()` hands the registry to Lua, and `package.loaded` is a global table, so a script can attach a metatable with `__index` / `__newindex` to them; the plain calls would then run user code with no continuation (and, by the `ltm.cpp:131/156` guard, refuse a freeze rather than corrupt). Default state has no such metatables. `hookf` (D1), `luaL_traceback` (A7), `io.close()` with no argument, `io.lines`, `io.input`/`io.output` are the guest-reachable users |

## Summary

Counts (A1 is split: its k form counts as READY, its plain forms as NOT READY):

- READY: 52 rows (B1-B5, B7-B11 = 10; C1-C2 = 2; D2, D3, D5 = 3; P1-P9 = 9; O1-O2 = 2; S1-S8 = 8; T1-T13 = 13; A1 k form, A6-A9 = 5).
- NOT READY: 13 rows (B6, C3, C4, C5, D1, M1, M2, O3, A1 plain forms, A2, A3, A4, A5).
- UNCLEAR: 2 rows (D4, X1).
- Not counted: HOST rows I1, H1-H7; P10 (pure).

NOT READY, by whether a guest script can reach it:
- Guest-reachable, user code runs with no continuation: B6 `load` with a reader function (lbaselib.cpp:455); C4 `coroutine.close` (lcorolib.cpp:226) and C5 (:251); C3 `coroutine.wrap` function's error path (lcorolib.cpp:107); D1 debug hooks (ldblib.cpp:383, engine refuses to freeze in hooks by design); M1 `math.min` (lmathlib.cpp:249) and M2 `math.max` (:264) with `__lt`; O3 `os.exit(code, true)` (loslib.cpp:427, terminal).
- Not reachable from a script (no library caller): A1 plain `luaL_toistring` / `luaL_tolstring`, A2 `luaL_len`, A3 `luaL_callmeta`, A4 `luaL_requiref` (init only), A5 `luaL_dofile` / `luaL_dostring`.

The engine's own guard (`nfz`) makes every NOT READY row safe: while any of them is on the stack, a `lua_freeze` from the budget callback destroys the chain instead of freezing (`killchain`), so the C stack is never lost; `lua_nofreeze` tells the callback beforehand.
