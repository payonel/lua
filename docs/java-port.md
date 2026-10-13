# Java port

The C++ engine on `lua-c++` was shaped so that it transcribes to Java line
by line ([tasks.md](tasks.md): phases L, M, C, R and J, and the port
checklist in C12). This branch, `lua-java`, holds that transcription. The
C++ sources stay in the tree as the reference until the Java engine passes
the same tests; the Java sources are under `java/`.

The port is named LungoLua: its packages are under `com.payonel.lungolua`
(the namespace of `com.payonel.modularcomputers`), and its jar is
`java/build/lungolua.jar`. The engine builds without its tests (the
package `com.payonel.lungolua.tests`, in `lungolua-tests.jar`): only the
debug build needs them, and it reaches them by name (below, Build).

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
  (`tests/all.lua`, `tests/deep`, the gate). No Lua-visible
  difference is allowed.
- **Every allocation is accountable**, as in the C++: the Java engine
  charges what the C++ charges (the `cost::` constants), whatever the JVM
  really uses.

## Layout

| C++ namespace | Java package | Classes (one per C++ file; types of a header are classes of their own) |
|---|---|---|
| `lua::bytes` | `com.payonel.lungolua.bytes` | `ByteView`, `ByteSpan`, `ByteOwner`, `Bytes` (bytes.h); `InternalString`, `FmtScan` (internal_string.h); `Lt` (literals.h); `Lctype`; `Lzio` with `ZIO`, `MBuffer`; `Lload` |
| `lua::object` | `com.payonel.lungolua.object` | `TValue`, `GCObject`, `GCListed`, `TString`, `Udata`, `Proto`, `UpVal`, `Closure`, `CClosure`, `LClosure`, `CFuncInfo`, `Table`, `Node`, `BudgetGroup` and the debug-information records (lobject.h); `Lobject`; `Llimits`; `Lstring`; `Lfunc`; `Lnames` |
| `lua::runtime` | `com.payonel.lungolua.runtime` | `lua_State`, `global_State`, `CallInfo` (lstate.h); `Lstate`; `Lmem` with `Cost`; `Ldo`; `Ltm` |
| `lua::gc` | `com.payonel.lungolua.gc` | `Lgc` |
| `lua::vm` | `com.payonel.lungolua.vm` | `Lvm`, `Ldebug`, `Lopcodes` |
| `lua::compiler` | `com.payonel.lungolua.compiler` | `Llex`, `Lparser`, `Lcode`, `Ldump`, `Lundump` and their records |
| global API (`lua_*`, `luaL_*`) | `com.payonel.lungolua.api` | `LuaApi` (lapi.cpp, lua.h), `LuaAuxLib` (lauxlib.cpp), `HostString` |
| `lua::lib` | `com.payonel.lungolua.lib` | one class per library (`Lbaselib`, `Lstrlib`, …), `Linit` |
| `lua::platform` | `com.payonel.lungolua.platform` | `OsApi`, `LocaleUtil`, `Libm`; `Readline` (the JVM's readline library); `Environ` (the environment as bytes) |
| `lua::host` | `com.payonel.lungolua.host` | `Lua` (the standalone interpreter, `main`) |
| tests | `com.payonel.lungolua.tests` | `Ltests` (the debug build's `T` library, behind `runtime.TestHooks`) and the test programs; `lungolua-tests.jar` |
| tests/libs | `com.payonel.lungolua.tests.libs` | `Lib1`, `Lib11`, `Lib2`, `Lib21`, `Lib22` (the test C modules), `LibDecls` (lib_decls.h) |

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

## Porting a file

These are binding for every file; when in doubt, read how the classes
already in `java/src` do it.

**Where things go.** Each C++ file (`lxxx.cpp` with its `lxxx.h`) is one
class, named in the table below; its header's constants and inline
functions are static members of the same class. A C++ type's methods
(`Table::luaH_get`, `TString::luaS_createstr`) are methods of that type's
class (instance or static, as in C++); the type classes already exist with
their fields (lobject.h, lstate.h), and only the file that defines the
methods adds them.

| C++ | Java class |
|---|---|
| lobject.h/.cpp | `com.payonel.lungolua.object.Lobject` (+ `FmtArg`, `FmtArgs`) |
| llimits.h | `com.payonel.lungolua.object.Llimits` (done) |
| lstring.h/.cpp | `com.payonel.lungolua.object.Lstring`; methods on `TString`, `Udata` |
| ltable.h/.cpp | `com.payonel.lungolua.object.Ltable`; methods on `Table` |
| lfunc.h/.cpp | `com.payonel.lungolua.object.Lfunc`; methods on `Proto`, `UpVal`, `CClosure`, `LClosure`, `CFuncInfo` |
| lnames.cpp | `com.payonel.lungolua.object.Lnames` |
| lmem.h/.cpp | `com.payonel.lungolua.runtime.Lmem` (`cost::` is `com.payonel.lungolua.runtime.Cost`, done) |
| lstate.h/.cpp | `com.payonel.lungolua.runtime.Lstate` (the slot accessors over the stack's columns) |
| ltm.h/.cpp | `com.payonel.lungolua.runtime.Ltm` |
| ldo.h/.cpp | `com.payonel.lungolua.runtime.Ldo` (`LuaError` done) |
| lhack.h | `com.payonel.lungolua.runtime.Lhack` (done) |
| lgc.h/.cpp | `com.payonel.lungolua.gc.Lgc` (+ `EphIndex`) |
| lvm.h/.cpp | `com.payonel.lungolua.vm.Lvm` |
| ldebug.h/.cpp | `com.payonel.lungolua.vm.Ldebug` |
| lopcodes.h/.cpp, lopnames.h | `com.payonel.lungolua.vm.Lopcodes` |
| lctype.h/.cpp | `com.payonel.lungolua.bytes.Lctype` |
| lzio.h/.cpp | `com.payonel.lungolua.bytes.Lzio` (+ `ZIO`, `MBuffer`) |
| lload.h/.cpp | `com.payonel.lungolua.bytes.Lload` (+ its records) |
| util_cast.h/.cpp | `com.payonel.lungolua.bytes.UtilCast` |
| llex.h/.cpp | `com.payonel.lungolua.compiler.Llex` (+ `LexState`, `Token`, …) |
| lparser.h/.cpp | `com.payonel.lungolua.compiler.Lparser` (+ `FuncState`, `expdesc`, `Dyndata`, …) |
| lcode.h/.cpp | `com.payonel.lungolua.compiler.Lcode` |
| ldump.cpp | `com.payonel.lungolua.compiler.Ldump` |
| lundump.h/.cpp | `com.payonel.lungolua.compiler.Lundump` |
| lua.h, lapi.h/.cpp | `com.payonel.lungolua.api.LuaApi` (constants done) |
| luaconf.h | `com.payonel.lungolua.api.LuaConf` (done) |
| lua_internal.h | `com.payonel.lungolua.api.LuaInternal` (its constants and inline functions; each function goes to the class of the file that defines it) |
| lauxlib.h/.cpp | `com.payonel.lungolua.api.LuaAuxLib` |
| lualib.h | `com.payonel.lungolua.lib.Lualib` |
| lbaselib.cpp, lcorolib.cpp, … | `com.payonel.lungolua.lib.Lbaselib`, `com.payonel.lungolua.lib.Lcorolib`, … |
| linit.cpp | `com.payonel.lungolua.lib.Linit` |
| os_api.h/.cpp, locale_util.h/.cpp | `com.payonel.lungolua.platform.OsApi`, `com.payonel.lungolua.platform.LocaleUtil` |
| C's `<cmath>` (`pow`, `fmod`, `floor`, `frexp`, …) | `com.payonel.lungolua.platform.Libm` (done) |
| lua.cpp | `com.payonel.lungolua.host.Lua` |
| ltests.h/.cpp | `com.payonel.lungolua.tests.Ltests` |
| test_public_api.cpp, tests/engine/engine_tests.cpp, tests/gate/gate.cpp | `com.payonel.lungolua.tests.PublicApi`, `EngineTests`, `Gate` (launchers `java/test_engine`, `java/test_gate`) |
| tests/libs/libN.cpp, lib_decls.h | `com.payonel.lungolua.tests.libs.LibN`, `com.payonel.lungolua.tests.libs.LibDecls` |

**Calls keep their C shape.** A file `import static`s the classes whose
functions it calls (`import static com.payonel.lungolua.object.Lobject.*;`), so
`ttisnil(o)`, `luaD_call(L, f, n)` and `setobj(L, a, b)` read as in C++.
Methods of the type classes are called on the object
(`t.luaH_get(key, res)`) or on the class (`TString.luaS_createstr(L, s)`).

**Types.**
- `lua_Integer`, `lua_Unsigned` → `long`; `lua_Number` → `double`;
  `int`, `unsigned`, `l_uint32` and other bit containers → `int`;
  `lu_byte`, `ls_byte`, `char` (a byte) → `byte` (read with `& 0xFF`
  where it can pass 127; tags never do); `size_t`, `l_mem`, `lu_mem` →
  `long` where the value is a memory count, else `int` (lengths, indices,
  sizes of arrays); `StkId` → `int`; `TStatus` → `byte`; `bool` →
  `boolean` (the C++ keeps every true/false in a `bool`; where one is
  stored as a number, in a stream byte or a box's slot, the Java writes
  the conversion out: `? 1 : 0`, `!= 0`).
- Unsigned operations: `(lua_Unsigned)a < (lua_Unsigned)b` →
  `Long.compareUnsigned(a, b) < 0`; unsigned `/`, `%` →
  `Long.divideUnsigned`, `Long.remainderUnsigned`; logical right shift →
  `>>>`; `(unsigned char)c` → `c & 0xFF`; conversions of a `lua_Unsigned`
  to a `lua_Number` need the unsigned value (`(double)` of a negative long
  is wrong: write the conversion out).
- A pointer to a struct → a reference to its object; `nullptr` → `null`.
  `&array[i]` → `array[i]` (the element object). Pointer comparisons →
  `==` on references.
- `std::span<T>` → `T[]`; `.size()` → `.length`; `std::vector<T>` →
  `T[]` resized as the C++ resizes it (or `ArrayList` where the C++ uses
  `push_back` freely); `std::optional<T*>`/`std::optional<byte_view>` →
  a nullable reference; `std::optional<lua_Integer>` → `OptionalLong`,
  `<lua_Number>` → `OptionalDouble`, `<int>` → `OptionalInt`, `<bool>` →
  a nullable `Boolean`; `std::unique_ptr<T>` → a reference.
- `byte_view`, `byte_span`, `byte_owner`, `byte_inline<N>` → `ByteView`,
  `ByteSpan`, `ByteOwner` (`new ByteOwner(N)`); `HostString` →
  `com.payonel.lungolua.api.HostString`; `const char*` names (registrations) → `String`;
  `InternalString` → `com.payonel.lungolua.bytes.InternalString`; `lt::x` → `Lt.x`.
- A `TValue` is never shared: a C++ struct copy (`*a = *b`, `getobj`,
  `copyobj`, `setobj`) copies fields. A C++ local `TValue v;` is
  `TValue v = new TValue();`. `getslot(L, i)` returns a fresh copy.
- Arrays of structs (`TValue[]`, `Node[]`, `LocVar[]`, `Upvaldesc[]`,
  `AbsLineInfo[]`, `Vardesc[]`, `Labeldesc[]`, …) hold one object per
  element, made when the array is made (the allocator fills new slots);
  an element assignment `a[i] = a[j]` of a struct copies the fields
  (never stores the same object twice). Arrays of pointers hold
  references (`null` default).
- A thread's stack is three columns on `lua_State` (`stack_prim`,
  `stack_ref`, `stack_tt`), reached only through the slot accessors in
  `Lstate` (`getslot`, `setslot`, `copyslot`, `slotival`, `setsloti`, …).
  A table's array part is `array_prim`, `array_ref`, `tags`, reached
  through ltable.h's accessors. A `Node` is a `TValue` (its value) with
  the key fields `key_tt`, `key_prim`, `key_ref` and `next`: C++
  `&n->val`/`gval(n)` is the node itself; `n->key_val.ref` is
  `n.key_ref`.
- Tags (`LUA_V*`) are compile-time constants, so a C++ `switch` on a
  tag stays a `switch`; a `case ctb(LUA_VLNGSTR):` label is
  `case LUA_VLNGSTR | BIT_ISCOLLECTABLE:`.
- A Java array holds at most `Integer.MAX_VALUE - 8` elements
  (`Llimits.MAX_JARRAY`). `MAX_SIZE` stays the C++'s; a length that can
  pass the array limit is computed in `long`, and Lmem refuses a request
  past it as the allocator refuses one (Lua's "not enough memory"), so
  the narrowing to `int` comes after the refusal.
- Bit casts: `std::bit_cast<lua_Number>(i)` →
  `Double.longBitsToDouble(i)`; the reverse →
  `Double.doubleToRawLongBits(d)`.

**Functions as values.** A C function (`lua_CFunction`), continuation
(`lua_KFunction`) or budget callback that is used as a value gets one
`static final` field of the same name holding its method reference,
declared right after the method:
`static final lua_CFunction luaB_next = Lbaselib::luaB_next;`. Every use
as a value names the field, so the function has one identity (the name
registry and `lua_tocfunction` comparisons depend on it). Static objects
the C++ makes at file scope (`static CFuncInfo cfi_next(luaB_next,
"base.next");`) are `static final` fields in the same order, after the
fields they use. Interfaces (`lua_Reader`, `lua_UserObject`, …) are
implemented by named classes or anonymous classes as the C++ does.

**Errors and control flow.** `luaD_throw` throws `LuaError`; a
`[[noreturn]]` function returns `RuntimeException` (it never returns)
so that a caller that needs it can write `throw luaG_runerror(...)`;
callers that continue past it in C++ (unreachable) just call it. A C++
`try`/`catch (...)` → `try`/`catch (Throwable)` where it catches Lua
errors and freezes (`catch (LuaError e)` where the C++ catches only
those); `throw;` → rethrow the caught object. `assert(x)` → `assert x;`
(the debug build runs with `-ea` and `-Dlua.debug=true`). Labeled blocks
and `break label` replace the few remaining `goto`s.

**A stopped load's frames** (llex, lparser, lundump, lzio, ldo's
`f_parser`): `com.payonel.lungolua.bytes.LoadFrame` is the base. A frame type is a
subclass with the C++ frame's fields, a copy constructor (`X(X o)`:
`super(o)` and every field, nested value structs such as `expdesc`,
`FuncState` and `BlockCnt` copied deeply), `copy()` returning
`new X(this)` and `size()` returning the C++ `sizeof(X)` as g++
gives it on x86-64 (measure it: a small program that includes the C++
header; the base `LoadFrame` is 24). `X l;` is
`X l = new X();`; `l = static_cast<const X&>(luaZ_restore(rz, LF_X))` is
`l = new X((X) luaZ_restore(rz, LF_X))`; `new (std::nothrow) X(*this)` is
`new X(this)`.

**Other.**
- `sizeof(T)` in a charge → the `Cost` constant; `lua_lock`/`lua_unlock`,
  `luai_userstate*`, `LUAI_MAXCCALLS` and the other lhack.h names are in
  `Lhack`. Its three calls into `ltests` (`luai_tracegc`, `_lua_newstate`,
  `luai_openlibs`) go through the interface `TestHooks`, whose instance
  (`Ltests.hooks`) `Lhack` looks up by name the first time the debug build
  calls one: the C++ picks them at compile time (`#ifdef LUA_DEBUG`), the
  Java at run time, so the engine compiles and runs without the tests
  package, and the debug build without it stops with an error naming
  `lungolua-tests.jar`.
- Allocation (`Lmem`): a denied allocation returns `null` (the C++
  `data() == nullptr`); an empty block argument (`{}`) is `new T[0]`,
  never `null`; a count of 0 gives back the block and returns an empty
  array; `luaM_free_*` and span releases are only the accounting (the
  caller drops the reference).
- The JVM running out of heap (`OutOfMemoryError`) is the allocator
  refusing: Lmem's span allocators give back the charge and refuse (as
  a refusal does), and a protected call catches a stray one as
  `LUA_ERRMEM`.
- C's math functions are `Libm`'s, with the C names: `StrictMath`, with
  C's special cases where Java's differ (`pow(1, y)` and
  `pow(-1, ±inf)` are 1), so results do not depend on the JVM.
- Templates → a generic method, or one method per instantiation where
  the C++ needs a primitive type.
- Lambdas → Java lambdas; a lambda that sets a captured local in C++ sets
  a field of a small holder object in Java.
- Comments: keep the C++ comments (upstream's and the branch's); add none.
- Names: keep every C++ name; one that is a Java keyword gets `_kw`.
- Do not change behavior, even where the C++ looks wrong; report it.

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

`make -C java` compiles the engine with the JDK named by `JDK` (default:
the Temurin 25 under `~/.gradle/jdks`) into `java/build/lungolua.jar`,
without the tests package, so that nothing in the engine can depend on it;
then the tests package against that jar into
`java/build/lungolua-tests.jar`; then the AOT cache of both,
`java/build/lungolua.aot`. The launchers run the interpreter or a test
program with both jars on the class path (the upstream suite's C modules
are Java classes of the tests). The debug build is the same jars run with
`-ea -Dlua.debug=true`; `lungolua.jar` alone is a release build.

The launchers (`java/lua`, `java/lua-debug`, `java/test_*`) and the test
target's own JVM (`JFLAGS`) run with the JVM's own JIT options.
`luaV_execute` calls a function for each opcode that stays in its frame
(as the C++ does, for this: `op_move`, …), so its 3.5 KB of bytecode are
under HotSpot's two 8000-byte limits: C2 compiles it (`HugeMethodLimit`)
and inlines into it (`DesiredMethodLimit`, which counts the inlined
callees too). With every opcode written out in it, it had 10.9 KB: C2
compiled it only with `-XX:-DontCompileHugeMethods`, inlined nothing into
it without `-XX:-ClipInlining`, and compiled it worse as long runs went
on. Now `tests/bench/opcodes.lua` (10 rounds of a mix of every opcode)
runs 1.7 times as fast once warm (6.0 → 3.5 s a round), and
`-XX:-ClipInlining` would make it 18% slower again (short runs: the same
either way). No other method is over the limits but `Lt`'s static
initializer, which runs once.

The AOT cache (JDK 25: JEPs 483 and 514) holds the engine's classes loaded
and linked, as a training run (`java/aot-training.lua`: a little of every
library and of the compiler) left them: the JVM starts in about 45 ms
instead of 165, and `tests/all.lua`, which starts it some 80 times, runs
in 14 s instead of 23. `make` records the training (`-XX:AOTMode=record`)
and makes the cache from it (`-XX:AOTMode=create`) with the launchers'
options, and without the training's profiles (`-XX:-AOTRecordTraining`, a
diagnostic option; JEP 515): with them, C2 compiles `luaV_execute` for the
training's mix of instructions, and loops ran up to twice as slow. A cache
is valid only for the JVM build that made it, the jars of the class path
as they were (`lungolua.jar`, `lungolua-tests.jar` and the JLine jars the
first one's manifest names, by size and time), the same module options
(`--enable-native-access`) and the same compressed-pointer settings. The
launchers pass it (`-XX:AOTCache`) only when it is newer than both jars
and `JDK` is the build's (`build/jdk`; a
JDK before 24 refuses the option), with the JVM's messages about it off
(`-Xlog:aot*=off`), since the tests compare outputs. A cache that does not
match all the same (the tree copied without its times, a JDK updated in
place) is dropped silently, but the JVM then starts without the JDK's own
class data too, in about 215 ms: `make -C java` remakes the cache when the
jars, the JLine jars, the training or the JDK's `release` file is newer;
else remove it. `java -jar java/build/lungolua.jar` runs the release
interpreter without the tests and the cache.

Each push to `lua-java` on payonel/lua that changes `java/` (or a manual
run from the Actions tab) runs `.github/workflows/lungolua.yml`: it builds
`build/lungolua.jar` and `build/lungolua-sources.jar` (`make -C java
build/lungolua-sources.jar`: the engine's sources, without the tests) with
Temurin 25, runs the jar once, and publishes both to GitHub Packages as
`com.payonel:lungolua:0.1.0-<the commit's first 8 hex digits>`, with the
workflow's own token. GitHub Packages asks for a login even to read: a
build that takes the jar from `https://maven.pkg.github.com/payonel/lua`
needs a classic personal access token with `read:packages`, kept outside
the build (`~/.gradle/gradle.properties`: `gpr.user`, `gpr.key`).

The readline's line editor is JLine 3.30.17 from Maven Central
(`org.jline:jline-reader`, `jline-terminal` and `jline-terminal-ffm`,
BSD-3), whose jars are in `java/lib` with its license (`LICENSE-jline.txt`)
and a README that gives their source and checksums. The sources compile
against them; `lungolua.jar`'s manifest names them (`Class-Path: ../lib/…`,
relative to the jar) and enables native access for them
(`Enable-Native-Access: ALL-UNNAMED`), so that `java -jar
java/build/lungolua.jar` runs as the launchers do. The launchers pass
`--enable-native-access=ALL-UNNAMED`: JLine's FFM terminal calls the C
library through Java's foreign function API (no native library of its
own), which the JVM otherwise allows only with a warning.

## Status

| Step | State |
|---|---|
| 0. Skeleton | done |
| 1. Core | done: every core file transcribed; `print("hello")` runs |
| 2. Libraries | done: base, coroutine, table, string, utf8, math, io, os, package, debug |
| 3. Platform | done: `OsApi` (stdio, time and strftime as glibc, processes, dynamic libraries as Java classes), `LocaleUtil` (C locale), `Libm`, `Readline` |
| 4. Host and tests | done: `com.payonel.lungolua.host.Lua` (`java/lua`, `java/lua-debug`), `Ltests`, and the C++ build's test programs; `make -C java test` (`DEBUG=1`: the debug build) runs them as the C++ `make test` does, and passes in both builds: the API tests (52940 and 53017 checks, with the upstream suite run through the API), the engine tests (147), the upstream suite, the deep tests, the gate (97, 17, 3), the Styx gate and the Styx tests (36191 and 38293 checks): the C++'s counts |
| 5. Styx | done: `Lstyx`; its streams are the C++'s byte for byte (the trailer's address fixes aside). Moved with the C++'s to payonel/styxlua (2026-10-13): `master` is `com.payonel.styxlua`, built against `com.payonel:lungolua` and `lungolua-tests` |

The platform layer's dynamic libraries are Java classes, and
`os_dlopen` answers as glibc's `dlopen` (`RTLD_NOW`) does: a path with a
`/` loads the Java library its file's base name names (`libs/lib2-v2.so`
is `com.payonel.lungolua.tests.libs.Lib22`) if the file exists, whatever it holds
(`make -C java` makes the test modules' files, dated 1970 so that the C++
build still makes its own); a name without a `/` loads a registered
system library (`libreadline.so` is `com.payonel.lungolua.platform.Readline`); `""` is
the program. A library's symbols are its class's public static fields;
its undefined ones (`lib11` takes `lib1_export`) must come from a library
loaded with global symbols. Errors have glibc's messages.

`Readline` stands for GNU readline (`readline`, `add_history`,
`rl_readline_name`). When the standard input and output are terminals,
`readline` edits the line with JLine: one line reader for the process,
made at the first prompt and named by `rl_readline_name`, on the system
terminal (JLine's FFM provider when native access is enabled, else its
`stty` one), which it reads and writes as UTF-8. It edits as GNU
readline's emacs mode: the arrow, Home and End keys in both their ANSI
forms, Up and Down the previous and next history line; no completion, no
`!` history expansion, no history file. The history holds the lines
`add_history` was given, all of them (lua.c gives each statement, its
lines joined), and no others. Ctrl-D on an empty line ends the input.
Ctrl-C does as in GNU readline: it echoes `^C`, restores the terminal and
gives SIGINT to the program's handler, which at lua.c's prompt is the
default, so the process ends by the signal (the JVM raises a signal only
for a handler of its own, so a shell sends it). Otherwise (the input or
the output is not a terminal, or no terminal can be made) `readline`
shows its prompt and reads the line from the standard input, and echoes
the line when the input is not a terminal, as GNU readline does.

The JVM converts file names, command lines and the environment between
its strings and the system's bytes with the charset of the locale it
starts in (`sun.jnu.encoding`), and no property changes that. The engine's
strings cross to Java as UTF-8, so the launchers (`java/lua`,
`java/lua-debug`, `java/test_*`) start the JVM with `LC_ALL=C.UTF-8` when
the locale's charset is not UTF-8 (`locale charmap`), and pass the values
`LC_ALL`, `LC_CTYPE` and `LANG` had as properties (`lua.env` lists the
names, `lua.env.NAME` holds a value; no such property: the variable was
unset). `Environ` hands the program the environment it was started with:
`os.getenv` reads it as bytes from `/proc/self/environ` (once;
`System.getenv` where that cannot be read) with those variables as they
were, `os.setlocale("")` reads the same, and a command (`os.execute`,
`io.popen`) gets back the variables the launcher changed, the rest of its
environment being the JVM's, byte for byte.

Known differences from the C++ build (the JVM's, not the port's):
- Only the Java libraries load (the test modules and readline); any other
  C library fails to load.
- The readline (JLine) reads no init file (`INPUTRC`, `~/.inputrc`,
  `/etc/inputrc`), so their bindings and settings do not apply; Tab does
  not complete file names (GNU readline's default); the terminal is UTF-8
  in every locale; Ctrl-Z stops the process without echoing `^Z`. The
  lines read are GNU readline's; the screen updates differ (JLine's
  redraws, and its keypad and mouse-mode sequences).
- `os_system`/`io.popen` statuses: a command killed by a signal can show
  as an exit code 128+n.
- Paths, commands and the environment are exact in every locale: a path
  or command reaches the system as its bytes, and `os.getenv` answers
  with the environment's bytes, valid UTF-8 or not. The limit: a path or
  command whose bytes are not valid UTF-8 cannot pass through Java's file
  and process APIs, which take text (each invalid byte becomes U+FFFD);
  so too an original `LC_ALL` that is not valid UTF-8, which a command
  gets back as text.
- `os.setlocale` knows only the "C" and "POSIX" locales (`LocaleUtil`):
  where the environment names another locale, `os.setlocale("")` answers
  nil, and the C++ the locale's name.
- The launchers are `/bin/sh` scripts: an environment variable whose
  name is not a shell name (`A-B`) does not reach the JVM where the shell
  drops it (dash does), and `os.getenv` does not see it.
- Accepted limitation: a table holds at most 2^31 entries (an array
  part of 2^30, `MAXABITS` 30, plus a hash part of 2^30), where stock
  Lua holds 3·2^30 (array part 2^31): a Java array cannot have 2^31
  elements, and the C++ keeps the same limit so that both engines agree.
  Past it, a table raises "table overflow". The JVM's heap (by default a
  quarter of the machine's memory; `-Xmx` sets it) usually ends a table
  sooner, with "not enough memory".
- Accepted limitation: a string or buffer holds at most
  `Integer.MAX_VALUE - 8` bytes (about 2 GiB), the JVM's largest array
  (`MAX_JARRAY`); a longer one is refused as memory ("not enough
  memory"), where the C++ is bound only by `MAX_SIZE` (2^63 - 1) and the
  memory there is.
