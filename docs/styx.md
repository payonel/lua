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
- A metatable field (`__persist`) lets a table (and, once supported,
  userdata) control its own persistence: `true` for literal, `false` for
  forbidden, or a function that returns a restore closure.
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
    `special` (`__persist`), `paths` (error paths), `tbc`
    (to-be-closed variables), `protos` (shared prototypes), `errors`,
    `memory`
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
  2. strings (done: `strings.lua`)
  3. tables, with their layout (done: `tables.lua`, ADR-17)
  4. shared references and cycles (done)
  5. metatables (done); `__persist` for tables (done: `special.lua`,
     ADR-18)
  6. permanents (done: `permanents.lua`; unpersist takes the inverse table,
     as in Eris; any value, userdata included, can be a permanent)
  7. Lua closures, closed and open upvalues, including shared upvalues
     (done: `closures.lua`, `open_upvalues.lua`, ADR-16)
  8. coroutines (first cut done: `threads.lua`, C++ cross-state tests),
     with to-be-closed variables (done: `tbc.lua`, ADR-20)
  9. userdata (not supported yet; see below)
  10. error reporting with a path to the offending value (done:
      `paths.lua`, ADR-19)
  11. host side persist of a value or the whole state (done: C++ tests
      `host_*`, ADR-21); host side restore (next)
- Items are added or reordered as the tests teach us something.

## Not supported yet

Each entry is an edge to return to and resolve on its own, with tests. None
of them is a limit of the design. "Today" says what happens now; every one
of these raises a clear error unless it says otherwise.

### Threads
- **To-be-closed variables of C functions** (a C function that called
  `lua_toclose` and yielded). Today: error ("a thread with a hook or
  finalizer call", the frame flag check). Lua to-be-closed variables work
  (ADR-20).
- **Yields through continuations**, for example a yield inside `pcall`,
  `xpcall`, a metamethod or an iterator called from C, or any C function
  that yields with a continuation. Only a plain `coroutine.yield` from Lua
  code works. Today: error ("a thread with a continuation function").
- **Hook yields, and calls made from hooks or finalizers** in a suspended
  thread. Today: error.
- **Running threads**: the caller, a thread that resumed another (status
  normal), and the main thread while it runs. Today: error. An open
  upvalue owned by such a thread is persisted as its current value
  (ADR-16).
- **Threads in an error state** (dead after an error). Today: error.
- **Hooks, `allowhook`, per-thread counters**. Today: not persisted; a
  restored thread starts with the defaults of a new thread.

### Functions
- **C functions and C closures** other than through the permanents table.
  Today: error ("cannot persist a C function").

### Userdata
- **Full userdata** (bytes, metatable, user values, host objects), and
  `__persist` on userdata. Today: error ("persisting a userdata value is not
  supported yet"); userdata work only as permanents.
- **Light userdata** other than as permanents. Today: the same error.

### Tables
- **Iteration order (confirmed bug)**: a restored table has the original's
  array size, hash size and length hint (ADR-17), but its hash entries are
  reinserted, so `pairs` order over the hash part differs once the table
  has had deletions and reinsertions (200 of 200 trials). A coroutine
  suspended inside a `pairs` loop then skips or repeats keys after restore
  (68 of 100 trials). Plan: ADR-22.
- **A `__persist` restore closure that reaches the value it rebuilds**
  (directly or through its upvalues). Today: unpersist error ("invalid
  persisted data"); the value has no object yet while its closure is read.
- **Weak tables and `__gc` state**: contents persist like any table, and a
  restored object with `__gc` is finalized normally, but the collector's
  finalizer queue, mark state and weak-clearing state are not persisted.

### States and memory
- **Whole-state restore on the host** (the counterpart of
  `luaL_persiststate`, ADR-21). Today: host persist only.
- **The main thread's API stack** is not part of a state snapshot; the host
  manages it. Today: not persisted.
- **A `__persist` function in a host persist**: it would run Lua code in
  the state being snapshotted. Today: error ("not supported yet in a host
  persist"); `true` and `false` work.
- **Recursion depth of the script path**: the host persist keeps its own
  work stack (no depth limit); the script path still recurses (see
  "Recursion depth").
- **Policy for restoring over a memory limit** (for a host-driven restore).
  Today: unpersist is charged and limited like any script allocation.
- **The main thread** as a persisted value.

### Format and robustness
- **Recursion depth**: persist and unpersist recurse on the C stack and
  raise an error past a depth of 10000 (`MAX_DEPTH`), a limit stock Lua
  does not have. To be replaced by an iterative walk.
- **Untrusted input**: unpersist builds prototypes from the instructions in
  the stream without verifying them (as with binary chunks), and checks the
  thread state it reads only for consistency with itself. Do not unpersist
  data you do not trust.
- **Compact output**: integers and counts are fixed 8-byte values; sizes
  and the format version are not final.
- **Paths for errors raised by user code**: an error raised inside a
  `__persist` function reaches the caller as it was raised, without the
  path (ADR-19).
- **`settings`**. Today: error ("not implemented").

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
its stack values, then its frames, then its to-be-closed list (stack
levels), so that unpersist can fill the stack before any frame exists
(see ADR-15, ADR-20).
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

### ADR-13: Prototypes encoded natively, once; closures built natively
**Status:** accepted (revised twice)
A prototype gets a reference id, keyed by its identity, and is written once,
field by field: its header fields, its code as raw instructions, then its
constants, upvalue descriptions (with their names), nested prototypes,
source, line information and local variables. Constants and names are
ordinary values (strings get reference ids like any other), and nested
prototypes are prototypes with ids of their own, so a nested prototype is
shared with every closure of it in the stream. No `lua_dump`/`lua_load`.
Unpersist builds a prototype as the engine's loader does: it is anchored
first (by a holder closure), and every part is made valid for the collector
before anything that can run it. Closures are built as `OP_CLOSURE` builds
them: a new closure on the shared prototype, anchored on the stack before
its upvalues are created, under the API lock because it allocates. Upvalues
are then set (closed), joined (shared) or reopened (ADR-16).
Instructions are not verified, as with binary chunks (see "Untrusted
input").

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
  main thread with frames: persisting a running thread's frames is not
  supported yet, so the upvalue is persisted as its current value, as a
  closed upvalue. Closures that shared it still share one upvalue after
  restore.

### ADR-17: Tables keep their layout
**Status:** accepted
A table's array size, hash size and length hint are persisted, and
unpersist creates the table with those sizes before inserting the entries
(the hash part has room for all of them, so nothing rehashes) and then
restores the hint. A table with holes has several borders, and which one
`#t` returns depends on that layout, so without this a restored table
could report a different length. Sizes are checked; the table limits
themselves are left to `lua_createtable`.

### ADR-18: `__persist` for tables
**Status:** accepted
As in Eris, a table's metatable may hold `__persist` (read raw): absent or
`true` persists the table literally, `false` forbids it, and a function is
called with the table and must return a closure. That closure is persisted
in the table's place; unpersist calls it with no arguments and it must
return a table, which takes the table's reference id (reserved before the
closure is read, so sharing is kept). Any other `__persist` value is an
error. Userdata are not supported yet, so `__persist` on userdata has no
effect today.

### ADR-19: Errors say where
**Status:** accepted
Every error persist raises ends with the path from the root to the value
that failed, for example
`persisting a userdata value is not supported yet (at root.config.output)`.
Each step of the walk records how it reached the value (table key, the key
itself, metatable, upvalue, thread stack slot, `__persist` closure,
permanent key) in a node on the C stack, so nothing is allocated unless an
error is raised. Path syntax: `.name` for string keys that are names,
`["text"]` for other strings, `[3]`, `[2.5]`, `[true]`, `[<table>]` for
object keys, `<key T>` when the key itself fails, `<metatable>`,
`<upvalue name>` (or `<upvalue N>` without debug names), `<stack N>`,
`<__persist>`, `<permanent key>`.
Unpersist errors about the stream give the byte offset where the problem
was found: `invalid persisted data (at byte N)`.
Memory errors stay "not enough memory", and errors raised by a `__persist`
function pass through unchanged.

### ADR-20: To-be-closed variables
**Status:** accepted
A suspended thread's to-be-closed variables are persisted as their stack
levels. Unpersist registers them, once the stack and frames are complete,
with the engine's own `luaF_newtbcupval` (which also checks each value
still has a `__close` metamethod), run protected on the restored thread;
an error there is raised on the caller (ADR-15). After restore they are
closed exactly as in the original: on leaving their scope, on return, and
by `coroutine.close`, in reverse order across frames.

### ADR-21: Host side persist
**Status:** accepted
`luaL_persistvalue(L, perms, idx, w, msg)` and
`luaL_persiststate(L, perms, w, msg)` (in `lualib.h`) persist a value, or
the whole state, of a state that is not running (no Lua frame on the main
thread). They are for the host, not for scripts:
- The walk reads the objects directly. Its bookkeeping (reference ids,
  pending work, error paths) is host memory and the stream goes to a
  `lua_Writer` in 64K pieces, then `nullopt`; nothing is allocated in the
  state, no Lua code runs and no collection starts. The result is a
  consistent snapshot, and a state at its memory cap can be persisted (the
  test denies every allocation during the call).
- It keeps its own work stack, so it has no depth limit.
- The stream is the script path's format: for the same value both produce
  the same bytes (tested over a set of value graphs), which keeps the two
  writers from drifting apart.
- A state stream is `TAG_STATE`, the string hash seed, the registry, then
  the metatable of each basic type. The main thread is written as
  `TAG_MAINTHREAD` (restored as the restoring state's main thread); a
  script's unpersist rejects that tag, so a stream cannot hand a script the
  main thread.
- Errors are returned with their path (`registry...`, `root...`), and the
  writer can stop the persist by returning non zero.

### ADR-22: Table order (planned)
**Status:** proposed
A restored table must iterate in the original's order, or a coroutine
suspended in a `pairs` loop skips or repeats keys (see "Iteration order").
Order is the node layout, which depends on the string hash seed, on the
order of insertions and on deletions, so rebuilding by insertion cannot
match it. Plan:
- Persist the hash part slot by slot (key, value and collision link of
  each node, and the free-slot position) instead of as a list of entries.
- Engine support: a general table operation that installs a hash part
  from such a slot list exactly, then checks that every key is found by
  normal lookup; when the restoring state hashes strings differently
  (another seed), the check fails and the table is rebuilt by insertion as
  today.
- State snapshots carry the seed (ADR-21), so a host restore recreates the
  state with the same seed and gets the exact order back.
For comparison, Eris rebuilds every table with `lua_next`/`rawset` and no
presizing, so it has this problem (and the `#t` one of ADR-17).

### Required tests for later work
- Host restore of a state persisted at its memory cap must succeed and the
  restored state must resume (host persist at the cap is tested, ADR-21).
  Scripts must have no way to use this to get around their memory limit.

## Open questions

- Format: versioned header or bare bytes.
- Whether whole-state snapshot is a root kind of Styx or a separate
  library built on the same core.
- Exact `settings` list and defaults.
