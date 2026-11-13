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

The assemblies carry ReadyToRun code for the build machine's platform
(`make` publishes for the SDK's RID) beside their IL, as the Java build
keeps an AOT cache: the interpreter starts in about 0.045 s instead of
0.07. On any other platform the runtime runs the IL: nothing has to be
built per host. `luaV_execute` is `AggressiveOptimization`, so the main
loop always runs the JIT's optimized code (precompiled code is replaced
only after many calls, and `luaV_execute` is called seldom and loops
long).

## Where the C# goes beyond the Java

What the JVM made the Java allocate, and C# need not:

- Inlining: the C++'s inline functions (`l_sinline`, `l_forceinline`,
  header `inline`) of at most 12 lines, and every method of at most 3,
  are `[MethodImpl(MethodImplOptions.AggressiveInlining)]`: the JIT's
  budget for the huge `luaV_execute` would otherwise drop them (C2 inlined
  them by itself).
- Value types: `ByteView` is a `readonly struct`, as the C++'s
  `byte_view` is a value (`null` stays "no view": the null literal
  converts to the default view, and `==` compares array, offset and
  length); `IntResult` and `NumResult`, the Java's holders for a
  conversion's out value, are structs passed by `ref`; the API's
  `IndexAddr`, `lua_ResumeResult`, the platform's results, `ParseResult`
  and Compat's `Optional*` are structs.
- Not changed: `TValue` and `Node`. Hash nodes as a struct array would
  be the C++'s layout (and less memory), but `luaH_get` and its kin hand
  out a node's value as a `TValue` reference that callers read and write,
  which needs `TValue` itself to be a struct: a redesign of the engine's
  value handling, not a port fix. Per-thread scratch values for short-lived
  temporaries measured no gain (.NET's young allocations are cheap).

`tests/bench/opcodes.lua`, a round once warm: C++ 3.1 s, Java 3.0 s,
C# 3.8 s (6.7 s as first transcribed). The upstream suite runs in about
16 s (release), as the Java's.

## Host data through frozen threads

`csharp/src/Tests/Inbox.cs` (`lungolua-tests.dll Inbox`, part of
`make test`) shows on the C# engine how host data reaches Lua code that
never yields, the pattern of ModularComputers' manual
(`docs/examples/InboxExample.java`, `docs/os/inbox.md`) and of its firmware
(`machine.lua`, `LuaRunner`):

1. Inbox: a budget freezes the program every 10,000 steps; between slices
   the host moves what a producer on another .NET thread queued into a
   table the program reads, and thaws it; each message is gone from the
   table by the next freeze (the program took it at its first look), and
   the program's `pcall` never sees a freeze.
2. Interrupts: the budgeted program is a scheduler running a child that
   never yields; the host thaws with a stop above the child only when it
   delivered data, so the scheduler wakes for each message (and only then)
   while the child is preempted, not asked to yield.
3. Firmware: the same with the firmware in Lua, as the mod's: the host only
   fills a mailbox; the firmware, unbudgeted, drains it with a C function at
   each freeze, appends to a `{head, tail}` ring, and calls
   `debug.thaw(kernel, child)` when there is news.

## Status

`make -C csharp test` passes in both builds, with the Java's counts: the
API tests (52958 checks; 53035 in the debug build), the engine tests (147),
the upstream suite, the deep tests, and the gate (97, 17, 3), and
the inbox tests above (28 checks). Every test program of the C++ and Java
builds is ported (the API tests, the engine tests, the gate, the debug
build's `T` library, the test C modules); `make -C csharp full` runs both
builds, as the C++'s `make full`. Not covered: Styx (its tests and the
Styx gate live in payonel/styxlua, against the engine packages; a C#
Styx would be a port of its own). The platform
layer (`OsApi`, `OsFile`, `Environ`, `Readline`) and the interpreter's
signal handling are written against .NET's library (not transcribed: the
JVM's file, process and terminal APIs have no counterpart); the readline's
line editor is a small `Console.ReadKey` editor in place of JLine.

## Known differences

From the C++ and Java builds (the platform's, not the port's):
- Math functions are the platform C library's (see Transcription): glibc
  on Linux, as the C++ build; on another system the last bit of a result
  can differ (the Java's StrictMath does not).
- The readline's line editor is a small editor of its own (JLine has no
  .NET counterpart): Left/Right/Home/End, Backspace/Delete, Up/Down
  through the history, Ctrl-A/E/B/F/P/N/K/U; no completion, no init file;
  every character is one column wide (wide characters are drawn misplaced);
  the line is redrawn whole. Ctrl-C is the terminal's SIGINT, echoed as
  `^C`.
- Child processes (`os.execute`, `io.popen`) get their environment from
  .NET's strings: a variable whose bytes are not valid UTF-8 reaches them
  with U+FFFD in place of the bytes.
- Files: appending is a seek to the end before each write (no `O_APPEND`
  in .NET; not atomic across processes); `rename` across file systems
  copies; files .NET opens are close-on-exec; non-blocking reads block on
  pipes and terminals.
- The only native call is libc's `lseek`, on the standard streams 0-2
  (no managed call moves a shared descriptor's offset).
- Test C modules are C# classes (`Payonel.LungoLua.Tests.Libs.LibN`); any
  other C library fails to load ("cannot load C code into .NET").
- The limits the Java documents (a table holds at most 2^31 entries, a
  string at most about 2 GiB) hold for .NET's arrays as well.

## Decisions taken in the port, for review

The port was done without the owner at hand; these are the choices it made where the task left them open.

- **D1 (starting point).** "Mainline" read as branch `lungolua` at 1998d72a, equal to origin/lungolua; it already contains all of upstream `master` (0 commits behind).
- **D2 (source of the transcription).** C# is transcribed from the Java port (itself the C++'s line-by-line transcription), not from the C++ directly: Java and C# share the object model (references, GC, exceptions, no pointers), so the Java's answers to "how does C++ X look without pointers" carry over. The C++ stays the specification.
- **D3 (runtime).** .NET 10 (the LTS current in 2026-11), SDK installed in ~/.dotnet10 by Microsoft's dotnet-install script (no sudo; like the JDK under ~/.gradle/jdks). The system has only .NET 8 (support ends 2026-11). No NuGet packages: the engine depends on the .NET runtime only.
- **D4 (layout).** `csharp/` beside `cpp/` and `java/`: sources in csharp/src/<Package>/<Class>.cs (no com/payonel/lungolua nesting), namespaces Payonel.LungoLua.<Package> (Api, Bytes, Compiler, Gc, Host, Lib, Object, Platform, Runtime, Vm, Tests), class and member names as the Java's (C++'s). Assemblies: lungolua.dll (engine + the interpreter's entry point, as lungolua.jar) and lungolua-tests.dll (the debug build's T library and the test programs, as lungolua-tests.jar). Output in csharp/build (dotnet's artifacts layout under build/artifacts; nothing in src).
- **D5 (bytes).** Java `byte` -> C# `sbyte` everywhere (Java's byte is signed; keeping the sign keeps every `& 0xFF`, comparison and cast as the Java has it). At I/O boundaries an sbyte[] is reinterpreted as byte[] ((byte[])(object)a, which the CLR allows), never copied.
- **D6 (functions as values).** The Java's functional interfaces (lua_CFunction, lua_KFunction, lua_Budget, lua_Hook, lua_ObjectFactory, lua_CClosureKind.Save/Make) are C# delegates, with a `call` extension method each (Compat/DelegateCalls.cs) so call sites keep `f.call(L)`. C# forbids a field and a method of the same name, which the Java uses for "the function's one identity" (`static final lua_CFunction luaB_next = Lbaselib::luaB_next`): the field is `NAME_fn`. Delegate equality is by method and target, and each field holds one delegate, so identity comparisons (lua_tocfunction, the name registry) hold.
- **D7 (static initialization order).** A Java `static { }` block becomes a static readonly field initialized by a method at the block's place, so it runs in textual order with the field initializers (a C# static constructor would run after all of them).
- **D8 (assertions).** Java `assert c;` -> `{ if (JAssert.on && !(c)) throw new AssertionError(); }`; JAssert.on is the debug build's switch (-Dlua.debug=true: the Java debug build runs with -ea and lua.debug together).
- **D9 (system properties).** The Java's system properties (lua.debug, lua.progname, lua.env*, publicapi.lua) are leading `-Dname=value` arguments of the C# programs, parsed before any engine type initializes (SysProps); the launchers pass them as the Java launchers pass theirs.
- **D10 (math).** Java's StrictMath (fdlibm, the same results on every JVM) has no .NET counterpart: Libm's functions use System.Math, which is the C library's (glibc on Linux, the same as the C++ build's; another libm on Windows/macOS could differ in the last bit). Porting fdlibm would make results platform-independent as the Java's; not done (open item).
- **D11 (tooling).** The mechanical Java->C# translator and its error-driven fixers live in the session scratchpad, not in the repository (as the Java port's split.py did): the C# sources are the maintained form from the first commit on.
- **D12 (publishing).** No GitHub workflow or packages for C# (the user: build locally, do not push).
- **D13 (threads).** The interpreter and the test programs run Lua on a thread with a large stack (the C++ build's C stack: the CLR cannot catch a stack overflow, where the JVM can).
- **D14 (value types).** The user asked (mid-port) to use C#'s value types and avoid Java-forced allocations: done as a review pass after the transcription passes all tests, so each change is checked against a green baseline.
- **D15 (readline).** JLine has no .NET counterpart and no package is allowed: Readline's line editor is new code (not a transcription), a small Console.ReadKey editor: Left/Right/Home/End, Backspace/Delete, Up/Down through add_history's list, and Ctrl-A/E/B/F/P/N/K/U as emacs mode; Ctrl-D ends input on an empty line. Ctrl-C is the tty's own SIGINT (as GNU readline), not a key read and re-raised with `kill -INT` as the Java did. Console.InputEncoding is forced to UTF-8 (as the Java forced JLine's). Every character counts one column (wide characters are drawn misplaced).
- **D16 (signals).** lua.c's SIGINT handling uses PosixSignalRegistration; SIG_DFL disposes the registration so the process dies by the signal.
- **D17 (one P/Invoke).** libc lseek, only on fds 0-2: no managed call moves a shared descriptor's offset, and without it a seekable stdin/stdout would not share its offset with child processes (io.stdin:seek). Everything else is managed.
- **D18 (standard streams).** A seekable descriptor (a redirected file) goes through .NET's Console streams; a terminal or pipe through a FileStream on the fd (avoids Console's line editor on a terminal stdin and Console swallowing EPIPE).
- **D19 (processes).** io.popen/os.execute run `/bin/sh -c cmd sh` through System.Diagnostics.Process; the Java's named pipe is gone (.NET keeps a child's pipes open after the child exits, as C's popen). The child's environment is rebuilt from strings by .NET, so a variable whose bytes are not UTF-8 reaches a command as U+FFFD (the Java passed it through).
- **D20 (files).** fopen modes map to FileStreamOptions with no buffering of .NET's own; append is a seek to the end before each write (no O_APPEND in .NET: not atomic across processes); System.IO.DisableFileLocking is set (C takes no advisory lock); the temporary directory is still the property java.io.tmpdir (default /tmp).
- **D21 (dynamic libraries).** Test C modules are C# classes found by reflection (engine assembly, then lungolua-tests by name, then lungolua-tests.dll beside lungolua.dll); a symbol NAME also answers as the field NAME_fn (D6). The message "cannot load C code into the JVM" reads "cannot load C code into .NET". The debug build finds Ltests the same way (Lhack).
- **D22 (test programs).** CHECK lines come from [CallerLineNumber] (C# source lines), where the Java used StackWalker; heap_live is GC.GetAllocatedBytesForCurrentThread (the Java's per-thread allocated bytes). A Java text block keeps its final newline and a C# raw string does not: each has a blank line before its closing quotes.
- **D23 (Java maps of Integer).** The translator wrote HashMap<X, Integer> as HashMap<X, int>, where a missing key reads 0 instead of null: two in Lgc (luaC_makedue, luaC_ordermarked; EngineTests found it) and one in a test are int?; no other map is null-tested.
- **D24 (inlining).** C# methods get [MethodImpl(AggressiveInlining)] where the C++ marks the function inline (l_sinline, l_forceinline, header inline) and the C# body is at most 12 lines, and on every method of at most 3 lines: the JIT's budget for the huge luaV_execute otherwise drops the C++'s inlining (6.7 -> 4.0 s a round of tests/bench/opcodes.lua).
- **D25 (value types, user's request).** Structs: IntResult/NumResult (passed by ref: the Java's out-holders), ByteView (readonly struct: C++'s byte_view is a value; `null` = no view = default, == compares array/offset/length instead of references), lua_ResumeResult, OsResult/OsOpenResult/OsSystemResult, ParseResult, Optional*. Not done (yet): TValue (Node derives from it and code mutates values through references) and Node[] as a struct array (the largest remaining win for tables, invasive).
- **D26 (runtime settings).** Defaults kept: gen0 size, non-concurrent GC, TieredCompilation off, TieredPGO off were each no better on tests/bench/opcodes.lua.
- **D27 (ReadyToRun).** The build publishes ReadyToRun code for the build machine's RID inside the assemblies (startup 0.07 -> 0.045 s; IL kept, so other platforms JIT as usual: no per-host build for users); luaV_execute is AggressiveOptimization so the main loop is never stuck in precompiled code.
- **D28 (allocation that did not pay).** A per-thread scratch TValue for the table-read opcodes' result (gettable, geti, getfield, self, gettabup; safe: nothing runs between the lookup and the copy) measured no difference in an alternating A/B (3.9 vs 3.9 s a round): .NET's allocation of short-lived objects is a pointer bump and gen-0 collections are cheap. Reverted, to keep the C# the Java's shape where C#'s means buy nothing.
- **D29 (not done: Node[] and TValue as structs).** The largest remaining value-type change: hash nodes as a struct array (the C++'s layout; C# on tests' table loop peaks at 89 MB where C++ uses 11 MB). luaH_get and kin hand out the node's value as a TValue reference (C++'s const TValue*), which callers read and write; a struct array can give out `ref`s only if TValue is a struct too, and TValue is a mutable reference type through the whole engine (temporaries filled by callees, Node : TValue). A redesign, not a review fix: left for the user to decide.
- **D30 (API index).** LuaApi's IndexAddr (the slot or value an API index names, made on every API call) is a readonly struct.
- **D31 (tests).** Every test program of the C++/Java builds is ported (PublicApi, EngineTests, Gate, Ltests, the test C modules); make -C csharp full runs both builds as the C++'s make full. Added for the user: Tests/Inbox.cs, host data through frozen threads (ModularComputers' InboxExample, the same with thaw stops, and its firmware's drain/ring shape in Lua). Not covered: Styx (its tests live in payonel/styxlua).
