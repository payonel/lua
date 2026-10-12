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
| `lua::platform` | `lua.platform` | `OsApi`, `LocaleUtil`, `Libm` |
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
| lobject.h/.cpp | `lua.object.Lobject` (+ `FmtArg`, `FmtArgs`) |
| llimits.h | `lua.object.Llimits` (done) |
| lstring.h/.cpp | `lua.object.Lstring`; methods on `TString`, `Udata` |
| ltable.h/.cpp | `lua.object.Ltable`; methods on `Table` |
| lfunc.h/.cpp | `lua.object.Lfunc`; methods on `Proto`, `UpVal`, `CClosure`, `LClosure`, `CFuncInfo` |
| lnames.cpp | `lua.object.Lnames` |
| lmem.h/.cpp | `lua.runtime.Lmem` (`cost::` is `lua.runtime.Cost`, done) |
| lstate.h/.cpp | `lua.runtime.Lstate` (the slot accessors over the stack's columns) |
| ltm.h/.cpp | `lua.runtime.Ltm` |
| ldo.h/.cpp | `lua.runtime.Ldo` (`LuaError` done) |
| lhack.h | `lua.runtime.Lhack` (done) |
| lgc.h/.cpp | `lua.gc.Lgc` (+ `EphIndex`) |
| lvm.h/.cpp | `lua.vm.Lvm` |
| ldebug.h/.cpp | `lua.vm.Ldebug` |
| lopcodes.h/.cpp, lopnames.h | `lua.vm.Lopcodes` |
| lctype.h/.cpp | `lua.bytes.Lctype` |
| lzio.h/.cpp | `lua.bytes.Lzio` (+ `ZIO`, `MBuffer`) |
| lload.h/.cpp | `lua.bytes.Lload` (+ its records) |
| util_cast.h/.cpp | `lua.bytes.UtilCast` |
| llex.h/.cpp | `lua.compiler.Llex` (+ `LexState`, `Token`, …) |
| lparser.h/.cpp | `lua.compiler.Lparser` (+ `FuncState`, `expdesc`, `Dyndata`, …) |
| lcode.h/.cpp | `lua.compiler.Lcode` |
| ldump.cpp | `lua.compiler.Ldump` |
| lundump.h/.cpp | `lua.compiler.Lundump` |
| lua.h, lapi.h/.cpp | `lua.api.LuaApi` (constants done) |
| luaconf.h | `lua.api.LuaConf` (done) |
| lua_internal.h | `lua.api.LuaInternal` (its constants and inline functions; each function goes to the class of the file that defines it) |
| lauxlib.h/.cpp | `lua.api.LuaAuxLib` |
| lualib.h | `lua.lib.Lualib` |
| lbaselib.cpp, lcorolib.cpp, … | `lua.lib.Lbaselib`, `lua.lib.Lcorolib`, … |
| linit.cpp | `lua.lib.Linit` |
| lstyx.cpp | `lua.lib.Lstyx` |
| os_api.h/.cpp, locale_util.h/.cpp | `lua.platform.OsApi`, `lua.platform.LocaleUtil` |
| C's `<cmath>` (`pow`, `fmod`, `floor`, `frexp`, …) | `lua.platform.Libm` (done) |
| lua.cpp | `lua.host.Lua` |
| ltests.h/.cpp | `lua.tests.Ltests` |

**Calls keep their C shape.** A file `import static`s the classes whose
functions it calls (`import static lua.object.Lobject.*;`), so
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
  `boolean` (C `int` used as a boolean stays `int`).
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
  `lua.api.HostString`; `const char*` names (registrations) → `String`;
  `InternalString` → `lua.bytes.InternalString`; `lt::x` → `Lt.x`.
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
- `size_t` of a string, userdata or buffer is a Java array length, so
  `MAX_SIZE` (llimits.h: representable as a `lua_Integer` and as a
  `size_t`) is the largest array the JVM makes, `Integer.MAX_VALUE - 8`,
  as on a platform whose `size_t` has 32 bits: the C++'s own checks
  against it raise Lua's errors ("string length overflow", "resulting
  string too large", …) where Java would fail.
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
`f_parser`): `lua.bytes.LoadFrame` is the base. A frame type is a
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
  `Lhack`.
- Allocation (`Lmem`): a denied allocation returns `null` (the C++
  `data() == nullptr`); an empty block argument (`{}`) is `new T[0]`,
  never `null`; a count of 0 gives back the block and returns an empty
  array; `luaM_free_*` and span releases are only the accounting (the
  caller drops the reference).
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

`make -C java` compiles the sources with the JDK named by `JDK` (default:
the Temurin 25 under `~/.gradle/jdks`) into `java/build/lua.jar`.
`luaV_execute` is larger than the 8000 bytes of bytecode HotSpot compiles
by default: the interpreter runs with `-XX:-DontCompileHugeMethods`.
The debug build is the same jar run with `-ea -Dlua.debug=true`.

## Status

| Step | State |
|---|---|
| 0. Skeleton | done |
| 1. Core | done: every core file transcribed; `print("hello")` runs |
| 2. Libraries | done: base, coroutine, table, string, utf8, math, io, os, package, debug |
| 3. Platform | done: `OsApi` (stdio, time and strftime as glibc, processes), `LocaleUtil` (C locale), `Libm` |
| 4. Host and tests | `lua.host.Lua` with the `java/lua` and `java/lua-debug` launchers; `Ltests`; suites: in progress |
| 5. Styx | done: `Lstyx`; its streams are the C++'s byte for byte (the trailer's address fixes aside) |

Known differences from the C++ build (the JVM's, not the port's):
- No C libraries: `package.loadlib` and C modules answer as stock Lua
  without dynamic libraries; the REPL has no readline (the host warns
  that it cannot load it, also under `-E`).
- `os_system`/`io.popen` statuses: a command killed by a signal can show
  as an exit code 128+n.
- Paths, commands and environment names cross to Java as UTF-8.
- Strings and buffers end at the JVM's largest array (see `MAX_SIZE`).
