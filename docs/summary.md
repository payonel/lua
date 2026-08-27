# Project summary — C++ → Java portability effort

## Project overview

A C++ fork of Lua 5.5 (branch `lua-c++`), being methodically transformed with
one end goal: make every construct in the sources translate line-by-line into
Java, which is the constraining target (no unions, no pointer arithmetic, no
interior pointers, no operator overloading). The original `.c` files are now
`.cpp`.

Memory-footprint parity with upstream Lua is explicitly **not** a goal —
representability is. The guiding idea is that when the port to Java happens,
it should be a near-mechanical transcription rather than a redesign.

Beyond the C→Java roadmap, a major project goal is that a `lua_State` must be
fully serializable/deserializable — this drives the function-callback design
toward stable registry identifiers.

**Ground rules:** little-endian only, ASCII only, no logic macros, the Lua
test suite (`ltests.cpp` and `tests/all.lua`) is immutable unless explicitly
told to adjust it, every commit independently verified by `make full`
(release + debug, asserts live in both). Additional custom testing goes in
`test_public_api.cpp` and `tests/deep/*.lua`.

**Never add new comments to the source code.** Only comments inherited from
upstream Lua master are kept; any agent- or bot-added comment must be
removed. Fixes applied from this doc must not introduce explanatory comments.

### Terminology and the boundary

- **Consumer code** — code compiled against the SDK that drives the engine as
  a library, e.g. `test_public_api.cpp`. Post-port this is Java application
  code.
- **User code** — Lua script code run by the engine, e.g. the
  `tests/deep/*.lua` suite.
- **Boundary** — the separation between consumer/user code and the internal
  engine.

Post-port, the engine keeps its C byte-level behavior end to end: all parsing
and all strings stay `byte[]` (UTF-8), null-terminated, enforced by
`char*`/`byte[]` in Java rather than `java.lang.String`. The internal string
system operates byte-for-byte as it did in C. Only **consumer code** speaks
`HostString` — a wrapper around a Java `String` (UTF-16). The boundary is
exactly that mote between UTF-16 Java strings on the consumer side and the
internal UTF-8 `byte[]` system on the engine side: text crosses it with
strict UTF-8 ↔ UTF-16 conversion and never penetrates the engine as UTF-16.

## Open cleanup tasks

Each of these is independent; pick any one up without needing the others.
The exception is task 0 below — the C++ namespace/class staging — which
touches nearly every file and should be coordinated with whichever of
tasks 1-6 are in flight so diffs don't collide.

### 0. C++ namespace/class staging for the Java port shape

**Effort:** very high (whole-codebase) · **Risk:** medium — mechanical by
design, but touches every file; must land in reviewable increments, each
independently verified by `make full`

**Goal:** reorganize the *existing C++* source into namespaces and classes
that are a near-mechanical stand-in for the eventual Java package/class
layout, so the actual Java port becomes a transcription rather than a
redesign. This is a pure staging move — no behavior change, no Lua-visible
change, and performance-critical call shapes (inlining, hot-path dispatch)
must be preserved exactly; only the *organization* of the code moves.

**Package/namespace layout** (`lua::x` in C++ → `lua.x` in Java):

- `lua::runtime` — `LuaState`, `GlobalState`, `CallInfo`, the collapsed
  `TValue`/`Value` representation.
- `lua::object` — the `GCObject` hierarchy: `LuaObject` (base), `LuaString`,
  `LuaTable`, `LuaUserdata`, `LuaClosure`/`CClosure`/`LClosure`, `Proto`,
  `UpVal`, `CFuncInfo`. This one is close to free — `lobject.h`/`lstate.h`
  already declare these as `struct X : GCObject`.
- `lua::vm` — `Interpreter` (the `lvm.cpp` dispatch `switch`, kept as a
  switch — closest to today's shape, best for hot-loop inlining, matches the
  project's line-by-line-fidelity preference over a handler-per-opcode
  redesign), `OpCodes` (opcode metadata from `lopcodes.cpp`), `Debug`
  (`ldebug.cpp`'s `luaG_*` — internal error/traceback/line-info machinery;
  keep distinct from `lua::lib::DebugLib`, the `debug.*` Lua-visible
  library, despite the shared C prefix).
- `lua::gc` — `GarbageCollector` (`lgc.cpp`'s incremental/generational state
  machine), separate from the `LuaObject` hierarchy it operates on.
- `lua::compiler` — `Lexer`, `Parser`, `CodeGen` from `llex.cpp`/
  `lparser.cpp`/`lcode.cpp`.
- `lua::lib` — one class per stdlib file: `BaseLib`, `StringLib`, `TableLib`,
  `MathLib`, `OsLib`, `IoLib`, `CoroutineLib`, `DebugLib`, each registering
  its functions through the `CFuncInfo` identity layer (ties to task 2,
  function-callback port design — the registration-before-load contract
  plugs in here).
- `lua::api` — the public embedding surface: `lapi.cpp` → `LuaState`'s public
  methods, `lauxlib.cpp` → `LuaAuxLib`. Only this namespace touches
  `HostString`.
- `lua::bytes` — `byte_view`/`byte_span` and the `Zio` buffered-reader
  (`lzio.*`). Shared infrastructure used by the compiler, `lundump`, and
  stdlib scratch buffers alike — doesn't belong to any one of the packages
  above.
- `lua::platform` — `os_api.*`. The designated per-host swap seam (task 4):
  dlopen, file I/O primitives, `getenv`. Kept visibly separate from
  `lua::lib` because this is the one namespace expected to look completely
  different in the Java port; everything else should transcribe near
  verbatim.
- Top-level / `lua::host` — `lua.cpp` (the CLI) becomes the launcher, not
  engine code.

**Visibility mapping**, applied per function during the move:

- `static` (translation-unit-private, ~500 today) → `private` method on
  whichever class now owns the operation. Mechanical in most cases —
  `ltable.cpp`'s `static` helpers all take a `Table*` first arg already, so
  they become private `LuaTable` methods with that parameter dropped.
- `LUAI_FUNC` (engine-internal, cross-file, ~250 today) → package-private
  (default access) — that is exactly its current scope.
- `LUA_API` (public C API, 123 in `lua.h`) → `public`, on `LuaState`/
  `LuaAuxLib`. This is where the public API shape is allowed to change
  — see the note below.
- Free functions whose first argument is always a specific type (`lmem.cpp`'s
  `luaM_*`, which all take `lua_State*`/`global_State*`) become methods on
  that type directly — `LuaState::alloc(...)`, etc. — not a generic utility
  class. No new namespace needed for these; they land in `lua::runtime`.

**Performance constraint:** this is a reorganization, not a rewrite. Methods
that are `constexpr`/`inline`/hot-path today stay that way after the move;
class-method dispatch must not introduce virtual-call overhead where the
call was static/direct before (e.g. `Interpreter`'s dispatch loop and
`LuaTable`'s hot accessors should end up as concrete, non-virtual methods,
not funneled through the `GCObject`/`LuaObject` base's virtual interface).

**Public API surface is allowed to change shape** as part of this move
(namespacing, `lua_State*`-first-arg → `LuaState::method()`, etc.) — this is
an accepted, intended consequence, not a regression to avoid. It requires
updating `test_public_api.cpp` and any other embedder-facing test/consumer
code to the new call shapes.

**What must not change:** Lua-visible language behavior. `tests/deep/*.lua`
and `tests/all.lua` (user-code-level) need zero changes. `ltests.cpp` is
nominally immutable per the ground rules, but it pokes at engine internals
directly (debug hooks, introspection) and will need **mechanical symbol/call
-shape updates only** (renamed/relocated function calls, no behavior change)
to keep compiling against the new namespace layout — flag each such edit
explicitly rather than treating the immutability rule as silently waived.

### 1. Locale surface

**Effort:** medium · **Risk:** medium (touches immutable test assertions)

Pattern classes (`%a`/`%l`…), `string.upper`/`string.lower`, and string `<`
collation currently lean on libc `ctype`/`strcoll` (`lctype.cpp`/`.h`,
`locale_util.cpp`), which are locale-sensitive and asserted by the immutable
tests under `pt_BR`. Java has no process-global `setlocale`.

```cpp
locale_util.cpp:45  std::optional<byte_view> locale_setlocale(int category, std::optional<byte_view> locale)
locale_util.cpp:51    res = setlocale(category, l.c_str());   // libc call, needs NUL-terminated arg
```

Needs an engine-owned byte-classification/collation table plus an explicit
threaded "current locale" field, replacing the libc `ctype`/`strcoll` calls.
`locale_util.cpp`'s `std::string` use (line 50-51) is a libc-boundary
artifact of this and folds away once the libc call is replaced.

### 2. Function-callback port design

**Effort:** medium · **Risk:** low (shapes are already Java-portable; this is about persistence, not portability)

The `CFuncInfo` identity layer is in place. Remaining pieces, all needed so a
`lua_State` can be serialized with live callback references:

- `lua_KFunction`/`lua_KContext` continuation identifiers — currently raw
  function pointer + `ptrdiff_t` context (`lua.h:86,96`; used at
  `ldo.cpp:762`, `lua_yieldk`). Needs the same stable-identity treatment
  `CFuncInfo` gave `lua_CFunction`.
- `lua_Alloc` replacement — currently a raw function pointer
  (`lua.h:105,127,336`).
- `CClosure` function-slot conversion (`lobject.h:366`) — still holds a raw
  callable slot rather than a serializable reference.
- The registration-before-load contract (a name→callback registry so a
  deserialized chunk resolves callbacks by stable id instead of by raw
  pointer) — not yet designed.

### 3. Serialization layer

**Effort:** high · **Risk:** high (the big design item)

Serialize/deserialize a complete `lua_State` (globals, string table, tables,
protos, upvalues, suspended coroutines). The chunk byte format is already
fixed (see item 4 below and the completed bytecode-serialization work), so
this is the remaining big design item, gated on item 2's registry contract
for anything holding a callback reference.

### 4. `loadlib`/`os_api` abstraction seam (native library loading)

**Effort:** high (design) but low-risk, mechanical once decided · **Risk:** low for shape, high for the native reality

`require` of a C module (`.so`) walks `loadlib.cpp`, which asks the OS layer
(`os_api.cpp`) to `dlopen`/`dlsym` and calls the result as a `lua_CFunction`.
There is no `dlopen` for the JVM and a `.so` is not loadable as a Lua module
the way it is in C — this is the one place the engine pulls native machine
code in at runtime.

The native-boundary punning in `os_api.cpp` is correct where it lives:

```cpp
OsLib* os_dlopen(byte_view path, bool global)
{ return reinterpret_cast<OsLib*>(dlopen(p.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL))); }
void* os_dlsym(OsLib* lib, byte_view sym) { return dlsym(reinterpret_cast<void*>(lib), s.c_str()); }
void os_dlclose(OsLib* lib) { dlclose(reinterpret_cast<void*>(lib)); }
```
`OsLib` is an opaque incomplete type; at the Java port this file is
reimplemented (not transcribed) against JNI/FFM `SymbolLookup` or a registry
of statically-linked extensions, and the handle becomes a Java object.

The still-open part is that the **policy layer** (`loadlib.cpp`) still holds
native-typing casts that belong behind the `os_api` seam:

```cpp
loadlib.cpp:45   OsLib* lib = os_dlopen(path, seeglb != 0);
loadlib.cpp:48   return reinterpret_cast<void*>(lib);                 // handle punned to void* for storage
loadlib.cpp:53   lua_CFunction f = reinterpret_cast<lua_CFunction>(os_dlsym(reinterpret_cast<OsLib*>(lib), sym));
loadlib.cpp:158  os_dlclose(reinterpret_cast<OsLib*>(*p));
```
Plus the handle is boxed twice — as full userdata with a `__gc` that calls
`os_dlclose`, and as light userdata in `registry._CLIBS[path]` for dedupe
(`lib_gc`, `createlibstr`, `addtoclib` in `loadlib.cpp`). In Java the handle
is just an object reference; `__gc`-driven `dlclose` becomes explicit
close/phantom-reachability cleanup, and `_CLIBS` is an ordinary map.

**Proposed direction:** make `loadlib` as abstract as possible — it should
express only policy (search paths, `_CLIBS` dedupe, error messages, the
registration/`__gc` lifecycle) and delegate every native act (open, resolve a
symbol to a callable, close) to `os_api`. Move the `reinterpret_cast`s and
handle typing out of `loadlib.cpp` into `os_api`, and widen the `os_api` dl
interface so `loadlib` never holds a raw handle. This makes porting
`loadlib` to Java a straight transcription — only `os_api`'s dlopen family
gets reimplemented against the JVM (a likely target: a registry of
pre-registered native extensions, so `require` of a C module resolves a name
to an already-bound Java `LuaCFunction` rather than dlsym-ing raw code at
runtime). Gated behind wanting to touch the loader — not urgent on its own.

### 5. `lundump` byte-reinterpretation (memmap abstraction)

**Effort:** medium · **Risk:** medium

The undump path hands out a `std::span<T>` over a raw byte region by casting
the byte address to `T*` — three sites in `lundump.cpp`:

```cpp
lundump.cpp:187  f->code = std::span<Instruction>((Instruction*)(getaddr_(S, n * sizeof(Instruction))), n);
lundump.cpp:284  f->lineinfo = std::span<ls_byte>((ls_byte*)(getaddr_(S, n * sizeof(ls_byte))), n);
lundump.cpp:297  f->abslineinfo = std::span<AbsLineInfo>((AbsLineInfo*)(getaddr_(S, n * sizeof(AbsLineInfo))), n);
```

Java `byte[]` cannot be reinterpreted as `int[]` or as an array of structs;
this needs an explicit mapping layer. Two distinct sub-problems:

- **`byte[]` → primitive integral** (`code`, `lineinfo`) — solvable with
  `java.nio.ByteBuffer`/`.asIntBuffer()` or `java.lang.foreign.MemorySegment`
  with a `JAVA_INT` value layout. `ls_byte` (a `signed char`) is trivially
  the bytes themselves.
- **`byte[]` → struct array** (`abslineinfo`, `AbsLineInfo = {int pc; int
  line;}`) — Java can't overlay an object on raw bytes, but since the struct
  is exactly two `int32_t`, this reduces to the primitive-integral problem
  plus a stride-2 access convention (`[pc0, line0, pc1, line1, ...]`); the
  `AbsLineInfo` object is materialized on demand rather than overlaid.

**Confirmed by scan:** `Instruction` (`l_uint32`) unsignedness is used only
in the bit-container sense (logical right shift + mask, `lopcodes.h`'s
`MASK1`/`GET_OPCODE`/`getarg`), never as a signed-vs-unsigned magnitude or
comparison. So Java can store an `Instruction` in a signed `int` and get
identical results **provided every decode site uses `>>>`** instead of `>>`
(Java's `>>` sign-extends; C's shift on unsigned `Instruction` is logical).
This is a per-decode-site rule, not a representation blocker. Same
bit-container usage pattern elsewhere for `l_uint32` (UTF-8 decode sentinels,
call-status flags) — no changes needed there beyond the same `>>>` rule at
the eventual port.

**Proposed direction (not now):** a memmap abstraction on the Java side — a
thin little-endian reader over the backing `byte[]` yielding `int`, used for
`code` (decoded with `>>>`) and `abslineinfo` (int-pairs); `lineinfo` stays
raw bytes. Mirror the existing defined-width byte format so the reader is
format-driven, not layout-driven. **Follow-up for the port checklist:** audit
that every Instruction right-shift in the Java port uses `>>>` — the C
correctness here rides on `Instruction` being unsigned, and that guarantee is
exactly what's lost crossing to Java's signed `int`.

### 6. SIGINT / `volatile` interruption model

**Effort:** low (fields already correct) / medium (signal wiring) · **Risk:** low

`volatile` fields drive the async-interruption (SIGINT) model and one file-close
guard:

```cpp
lstate.h:99    volatile l_signalT trap;      /* function is tracing lines/counts */
lstate.h:213   volatile lua_Hook hook;
lstate.h:220   volatile l_signalT hookmask;
liolib.cpp:118 volatile lua_CFunction cf = p->closef;  /* avoids longjmp/close reorder */
ldo.cpp:49     volatile TStatus status;                /* in lua_longjmp */
```
These map cleanly to Java `volatile` (async writer, polling reader) — no
change needed to the fields themselves. The C-ism is the writer: `lua.cpp`
still installs a real OS signal handler (`setsignal(SIGINT, laction)` at
`lua.cpp:145`, restored at `:147`), which the VM polls `trap` for at
dispatch. The engine can't install OS signal handlers in the port (no engine
threads on the JVM side).

**Proposed fix:** replace the OS signal handler with a host-invoked interrupt
entry point; the engine never installs a signal handler itself.

```cpp
// engine side: unchanged polling of L->trap in the dispatch loop
// host side (replaces laction/SIGINT in lua.cpp):
void luaState_interrupt(lua_State* L);   // sets hook flags; host wires it to SIGINT if it wants
// Java: volatile fields on LuaState; host calls luaState.interrupt()
```
`liolib.cpp:118`'s `volatile` guards against reordering around the C++
throw/close path; re-evaluate once error handling is fully exception-clean —
it likely becomes an ordinary field in Java.

## Accepted design notes (not tasks)

### Config macros with pointer puns (`lhack.h`, debug-only)

**Status: no action required now** — flagged for awareness only, not a task.

The only remaining `#define`s in the tree are build/platform config, which is
in scope to keep. Two are worth noting because they contain C-isms that would
need handling *if* they ever reached the port surface — but both are the
debug/host-hook layer and the compiler-hint layer, both dropped/replaced
wholesale at the port rather than transcribed:

```cpp
lhack.h:22  #define _getlock(l)   (*((uint64_t*)(lua_getextraspace(l))))
lhack.h:23  #define _getplock(l)  (ismainthread(l) ? ((uint64_t*)(lua_getextraspace(l))) \
                                                   : ((uint64_t*)(*((void**)(lua_getextraspace(l))))))
lhack.h:29  #define lua_lock(l)   assert((*_getplock(l))++ == 0)
lhack.h:30  #define lua_unlock(l) assert(--(*_getplock(l)) == 0)
```
These cast `lua_getextraspace(l)` to `uint64_t*`/`void**` — pointer punning
into a raw buffer, existing only to assert the no-reentrancy contract in
debug builds. At the port, `lua_lock`/`lua_unlock` and the extraspace
punning are dropped entirely — the no-threads constraint means the lock
contract is a no-op, or at most a plain boolean/reentrancy-assert field on
`LuaState` with no pointer punning:

```cpp
// Java: assert reentrancyGuard++ == 0;  ...  assert --reentrancyGuard == 0;
```

```cpp
luaconf.h:20  #define luai_likely(x)   (__builtin_expect(((x) != 0), 1))
luaconf.h:21  #define luai_unlikely(x) (__builtin_expect(((x) != 0), 0))
```
`__builtin_expect` has no Java equivalent and simply disappears at the port —
branch hints are the JIT's job.
