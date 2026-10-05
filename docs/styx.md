# Styx

Styx is the persistence library for this Lua: it turns Lua values (and, in
time, whole states) into a string and back, callable from Lua script with no
C API involvement.

Lineage: Pluto (Lua 5.1) -> Eris (a rewrite of Pluto for Lua 5.2, later
5.3) -> Styx. We take Eris's API as the inspiration and design the
internals for this codebase.

This file records plans and ADRs. It is the place to put decisions as they
are made; keep entries short.

## Terminology

The API names are `persist` and `unpersist`. In design docs, chat and code
comments, "save" and "load" are synonyms for them (a "saved state" is a
persisted one).

## Goals

- Persist a Lua value graph (tables, strings, numbers, closures with their
  upvalues, coroutines, userdata) to a string and restore it fully.
- Extremely easy to use from script: one call to persist, one to restore.
- Everything is assumed possible. Problems are solved as they are met, one
  at a time, driven by tests.

## Script API (inspired by Eris)

```lua
local s = styx.persist([perms,] value)     -- value -> string
local v = styx.unpersist([perms,] s)       -- string -> value
local x = styx.settings(name [, value])    -- query or change a setting
```

- `perms` is the permanents table: it maps objects to names. On persist an
  object found in `perms` is written as its name; on unpersist the name is
  resolved back to the object in the `perms` supplied to that call.
- A metatable field (`__persist` by default) lets a table or userdata
  control its own persistence: `true` for literal, `false` for forbidden,
  or a function that returns a restore closure.
- Settings are the open list; the first candidates are error-path
  generation, a recursion limit, and the name of the metatable field.

## Layout and running

- `lstyx.cpp`: the library (`lua::lib`, `luaopen_styx`). Today
  `persist`/`unpersist` handle nil, booleans, numbers, strings, tables
  (with metatables, shared references and cycles), Lua closures with closed
  upvalues, and suspended or unstarted coroutines, plus the permanents table.
  `settings` raises "not implemented".
- `tests/styx/`: the Styx tests.
  - `styx_tests.cpp`: C++ tests (using the C API; internals are reachable
    since the test links the raw objects) plus a runner that executes every
    `tests/styx/*.lua`, once per GC mode (incremental, generational, and a
    constant mode with pause 0), each time in a fresh state. Output lists
    every test and case, like `tests/deep/all.lua`.
  - `support/harness.lua`: shared by the scripts (`require "harness"`).
    `case(name, fn)` runs and reports one case; `roundtrip` persists and
    unpersists a value and checks it (see ADR-14); `rejects_truncation`
    feeds every proper prefix of a stream to unpersist.
  - `*.lua`: Lua script tests, one per area: `primitives`, `strings`,
    `tables`, `closures`, `open_upvalues`, `threads`, `permanents`,
    `errors`, `memory`
    (accounting and allocation failures), `processes` (unpersist in a
    separate interpreter process), `resume_print`.
  - Debug builds (`make debug styx-test`) also open the test library `T`,
    which enables the heap invariant checks and the allocation-failure
    tests.
- `make styx-test` builds and runs just the Styx tests (`test_styx`, run
  from `tests/styx`). `make test` is the all-tests target and runs them
  last, after everything else. `make debug styx-test` selects the debug
  variant, like the other targets.
- The engine provides Styx by default but not as a global:
  `luaL_openlibs` puts it in `package.preload`, so `require "styx"` works
  and `_G.styx` is nil. A host that wants it global can call
  `luaL_requiref(L, "styx", luaopen_styx, 1)`.

## Development approach

- The first versions are intentionally incomplete and non-working.
- Test-driven, on both fronts: each capability starts as a failing test,
  written as a Lua script and as a C++ test, then the implementation is made
  to pass it. One issue at a time.
- Capabilities, in the order planned. Done items have tests in
  `tests/styx/`:
  1. nil, booleans, numbers (done: `primitives.lua`)
  2. strings (done: `tables.lua`)
  3. tables (done, see skipped items)
  4. shared references and cycles (done)
  5. metatables (done); `__persist` (not started)
  6. permanents (done; unpersist takes the inverse table, as in Eris)
  7. Lua closures and closed upvalues, including shared upvalues (done:
     `closures.lua`)
  8. coroutines (first cut done: `threads.lua`, and a two-state C++ test)
  9. userdata (not started)
  10. error reporting with a path to the offending value (not started)
- Items are added or reordered as the tests teach us something.

## Skipped for now (return to these)

Deliberately left out of the first cut of thread support. Each raises a
clear error unless noted.

- **Threads with to-be-closed variables**: error.
- **Yields through continuations** (for example a yield inside `pcall` in
  the coroutine): error. Only a plain `coroutine.yield` from Lua code works.
- **Hook yields, calls from hooks or finalizers**: error.
- **Running or resuming-another threads, and the main thread**: error.
  Threads in an error state: error.
- **Hooks, `allowhook`, per-thread counters**: not persisted; a restored
  thread starts with the defaults of a new thread.
- **C closures, light userdata, full userdata**: error (C functions work
  only through the permanents table).
- **Proto sharing**: every closure persists its own copy of its prototype;
  two closures of one prototype restore with two prototypes.
- **Table layout fidelity**: entries are rebuilt, so array and hash
  sizing and iteration order can differ from the original. This is the
  ADR-4 motivation and is still open.
- **Weak tables and `__gc` finalizer state**: contents persist like any
  table; the collector's finalizer queue, mark state and weak-clearing
  state are not persisted.
- **Recursion depth**: persist and unpersist recurse on the C stack and
  raise an error past a depth of 10000 (`MAX_DEPTH`). This is a new limit
  and should be replaced by an iterative walk or a configurable setting.
- **`__persist` metamethod and `settings`**.
- **Untrusted input**: unpersist loads bytecode through `lua_load` and
  trusts the thread state it reads (only basic sanity checks). Do not
  unpersist data you do not trust.
- **Compact output**: integers and counts are fixed 8-byte values; sizes
  and the format version are not yet considered final.

## ADRs

### ADR-1: Name and lineage
**Status:** accepted
Styx, following Pluto and Eris (Lua persistence libraries named for dwarf
planets); Styx is one of Pluto's moons.

### ADR-2: Script-facing API follows Eris
**Status:** accepted
`persist`, `unpersist`, `settings`, with the optional permanents table as
the first argument. Users of Eris or Pluto should find it familiar.

### ADR-3: Permanents are registered by the caller
**Status:** accepted
The caller supplies the permanents table, as in Eris. This is a workable
solution. There are no exceptions (see ADR-7).

### ADR-4: Built into the engine, not on the public C API
**Status:** proposed
Styx is written inside the engine as a `lua::lib` package, so it can read
object internals directly instead of going through the public C API.
Motivation: fidelity of the restored value (for example table layout and
iteration order) and no dependence on what the API happens to expose.

### ADR-5: Incremental, test-driven delivery
**Status:** accepted
Early versions are incomplete by design. Each gap is closed by a failing
test first, then a fix.

### ADR-6: Scope assumes everything is possible
**Status:** accepted
The plan does not list things as impossible. Hard areas are worked out
when a test reaches them.

### ADR-7: No standard-library loading in persist or unpersist
**Status:** accepted (supersedes the interim auto-load layer)
Persist and unpersist never open or load any library. Every non-basic value
that is not persisted by value (including `print` and the other library
functions) resolves only through the caller's permanents table, so the
unpersisting script decides what a key means (for example, mapping
`"print"` to a local function). The interim auto-load layer that this ADR
originally described was never implemented and is dropped.

### ADR-8: Styx is requirable by default, not a global
**Status:** accepted
`luaL_openlibs` preloads Styx (`LUA_STYXLIBK` in the `linit` library
list), so `require "styx"` works in every default state while `_G.styx`
stays nil. Scripts and tests load it with `require`.

### ADR-9: Tests are Lua scripts and C++, in `tests/styx/`
**Status:** accepted
Every capability is covered from script and from C++ for careful testing,
in `tests/styx/`, run by `make test` with everything else.

### ADR-10: Stream format, first cut
**Status:** proposed
A persisted string is the header `"Styx\x01"` (name plus format version)
followed by one value: a tag byte and a payload. Integers and floats are 8
raw little-endian bytes (the engine is little-endian only). Output is built
in a `luaL_Buffer` so it is memory-accounted. Unpersist rejects a missing
header, unknown tags, truncation and trailing bytes.
A thread is written as its status, stack top and maximum frame top, then
its stack values, then its frames, so that unpersist can fill the stack
before any frame exists (see ADR-15).
Non-basic values (strings, tables, functions, threads) get a reference id in
first-seen order; a repeat is written as a back reference, which preserves
sharing and cycles. An object found in the permanents table is written as
its permanent key instead of its contents. Persist keeps its object-to-id
table on the Lua stack, so the bookkeeping is memory-accounted too.

### ADR-11: Permanent keys are any persistable value
**Status:** accepted
A permanent key (the value paired with an object in the permanents table)
may be any persistable value: number, string, and so on. Styx puts no
constraint on its type. The persisting and unpersisting runtimes are
distinct (different processes or machines) and share nothing but the
persisted data, but both scripts are written by the same author, who is
responsible for keeping each key's meaning stable across runtimes and across
script versions. No key is generated by Styx unless a generator is
deterministic and reproducible in the other runtime.

### ADR-12: `CFuncInfo` carries no name
**Status:** accepted
The `CFuncInfo` name field was added with the GC-object design (`809c5b19`,
`6d05ae02`) "for serialization", and every library function was given a
name string. With ADR-3 and ADR-11 the permanents table supplies all
persistence keys, so Styx needs no engine-side name. The field, the named
constructor, the `name` parameter of `lua_createcfuncinfo` and
`luaF_newCFuncInfo`, and every name string at the registration sites were
removed, and the logical accounting cost went from 64 to 24. `CFuncInfo`
stays the identity object and still holds the function, which maps to a
method handle in Java.

### ADR-13: Lua function prototypes reuse `lua_dump`/`lua_load`
**Status:** proposed
A closure is written as its prototype's binary chunk (`lua_dump`, debug
info kept) plus its upvalues, and restored with `lua_load`; the restored
upvalues replace the freshly created ones, sharing via `lua_upvalueid` and
`lua_upvaluejoin`. This reuses the engine's existing, tested prototype
format instead of a second one.

### ADR-14: Bugs fail where they happen
**Status:** accepted
Eris's bugs were very hard to find because their symptoms (GC crashes,
wrong values after a resume) appeared far from their cause. Styx is built
and tested so that a bug shows up where it happens:
- No half-built objects: every restored object is valid at every
  allocation point, and nothing is patched up afterwards through side
  tables.
- After every persist and unpersist the tests check the engine's heap
  invariants (`T.checkmemory` in debug builds).
- Restored graphs are compared with the original by structure and sharing,
  and re-persisting a restored value must give a stream of the same size.
- Every script runs under incremental, generational and constant GC.
- Every proper prefix of a valid stream must be rejected with an error.
- Persist and unpersist must fail cleanly (an error, intact heap) when any
  single allocation fails.
- Errors name what failed and, in time, where (a path to the value).

### ADR-15: Restoring a thread never raises an error on it
**Status:** accepted
The thread being restored is not running and has no error handler, so an
error raised on it would reach the main thread's handler instead of the
caller's (and, in debug builds, break the API lock count). Its stack is
grown with `lua_checkstack` and its frames with the non-raising
`luaE_extendCI`; a failure is raised on the calling thread as a memory
error, as `coroutine.resume` does when it grows a coroutine's stack.
The thread is valid whenever a collection can see it: the stack is filled
while only the base frame exists, the frames are read into spare
CallInfos (only an emergency collection, which keeps them, can run
meanwhile), each frame is checked against the stack (a Lua frame's slot
holds a Lua closure and its saved pc is inside its code), and then all
frames are attached at once. No engine change was needed.

### ADR-16: Open upvalues
**Status:** accepted
An open upvalue (a closure over a local of a function that is still
running) is persisted by its owner:
- Owner is a thread that can be persisted (suspended, unstarted): the
  stream holds the owner thread and the stack level. Unpersist restores the
  thread (if not already) and uses the engine's own `luaF_findupval` on it,
  so every closure over that local, and the restored thread's stack, share
  one open upvalue again. `luaF_findupval` allocates on the restored thread,
  so it runs protected on that thread and a failure is raised on the caller
  (ADR-15).
- Owner is running (the caller, or a thread that resumed another) or is the
  main thread with frames: those frames cannot be persisted, so the
  upvalue is persisted as its current value, as a closed upvalue. Closures
  that shared it still share one upvalue after restore.

### Required tests for later work
- Host-side persist (from the C API, on a state that is not running) of a
  state that is at its memory cap must succeed, and the restored state
  must resume. Scripts must have no way to use this to get around their
  memory limit.

## Open questions

- Format: versioned header or bare bytes.
- Whether whole-state snapshot is a root kind of Styx or a separate
  library built on the same core.
- Exact `settings` list and defaults.
