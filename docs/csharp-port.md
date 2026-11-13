# C# port

The C# sources, under `csharp/` beside `cpp/` and `java/` on branch
`lungolua`, are a transcription of the Java port
([java-port.md](java-port.md)), which is itself the C++'s line-by-line
transcription. The engine is managed code only: it needs the .NET runtime
and nothing else (no native library, no NuGet package), as the Java port
needs only a JVM. All three run the test scripts in `tests/`.

## Rules

The rules of [java-port.md](java-port.md) hold: transcribe, do not
redesign; the C++ is the specification; same behavior, same tests; every
allocation is accountable as in the C++ (the `Cost` constants). The C#
follows the Java wherever the two languages agree, so that a change made in
C++ and ported to Java ports to C# by the same edit.

## Layout

| Java | C# |
|---|---|
| `java/src/com/payonel/lungolua/<pkg>/<Class>.java` | `csharp/src/<Pkg>/<Class>.cs` |
| package `com.payonel.lungolua.<pkg>` | namespace `Payonel.LungoLua.<Pkg>` (`Api`, `Bytes`, `Compiler`, `Gc`, `Host`, `Lib`, `Object`, `Platform`, `Runtime`, `Vm`, `Tests`, `Tests.Libs`) |
| `lungolua.jar` (engine and interpreter) | `csharp/build/lungolua.dll` (`src/LungoLua.csproj`) |
| `lungolua-tests.jar` | `csharp/build/lungolua-tests.dll` (`src/Tests/LungoLua.Tests.csproj`) |
| the Java library's calls (`Long.compareUnsigned`, `String.length()`, `ArrayList`, …) | `Payonel.LungoLua.Compat` (`src/Compat`): the same names with the Java semantics (`JLong.compareUnsigned`, an extension `length()`, `ArrayList<T>`, …) |

Class, method and field names are the Java's (the C++'s). A name that is
a C# keyword is written with C#'s `@` (`@base`, `@object`), which keeps
the name.

## Transcription

What changes from the Java, everywhere:

- `byte` is `sbyte` (Java's byte is signed: every `& 0xFF`, comparison and
  cast stays as the Java has it); `boolean` is `bool`; `String` is
  `string`; `Object` is `object`. An `sbyte[]` crosses to .NET's I/O as a
  `byte[]` by reinterpretation (`(byte[])(object)a`), never by copy.
- Package-private members are `internal` (the tests assembly sees them:
  `InternalsVisibleTo`); a method that overrides a class's method is
  `override` and the one it overrides `virtual`; a field that hides an
  inherited field is `new` (Java's field hiding).
- Functional interfaces (`lua_CFunction`, `lua_KFunction`, `lua_Budget`,
  `lua_Hook`, `lua_ObjectFactory`, `lua_CClosureKind.Save` and `Make`)
  are delegates; `Compat/DelegateCalls.cs` gives each a `call` extension
  method, so that calls read `f.call(L)` as in the Java. The other
  interfaces (`lua_Reader`, `lua_Writer`, `lua_Alloc`, `lua_UserObject`,
  `ProtectedCall`, …) stay interfaces.
- C# forbids a field and a method of the same name, which the Java uses
  for a function's one identity (`static final lua_CFunction luaB_next =
  Lbaselib::luaB_next;`): the field is `luaB_next_fn`, and every use as a
  value names it.
- A `static { }` block is a static field whose initializer runs it, at
  the block's place: C# runs a static constructor after all the field
  initializers, Java runs blocks and initializers in textual order.
- `assert c;` is `{ if (JAssert.on && !(c)) throw new AssertionError(); }`:
  `JAssert.on` is the debug build's switch (`-Dlua.debug=true`).
- A `switch` section that falls into the next ends with `goto case X;`
  (C# needs it said); a labeled `break`/`continue` is a `goto` to a label
  after the statement or at the end of the loop body.
- Java's system properties (`lua.debug`, `lua.progname`, `lua.env*`,
  `publicapi.lua`) are leading `-Dname=value` arguments of the C#
  programs, read before any engine type is initialized (`SysProps`).
- A local in a nested block that has the name of a local of the
  enclosing block (which Java allows when the outer one is declared
  later, and C# never) has a `_` appended.
- Light userdata (the tests' integers) are boxed `long`s, one box per
  value (`UtilCast`), compared by reference as the Java's `Long` objects.
- `java.lang.StrictMath` is fdlibm, the same on every JVM; .NET has no such
  library, and `Libm` uses `System.Math`, which is the C library's (glibc on
  Linux, as the C++ build): see Known differences.

## Build

`make -C csharp` builds both assemblies with the .NET 10 SDK named by
`DOTNET` (default `~/.dotnet10/dotnet`, installed with Microsoft's
`dotnet-install.sh --channel 10.0 --install-dir ~/.dotnet10`) into
`csharp/build/`. The launchers (`csharp/lua`, `csharp/lua-debug`,
`csharp/test_engine`, `csharp/test_gate`) run them with the runtime that
built them (`build/dotnet`); the debug build is the same assemblies run
with `-Dlua.debug=true`. `make -C csharp test` (`DEBUG=1`: the debug
build) runs the tests as `make -C java test` does, in `csharp/run` (or
`csharp/run-debug`), so that all builds can test at once.

## Status

`make -C csharp test` passes in both builds, with the Java's counts: the
API tests (52958 checks; 53035 in the debug build), the engine tests (147),
the upstream suite, the deep tests, and the gate (97, 17, 3). The platform
layer (`OsApi`, `OsFile`, `Environ`, `Readline`) and the interpreter's
signal handling are written against .NET's library (not transcribed: the
JVM's file, process and terminal APIs have no counterpart); the readline's
line editor is a small `Console.ReadKey` editor in place of JLine.

## Known differences

- Math functions are the platform C library's (see Transcription).
