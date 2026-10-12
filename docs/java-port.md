# Java port

The C++ engine on `lua-c++` was shaped so that it transcribes to Java line
by line ([tasks.md](tasks.md): phases L, M, C, R and J, and the port
checklist in C12). This branch, `lua-java`, holds that transcription. The
C++ sources stay in the tree as the reference until the Java engine passes
the same tests; the Java sources are under `java/`.

## Rules

The rules of [summary.md](summary.md) and [tasks.md](tasks.md) hold, with
these for the port itself:

- **Transcribe, do not redesign.** Each C++ file becomes one Java class in
  the package of its namespace, with the same function names, the same
  order and the same comments (upstream's). A construct changes only where
  Java has no form for it, and then as the port checklist (tasks.md C12,
  Phase J) says.
- **The C++ is the specification.** Where the Java and the C++ disagree,
  the Java is wrong, unless a test shows the C++ is.
- **Same behavior, same tests.** The Java engine runs the same Lua tests
  (`tests/all.lua`, `tests/deep`, the Styx tests, the gates). No Lua-visible
  difference is allowed.
- **Every allocation is accountable**, as in the C++: the Java engine
  charges what the C++ charges (the `cost::` constants), whatever the JVM
  really uses.

## Layout

| C++ namespace | Java package | Classes (one per C++ file; types of a header are classes of their own) |
|---|---|---|
| `lua::bytes` | `lua.bytes` | `ByteView`, `ByteSpan`, `ByteOwner`, `Bytes` (bytes.h); `InternalString`, `FmtScan` (internal_string.h); `Lt` (literals.h); `Lctype`; `Lzio` with `ZIO`, `MBuffer`; `Lload` |
| `lua::object` | `lua.object` | `TValue`, `GCObject`, `GCListed`, `TString`, `Udata`, `Proto`, `UpVal`, `Closure`, `CClosure`, `LClosure`, `CFuncInfo`, `Table`, `Node`, `BudgetGroup` and the debug-information records (lobject.h); `Lobject`; `Llimits`; `Lstring`; `Lfunc`; `Lnames` |
| `lua::runtime` | `lua.runtime` | `lua_State`, `global_State`, `CallInfo` (lstate.h); `Lstate`; `Lmem` with `Cost`; `Ldo`; `Ltm` |
| `lua::gc` | `lua.gc` | `Lgc` |
| `lua::vm` | `lua.vm` | `Lvm`, `Ldebug`, `Lopcodes` |
| `lua::compiler` | `lua.compiler` | `Llex`, `Lparser`, `Lcode`, `Ldump`, `Lundump` and their records |
| global API (`lua_*`, `luaL_*`) | `lua.api` | `LuaApi` (lapi.cpp, lua.h), `LuaAuxLib` (lauxlib.cpp), `HostString` |
| `lua::lib` | `lua.lib` | one class per library (`Lbaselib`, `Lstrlib`, …), `Linit`, `Lstyx` |
| `lua::platform` | `lua.platform` | `OsApi`, `LocaleUtil` |
| `lua::host` | `lua.host` | `Lua` (the standalone interpreter, `main`) |
| tests | `lua.tests` | `Ltests` (the debug build's `T` library) |

Type names keep their C names (`lua_State`, `TValue`, `CallInfo`);
upstream's file prefixes stay on the functions (`Ldo.luaD_call`). Renaming
is optional and comes last (tasks.md C13).

## Transcription

The port checklist in tasks.md (C12, and Phase J's new rows) gives each C++
idiom its one Java form. The ones every file meets:

- `lua_Integer` → `long`; `lua_Number` → `double`; `lua_Unsigned` → `long`
  with `Long.compareUnsigned`, `Long.divideUnsigned`, `>>>`; `l_uint32`,
  `unsigned int` bit containers → `int`; `lu_byte` → `byte` (read with
  `& 0xFF` where the value can pass 127); `size_t` lengths, `StkId`,
  `l_mem` → `int` or `long` as the value's range needs (`l_mem` is `long`).
- A `TValue` is a Java object with the fields `prim` (`long`), `ref`
  (`Object`) and `tt` (`byte`). It is never shared: `getobj`/`copyobj`
  copy the fields. A thread's stack and a table's array are columns
  (`long[]`, `Object[]`, `byte[]`) behind the slot accessors.
- `byte_view`, `byte_span`, `byte_owner` → `ByteView`, `ByteSpan`,
  `ByteOwner` (a `byte[]` with an offset and a length); `std::optional` →
  a nullable reference (or a small result class for primitives);
  `std::span<T>` → `T[]`; `std::unique_ptr<T>` → a plain reference.
- Errors and freezes (C++ exceptions) → Java exceptions; `[[noreturn]]`
  functions throw and are called as `throw`ing statements.
- Function pointers that became interfaces (`lua_Reader`, `lua_Writer`,
  `lua_Alloc`, `lua_UserObject`, `ProtectedCall`, …) → Java interfaces or
  abstract classes; `lua_CFunction` → a functional interface.
- Inlining hints, branch hints and `lhack.h` disappear.
- A C++ name that is a Java keyword gets `_kw` (`lt::assert` →
  `Lt.assert_kw`), as the C++ did for its own keywords; `lt::x` → `Lt.x`.

## Order

1. **Skeleton (phase 0).** The headers: every type with its fields, every
   constant, every inline function, and every function a header declares
   as a stub in its class. After this the Java tree compiles.
2. **Core.** `lobject`, `lstring`, `ltable`, `lfunc`, `lmem`, `lstate`,
   `ltm`, `ldo`, `lgc`, `lvm`, `ldebug`, `lopcodes`, `lzio`, `llex`,
   `lparser`, `lcode`, `lundump`, `ldump`, `lload`, `lapi`, `lauxlib`,
   `lbaselib`, `linit`: the first goal is `print("hello")`.
3. **Libraries.** The rest of `lua::lib`, `lnames`, the platform layer.
4. **Host and tests.** `lua.cpp` as the Java `main`; `ltests`; the Lua
   suites, then the Styx tests and the gates.
5. **Styx.** `lstyx`.

Each step is a commit; the tree compiles at every commit.

## Build

`make -C java` compiles the sources with the JDK named by `JDK` (default:
the Temurin 25 under `~/.gradle/jdks`) into `java/build/lua.jar`.

## Status

| Step | State |
|---|---|
| 0. Skeleton | started |
