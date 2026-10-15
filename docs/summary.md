# Project summary — C++ → Java portability effort

## Goal

A C++ fork of Lua 5.5 (`cpp/` on branch `lungolua`), being methodically transformed with
one end goal: make every construct in the sources translate line-by-line into
Java, which is the constraining target (no unions, no pointer arithmetic, no
interior pointers, no operator overloading). The original `.c` files are now
`.cpp`.

Memory-footprint parity with upstream Lua is explicitly **not** a goal —
representability is. The guiding idea is that when the port to Java happens,
it should be a near-mechanical transcription rather than a redesign.

A second goal is that a `lua_State` must be fully serializable and
deserializable; that work is Styx, now its own library in
[payonel/styxlua](https://github.com/payonel/styxlua) (both ports, on its
`master`), whose design record is its
`docs/styx.md`. It drives the
function-callback design toward object identities (`CFuncInfo`) instead of
raw function pointers.

The remaining work is tracked in [tasks.md](tasks.md).

## Layout and packages

Branch `lungolua` of payonel/lua (a fork of lua/lua, whose `master` stays
upstream's) holds both ports:

- `cpp/`: the C++ engine and its test programs (`make -C cpp`, `make -C cpp
  full`).
- `java/`: LungoLua, its transcription to Java ([java-port.md](java-port.md)).
- `tests/`: the test scripts both run. Each build tests in its own copy
  (`cpp/run`, `java/run`, `java/run-debug`; `RUN_DIR` names another), so
  they can run at once.

Each push to `lungolua` (but one that changes only `docs/`) publishes to the
repository's GitHub Packages, as `1.1.<the number of commits on the
branch>-<the commit's first 8 hex digits>` (so a later commit's version is
higher, as Gradle and Maven order versions):
`com.payonel:lungoluacpp` and `lungoluacpp-tests`
(`.github/workflows/lungoluacpp.yml`), and `com.payonel:lungolua` and
`lungolua-tests` (`lungolua.yml`). A version names the same commit in all
four.

## Ground rules

- Little-endian only, ASCII only, no logic macros.
- The Lua test suite (`cpp/ltests.cpp` and `tests/all.lua`) is immutable unless
  explicitly told to adjust it. Additional custom testing goes in
  `cpp/test_public_api.cpp` and `tests/deep/*.lua`.
- Every commit is independently verified by `make full` (release + debug,
  asserts live in both).
- Lua-visible language behavior does not change.
- **Never add new comments to the source code.** Only comments inherited from
  upstream Lua master are kept; any agent- or bot-added comment must be
  removed.

## Terminology and the boundary

- **Consumer code** — code compiled against the SDK that drives the engine as
  a library, e.g. `test_public_api.cpp`. Post-port this is Java application
  code.
- **Script code** — Lua code run by the engine, e.g. the `tests/deep/*.lua`
  suite.
- **Boundary** — the separation between consumer/script code and the internal
  engine.
- **Host** — the platform outside the VM that runs the engine: a C++
  application (lungoluacpp) or a JVM application (lungolua). It drives the
  engine through the C++ or Java API.
- **Firmware** — layer 0: script code that runs with no budget in effect
  (thread zero and the threads it runs unbudgeted). It sets the budgets of
  the user threads, freezes, thaws and persists them, and sandboxes them; it
  sits between the host and the user threads.
- **User thread** — layer 1 and above: a thread that runs under a budget.
  `user_co` is the root user thread, the one its budget was set on. What
  runs in user threads (an embedder's operating system and programs) is the
  embedder's business: the engine only tells firmware from user threads.

Post-port, the engine keeps its C byte-level behavior end to end: all parsing
and all strings stay `byte[]` (UTF-8), null-terminated, enforced by
`char*`/`byte[]` in Java rather than `java.lang.String`. The internal string
system operates byte-for-byte as it did in C. Only **consumer code** speaks
`HostString` — a wrapper around a Java `String` (UTF-16). The boundary is
exactly that line between UTF-16 Java strings on the consumer side and the
internal UTF-8 `byte[]` system on the engine side: text crosses it with
strict UTF-8 ↔ UTF-16 conversion and never penetrates the engine as UTF-16.

## History

The branch forks upstream Lua 5.5 master at `53b41d0c` (2026-06-03). The
work so far, in roughly the order it landed:

### 1. C++ conversion and control flow

- Sources renamed `.c` → `.cpp`, `#pragma once`, unifdef'd, clang-formatted.
  An upstream bug in `OP_VARARGPREP`'s IT bit was fixed along the way.
- Every `goto` removed: retry loops, lexer escape actions, match/format
  bodies, frame transitions. The interpreter's jump-table dispatch became a
  plain `switch`; `pc`/`savedpc` became integer indices.
- `stdarg`/`va_list` purged: internal error formatting uses typed `FmtArgs`
  appends; `lua_pushfstring`/`luaL_error` carry arguments on the Lua stack;
  `lua_gc` is no longer variadic; `l_sprintf` became an explicit
  `string.format` directive engine.
- Struct-hack flexible arrays replaced by separately allocated arrays; the
  chunk format became field-wise fixed-width serialization (upstream
  format-0 framing restored on top).
- Numeric representation pinned: `lua_Number` is IEEE binary64,
  `lua_Integer` is 64-bit, the radix character is `.`, float↔string uses
  `std::from_chars`/`to_chars` and Lua's own hex-float code.
- Lua errors propagate as C++ exceptions (`LuaError`) with depth-matched
  recovery; the `jmp_buf` machinery is gone.

### 2. Unions and the value representation

- `union Value` became a two-column struct `{prim, ref}`; typed accessors are
  the representation seam. The Lua stack was transposed into parallel
  prim/ref/tag columns, then returned to a `TValue` array behind the same
  slot accessors for C++ performance (`396e7bb5`); the accessors stay the
  seam a Java port stores columns behind. `lapi` index resolution was
  redesigned around by-copy reads and discriminated addresses (Value
  steps C1–D).
- `StkId` is an index, not a pointer. Upvalues reference the stack by index;
  the to-be-closed list is an explicit `StkId` array on `lua_State`.
- `GCUnion` was replaced by C++ inheritance (`struct X : GCObject`); the
  `Closure`, `Node`, `CallInfo`, `expdesc`, `Vardesc` and `SemInfo` unions
  were flattened; alignment unions became `alignas`.
- Hash-part addressing uses node indices; `lcode` indexes `code[]` by `int`;
  the `UpVal**` upvalue reference became value/index access.
- Endianness is fixed by `static_assert` and `std::endian`.

### 3. Strings and the host boundary

- `InternalString` (engine bytes) and `HostString` (consumer text) were
  separated; `HostString` exists only at the public API.
- All string literals moved to `lit<>`/`literals.h` constants (15-part
  migration). The engine walks text via `byte_view`/`byte_span` plus
  offsets; `std::string_view` and `char*` cursors are gone from the core.
- String interning uses an intrusive table with lazy long-string hashing;
  `strcache` was deleted.
- All host OS calls are isolated in `os_api.*`; locale-dependent logic in
  `locale_util.*`. `liblua.a` is sealed to the published API surface, and
  `test_public_api.cpp` plus `tests/deep/*.lua` pin the consumer- and
  user-visible behavior (verified against a stock lua-master build).
- C functions carry identity through `CFuncInfo` (static for library
  functions, GC-managed otherwise), so `lua_topointer` and table keys work
  without raw function-pointer identity. A `CFuncInfo` holds only the
  function (a method handle in Java); it has no name. Persistence keys come
  from the caller's permanents table (see Styx's `docs/styx.md`).

### 4. Macro removal

Effectively every logic macro was converted: value/tag/type tests, GC
helpers, instruction encode/decode, VM code templates, service headers,
`lctype` classes, version/limit/opcode constants (`constexpr`).
`lua_assert` became plain `assert`, enabled in release builds. Only
build/platform configuration `#define`s remain (`luaconf.h`, `llimits.h`,
`lhack.h`).

### 5. Memory ownership

- `lua_Alloc` was reshaped into a permission hook: the engine owns memory and
  the host only approves or denies each request.
- Allocation is typed: per-type GC object allocators and frees, and one
  central allocation function per array element type (`luaM_alloc_X_span`,
  with `safegrow` wrappers that fold in limit/error text).
- Size fields such as `asize`, `lsizenode`, the seven `Proto` size ints,
  `nupvalues` and `nuvalue` were replaced by owning spans. A detour through
  `std::vector` was tried and reverted in favor of spans with central
  accounting.
- Casts were tightened: C-style pointer casts became `static_cast`; GC
  upcasts, interior-pointer strips, raw mem-ops and the `LX` wrapper were
  removed; pointer arithmetic is confined to `bytes.h`.

### 6. Class staging (started)

The first moves toward the Java class layout have landed: `Table` owns the
`luaH_*` operations as methods with its `static` helpers as private methods,
and the `lstring`/`lfunc` operations moved onto `TString`, `Udata`, `Proto`,
`UpVal`, `CClosure`, `LClosure` and `CFuncInfo`. Method names still carry
their C prefixes.

### 7. Port-prep plan (`docs/tasks.md`, L → M → C)

The plan ran to completion. Each task was committed separately, with
`make full` passing and an A/B check where the task touched a hot path.
Decisions and measurements are recorded per task in `tasks.md`.

- **Line changes (L1–L12):**
  - Pointer-to-pointer list walks became `GCListed` sentinel heads.
  - Out-parameters became result structs.
  - `void*` callbacks became interfaces (`ProtectedCall`, `lua_Reader`,
    `lua_Writer`, `lua_Alloc`, `lua_WarnFunction`).
  - Raw byte operations went into `bytes.h`, and addresses-as-values
    into identity helpers.
  - Lua-visible widths were pinned as named constants.
  - Integer arithmetic became signed, with named wraparound helpers.
    `StkId` became signed (`ptrdiff_t`), with negative sentinels and
    upstream's overflow-safe stack checks.
  - ctype calls go through `locale_util`, and time functions and dynamic
    loading through `os_api`.
  - Chunk I/O became typed spans, keeping fixed-buffer zero-copy
    loading.
  - Casts dropped from 985 to 550.
- **Memory changes (M1–M6):**
  - A limit audit found parity with upstream. The one recorded
    deviation (`MAXASIZE` 2³⁰) is not visible to Lua.
  - Accounting uses named per-unit cost constants (frozen at the
    previous sizes), so every allocation is charged.
  - Table storage is reached only through value accessors, and `TValue`
    copies are compiler-enforced to be named operations.
  - Userdata holding native state uses `lua_UserObject` payloads, and
    the extra space became a typed slot.
  - Byte storage is split into `byte_owner` (owns), `byte_span`/
    `byte_view` (borrow) and `byte_inline<N>` (inline scratch). The split
    sped up formatting paths by up to 31%, and an AddressSanitizer pass
    found and fixed two latent memory bugs.
- **Class changes (C0–C12):**
  - Every file sits in a package namespace (`lua::bytes`, `object`,
    `gc`, `runtime`, `vm`, `compiler`, `api`, `lib`, `platform`,
    `host`).
  - Upstream's prefixes name the classes.
  - The public API keeps its shape, with C++ linkage and the archive
    seal adapted to it.
  - The `LUAI_*` macros are gone.
  - The final audit re-ran every task's acceptance check and wrote the
    C++ → Java port checklist.
  - Renaming (C13) was left out on purpose.
