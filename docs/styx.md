# Styx

Styx is the persistence of this Lua's kernel: the host persists a whole
state at rest into a string, and restores it into a new state that goes on
exactly as the original would have. Scripts cannot persist (ADR-35).

Lineage: Pluto (Lua 5.1) -> Eris (a rewrite of Pluto for Lua 5.2, later
5.3) -> Styx. It started from Eris's API (a script library that persists
any value); it is now the kernel's persistence only, which lets a restore
keep what Pluto and Eris could not, such as the order of every table
(ADR-22, ADR-35).

This file records plans and ADRs. It is the place to put decisions as they
are made; keep entries short.

## Terminology

The API names are `persist` and `unpersist` (restore). In design docs, chat
and code comments, "save" and "load" are synonyms for them (a "saved
state" is a persisted one).

## Goals

- Persist a whole state (every value it holds: tables, strings, numbers,
  closures with their upvalues, coroutines, programs frozen with their C
  frames, user objects of registered kinds) and restore it so that it goes
  on exactly as the original would have (ADR-31).
- One call each for the host.
- Everything is assumed possible. Problems are solved as they are met, one
  at a time, driven by tests.

## Host API

```cpp
struct luaL_StyxResult { int status; HostString msg; };
luaL_StyxResult luaL_persiststate(lua_State* L, int perms, lua_Writer* w);

struct luaL_RestoreSetup { virtual void setup(lua_State* L) = 0; };
struct luaL_Restored { lua_State* L; HostString msg; };
luaL_Restored luaL_restorestate(lua_Alloc* f, byte_view data, luaL_RestoreSetup* setup);
```

- The kernel's workflow: the kernel (a coroutine the host resumes) freezes
  or suspends its programs and yields back to the host; the host persists
  the state there, with no Lua code running. To restore, the host calls
  `luaL_restorestate`: it makes a new state with the stream's seed, runs
  the host's `setup` (the same libraries and the host's permanents, in the
  same order as for the original; it leaves the inverse permanents table,
  or nil) and restores the state into it. The host anchors in the registry
  what it needs back, such as the kernel coroutine (the main thread's stack
  is not part of a state). The restore gives back the thread the state was
  persisted from: the main thread, or, for a main thread frozen by its
  budget, the host's other thread (ADR-24).
- `perms` is the permanents table: it maps objects to names. On persist an
  object found in `perms` is written as its name; on restore the name is
  resolved back to the object in the inverse table the setup leaves. A
  permanent comes back as the restoring side's object, with what it holds
  there: a permanent userdata has the setup's metatable, so the metatable
  is a permanent too wherever the state names it (a library checks its
  userdata against the metatable in the registry: `luaL_checkudata` on
  `io.stdin` fails, "FILE* expected, got FILE*", when `registry["FILE*"]`
  was persisted as a copy).
- A metatable field (`__persist`) lets a table (and, once supported,
  userdata) control its own persistence: `true` (or absent) for literal,
  `false` for forbidden. Nothing else is accepted (ADR-18).

## Layout and running

- `lstyx.cpp`: the library (`lua::lib`). The round trip of one value
  (`lua_styxpersistvalue`, `lua_styxunpersistvalue`, and the module the
  test scripts require as `styx`, `lua_styxtestmodule`) stays inside the
  engine for the tests of the serializer: a state is persisted as the
  values it holds (ADR-35).
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
    (to-be-closed variables), `protos` (shared prototypes), `children`
    (child coroutines and permanents), `errors`, `memory`
    (accounting and allocation failures), `resume_print`. A kernel's state
    restored in another process is the C++ test
    `kernel_in_another_process` (the test restores it in a child of
    itself).
  - Debug builds (`make debug styx-test`) also open the test library `T`,
    which enables the heap invariant checks and the allocation-failure
    tests.
- `make styx-test` builds and runs just the Styx tests (`test_styx`, run
  from `tests/styx`). `make test` is the all-tests target and runs them
  last, after everything else. `make debug styx-test` selects the debug
  variant, like the other targets.
- The Styx gate (`make styx-gate`, `tests/gate/gate.cpp --styx <n>`; `make
  test` runs it): the deep tests and the upstream suite run on the main
  thread with a budget that freezes at every budget point, as the freeze
  gate runs them, and at every n-th freeze the host persists the whole
  state, restores it into a new state (`luaL_restorestate`, which gives
  back the host's other thread), closes the old one and goes on in the
  new one; a freeze where the state holds what cannot be persisted is
  counted and the next one tried. The scripts must end as with no
  persist. Its permanents are every C function and userdata reachable
  from the registry and the string metatable, and each such userdata's
  metatable (see "Host API"). The deep tests run with a persist at every
  7th freeze (about 1900 restores); the upstream suite at every 5000th
  (about 210), in its portable mode (`_port`): the C libraries its other
  tests load hold C functions that have no names, after which nothing
  could be persisted (the freeze gate runs those tests).
- Every restore of the Styx gate keeps every object: a census counts the
  objects of the original (after a whole cycle that calls no `__gc`; it is
  closed next) and of the restored state (which ended with such a cycle),
  by type, and they must match. Not counted, as not persisted: an open
  upvalue no closure refers to any more (its thread keeps it until its
  frame returns), and a stopped load's half-made parse (the restored load
  makes it again from its kept input). A restore where the program stopped
  its collector is not counted. Memory may differ a little (thread stacks,
  call lists), objects may not.

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
  11. host side persist and restore of a value or the whole state (done:
      C++ tests `host_*`, ADR-21, ADR-24)
  12. finalizers and the collector: marks, their order, and in a whole
      state the finalizer list and the collector's settings (done:
      `finalizers.lua`, C++ tests `host_state_*`, ADR-28)
  13. `next` from a deleted key (done: `tables.lua`, ADR-29)
  14. table order with object keys (done: `tables.lua`, C++ tests
      `object_key_order_across_states`, `library_keys_across_states`,
      `state_next_hash`, ADR-30)
  15. a restored VM goes on as the original would, for what is
      deterministic within a run (done: C++ tests
      `snapshot_continues_identically`, `state_restore_needs_its_state`,
      ADR-31; what is not deterministic yet: "Determinism" below)
  16. preemptive persistence: suspendable C functions (planned: ADR-32)
- Items are added or reordered as the tests teach us something.

## What looks unpersistable

Everything a Lua state holds that seems impossible to persist, with why it
seems so, what Styx does today, and the kind of solution it needs. Each
item is resolved on its own, with tests; the details are in "Not supported
yet" below. Four kinds of solution cover them:
- **Identity re-bound by the host**: the restoring side supplies the object
  (the permanents table, ADR-3, ADR-11); what remains is giving such
  objects the same hash, where they are table keys (ADR-30).
- **State by value**: what sits behind C (bytes, upvalues, library state)
  saved and rebuilt, through an interface the owner of that state provides.
- **C frames**: a stable description of a C function's frame and its
  continuation, so a stopped coroutine can go on through it: every
  internal C function that calls back into Lua becomes *freezable*
  (ADR-33).
- **External resources**: a host protocol to reopen, reconnect or replay.

### Values
1. **File handles** (`io.open`, `io.stdin`, `io.stdout`, `io.stderr`,
   `io.tmpfile`, `io.popen`): userdata over an operating-system file (its
   position, buffered data, mode) or a child process. Today: only as
   permanents; the restoring side's handle is used. Needs: external
   resources (reopen, seek), or the host providing its own file objects.
   Planned (to discuss): a handle saves its path, its buffer state and its
   position; a restored handle sees it is not open, reopens the file, seeks
   and refills its buffer. The restoring host must provide the same files;
   if it does not, the IO fails then, at no fault of the library or the
   script.
2. **C functions**: their identity is a code address. Today: permanents.
   Needs: identity re-bound by the host (done); a stable hash as keys.
3. **Full userdata**: bytes, a metatable and user values, often a host
   object (a Minecraft component, a buffer). Today: permanents only.
   Needs: state by value, through an interface the owner provides.
4. **Light userdata**: a bare pointer. Today: permanents only. Needs:
   identity re-bound by the host.
5. **C closures**: a C function with upvalues. Several library functions
   look like Lua functions but keep their state in C: `coroutine.wrap`
   (holds the coroutine), the `string.gmatch` iterator (the string, the
   pattern and the position: a coroutine suspended in a
   `for w in s:gmatch(p)` loop cannot be persisted), `io.lines` and
   `file:lines` (the file), `math.random` (the generator), and functions
   the host makes with bound state. Today: permanents only, with the
   restoring side's upvalues. Needs: identity for the function, its
   upvalues by value.
6. **Loaded C code**: modules from `require` of a C library or
   `package.loadlib` are functions from a shared library; the registry's
   `_CLIBS` holds the library handles (unloaded by a finalizer). Needs:
   identity re-bound by the host (the module loaded again on restore).

### Execution states
7. **Running threads**: the coroutine calling `persist`, coroutines that
   resumed another (status normal), the main thread from the script API.
   Today: error. Needs: C frames (they wait inside `coroutine.resume` or
   inside `persist` itself); a frozen chain crosses `coroutine.resume`
   (ADR-33), and a frozen main thread persists from the host (ADR-34).
8. **A coroutine suspended inside a C call that continues later**: a yield
   inside `pcall`, `xpcall`, `dofile`, a `__pairs` metamethod, or any C
   function that calls or yields with a continuation (`lua_callk`,
   `lua_pcallk`, `lua_yieldk`). Today: persisted when the continuation has
   a name (the engine's and the library's have, ADR-34); a host's C
   function needs its continuation registered. Freezable C functions
   (ADR-33) are what preemption needs for every C function that calls back
   into Lua.
9. **A coroutine suspended inside a hook or a `__close` handler**. Today:
   persisted as other C frames (ADR-34), a `__close` frozen at a host call
   or at the host's close of a thread included.
10. **To-be-closed variables of C functions** (`lua_toclose`). Today:
    error (Lua's own work, ADR-20). Needs: C frames.
11. **Hooks set from C** (not by `debug.sethook`). Today: error. Needs:
    identity for the hook function, as for C functions.
12. **An object waiting for its finalizer that another finalizer made
    reachable again**. Today: restored, finalized when unreachable again
    rather than at the next step (ADR-28).

### Hidden state behind values that persist
13. **Library state kept in C or the host**: `math.random`'s generator,
    `io`'s default input and output (file handles in the registry),
    `os.setlocale` (the process's), the warning system's on/off state,
    `table.sort`'s time-based pivot. Needs: state by value; see
    "Determinism".
14. **The host's API stack** on the main thread. Today: not part of a
    snapshot of an idle state; the host manages it. A frozen main thread
    is persisted whole, its frames and stack included (ADR-34).
15. **Opaque user C functions** (calling back into Lua with plain
    `lua_call`) on the stack of a thread being persisted. Allowed in the
    VM; persisting fails, by design (ADR-32), with a follow-up to wait for
    them (a deferred or blocking suspend).

### Outside the VM
16. **External state**: the clock and time, environment variables, file
    contents, sockets, child processes, the host's event queue, timers and
    world. Outside the persistence layer: the host saves, replays or
    reconnects it (see "Determinism"). To discuss:
    - **Sockets** cannot seek back to an earlier state. A socket read must
      still be freezable (within one process the socket stays open), but it
      may keep nothing a stream can carry, so a restored one fails. Either
      a query reports that the state cannot be persisted while such a read
      is on a stack (`styx.isPersistable(L)`, which may be messy), or
      socket reads are an exception that cannot be frozen. Sockets are not
      in the standard library.
    - **HTTP** is likely a Lua-script library over standard-library C
      helpers, made freezable later. Sending a request again after a
      restore is a bad idea: responses are not static.

### Refused by design or by choice
17. **Persistable, but refused**: tables whose `__persist` is `false`;
    a `__persist` that is not a boolean (a function would be user code
    run by persist and unpersist outside any budget, ADR-18); a budget
    callback that is not a permanent in a value round trip (the tests',
    ADR-34).

## Not supported yet

Each entry is an edge to return to and resolve on its own, with tests. None
of them is a limit of the design. "Today" says what happens now; every one
of these raises a clear error unless it says otherwise.

### Threads
- **A continuation or context that has no name** (a host's C function that
  calls with a continuation it did not register). The engine's own are
  named; a frozen thread with one of the host's fails to persist with
  "a continuation that has no name" (ADR-34).
- **Running threads**: the caller, a thread that resumed another and is not
  frozen (status normal), and the main thread while it runs. Today: error.
  An open upvalue owned by such a thread is persisted as its current value
  (ADR-16). A frozen chain is not running (ADR-34), and a frozen main
  thread persists from the host, from another thread of the state.
- **Hooks set from C** (not by `debug.sethook`) on a persisted thread.
  Today: error ("a hook not set by the debug library is not supported
  yet"). Debug-library hooks work (ADR-26).
- **A load that read before it had a budget** (a budget set on its thread
  while it was reading): it did not keep that input, so a restored copy
  cannot go again to where it stopped. Today: its thaw fails with "invalid
  state of a stopped load" (`load` returns it as an error).

### Functions
- **C functions and C closures** the engine does not name (ADR-34). The
  engine's light C functions persist by name, and the C closures of a few
  kinds by value (`coroutine.wrap`'s function, the `string.gmatch`
  iterator, `require` and the package searchers). Others only through the
  permanents table, where the restoring side's closure is used, with its
  own upvalues: for example `math.random`'s generator state is an upvalue
  of a C closure, so a restored state draws from the restoring state's
  generator (also `io.lines` and `file:lines`, which hold the file).
  Today: error ("cannot persist a C function").
- **Loaded C code**: C modules (`require` of a C library,
  `package.loadlib`) and the library handles in the registry's `_CLIBS`.
  Today: their functions only as permanents, the handles as userdata.

### Userdata
- **Full userdata** (bytes, metatable, user values, host objects), and
  `__persist` on userdata. Today: error ("persisting a userdata value is not
  supported yet"); userdata work only as permanents, except the user
  objects of a registered kind, such as the boxes the library functions
  keep their state in (ADR-34).
- **Light userdata** other than as permanents. Today: the same error.
- **File handles** (`io.open`, the standard files, `io.tmpfile`,
  `io.popen`): userdata over an operating-system file or a child process,
  also held as `io`'s default input and output. Today: only as permanents
  (the restoring side's handles).
- **Garbage userdata waiting for its finalizer** in a whole-state snapshot:
  the snapshot includes every object marked for finalization (ADR-28), so
  it fails as for any userdata ("(at finalizer list)"); a full collection
  before the snapshot finalizes it.

### Tables
- **Iteration order with another seed, or with keys the restoring side
  supplies differently**: restored with the persisting state's seed, a
  table iterates in the same order, whatever its keys (ADR-22, ADR-30).
  With another seed (strings hash otherwise), or when a key is an object
  the restoring side supplies with another hash, the table is rebuilt: its
  contents are kept, its `pairs` order can differ, so a coroutine
  suspended in a `pairs` loop over it can skip or repeat keys, and `next`
  from a key deleted from it fails. Such keys: a permanent not made the
  same way in the restoring state (library objects made in the same order
  with the same seed hash the same), and light userdata and the light C
  functions of a host that the engine does not name, which hash by
  address (the same in one process). The engine's light C functions hash
  by their names, the same in every process.
- **A finalizer already due whose object a finalizer made reachable
  again** (the object waited to be finalized when it was persisted): it is
  restored marked, so its finalizer runs when it is unreachable again, not
  at the next collection step (ADR-28).

### States and memory
- **The main thread's API stack** of an idle state is not part of a state
  snapshot; the host manages it. Today: not persisted. A frozen main
  thread's stack is (ADR-34).
- **The main thread in a value round trip** (the tests'): reaching the
  main thread fails: as a running thread when it is the caller or above
  it, otherwise with "cannot persist the main thread from a script". A
  state persists an idle main thread as a reference and restores it as the
  restoring state's main thread (ADR-21, ADR-24), and a frozen one whole
  (ADR-34).

### Determinism
A restored VM goes on as the original would for what is deterministic
within one run (ADR-31). These are not deterministic yet, within a run or
across a restore, and are to be revisited:
- **`math.random`**: its generator state is an upvalue of a C closure (a
  permanent), so a restored state draws from the restoring state's
  generator; and the library seeds it from the time and an address when it
  is opened.
- **`table.sort`**: on a badly balanced partition it picks a pivot from the
  time (arrays over 100 elements).
- **Addresses** need a portable answer: `tostring` of a table, function,
  thread or userdata and `%p` show addresses; light userdata and a host's
  light C functions the engine does not name hash by address as keys, so a
  table keyed by them is rebuilt in another process (contents kept, order
  not).
- **The collector**: when finalizers run, when weak tables lose entries,
  the collector's progress, `collectgarbage("count")` and where a memory
  cap is hit depend on its cycle and on the memory accounted, which a
  restore starts anew (and restored objects are accounted anew: a thread's
  stack is sized to what it holds, for example). Objects made by
  finalizers take hashes when the finalizers run.
- **Host input and output** is outside the persistence layer: `os.time`,
  `os.clock`, `os.getenv`, `os.setlocale`, file contents, sockets, child
  processes, the host's callbacks, event queue, timers and world, and the
  warning system's state (`warn("@on")` is the host's warning function, not
  the state's data).

### Format and robustness
- **Untrusted input**: unpersist builds prototypes from the instructions in
  the stream without verifying them (as with binary chunks), and checks the
  thread state it reads only for consistency with itself. Do not unpersist
  data you do not trust.
- **A final format**: the format (version 10, ADR-10) may still change;
  no stream of an earlier version is read.
- **The hash seed in streams** is not hidden: every stream starts with the
  persisting state's string hash seed (`luaL_restorestate` makes the state
  with it), and each object carries a hash derived from it. Whoever can
  read a stream can read the seed. Scripts cannot persist; the host keeps
  its streams.

## ADRs

### ADR-1: Name and lineage
**Status:** accepted
Styx, following Pluto and Eris (Lua persistence libraries named for dwarf
planets); Styx is one of Pluto's moons.

### ADR-2: Script-facing API follows Eris
**Status:** accepted
`persist` and `unpersist`, with the optional permanents table as
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
**Status:** superseded by ADR-35
`luaL_openlibs` preloaded Styx, so `require "styx"` worked in every
default state. Styx is no longer a library of the engine: scripts cannot
persist, and the test scripts require the tests' own module.

### ADR-9: Tests are Lua scripts and C++, in `tests/styx/`
**Status:** accepted
Every capability is covered from script and from C++ for careful testing,
in `tests/styx/`, run by `make test` with everything else.

### ADR-10: Stream format
**Status:** accepted (version 10)
A persisted string is the header `"Styx\x0a"` (name plus format version),
the persisting state's string hash seed (8 bytes, ADR-22), then one value
(or, for a whole state, `TAG_STATE`, ADR-21): a tag byte and a payload
(a table, function or thread starts with its hash, ADR-30);
then the trailer, what only the whole walk knows (the tables to mark for
finalization, ADR-28, and the identities of dead keys, ADR-29), and the
trailer's length, written backward so that unpersist reads it first, from
the end. Integers, reference ids, lengths, counts, table sizes
and prototype fields are variable-length integers (zigzag, 7 bits per byte:
-64..63 take one byte); floats are 8 raw little-endian bytes (the engine is
little-endian only), instructions 4 raw bytes each, and a thread's frames,
to-be-closed list and hook fields fixed 8-byte values. Version 2 made
streams about 70% smaller than version 1's fixed 8-byte integers; version
3 added the seed and the layout mode of tables (ADR-22); version 4 the
trailer; version 5 objects' hashes; version 6 frozen threads (ADR-34);
version 7 a budget group's queue of waiting `__gc`, the kinds of marked
user objects in the trailer and a frozen main thread written whole, and
dropped the tag of a table rebuilt by a `__persist` function; version 8
the thread a whole state was persisted from (ADR-24); version 9 a frozen
`table.sort`'s state as its own kind of user object ("table.sort");
version 10 the values a thread's call or return hook is given (what
`debug.getinfo` "r" reads in the hook). Output
is built in a `luaL_Buffer` so it is memory-accounted. Unpersist rejects a
missing header (or another version), unknown tags, truncation and trailing
bytes.
A thread is written as its status (unstarted or dead, suspended, or dead
after an error, which keeps its frames and error value), stack top and
maximum frame top, then its stack values, then its frames, then its
to-be-closed list (stack levels), then its hook, so that unpersist can
fill the stack before any frame exists (see ADR-15, ADR-20, ADR-26).
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
**Status:** accepted (boolean only since version 7)
A table's metatable may hold `__persist` (read raw): absent or `true`
persists the table literally, `false` forbids it ("cannot persist a table:
its __persist is false"). Any other value is an error ("__persist must be
a boolean"), and is not called.
- Eris also accepts a function, called with the table at persist, that
  returns a closure persisted in the table's place and called at
  unpersist to rebuild it. Both are user code that persist and unpersist
  would run outside any budget (they are not freezable points of a
  program), so a program could run there without limit; the function form
  was removed.
- Userdata are not supported yet, so `__persist` on userdata has no effect
  today.

### ADR-19: Errors say where
**Status:** accepted
Every error persist raises ends with the path from the root to the value
that failed, for example
`persisting a userdata value is not supported yet (at root.config.output)`.
Each step of the walk records how it reached the value (table key, the key
itself, metatable, upvalue, thread stack slot, permanent key) in a node
on the C stack, so nothing is allocated unless an error is raised. Path syntax: `.name` for string keys that are names,
`["text"]` for other strings, `[3]`, `[2.5]`, `[true]`, `[<table>]` for
object keys, `<key T>` when the key itself fails, `<metatable>`,
`<upvalue name>` (or `<upvalue N>` without debug names), `<stack N>`,
`<permanent key>`.
Unpersist errors about the stream give the byte offset where the problem
was found: `invalid persisted data (at byte N)`.
Memory errors stay "not enough memory".

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
`luaL_persiststate(L, perms, w)` (in `lualib.h`) persists the whole
state, which is not running (no Lua frame on the main thread); the round
trip of one value (`lua_styxpersistvalue`) is the tests' (ADR-35). They
are for the host, not for scripts:
- The walk reads the objects directly. Its bookkeeping (reference ids,
  pending work, error paths) is host memory and the stream goes to a
  `lua_Writer` in 64K pieces, then `nullopt`; nothing is allocated in the
  state, no Lua code runs and no collection starts. The result is a
  consistent snapshot, and a state at its memory cap can be persisted (the
  test denies every allocation during the call).
- It keeps its own work stack, so it has no depth limit (ADR-27).
- The stream is the script path's format: for the same value both produce
  the same bytes (tested over a set of value graphs), which keeps the two
  writers from drifting apart.
- A state stream is `TAG_STATE`, the string hash seed, the registry, the
  metatable of each basic type, then every object marked for finalization,
  the collector's settings (ADR-28) and the hash of the next object made
  (ADR-30). The main thread is written as
  `TAG_MAINTHREAD` (restored as the restoring state's main thread); a
  script's unpersist rejects that tag, so a stream cannot hand a script the
  main thread.
- A main thread frozen by its budget (a program the host runs on it,
  ADR-34) is not running: the host persists the state, or a value that
  reaches it, from another thread of the state (the main thread's own
  stack is the frozen program's: "the main thread is frozen: use another
  thread of the state"). It is written whole as `TAG_MAINTHREAD_FROZEN`:
  its stack, frames and freeze, as any frozen thread.
- Errors are returned with their path (`registry...`, `root...`), and the
  writer can stop the persist by returning non zero.

### ADR-22: Table order
**Status:** accepted
A restored table must iterate in the original's order, or a coroutine
suspended in a `pairs` loop skips or repeats keys (it did in 68 of 100
trials before this). Order is the node layout, which depends on the
string hash seed, on the order of insertions and on deletions, so
rebuilding by insertion cannot match it.
- Every stream starts with the persisting state's seed. A table whose hash
  keys all hash by content (strings, numbers, booleans; dead keys are
  fine) is written in layout mode: its array entries, then the free-slot
  position and every node in slot order (no key, a dead key, or a key and
  its value), each with its chain link.
- Unpersist with the same seed installs the nodes as they were with the
  engine's `Table::luaH_setnode` (on `lua-c++`, with `luaH_copyfrom` built on
  it and tested on its own in `test_engine`); with another seed it inserts
  the entries (a value restored in a state of another seed: only in the
  tests). `luaL_restorestate` makes the restoring state with the stream's
  seed (ADR-31, ADR-35).
- The writer reads the nodes when their turn comes and fails if a `__gc`
  run by the collector changed the table's hash part meanwhile.
- Object keys hash by the hash each object was given when made, which is
  persisted with it (ADR-30), so every hash part is written slot by slot.
- Tested: 100 churned tables keep their order; deleted keys after a
  collection; a coroutine suspended in a `pairs` loop neither skips nor
  repeats; across states with the same seed (order kept) and another seed
  (contents kept).
For comparison, Eris rebuilds every table with `lua_next`/`rawset` and no
presizing, so it has this problem (and the `#t` one of ADR-17), as Pluto
does: with each, a coroutine suspended in a `pairs` loop skipped or
repeated keys after a restore in 100 of 100 trials (ADR-35).

### ADR-23: Persisting a child coroutine; permanents are values
**Status:** accepted
A coroutine that is not running (unstarted, suspended or dead) is
persisted on its own or inside a larger value: as part of the whole state,
or as the value of a round trip (the tests'). What the coroutine captured from outside is written
by value unless it is a permanent; a permanent (table, function, userdata,
thread) is written as its key and restored as the object supplied for that
key, so the restored child stays connected to the restoring side's
objects. A captured variable owned by a scope that is not persisted is
restored as its value, unless the variable itself is a permanent (ADR-25)
or it is kept in a table that is a permanent. Tested from script
(`children.lua`) and from the host (`host_persists_child_thread`).

### ADR-24: Host side restore
**Status:** accepted
`luaL_restorestate(f, data, setup)` makes a new state with
allocator `f` and the stream's seed (it hashes as the persisting state
did, ADR-31), runs the host's `setup` protected (it opens what the host
provides and leaves the inverse permanents, key -> object, or nil), and
restores the whole state into it; it returns the restored thread the
state was persisted from, or NULL with a message and nothing left
allocated. (The round trip of one value, `lua_styxunpersistvalue`, is the
tests', ADR-35.)
- The thread the state was persisted from (`luaL_persiststate`'s `L`) is
  written with the state: the main thread, or a thread the state holds
  (else the persist fails: "persist from the main thread or from a thread
  the state holds"). For a main thread frozen by its budget that is the
  host's other thread, anchored before; the restore gives it back, so the
  host reaches the restored state, and persists it again, without touching
  the frozen main thread. That thread comes back with its stack as it was
  persisted, what the host pushed for the persist (the permanents table)
  included: the host pops it there too, or each restore adds to the stack
  the next persist keeps.
- The restore runs in a protected call on the idle state with the
  machinery of `unpersist`; only host restores accept the main thread
  reference.
- The restore runs from another thread of the new state, so that a frozen
  main thread (ADR-21) is restored into its main thread, idle and with an
  empty stack; it is left as persisted (no variable closed, its stack
  whole through collections) until the host thaws it as any frozen
  thread.
- Everything restored is charged to the state through its allocator, so
  the host's allocator is the memory policy: a restore that it denies fails
  with "not enough memory" (and the state is closed). The state's registry
  and type metatables are replaced only after the whole stream was read;
  the collector's settings are applied last (ADR-28).
- Tested: a running program (closures with shared upvalues, a suspended
  coroutine with an open upvalue and a to-be-closed variable, string and
  number metatables, the main thread) restored whole in a new state; a
  state persisted at its memory cap restored and running on under the cap
  again; a restore that runs out of memory; a value's stream, or what is
  not a stream, refused; a setup that fails; a frozen main thread
  (frozen in its own frame with a to-be-closed variable, and in
  `table.unpack`) restored, collected and persisted again from the thread
  the restore gives back, three times over, then thawed to the original's
  end; a persist from a thread the state does not hold, refused. Scripts
  cannot reach any of this, so it gives them no way around their memory
  limit.

### ADR-25: Variables as permanents
**Status:** accepted
A captured variable (an upvalue) can be a permanent: on persist,
`perms[debug.upvalueid(f, i)] = key` writes the variable as its key; on
unpersist, `back[key]` must be a Lua function whose first upvalue is the
variable to share (for example `function() return x end`), and the
restored closures' upvalue is joined to it. Closures that shared the
variable share the supplied one. A missing key, or a value that is not a
Lua function with an upvalue, is an error. Both walkers support it; tested
in `permanents.lua` and among the host/script byte-identity samples.

### ADR-26: Debug hooks
**Status:** accepted
A thread's debug hook as `debug.sethook` sets it (the debug library's C
hook on the thread, the Lua hook function in the registry's hook table) is
persisted with the thread: its mask, base count, current count, last traced
pc and `allowhook`, then the hook function as a value. Unpersist installs
it as `debug.sethook` does and puts the count and last traced pc back, so
the restored thread produces the same hook events as the original (line,
call/return and count hooks, also persisted mid-count). The engine exposes
the debug library's hook as `luaL_debughook` (a general library function)
so the hook can be recognized and reinstalled; a hook set from C is not
supported yet. Tested in `hooks.lua`, a host/script byte-identity sample
and the allocation-failure sample.

### ADR-27: One walk each way, with no depth limit
**Status:** accepted
Persist is one walk for the script and the host (`Walker`), unpersist one
reader for both (`Restorer`). Neither recurses: the walk keeps a stack of
work and the reader a stack of frames (an object being built and the part
it waits for), so there is no depth limit, a limit stock Lua does not have
(a 200000-deep chain persists and restores from script and from the host).
- Script persist and unpersist keep these stacks in memory charged to the
  state (a byte buffer grown under the API lock); a host persist keeps them
  in host memory and allocates nothing in the state.
- A script's persist allocates, so the collector may run, and with it
  `__gc` metamethods (Lua code), so table entries, thread slots and upvalues are read when
  their turn comes rather than copied ahead; pending work never points at a
  value that may have been collected. Every key on the error path has a
  reference id, so it stays alive too.
- The reader anchors every object in the reference table from the moment
  it is made, so frames hold plain pointers and the Lua stack only holds the
  value being handed to the frame below.
- The error path is a stack holding exactly the steps from the root to the
  value at hand, written out only when an error is raised.
- The stream format did not change; script and host output stay byte for
  byte the same (tested), and the suite also runs under AddressSanitizer
  and UBSan.

### ADR-28: Finalizers and the collector
**Status:** accepted
Stock Lua finalizes an object if it was *marked* (by `setmetatable` with a
metatable that had `__gc` at that moment) and calls the finalizers of
objects collected together in the reverse order of their marks. Restoring
with `setmetatable` reproduced neither: an object met while its metatable
was still being built (a class that lists its instances) was never
finalized, an object already finalized was finalized again, one whose
`__gc` was set after `setmetatable` was finalized though the original is
not, and a failed unpersist finalized what it had half built.
- Persist writes the marks in the trailer: the persisted tables that are
  marked (also those whose finalizer is already due), oldest mark first, in
  the order the collector holds them. Permanents keep their own marks.
- Unpersist reads the trailer first and makes those tables before anything
  else, in that order, each marked as a script marks one (a metatable with
  `__gc`, replaced later); the value's table records fill them. Other
  tables get their metatable with no mark. A restored table is so marked
  exactly when the original was, whatever its metatable holds, and the
  finalizers run in the original's order. Marking as soon as a table is
  made also keeps it cheap: the collector finds a table to mark by walking
  its list from the newest object.
- A failed unpersist removes the metatables of the tables it made, so no
  finalizer runs on what it half built.
- A whole-state snapshot also persists every object marked for
  finalization, reachable or not: garbage waiting for its finalizer and
  finalizers already due run after the restore, once each. It persists the
  collector's mode, whether it is stopped, and its parameters as stored
  (through `collectgarbage("param")` they round and would not read back the
  same); a host restore applies them last.
- Not persisted, as stock Lua promises nothing about them: when within a
  cycle a finalizer runs, the collector's phase, colors and ages, and how
  far weak tables were cleared.
- A host restore ends with a whole cycle of the collector, unless the
  program stopped it (`luaC_fullgcdue`): what was garbage when the state
  was persisted goes then (dead entries of weak tables are cleared), and
  the `__gc` calls it finds due are made at the collector's next step, in
  the program's time (a user object's at its budget group's points), none
  in the restore itself. A new state's first cycle starts only once its
  heap has grown by about its size, so without it a host that restores
  more often than that starved a program that waits on its collector (a
  loop waiting for its own `__gc` never ended). In generational mode the
  cycle is a major collection, as the mode's own are.
- Tested: `finalizers.lua` (a class listing its instances, an instance
  restored first, finalized once, `__gc` set late, marked with `__gc` or
  the metatable removed, the order, a finalizer already due, permanents, a
  failing unpersist, truncation, a second round trip),
  allocation failure at every point in both directions with no finalizer
  run by a failed attempt, trailer checks in `errors.lua`, and the C++
  `host_state_*` tests (garbage and finalizers due, the order, settings in
  both directions, a garbage userdata), `restore_collects` (a `__gc`
  due and a dead weak entry after a restore, and neither with the
  collector stopped), and the Styx gate (a program waiting for its `__gc`
  with a restore at every freeze).
- Cost: none for values without marked tables; otherwise one pass over the
  collector's lists for their order.

### ADR-29: `next` from a deleted key
**Status:** accepted
A key removed from a table becomes a dead key at the next collection;
`next` still continues from it, comparing it by address, so a loop that
clears its current entry continues where it was. Restored dead keys had no
identity, so `next` from such a key failed after a restore ("invalid key to
'next'"); a coroutine clearing entries as it iterated stopped working.
- The engine's `Table::luaH_setnode` takes a dead key's identity (on
  `lua-c++`; its table copy keeps them too, tested in `test_engine`).
  Nothing dereferences it; the collector never marks dead keys.
- Persist cannot read a dead key's object (it may be collected), so it
  matches dead keys by address against the objects it persisted, after the
  walk: the trailer gives, for each match, the table, the node and the
  object, and unpersist with the same seed gives the dead node that
  identity. A dead key whose object was not persisted needs none: nothing
  restored can be passed to `next` as that key. A dead key whose object was
  collected and whose address a persisted object now has gets that
  object's identity, as `next` on the original would also take that object
  there.
- The walk keeps a bit per node of each table with dead keys (a table of up
  to 64 nodes in a 16-byte record) and, after it, reads those nodes again;
  matching sorts the smaller side (the dead keys, through a filter of
  addresses, or the objects). It costs nothing without dead keys and grows
  with their number: within noise when a fifth of 20000 records have two
  dead keys, about 13% more persist time when half the nodes of every table
  are dead keys (160000).
- Tested: `next` from every deleted key of churned tables; a coroutine
  clearing entries as it iterates resumes and visits the same keys; a dead
  key whose string is not persisted; allocation failure; trailer checks;
  host and script bytes alike; another seed (contents kept).
- Deleted object keys work as strings do (ADR-30). Not supported yet:
  another seed.

### ADR-30: Object keys keep their order
**Status:** accepted
A traversal that spans a save must continue as it was: `next` moves from
the current key's slot to the following ones, so a restored table must
have its keys in the same slots, or a coroutine suspended in a `pairs`
loop (or a script that keeps its last key across ticks) skips or repeats
keys. Object keys hashed by their address, which a restore cannot give
back (and which Java does not have).
- The engine gives every object that hashes by identity (tables, userdata,
  closures, threads) a hash when it is made (a sequence from the seed,
  `GCObject::hash`, in the header's padding: no object grew), and object
  keys hash by it (on `lua-c++`, A/B in its commit). The engine's light C
  functions hash by their names; light userdata and a host's unnamed C
  functions still hash by address. Strings take none: whether a
  string is new or reused depends on the collector, and the sequence must
  depend only on what the program makes. Persist writes the hash of
  every table, function and thread (as its place in the sequence, relative
  to the previous one written: mostly one byte) and unpersist gives it to
  the restored object when it is made, before it can be a key anywhere.
  Every hash part is then written slot by slot (ADR-22).
- Keys the restoring side supplies (permanents, light userdata, light C
  functions) keep their own hashes, and
  a restored object cannot take another's in their place: after a table
  is read, unpersist checks that every key of its hash part is in the
  chain from its main position (`Table::luaH_mainposition`, now in every
  build) and otherwise rebuilds the table with the same sizes (its
  contents, in another order). Library objects made in the same order with
  the same seed hash the same in both states, so tables keyed by them keep
  their order.
- Deleted object keys keep their identities as strings do (ADR-29).
- A whole-state snapshot carries the hash of the next object, so a
  restored state goes on giving new objects the hashes the original would
  have.
- Cost (A/B, 20000 records): streams +3.6% for records of small tables,
  +12% when each record also has a table with object keys (written slot by
  slot, as string-keyed tables are since version 3); persist +1.6% and
  +2.3%; unpersist 4% and 3% faster (slots installed, not inserted).
- Tested: `tables.lua` (tables, functions and threads as keys in order;
  deleted object keys and `next`; a suspended `pairs` loop; a loop that
  clears object keys; mixed keys; weak keys; permanents as the same
  objects keep the order, as other objects the table is rebuilt with its
  contents), allocation failure at every point
  for both paths, and the C++ tests across states (object keys with the
  same seed and another, library objects as keys, a snapshot's next
  hash). Without the restored hashes the order tests fail; without the
  check the rebuilt-table tests do.

### ADR-31: A restored VM goes on as the original would
**Status:** accepted
A VM persisted and restored anywhere (another machine, another process)
must go on exactly as it would have without the save: the same code after
the snapshot does the same things, in the same order. The only things a
restore may depend on are the persistence library's version and the
stream.
- What is deterministic within one run is kept across a restore: values,
  closures and upvalues, coroutines (suspended loops included), the order
  of every table (strings by the seed, objects by their hashes, ADR-22,
  ADR-30), `next` from deleted keys (ADR-29), the hashes of new objects
  (the snapshot's next hash), `#`, finalizer marks and their order and the
  collector's settings (ADR-28), hooks (ADR-26).
- The restoring state is made from the stream: `luaL_restorestate` creates
  it hashing as the persisting state did (the seed is the stream's, not the
  host's to supply). The host's part, in its setup: open the same
  libraries, and make its own permanents, in the same order as for the
  original, so they get the same hashes.
- Strings take no object hash (ADR-30): a string reused in one state and
  new in the other would shift every later object's hash.
- What is not deterministic yet, within a run or across a restore, is
  listed under "Determinism" in "Not supported yet".
- Tested: `snapshot_continues_identically` runs a program to a snapshot,
  then the same code in the original and in a restored copy (suspended
  `pairs` loops over strings and objects, new string and object keys,
  `next` from deleted keys, shared upvalues, `#`, `table.insert` and
  `remove`, a small sort, a to-be-closed variable, error messages, the
  order of `_G`, `package.loaded` and the registry) and compares the
  transcripts; with strings taking hashes, or without the snapshot's next
  hash, they differ. `restore_takes_the_seed`: the restored state has the
  persisted seed, with no seed from the host.

### ADR-32: Preemptive persistence and suspendable C functions
**Status:** proposed (plan); its mechanism (decisions 1, 2's use of the
continuation API for yields, 7, and the phases) is superseded by ADR-33
Styx will be a preemptive serializer: the host may suspend a VM and
persist it at any instruction, not only where a script yields. A C
function that called back into Lua may then be on the stack, with its
progress on the native C stack, which cannot be persisted. Waiting for a
safe point (deferred preemption) is rejected: suspension must succeed
wherever the VM is.

**Terms.**
- A **continuation** is the function Lua calls instead of returning into a
  suspended C function (`lua_KFunction`, with its context `ctx`).
- A **suspendable** C function keeps its progress across every call back
  into Lua as Lua-visible data, and is resumed through a continuation with
  a stable name. It can be suspended at any of its calls back into Lua,
  persisted, and resumed in another process. (A suspendable `table.sort`
  has a continuation that reads the sort's saved state and re-enters it.)
- A **yieldable** C function (upstream's term) has a continuation but may
  keep state a stream cannot carry: a pointer in `ctx`, progress in C
  memory. It survives a yield within one process.
- An **opaque** C function calls back into Lua with plain `lua_call` or
  `lua_pcall`; its progress is on the native C stack.
- A C function that never calls back into Lua is suspendable trivially:
  it cannot be on the stack where a hook fires and has no progress to keep;
  it needs only a stable name.

**Decisions.**
1. Preempted code runs in a coroutine the host resumes (the main thread
   cannot yield). The host suspends it through a **host interrupt**, not a
   debug hook: scripts keep `debug.sethook` for themselves, and a state has
   one hook per thread, so a hook the host relied on would be visible to
   scripts and replaceable by them. The interrupt is an engine feature of
   its own, outside the debug library: the host arms it (an instruction
   budget, or a flag set from outside, as a signal handler would), the VM
   checks it where it checks count hooks (the same trap, so no cost while
   unarmed), and when it fires the running coroutine is suspended at that
   instruction and control returns to the host's `lua_resume`. Scripts can
   neither see it nor change it. (Layered hooks tied to `_ENV` were
   considered and rejected: hooks belong to threads, `_ENV` to functions,
   and the debug library would have to expose the layers.) Styx persists
   the suspended thread.
2. Every C function our standard libraries put into a Lua runtime is
   suspendable, without exception: the library functions, the closures and iterators they
   return (`coroutine.wrap`'s function, the `string.gmatch` iterator,
   `io.lines`), the metamethods they install, the package searchers. We
   provide no opaque C function. Those that call back into Lua are ported
   (the inventory below); the rest are suspendable as they are. The
   mechanism is the upstream continuation API; `lua_call` stays in the C
   API for hosts (removing stock behavior is not an option), but no
   library function calls back into Lua with it.
3. User C functions may be opaque: the VM runs them, but persisting fails
   if one is on the stack of a thread being persisted, and the error names
   it (see the follow-up below).
4. Every library C function and continuation gets a stable name, and a
   suspendable function's `ctx` holds data only. A name the library gives
   its own functions would also let them persist without the permanents
   table, and hash the same in every process as table keys (the addresses
   item under "Determinism"). A C closure's upvalues are persisted by value
   (the `gmatch` iterator's position, `math.random`'s generator); a host
   resource among them (`io.lines`'s file) is an external resource.
5. The engine's own calls back into Lua, with no C function to make
   suspendable, become resumable in the engine: metamethods run from C API
   calls, finalizers, error message handlers, hooks, `__close` handlers
   run while unwinding or by `coroutine.close`.
6. The C API gains continuation variants of the calls that may run
   metamethods (`lua_geti`, `lua_seti`, `lua_gettable`, `lua_settable`,
   `lua_getfield`, `lua_setfield`, `lua_compare`, `lua_len`, `lua_arith`,
   `lua_concat`; `luaL_tolstring`, `luaL_len`, `luaL_callmeta`), so library
   code can be written suspendably.
7. Nested coroutines: `coroutine.resume` and `coroutine.wrap` become
   suspendable. A preemption of the coroutine they resumed suspends them
   too and goes up to the host, rather than being returned to the script
   as a yield; resuming them resumes the inner coroutine.

**Inventory** (the standard library's C functions that call back into
Lua, which need porting; every other library C function is suspendable as
it is):
- base: `pcall`, `xpcall`, `dofile`, `pairs` (`__pairs`) are yieldable
  already and become suspendable; `load` with a reader function,
  `tostring` and `print` (`__tostring`, `__name`), `ipairs` (`__index`).
- string: `string.gsub` (a replacement function, or a replacement table's
  `__index`); `string.format` (`__tostring`, for `%s` and in `%q`);
  arithmetic on strings (the string metatable calls the other operand's
  metamethod).
- table: `table.sort` (the comparator, `__lt`, `__index`, `__newindex`;
  its recursive quicksort becomes an explicit state), `table.insert`,
  `table.remove`, `table.move`, `table.concat`, `table.unpack`
  (`__index`, `__newindex`, `__len`).
- math: `math.min`, `math.max` (`__lt`).
- package: `require` (the searchers, the loader, the module's body).
- debug: the hook `debug.sethook` installs (it calls the Lua hook),
  `debug.debug`.
- coroutine: `coroutine.resume`, `coroutine.wrap` (decision 7),
  `coroutine.close` (runs `__close` handlers).
- auxiliary library: `luaL_tolstring`, `luaL_len`, `luaL_callmeta`; the
  host helpers (`luaL_dofile`, `luaL_dostring`, `luaL_requiref`) run
  outside a preempted coroutine and stay as they are.

**Phases**, each with tests and an A/B:
1. Continuation names and data-only contexts; Styx persists the frames of
   suspendable C functions.
2. The host interrupt (decision 1), with `coroutine.resume` and
   `coroutine.wrap` passing it up (decision 7); Styx persists a thread
   suspended by it.
3. The base library (`pcall`, `xpcall`, `dofile`, `pairs`, `tostring`,
   `print`, `ipairs`, `load`).
4. The table, string and math libraries (`table.sort` as an explicit
   state).
5. The engine's own calls back into Lua (decision 5) and the API's
   continuation variants (decision 6).
6. `coroutine.resume`, `coroutine.wrap`, `coroutine.close`, `require`,
   the debug library.
7. Preemption everywhere, tested by transcripts: a program preempted at
   every instruction (each count-hook position), persisted, restored and
   continued, against an uninterrupted run.

**Follow-up feature** (not now): opaque user C functions on the stack.
Persisting then fails (decision 3); a host could instead wait for them:
- a query: Styx inspects a state and returns whether it can be suspended
  now (an enum: suspendable; blocked by an opaque C function, naming it;
  running; ...);
- a deferred or blocking suspend: Styx injects an ad hoc hook just below
  the bottommost opaque C function's frame, so the coroutine is suspended
  when that function returns, and the host's handler persists it there; a
  blocking call can wait for that point. This applies only to opaque
  functions the host chose to allow; the library's own are suspendable.

### ADR-33: Freeze and thaw
**Status:** proposed (plan); supersedes ADR-32's mechanism

ADR-32 made library C functions yieldable through the continuation API and
preempted scripts with a yield. Tried on lua-c++ (2026-10-05, not
committed), it falls short:
- It changes stock behavior scripts can see: a `string.gsub` callback or a
  `table.sort` comparator becomes yieldable (`coroutine.isyieldable()`,
  `coroutine.yield` inside them), which four upstream tests check.
- A preemption by yield rides the debug hooks: they are per thread (and a
  `debug.sethook` hook does not carry into a new coroutine), off inside
  hooks and finalizers, never run inside C, and their yield lands at the
  nearest resume, so user code sees it.
- Measured, a script withholds preemption with a finalizer that loops
  (hooks off, cannot yield), a long C loop (`table.move` over a huge range,
  `table.insert` with a huge `__len`), a backtracking pattern (time grows
  like n^4), a loop in a coroutine it creates, or the `__close` of a
  coroutine closing itself.

**Terms.**
- **Freeze**: the VM stops where it is. The native C stack unwinds to the
  call that resumed the coroutine the freeze names; nothing a script can
  see changes: no values are yielded, no status changes, nothing is
  closed, and it is not an error (`pcall` does not catch it).
- **Thaw**: a frozen coroutine goes on exactly where it stopped.
- **Frozen**: the state of a coroutine stopped by a freeze, and of the
  coroutines it resumed.
- **Freezable**: a C function or an engine path that can be frozen in the
  middle. Its progress is data: a continuation with a stable name, a
  data-only context, a state box. A frozen VM thaws in this process, or is
  persisted and thaws in another. (It replaces ADR-32's "suspendable",
  which collides with Lua's "suspended" status of a yielded coroutine.)
- **Budget**: the number of steps between two calls of the budget
  callback.
- **Step**: the unit of work the budget counts, the same for script and C
  work (decision 1).

**Decisions.**
1. **One budget and one callback per VM, set once**: `lua_setbudget(L, n,
   f)` in C, `debug.setbudget(n, f)` in script (a sandbox hides `debug`;
   no test enumerates its keys). `n = 0` turns it off: stock behavior.
   Persisting it is harmless, not required. The budget counts **steps**,
   one for each of:
   - a VM instruction;
   - a C function call, and each hand-off between a C function and Lua (a
     call back into Lua, a return through a continuation);
   - an element of C work: what a library function processes, one at a
     time: a table slot read or written (`table.move`, `insert`, `remove`,
     `unpack`, `concat`), a comparison (`table.sort`, `math.min`), a
     pattern-matcher step (`find`, `match`, `gmatch`, `gsub`), a character
     (`utf8`, `string.byte`, `string.char`), and a chunk of bytes for bulk
     work (`string.rep`, `upper`, `reverse`, `format`, concatenation; the
     chunk size is a constant to tune, about 64 bytes).
   Hand-offs alone would miss the work that holds off a freeze: pure C
   loops never hand off. C work is charged at check points inside the
   loops, which are freezable: when the budget runs out there, the
   callback runs and may freeze the loop in its middle (the parser of
   `load` has one at each token, the binary loader at each function,
   constant and debug entry). Work that cannot be split (an allocation) is
   charged when it ends, and a freeze happens at the next check point; its
   cost is bounded by its input. Counting is deterministic: a program spends the same steps
   in every run.
2. **The callback.** Each time `n` instructions are spent, the VM calls the
   callback on the thread that spent them, with that coroutine as its only
   argument (others may be added once tests show the need). It runs fully
   unchecked: no hooks, no budget checks, and its own work is not
   counted. It may:
   - return: the budget refills and execution goes on;
   - call `freeze(co)` (`lua_freeze(L, co)`, `debug.freeze(co)`): when the
     callback returns, `co` and every coroutine it resumed freeze, and the
     call that resumed `co` gets control;
   - raise an error: an ordinary error, which scripts can catch; freezing
     is the tool for code that may not cooperate.
3. **Where a freeze lands is named by the callback.** The host names its
   kernel's thread and lands in its own `lua_resume`; a kernel names a
   user program and lands in its own `coroutine.resume`. That call returns
   as an empty yield (`true`, or `{LUA_YIELD, 0}`); the coroutines between
   pass the freeze through their `coroutine.resume` calls without seeing
   it. `co` must be in the running chain. The main thread can be frozen
   too: the freeze lands at the host's entry call (`lua_pcall` returns
   `LUA_YIELD`). Only a host `lua_call` outside protected mode cannot be
   frozen, as an error there goes to the panic function.
4. **States.** `isfrozen(co)` (`lua_isfrozen`, `debug.isfrozen`) tells a
   freeze from a yield. Statuses stay what they were: the top-most
   coroutine is "running", those that resumed others are "normal". A frozen
   coroutine is not "suspended", so `coroutine.resume` refuses it, as it
   refuses any running coroutine; a restored state machine goes on
   correctly.
5. **Thaw**: `thaw(co)` (`lua_thaw(co, from)`, `debug.thaw(co)`) goes on
   with a frozen coroutine and returns a resume result: values when it
   yields or returns, an error, or an empty yield with `isfrozen(co)` when
   it froze again. A restored state needs it.
6. **Stock behavior does not change.** Yieldability, the coroutine
   statuses, debug hooks, `pcall` and finalizer semantics and every entry
   point behave as in stock Lua 5.5; with a budget set, only the freezes
   are new. The upstream tests run unmodified, and the deep tests pin the
   stock behavior of every path this touches, checked against stock Lua
   5.5 built from `master`.
7. **Freezable everywhere.** Every C function our standard libraries put
   into a Lua runtime is freezable (ADR-32's decision 2 and inventory, by
   this mechanism): its continuations record where to thaw, while its
   calls stay exactly as yieldable as in stock (a `gsub` callback cannot
   yield). So are the engine's own calls into Lua: metamethods run from C
   API calls, message handlers, hooks, `__close` handlers (unwinding,
   `coroutine.close`, a coroutine closing itself), finalizers, and
   `coroutine.resume`/`coroutine.wrap` (a frozen chain crosses
   coroutines). Finalizers still run inside the engine, but only where a
   freeze can happen: between instructions, and in library functions that
   run them with a continuation (`collectgarbage`). An allocation inside a
   C function's API call leaves due finalizers for the next such point;
   the manual leaves finalizer timing unspecified.
8. **Persistence.** Styx persists every thread state: not started,
   suspended, frozen (with its chain), dead. A restored thread is in the
   state it was in; the caller checks `isfrozen` to choose `thaw` or
   `coroutine.resume`.
9. **Opaque user C functions** cannot be frozen in their middle (ADR-32's
   decision 3); a freeze in a coroutine running one waits until it
   returns (ADR-32's follow-up).

**In practice.** The host:

```cpp
static lua_State* K;   // the kernel's thread

static void onbudget(lua_State* co)  // co spent the budget
{
  if (sliceOver())
    lua_freeze(co, K);  // K and all it resumed freeze; lua_resume(K) returns
}

lua_setbudget(L, 10000, onbudget);   // once per VM
lua_ResumeResult r = lua_resume(K, nullptr, 0);
for (;;)
{
  if (lua_isfrozen(K))               // r is {LUA_YIELD, 0}
  {
    if (saveDue())
      luaL_persiststate(L, perms, &writer);   // .status, .msg
    r = lua_thaw(K, nullptr);        // next tick; returns like lua_resume
  }
  else if (r.status == LUA_YIELD)    // the kernel's own yield to the host
    r = lua_resume(K, nullptr, serve(K, r.nres));
  else
    break;                           // LUA_OK, or an error
}
```

A kernel, in script:

```lua
local current   -- the user program being run
debug.setbudget(10000, function(co)
  if sliceOver() then debug.freeze(current) end
end)

current = prog
local r
if debug.isfrozen(prog) then
  r = table.pack(debug.thaw(prog))
else
  r = table.pack(coroutine.resume(prog, reply))
end
if debug.isfrozen(prog) and saveDue() then
  coroutine.yield("save") -- the host persists the whole state (ADR-35)
end
```

**What carries over** from the yield-based work: the library
continuations, state boxes, the API's continuation variants and their fast
paths, `table.sort` as an explicit state, the hook and close machinery, and
`tests/deep/yields.lua` (as freeze tests). The library's call sites go back
to stock yieldability.

**Not yet**:
- Callback arguments beyond the coroutine.

**Phases**, each with tests (stock pins, checked against stock Lua) and an
A/B:
1. The budget, the callback, freeze and thaw for Lua frames and the main
   thread, across `coroutine.resume` and `coroutine.wrap`; steps for
   instructions, calls and hand-offs.
2. The library's continuations as freezable at stock yieldability, with
   their C work charged at check points (the pattern matcher records its
   calls as a freeze leaves them and makes them again after the thaw); the
   engine paths (message handlers, hooks, close, finalizers).
3. Styx persists frozen states and continuation names.

### ADR-34: Frozen threads
**Status:** accepted (supersedes the rejections of ADR-20 and ADR-26 for
C frames; the specification of freeze and thaw, section 13)

A user program stopped by its budget (a freeze) is persisted, restored in
the same state or another, and thawed: it goes on exactly as the original.
What a frozen chain is, to Styx:
- **Threads.** A frozen thread is not running, whatever its status says
  (the topmost one stays "running", the others "normal"). The chain is
  persisted from any of its threads: each has its parent and child
  (`frozenparent`, `frozenchild`), written as values, so the other threads
  of the chain come with it, and the restored links are made from either end.
  A thread also keeps how it froze (`frozen`), the function and message
  handler of the host call it froze in, its C calls (`nCcalls`, `ccbase`,
  `frozencc`) and how its variables were being closed. Its stack is
  written up to the top it froze at (`frozentop`): the messages a host's
  refused resume or close left above it are not its, as its thaw drops
  them. A thread's stack is restored with room for its frames' tops, to
  the stack's limit (a C function may have checked the stack for that
  many values) and past it for a thread handling a stack overflow (its
  room for errors, `ERRORSTACKSIZE`).
- **Frames.** A frame keeps what the freeze needs: its continuation (by
  name), the C calls it runs with, how an API call with a continuation goes
  on (`apiop`, `apifirst`) and, while a hook function runs, the state to
  finish the hook. Only what means something is written (a frame not in an
  API call or a hook writes zeros), and a stream that has stale or
  impossible values for a frame is rejected.
- **Names.** The continuations, the engine's light C functions and the
  budget callback of the debug library have stable names (`lnames.cpp`:
  the module and the function). A name resolves in the restoring process,
  whatever its addresses; a host registers its own continuations and
  callbacks the same way. A permanent given for a function wins over its
  name. A frozen thread with a continuation that has no name is not
  persisted.
- **State by value.** The state a library function keeps in a box (a user
  object, `luaL_Box`: integers and bytes) is persisted as that kind of
  object. A user object of a registered kind (`lua_registerobjectkind`) is
  persisted as its kind's name, its integers and bytes, its metatable and
  its user values (as nil if it says they are not part of its state,
  `persistuservalues`). The C closures of the kinds in the engine are persisted
  as the name of the kind, their upvalues, and for the gmatch iterator the
  position of its match. A pattern match stopped at one of its check points
  keeps itself in a match box (`string.match`: where the stopped call began,
  the captures, and a frame for each call of the matcher waiting), and goes
  on from it after a restore as in the original.
- **A load stopped in its middle.** `load` stops in a call of its reader
  function or at the check point of a token (each token read is a step),
  and its box (`load`) keeps what a restore needs: the input read so far
  (kept while the load has a budget) and the stop points passed. `loadfile`,
  `dofile` and `require` stop at the check points of tokens only: with a
  budget, the file is read whole first, and the box keeps it (a restored
  load does not read the file again). What the box keeps is counted by the
  host, not in the Lua heap, which a restore leaves as the program saw it.
  The parse itself, half made, is not persisted (its anchor table is in
  the box, a user value that is not part of its state). Restored, the load
  goes again over its input from the start, taking no step, making no check
  point and not calling the reader, up to where it stopped (in a call of
  the reader: the piece that call gave comes next), and goes on as the
  original: the same function, the same calls of the reader. In the process
  that froze it, a load goes on from its frames, with no second parse.
- **Budget groups.** A group is persisted with its limit, its steps left,
  the name of its callback, its value (the debug library's Lua function),
  the thread it was set on and its queue of waiting `__gc`; a thread keeps
  its own group and the one in effect. A thread of the chain with no group
  of its own (a kernel thread the program resumed) keeps only the one in
  effect. Restored in a state with no thread for the debug library's
  callbacks, the group makes one (the callback's `prepare`).
- **The callback is the kernel's.** In a value round trip (the tests',
  `lua_styxpersistvalue`) the callback's value must be a permanent ("cannot
  persist a budget callback that is not a permanent"): copied, it would be
  another function, with copies of the kernel's upvalues. A whole-state
  snapshot persists it as it does every function of the state.
- **Steps left inside the callback.** A group persisted while its callback
  runs is written with its full limit, which is what it has when the
  callback returns without a freeze.
- **Deferred `__gc`.** An object marked for finalization keeps its owner:
  the trailer says, for each marked object, whether it belongs to the
  kernel, to a group (the reference of the group in the stream) or to a
  group that is gone (it is then an ordinary object when it dies, as in the
  persisting state), and whether its `__gc` was already due (waiting in
  `tobefnz` or in a group's queue). The same holds for the user objects of
  a registered kind, whose kind the trailer also names. A group's queue is
  persisted with the group, in a state as in a value round trip, and
  unpersist makes the due ones due again in their original order
  (`luaC_makedue`), so each `__gc` runs once after the restore: a deferred
  one when a thread of its group goes on. The thread keeps `allowhook`, which is off while a drained
  `__gc` or a hook function runs (the drain must not nest). The collector's
  timing is not the original's (the debt of a restored state is another,
  see "Determinism"), so a program's `__gc` order that depends on when an
  automatic step comes can differ; the tests stop the collector where it
  matters.
- **The main thread.** A program the host runs on the main thread freezes
  as on any thread, and persists and restores from another thread of the
  state (ADR-21, ADR-24).
- **A freeze in a `__close` at a host call.** A `__close` run as a host's
  `lua_pcall` fails, or by the host's `lua_closethread` with no `from`, can
  freeze; the thread keeps that kind of freeze (and the status being
  closed with), and is persisted and thawed as any frozen thread: the
  thaw finishes the close.
- **Inside the budget callback.** A thread of the chain that spent the budget
  is still running there, so persisting it is refused ("a running thread");
  once the callback has frozen it, it persists. This is by design (not a
  workflow, specification 13.4): a kernel persists a thread once
  `debug.isfrozen(co)` is true.

Tests: `frozen.lua` (programs frozen at every budget point, persisted,
restored and thawed beside the original, in one state, a find frozen deep
in its backtracking and loads stopped at every point among them, of
strings, readers and files), and the C++ tests
`frozen_library_cases` and `frozen_engine_cases`, which freeze each case of
`tests/deep/libcases.lua` and `frozencases.lua` (library callbacks; drained
`__gc`, message handlers, `__close`, wrapped coroutines, `gmatch`, `sort`,
hooks) at every budget point of 17 steps, persist the whole state, restore it
in a new one and compare how both end; `frozen_deferred_gc` (a queue of a
group), a host's own continuation and budget callback (not persisted until
named) and a restore that runs out of memory at many sizes.


### ADR-35: The kernel's persistence only
**Status:** accepted (2026-10-11)
Styx persists one thing: a whole state at rest, which the host persists
and restores. Scripts cannot persist, and no value is persisted or
restored apart from the state it lives in.
- The workflow: the kernel (a coroutine the host resumes) freezes or
  suspends its programs and yields back to the host; the host then
  persists the state (`luaL_persiststate`), with no Lua code running. To
  restore, `luaL_restorestate` makes a new state with the stream's seed,
  runs the host's setup, restores the state into it, and the host resumes
  the kernel from where it anchored it in the registry. A kernel running
  several programs persists them all with it.
- Why: the order of a table depends on the state's hash seed, which every
  string and table of the state shares, the kernel's and the programs'
  alike. A value restored into a state with another seed cannot keep its
  order, and a state cannot change its seed while anything in it may be
  iterating. A whole state restored into a state made for it keeps every
  order: the restore makes the state, so the seed cannot be wrong, and the
  host has no step to forget. Pluto and Eris persist values and rebuild
  every table on restore: a coroutine suspended in a `pairs` loop over a
  table with inserts and deletes, persisted after 40 keys and restored in
  the same state, skipped or repeated keys in 100 of 100 trials with each
  (Eris on Lua 5.2: 5684 keys skipped and 1089 repeated in all; Pluto on
  Lua 5.1: 5774 and 1095).
- Gone: `luaL_persistvalue`, `luaL_unpersistvalue`, `luaL_unpersiststate`
  and `luaL_newstatefor`, and the script library (`require "styx"`,
  ADR-8). The round trip of one value stays inside the engine for the
  tests of the serializer (`lua_styxpersistvalue`, `lua_styxunpersistvalue`
  and the module the test scripts require as `styx`): a state is persisted
  as the values it holds, so each kind of value is tested on its own there.
- Tested: `kernel_persistence` runs a kernel coroutine with three user
  programs (tables with churn iterated by `pairs`, object keys, closures,
  a load with a reader, a sort with a comparator, a match), each with its
  budget, frozen in their callbacks or suspended; the host persists the
  whole state at every one of the kernel's yields, restores it with
  `luaL_restorestate` and resumes the kernel, and the original and the
  copy end as a run with no persist; `kernel_in_another_process` restores
  it in another process (its own addresses: a table keyed by library
  functions keeps its order there), replacing `processes.lua`.
  `restore_errors`, `restore_takes_the_seed`, `no_script_library`.
- A kernel keeps its programs clear of what Styx cannot persist (a
  userdata with no `__persist`, a C function or continuation with no
  name, a hook set from C). `debug.persistable(v [, permanents])` tells it
  before the host persists: true, or false and why, with the path of what
  cannot be written ("persisting a userdata value is not supported yet (at
  root<stack 2>)"), so that a failure in production names the program and
  what it holds. It is the host persist's walk with a writer that keeps
  nothing: no Lua code runs and nothing is allocated in the state; threads
  running now (the kernel's own) are taken as the host will find them, at
  rest. (`debug.detached`, which checked a thread for a persist apart from
  its state, is gone.)

## Open questions

- Format: versioned header or bare bytes.
- Whether whole-state snapshot is a root kind of Styx or a separate
  library built on the same core.
