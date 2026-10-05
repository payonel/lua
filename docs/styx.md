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

- `lstyx.cpp`: the library (`lua::lib`, `luaopen_styx`). Scaffold only:
  `persist`, `unpersist` and `settings` currently raise "not implemented".
- `tests/styx/`: the Styx tests.
  - `styx_tests.cpp`: C++ tests (using the C API; internals are reachable
    since the test links the raw objects) plus a runner that executes every
    `tests/styx/*.lua` in a fresh state.
  - `*.lua`: Lua script tests. Each starts with `require "styx"`.
- `make test-styx` builds `test_styx` and runs it from `tests/styx`.
  It is deliberately not part of `make test` / `make full` while Styx is
  built test-first and red tests are expected.
- Styx is not part of the standard library set: `luaL_openlibs` does not
  open it, and a default state has no `styx`. Hosts and tests open it
  explicitly (`luaL_requiref(L, "styx", luaopen_styx, 1)`).

## Development approach

- The first versions are intentionally incomplete and non-working.
- Test-driven, on both fronts: each capability starts as a failing test,
  written as a Lua script and as a C++ test, then the implementation is made
  to pass it. One issue at a time.
- Suggested order of capabilities, each with its own tests:
  1. nil, booleans, numbers (integers and floats, exact round trip)
  2. strings (short, long, embedded zeros)
  3. tables (array part, hash part, nested)
  4. shared references and cycles (identity preserved)
  5. metatables and `__persist`
  6. permanents
  7. Lua closures, upvalues (shared between closures), prototypes
  8. coroutines
  9. userdata
  10. error reporting with a path to the offending value
- Items are added or reordered as the tests teach us something.

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
solution. The only exception is the interim layer of ADR-7.

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

### ADR-7: Interim auto-loading of the standard libraries
**Status:** accepted (temporary)
Even the simplest test needs the library permanents solved first, so for
now the standard libraries are loaded automatically on unpersist. This
layer is to be removed later, test-first, in favor of ADR-3.

### ADR-8: Styx is not in the default state
**Status:** accepted
`luaL_openlibs` and `linit` do not open Styx. Tests and hosts load it
explicitly through `luaopen_styx`.

### ADR-9: Tests are Lua scripts and C++, in `tests/styx/`
**Status:** accepted
Every capability is covered from script and from C++ for careful testing,
in a dedicated test subdirectory run by `make test-styx`.

## Open questions

- Format: versioned header or bare bytes.
- Whether whole-state snapshot is a root kind of Styx or a separate
  library built on the same core.
- Exact `settings` list and defaults.
