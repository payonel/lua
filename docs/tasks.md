# Port-prep task plan

Everything still needed before the C++ sources can be ported to Java by
transcription. Goal, rules and history are in [summary.md](summary.md).

## Principles

These govern every task below. When a task's approach conflicts with them,
the principle wins and the task gets revised.

- **Honor the original.** Upstream Lua's shape, file boundaries, function
  names and performance decisions are kept, and so are the decisions already
  made on this branch: owning spans, per-type allocation functions, the
  prim/ref/tag stack columns, `std::optional` for nullable views, and
  `CFuncInfo` identity. A task changes a construct only because Java cannot
  express it, never for style.
- **Same cost or better.** A rewrite must not add work on a hot path: no
  added indirection, dispatch, allocation or conversion. When the original
  uses one indirect call (a function pointer), the replacement may use one
  indirect call (a virtual method), not more.
- **Think in Java too.** Where C++ and Java performance disagree, choose the
  shape that performs best in Java, measure the C++ cost (see A/B checks), and
  record it.
- **Signed-compatible.** Values are signed unless they genuinely need
  wraparound or bit-container behavior. `size_t` stays where it interfaces
  with standard containers (`.size()`, span/view lengths). Avoid casts: fix
  the declared type instead of converting at the use site.
- **C++ quality where Java is unaffected.** Keep C++ constructs that make
  intent readable when they still port to Java in one line. For example,
  `byte_view` (read-only), `byte_span` (read-write) and `byte_owner`
  (owning) all become `byte[]` + offset + length in Java. Their distinction
  is kept because it documents intent, and it costs the port nothing.
- **Invisible engine.** No new Lua-visible limits or behaviors. A Lua script
  must not be able to tell it is running on this engine.
- **Every allocation is accountable.** A Lua script is charged for the
  memory it asks for, and no allocation path goes uncharged. Exact byte
  parity is not the goal. When the true cost can't be measured, charge a
  logical value that scales with the request. Strings set the precedent:
  they are charged their character length, even though the `std::string`
  behind them uses more. This applies to every engine allocation in C++
  and in Java, including copies Java makes where C++ can alias memory.
- **No new comments; immutable tests.** As in `summary.md`. A required
  `ltests.cpp` edit is a mechanical call-shape update only and is called out
  in the commit message.

## How to work this plan

- **Phase order is fixed:** line changes (L), then memory changes (M), then
  class changes (C). The `Table` class move already landed. It stays: it is
  already the target shape, and reverting it would only add churn. Later
  phases edit its methods in place. No further class moves happen before
  phase C.
- Within a phase, tasks are independent unless a **Depends** line says
  otherwise. Every commit passes `make full`.
- Site lists come from a scan at `24eee778`. Re-grep before starting each
  task. The **Done when** check is the acceptance gate.

### A/B checks

Required for every task marked **A/B**, and recommended for the rest.

- **Baseline:** the commit before the task. **Candidate:** the task's last
  commit. Both use release builds from a clean `obj/`.
- **Time:** run `tests/all.lua` 5× per side and compare the median
  `total time`. Also run a focused micro-benchmark of the paths the task
  touches (for example, a table get/set loop for M3, or `pcall` in a loop
  for L3). Benchmarks live outside the repository; this branch removed
  measurement scripts deliberately (`b7b71ea4`).
- **Memory:** max RSS (`/usr/bin/time -v`, already used by `make full`) and
  `collectgarbage("count")` at the same checkpoints in the micro-benchmark.
- **Record** the numbers in a **Results** line under the task when it lands.
  Proposed thresholds: ≤1% time and ≤2% RSS on `all.lua`. Tasks that trade
  C++ cost for Java performance (M3, L9) record the C++ cost and are
  accepted case by case.

---

## Phase L — line changes

Local rewrites of individual constructs that have no Java equivalent. Object
layout and ownership don't change.

### L1. Pointer-to-pointer list handling

**Effort:** high · **Risk:** high (GC correctness) · **A/B**

Java has no address of a field, so every `T**` that points at "the link
field of whoever precedes me" must change. Each rewrite below keeps the
original algorithm and its O(1) behavior.

- **`getgclist`** (`lgc.cpp:183`) is a tag `switch` that returns
  `&X->gclist`. The holders are `Table`, `LClosure`/`CClosure`
  (through `Closure`), `lua_State`, `Proto` and `Udata`; each already
  has a `gclist` field. Add an intermediate base `GCListed : GCObject`
  that owns `gclist`, derive those types from it, and read
  `static_cast<GCListed*>(o)->gclist` directly. Memory is unchanged and the
  switch goes away. In Java this is a plain field on a superclass.
- **`linkgclist_(o, pnext, list)`** and the list heads on `global_State`
  (`gray`, `grayagain`, `weak`, `ephemeron`, `allweak`): return the new
  head, as in `g->gray = linkgclist(o, g->gray)`.
- **Sweeping** (`sweeplist`, `sweeptolive`, `sweep2old`, `sweepgen`,
  `findlast`, `separatetobefnz`, `correctgraylist`, `checkpointer`,
  `psurvival`, `pfirstold1`, the `nextlist` parameter):
  - The cursor becomes the *predecessor object*, with `nullptr` meaning
    "at the list head". The current `gcstate` already determines which list
    is being swept, so no list enum is needed. Unlinking writes either
    `prev->next` or the head field.
  - `global_State::sweepgc` (`lstate.h:509`) becomes that predecessor.
    Upstream re-anchors `sweepgc` when the object whose `next` it points at
    leaves the list (`luaC_checkfinalizer`, `sweeptolive`). Keep the same
    re-anchoring with the predecessor form; this is where the risk is.
  - The `&g->twups` walk works the same way.
- **Open upvalues** (`UpVal::previous` is `UpVal**`, plus `newupval(...,
  UpVal** prev)` and `&L->openupval`): `previous` becomes `UpVal*
  prevopen`, and `nullptr` means `L->openupval` is the head. Unlink stays
  O(1).
- **String table** (`lstring.cpp:186`): unlink a bucket chain with a
  predecessor pointer.
- **Out-references:** `lapi.cpp:1755` `GCObject** owner`,
  `lapi.cpp:1851` `LClosure** pf`, `lundump.cpp:134` `TString** sl`.
  These are handled in L2. `lvm.cpp:968` `UpVal** encup` is an array and
  becomes a `std::span<UpVal*>`.

**Done when:** `\w\s*\*\*\s*\w` matches only `argv`/`lua.cpp`. `gc.lua`,
`gengc.lua`, `memerr.lua` and `tests/deep` pass in both builds, and the
A/B shows no sweep regression.

**Status: done** (`b37ee7bc`).

**Decisions:**
- **Sentinel heads instead of null-means-head.** The list heads that code
  manipulates by address (`allgc`, `finobj`, `tobefnz`, and the five gray
  lists) became sentinel nodes embedded in `global_State` (`GCObject` /
  `GCListed`). Every `GCObject** p` became `GCObject* p` (predecessor node):
  `*p` → `p->next`, `p = &curr->next` → `p = curr`. This keeps every sweep,
  finalizer and gray-list algorithm line for line, with no null-means-head
  branches and no list enum. In C++ reading a head is the same single load
  (`g->allgc.next`). In Java it is one extra field load from a final
  sentinel object. `linkgclist_(o, list)` keeps upstream's
  `*pnext = *list; *list = o` shape.
- **`twups` and open upvalues** have no sentinel (one would be a whole
  `lua_State`/`UpVal`), so they use a predecessor pointer where `nullptr`
  means the head. `upisopen` now tests `owner != nullptr`, which is
  already null exactly when closed.
- **`lundump` `loadString`** takes a setter lambda (a template parameter,
  cold path) instead of `TString**`. That keeps upstream's order: the slot
  is written before `luaH_setint` can trigger an emergency GC, so the
  string stays anchored.
- **`sweepgen`**'s `GCObject** pfirstold1` (`&g->firstold1` or a local
  dummy) became `bool setfirstold1`, with identical effect.
  `checkpointer` returns the corrected pointer.
- **Deferred:** `lvm.cpp` `frameentry` (pointers to interpreter locals),
  `ldebug.cpp` `const InternalString**` name results and `l_mem* paddedold`
  go to L2. The `loadlib.cpp` `void**` overlay goes to M4. `lua.cpp`
  stays host code.

**Results:**
- `make full` green.
- Object sizes are unchanged (the `GCListed` base takes the slot each type
  already had). `global_State` is +104 bytes (912 → 1016) for the
  sentinels.
- A/B against `731cae2a` (5 interleaved runs, pinned CPU), median: all.lua
  5.52s → 5.56s (+0.7%); b_call, b_string, b_vm and b_table are within
  noise.
- b_gc runs 2.44s → 2.09s, and its peak RSS 62.9 → 68.5 MB. This is GC
  *phase*: `global_State`'s size is charged to `GCtotalbytes`, and +104
  bytes shifts when cycles start. With the start pinned to the old size,
  the `collectgarbage("count")` trajectories after a full collection are
  identical to baseline, so the collector algorithm is unchanged. M2's
  cost constants will remove the dependence on `sizeof(global_State)`.

### L2. Out-parameters

**Effort:** medium · **Risk:** low · **A/B** (the `lvm.h` converters)

Java can't take the address of a local. There are about 34 primitive
out-params and about 10 reference out-params. Treat hot and cold sites
differently:

- **Hot converters keep their shape.** `luaV_tonumber_`, `luaV_tointeger`,
  `luaV_tointegerns` and `luaV_flttointeger`, the inline
  `tonumber`/`tonumberns`/`tointeger`/`tointegerns` (`lvm.h`), and
  `lua_numbertointeger` (`llimits.h:150`) are used in about 32 places in
  `lvm.cpp`, mostly in opcode bodies.
  - The out-param becomes a pointer to a small result struct
    (`IntResult { lua_Integer v; }`, `NumResult`) instead of a
    pointer to a primitive. In C++ this is the same stack slot and identical
    code; in Java it is an object reference.
  - The Java port decides between a local allocation (relying on escape
    analysis after inlining) and a reusable per-`LuaState` holder.
  - Do **not** use `std::optional<lua_Integer>` here: its Java counterpart
    (`OptionalLong`) allocates.
- **Cold sites return values.** Return a value, an `std::optional` of a
  view or reference, or a small named struct:
  - `lobject.cpp`: `isneg`, `lua_strx2number`, `str2dec`, `l_str2dloc`,
    `l_str2d`, `l_str2int`
  - `lcode.cpp` `isSCnumber`
  - `ldebug.cpp`: `getbaseline`, `basicgetobjname(int* ppc)`, and every
    `const InternalString** name` (in `funcnamefromcall`, `getfuncname`,
    `kname`, `rname`, `getobjname`, `funcnamefromcode`)
  - `lbaselib.cpp:60` `b_str2int`, `ldblib.cpp:90` `getthread`,
    `lauxlib.cpp:877` `skipcomment`, `lstrlib.cpp:1896` `getoption`
  - `lapi.cpp:1755` `owner`, `lapi.cpp:1851` `pf`,
    `lundump.cpp:134` `sl`
- **Jump lists:** `luaK_concat(fs, int* l1, l2)` and
  `test_then_block(ls, int* escapelist)` change to
  `l1 = luaK_concat(fs, l1, l2)`. Callers usually pass a field (`&e->t`),
  and assigning a field is free in both languages.
- Pointers to *objects* that the callee fills in (`TValue* res`) are fine
  in Java and stay. `ltable.h:136` `fval2arr(lu_byte* tag)` moves to M3.

**Done when:** no parameter of primitive pointer type remains outside the
result-struct form, and no `const InternalString**` remains.

**Status: done** (`54a708c1`, `94667cf7`).

**Decisions:**
- **Hot converters use holders.** `IntResult`/`NumResult` (in `llimits.h`)
  replace the primitive out-param in every converter and in `forlimit`.
  Call shapes are unchanged; readers use `.v`. Port note: in Java, hoist
  the holders out of the `luaV_execute` opcode bodies (one allocation per
  `luaV_execute` entry) instead of relying on escape analysis.
- **`frameentry` is expanded inline.** `84668e05`'s shared forced-inline
  helper wrote six interpreter locals (`cl`, `k`, `code`, `pc`, `base`,
  `trap`) through pointers. Java can't do that, and keeping them true
  locals matters most for Java interpreter speed. A holder struct would
  turn `pc`/`base` into field loads on every instruction. The seven
  expansions are upstream's `startfunc`/`returning` block text. C++ codegen
  is unchanged, since the helper was already forced inline.
- **`isSCnumber`** keeps upstream's quirk that a failed float attempt
  leaves `isfloat` set (it is encoded into the instruction). So it fills an
  `SCImm {im, isfloat}` holder shared across attempts rather than
  returning a value.
- **Cold sites return small named structs**: `VarName`, `RegName`,
  `BaseLine`, `LocalSlot`, `NumParse`/`IntParse`, `Sign`, `PackOption`,
  `PackDetails`, `Capture`, `Utf8Code`, `ThreadArg`, `FirstChar`,
  `GenSweep`. `luaK_concat` and `test_then_block` return the jump-list
  head (`e->t = luaK_concat(fs, e->t, l2)`).
- **Left for later tasks:** `ltable.h` `fval2arr(lu_byte* tag)` (M3);
  `Instruction*` interior pointers into `code` (`lparser.cpp:1797,2185`,
  `lcode.cpp`) (L4). `lmathlib` `uint64_t* state` and `lcode`
  `finaltarget(Instruction* code)` are array parameters, which are fine
  in Java.

**Results:** `make full` green. A/B against L1 (`b37ee7bc`), 5–11
interleaved runs, medians: all.lua 5.06s → 5.00s; b_vm 0.62 → 0.63s;
b_arith 0.75 → 0.72s; b_call, b_string and b_gc within noise. RSS
unchanged.

### L3. `void*` context callbacks

**Effort:** medium · **Risk:** medium · **A/B** (`pcall`, coroutine resume)

Upstream pairs a function pointer with a `void* ud` that points at a typed
struct (`SParser`, `CloseP`, `CallS`, `LoadS`, `LoadF`, `str_Writer`,
the `int` in `resume` at `ldo.cpp:835`, `firstcall` at
`lbaselib.cpp:416`, `BuffFS`). The struct is the context; only the
`void*` erasure can't be transcribed.

- **Protected calls** (`Pfunc`, `luaD_rawrunprotected`, `luaD_pcall`,
  `LUAI_TRY`):
  - Each context struct becomes a class deriving from one small base with a
    single virtual `run(lua_State*)`. The function that today casts `ud`
    becomes that method, and data-free calls (`unroll`, `f_luaopen`) get
    stateless instances.
  - This is one indirect call, like the original function pointer, and maps
    to a Java interface. Do not use templates or capturing lambdas: they
    change the code's shape and would allocate per call in Java anyway.
  - Rename the leftover `struct lua_longjmp`/`errorJmp` to reflect what it
    now holds (an exception recovery frame).
- **Public hooks** (`lua_Reader`, `lua_Writer`, `lua_WarnFunction`,
  `lua_Alloc`): same pattern, an abstract interface replacing `fn` + `ud`.
  This is a public API change, so update `test_public_api.cpp` and
  `tests/libs`.

**Done when:** no `void* ud` parameters and no `static_cast<X*>(ud)`
remain.

**Status: done** (`7bd8c456`, `d31c19b8`).

**Decisions:**
- **`ProtectedCall`** (in `ldo.h`) has one virtual `run(lua_State*)`.
  Upstream worker functions keep their bodies and take the typed struct
  (`closepaux(L, CloseP*)`, `f_parser(L, SParser*)`, `f_call(L, CallS*)`,
  `pushbuff(L, BuffFS*)`); the struct's `run` forwards to them. Stateless
  or scalar-context calls (`unroll`, `resume(L, int n)`, `f_luaopen`,
  `dothecall`) get small wrapper structs.
- **`lua_longjmp`/`errorJmp` keep their upstream names.** C0's
  "keep upstream names" wins over the rename this task originally listed.
- **The warn interface receives the calling `lua_State`.** Upstream's
  lauxlib warn state machine (`warnfoff`/`warnfon`/`warnfcont`) uses
  `ud = L` only to call `lua_setwarnf`, which acts on the shared
  `global_State`. So the three became stateless static interface objects,
  and no per-state warn object is needed.
- **`lua_getallocf`** returns the `lua_Alloc*`; `lua_Allocator` is gone.
- **`ltests.cpp`'s `Memcontrol`** (an aggregate in `lhack.h`) stays a
  data struct. A `DebugAlloc` adapter forwards to `debug_realloc`.

**Results:** `make full` green. A/B (`b_call`: pcall, coroutine resume,
`__index` loop) against `94667cf7`: 2.30s → 2.30s median; all.lua
within noise.

### L4. Raw byte operations

**Effort:** low · **Risk:** low

- `bytes.h:197,205,211` `memcpy`/`memset`/`memmove` stay. They are the
  sanctioned primitives and map to `System.arraycopy`/`Arrays.fill`. Their
  signatures become `(dst, dstoff, src, srcoff, len)` with no
  `.data() + pos` at callers.
- `bytes.h:126-144`: float/double ↔ bytes through `reinterpret_cast` +
  `memcpy` → `std::bit_cast` plus an explicit little-endian byte order
  (Java: `Double.doubleToRawLongBits`).
- `lzio.cpp:64`: `memcpy` into `static_cast<char*>(b)` → `luaZ_read`
  takes a `byte_span` plus offset.
- `loslib.cpp:261`: `strftime(buff.data() + off, ...)` moves behind
  `os_api`.
- The `ltable.h` `getArrVal`/`getArrTag` interior pointers move to M3,
  which rewrites that storage anyway.

**Done when:** `\.data\(\)\s*[+-]` matches only `os_api.cpp` and
`ltable.h` (until M3), and `memcpy|memmove|memset` matches only `bytes.h`.

**Status: done** (`b0e36311`), with two items moved to other tasks.

**Decisions:**
- **`byte_view::from_double`/`from_float` are removed.** They returned a
  view over the caller's local `double`, which Java can't express.
  `byte_span::write_double`/`write_float` now write little-endian bytes
  from `std::bit_cast` bits. `toDouble`/`toFloat` assemble little-endian
  bytes the same way. Compilers fold both into a single load or store.
- **`lparser`** (`fixforjump`, call statement) indexes `code[pc]` instead
  of holding an `Instruction*` into it. `lcode` `finaltarget` takes a
  `std::span<const Instruction>`.
- **`loslib`** calls `os_strftime(out, off, fmt, tm)`. The rest of
  `loslib`'s libc time calls are a new task, L12.
- **Moved to L9:** `lzio` `luaZ_read(z, void* b, n)` and `lundump`
  `loadBlock(S, void*, size)` / `ldump` `dumpBlock(D, const void*, size)`.
  These read and write arbitrary typed memory as bytes, which is the chunk
  I/O problem L9 owns.
- **Moved to M3:** the `ltable.h` `getArrVal`/`getArrTag` pointers, and
  `lvm`'s `k + n` arithmetic on the `Proto::k` base pointer. Plain
  array-base pointers used only with indexing (`lvm` `code`) are arrays in
  Java and stay.

**Results:** `make full` green. A/B against `d31c19b8`: b_string (pack and
unpack of floats) 0.65s → 0.64s median; all.lua within noise.

### L5. Addresses as values

**Effort:** medium · **Risk:** medium · **A/B** (memory, if an id field is
added)

Java has no addresses. Two separate needs sit behind the current address
uses, and they have different costs:

1. **Hashing** (`ltable.cpp:109-117` `hashpointer` via `point2uint`)
   needs a well-distributed hash, not uniqueness. Route it through one
   function, `identityhash(const void*)`, which is the address in C++ and
   `System.identityHashCode` in Java. This costs no memory, and the
   behavior in C++ is unchanged.
2. **`%p` and `lua_topointer`** need uniqueness per live object:
   `pm.lua:448` and `tests/deep/identity.lua` assert that distinct objects
   format differently. `identityHashCode` can collide, so Java needs a
   unique id.
   - Route `lobject.cpp:877`, `lstrlib.cpp:1725`, `util_cast.h:23` and
     `lapi.cpp:~587` through one `identitytext(...)` / `identityof(...)`
     function. In C++ it stays the address.
   - **Decision, with an A/B on memory:** a 64-bit id on `GCObject`
     assigned from a counter on `global_State` (8 bytes per object), or a
     lazily populated id side table on `global_State` (no cost for objects
     that are never printed). Recommended: the side table, unless the A/B
     shows its lookup cost matters (it is used only by `%p`/`topointer`,
     so it shouldn't).
- `lua_rawgetp`/`lua_rawsetp`'s `const_cast<void*>` (`lapi.cpp:1041,1268`)
  goes away once light userdata keys use the same identity path. The
  `util_cast.h:27-28` `as_userdata(int)` forging is `ltests.cpp`-only and
  becomes a port-time note.

**Done when:** `point2uint`, `uintptr_t` and `const_cast` appear only
inside the two identity functions and `os_api.cpp`.

**Status: done** (`349f4409`).

**Decisions:**
- **Hashing keeps upstream's `point2uint`** (`llimits.h`, two overloads)
  as its single hook instead of a renamed `identityhash`; C0 keeps upstream
  names. Java maps it to `System.identityHashCode`.
- **`%p` text** goes through `util::pointertext(out, pos, p)` (object and C
  function overloads) at all three sites (`lobject` `luaO_pushvfstring`,
  `lstrlib` `string.format`, `util::func_to_string`).
- **Java `%p`/`lua_topointer` uniqueness**: no C++ change was needed;
  addresses are already unique in C++. In Java, `pointertext` and
  `lua_topointer` read an id from a lazily filled side table on
  `GlobalState`, so objects that are never printed cost nothing (a
  port-time choice; see the port-time notes).
- **Light userdata from a `const void*`** goes through a
  `setpvalue(TValue*, const void*)` overload, which holds the one
  `const_cast`. `const` disappears in Java.
- **Remaining by design:** `util::as_userdata` and the `const_cast`s in
  `ltests.cpp`. `ltests.cpp` is immutable, and the helper only forges test
  handles.

**Results:** `make full` green; no hot path changed.

### L6. Width-dependent `sizeof` in Lua-visible behavior

**Effort:** low · **Risk:** low

Java has no `sizeof`. Where a size is observable, pin it as a named constant,
following `9d35bf2c` (the string.pack `!` alignment pin).

- `lstrlib.cpp:1804-1946`: `string.pack` option widths and `SZINT`.
  Also `2051-2081` for float/double.
- `lstrlib.cpp:1542,1625,1711`: `sizeof(prefix)`/`sizeof(body)` on local
  arrays → `std::size()` (Java: `.length`).
- `lstrlib.cpp:2380` → `std::size(stringmetamethods)`.
- `ldump.cpp`/`lundump.cpp` (about 31 sites) → the chunk format's fixed
  widths.
- `lhack.h:58` `LUAL_BUFFERSIZE` → a literal. `LUA_EXTRASPACE` is
  handled by M5.

Accounting `sizeof`s are handled by M2.

**Done when:** `tpack.lua` and `tests/deep` pass, and `sizeof` remains
only in accounting and `static_assert`s.

**Status: done** (`29b16559`) for the Lua-visible sizes.

**Decisions:**
- **Each pinned width has a `static_assert`** against the host `sizeof`,
  so the C++ build still refuses a host where they differ.
- **`luaO_pushvfstring` `%c`** wrote through a view over a single local
  `char`; it is now a one-element array.
- **`sizeof` still present, owned by other tasks:**
  - **M2 (accounting and GC pacing):** `objsize`, `luaF_protosize`,
    `luaH_size`, `luaE_threadsize`, `sizeLclosure`/`sizeCclosure`,
    `LUAI_GCSTEPSIZE`, `lgc` step `sizeof(void*)`, `global_State`.
  - **M1 (limits):** `MAX_SIZE`, `MAX_LMEM`, `MAXASIZEB`, `MAXHSIZE`,
    `MAXABITS`, `MAXSTACK_BYSIZET`, and the
    `MAX_SIZE - sizeof(TString/Udata)` checks.
  - **L9 (chunk I/O):** `lundump`/`ldump`.
  - **M4 (userdata payloads):** the `sizeof(LStream/RanState/GMatchState
    /void*)` userdata sizes.
  - **M5:** `LUA_EXTRASPACE`.

**Results:** `make full` green, including `tpack.lua`.

### L7. Signed compatibility

**Effort:** high (many sites, mostly mechanical) · **Risk:** medium · **A/B**

Java has only signed `byte/short/int/long`. Almost no unsigned use in the
sources depends on being unsigned, and none of it can exceed the signed
maximum, so the default is to make it signed. Do it by changing declared
types, not by adding casts. Each step below is its own commit series.

1. **Counts, indices, sizes and limits:** `unsigned`, `unsigned int`,
   `unsigned short`, `lu_mem`, `l_mem`, and `size_t` wherever it is not a
   standard-container size → `int` / `lua_Integer` / `int64_t`, chosen
   by range. Examples: `nuvalue`, table sizes and hints (`asize`,
   `lenhint`, `MAXASIZE`), `findindex`, `computesizes`, `nums[]`, the
   `lstrlib` match offsets (`ptrdiff_t` → `lua_Integer`) and
   `lua_KContext` (`ptrdiff_t` → `int64_t`). `size_t` stays on
   `.size()` and span/view lengths. Where a signed value meets a `.size()`,
   change the declaration so the comparison is between like types;
   don't cast at the use site.
2. **`lu_byte` fields:** tags, variant bits, GC marks and age bits, TM
   flags and `CallInfo` status stay below 128 and become `int8_t`-safe as
   is. Fields that can reach 255 (`Proto::maxstacksize`, `numparams`,
   `FuncState::nups`/`freereg`, the instruction argument limits) widen to
   `int` or `short`, so Java never needs `& 0xFF`.
3. **Genuine wraparound and logical shifts:**
   - Lua integer arithmetic must wrap (`intop`, `l_castS2U`/`l_castU2S`),
     and string/seed hashing wraps. Signed overflow is undefined in C++, so
     these stay unsigned inside a few named helpers (`wrapadd`,
     `wrapmul`, …) that hold the only casts. In Java the helpers are
     plain `+`/`*`.
   - Lua's `>>` is a logical shift (`luaV_shiftl`), and so are `Instruction`
     field decodes. These use a named `ushr` (Java: `>>>`).
   - `math.ult` and the unsigned for-loop count use named `ult`/`udiv`
     (Java: `Long.compareUnsigned`/`divideUnsigned`).
   - `Instruction` itself becomes `int32_t`, with decoding through `ushr`.
     This preserves the bit-container semantics (see the port-time notes).

**Done when:** `unsigned`, `lu_mem`, `ptrdiff_t` and `uintptr_t` appear
only inside the wraparound/shift helpers, `os_api`, and declarations tied to
`size_t` container sizes. The number of casts is lower than at the start
(record the before/after count in Results).

**Status: done** (`0c55701a`, `65630970`, `7e85b1a7`).
- [x] Tables (`0c55701a`): sizes, counters, hints and in-part indices are
  `int`; traversal indices and borders are `lua_Integer`.
  - `MAXABITS` is now 30 (was 31) so a power-of-two array part fits `int`.
    Arrays past 2³⁰ entries keep living in the hash part, so nothing
    changes for Lua code; `MAXHBITS` stays 30.
  - Doubling loops guard the last doubling instead of relying on unsigned
    wraparound; binary searches use `i + (j - i) / 2`.
  - `hashmod`'s full-width unsigned `%` goes through `l_umod`
    (`llimits.h`).
  - A/B: b_table 0.23s → 0.23s, b_gc 1.87s → 1.87s.
- [x] Integer arithmetic (`65630970`): `llimits.h` has the wraparound
  helpers `l_intadd`/`l_intsub`/`l_intmul`/`l_intunm`, the bit helpers
  `l_intband`/`l_intbor`/`l_intbxor`/`l_intbnot`, `l_ult` and `l_umod`.
  These hold the only signed/unsigned casts for Lua integer math. `lvm`,
  `lobject` `intarith`, `ipairs`, `ltablib` and `math.abs`/`math.ult` use
  them. The `FORLOOP` count is a `lua_Integer`. `luaV_idiv`/`luaV_mod`
  test `n == 0 || n == -1` instead of `(lua_Unsigned)n + 1u <= 1u`.
  Range checks are written as signed comparisons.
- [x] Byte-width fields (`65630970`): `Proto::numparams`/`maxstacksize`,
  `Upvaldesc::idx`, `expdesc` `ind.t`/`ind.idx`/`var.ridx`,
  `Vardesc::ridx`, `FuncState` `nups`/`freereg`/`iwthabs`, and
  `lua_Debug` `nups`/`nparams`/`extraargs` widen to `int`.
  `luaO_codeparam`/`luaO_applyparam`/`ceillog2` and `gcparams` are `int`.
  Library counters (`LoadF::n`, `tonumber` base/digit, `table` `IdxT`,
  `RANLIMIT`) are `int` or `size_t`.
- [x] UTF-8 and the remaining sites (`7e85b1a7`): code points are `int`
  (max `0x7FFFFFFF`), and `luaO_utf8esc`/`FmtArgs::utf8` take
  `lua_Integer`. `luaS_newudata` takes `int nuvalue`. `l_ult` takes
  `lua_Integer`, so callers don't cast. The `table.unpack`/`insert`/`remove`
  checks use `l_intsub` + `l_ult`.

**Results:**
- `make full` passes. The A/B results are neutral:
  - all.lua: 5.48s → 5.39s min (load-shifted rerun 5.85s → 5.90s)
  - b_vm: 0.78 → 0.77
  - b_arith: 0.85 → 0.88 (median 0.98 → 0.95)
  - b_call: 2.44 → 2.40
  - b_string: 0.62 → 0.62
  - b_table: 0.25 → 0.25
- RSS stays within 1%.
- Struct sizes: `Proto` 160 → 168, `Upvaldesc` 16 → 24, `global_State`
  1016 → 1024 bytes. These are fixed per-function costs, and no per-value
  object grew.
- **Cast count** (C-style + named casts, excluding `ltests`): 1252 before
  L7 (`e8102b82`), 1113 after.

**Decisions so far:**
- **`ptrdiff_t` stays.** It is already a signed 64-bit type (Java `long`);
  renaming it to `lua_Integer` would be churn with no portability gain.
- **`Instruction` stays `l_uint32`.** Every field decode in `lopcodes.h`
  masks after shifting (`getarg`, `GET_OPCODE`), so the result is the same
  whether Java uses `>>` or `>>>`. Instruction words are already safe with
  signed integers.
- **The array accessors and VM integer-key fast paths move to M3.**
  `getArrTag`/`getArrVal`/`farr2val`/`fval2arr` (`lua_Unsigned k`) and the
  `(lua_Unsigned)key - 1 < size` fast paths in `lvm`/`lapi` are rewritten
  there for the column layout, so their index types change once.
- **Bit containers stay unsigned.** These are bit patterns, never
  quantities: string hashes (`TString::hash`, `luaS_hash`, `l_hashfloat`),
  seeds (`luai_makeseed`, `global_State::seed`, the `lua_newstate` seed
  in the public API), the sort pivot randomizer, `callstatus`
  (`l_uint32`), and instruction words (`CREATE_*`). Java holds them in
  `int` and uses `>>>` where they are shifted.
- **`(unsigned char)` byte reads stay.** Widening a `char` to 0..255
  (`zgetc`, `lstrlib` ctype arguments, `byteAt`) is the C spelling of
  Java's `& 0xFF`. Each one is a single conversion with no alternative
  type to declare.
- **Moved to other tasks:**
  - `lu_mem`/`l_mem` accounting, closure and thread sizes → M2
  - `ldump`/`lundump` `dumpAlign`/`loadAlign` and the varint → L9
  - `bytes.h` `write_number(unsigned long long)` stays, as the
    formatter for `lua_Unsigned` (Java: `Long.toUnsignedString`)
- **Signed-overflow traps.** Wherever the original relied on unsigned
  wraparound for a range test (`(lua_Unsigned)pos - 1u < e`), the
  rewrite goes through `l_intsub`/`l_ult`, not plain signed `-`/`<`.
  `pos` can be `minint`, so plain signed subtraction would be undefined
  behavior.

### L8. Locale routing

**Effort:** low · **Risk:** low

The locale seam already exists: `locale_util.*` wraps the ctype classes,
`tolower`/`toupper`, the radix point and `setlocale`, and `strcoll` sits
in `os_api`. Some direct libc ctype calls still bypass it, and they are
upstream's locale-sensitive calls:

- `liolib.cpp:388,412`: `isxdigit`/`isdigit`/`isspace` when reading
  numbers
- `lstrlib.cpp:1248,1346,1349,1376,1432,1438,1602`: `isdigit`/`isalpha`
  in format and `gsub` handling
- `lbaselib.cpp:75-81`: `isalnum`/`isdigit`/`toupper` in `tonumber` with
  a base

Route each through `locale_util` and keep upstream's locale-sensitive
semantics; don't swap in `lctype`'s ASCII tables. In Java, `locale_util`
is then the single file to reimplement.

**Done when:** no libc ctype call exists outside `locale_util.cpp`.

**Status: done** (`2b039407`). `locale_util` gains `locale_isalpha`,
`isdigit`, `isalnum`, `isxdigit` and `isspace`, written out of line like
the existing `locale_tolower`/`toupper`. `locale_util.cpp` is now the only
file that includes `<cctype>`.

**Results:** `make full` passes. The calls are in format-spec parsing, in
`tonumber` with a base, and in `io.read("n")`, none of which is a VM hot
path, so this task had no A/B.

### L9. `lundump` fixed-buffer loading

**Effort:** medium · **Risk:** medium · **A/B** (instruction fetch is the
hottest path in the VM)

Fixed-buffer loads are an upstream 5.5 feature and asserted by the
immutable tests (`api.lua:550-590` requires the load to account fewer than
2000 bytes). So the code must keep aliasing the buffer rather than copying
it. Today that aliasing is a pointer cast:

```cpp
lundump.cpp:187  f->code = std::span<Instruction>((Instruction*)(getaddr_(S, n * sizeof(Instruction))), n);
lundump.cpp:284  f->lineinfo = std::span<ls_byte>((ls_byte*)(getaddr_(S, n * sizeof(ls_byte))), n);
lundump.cpp:297  f->abslineinfo = std::span<AbsLineInfo>((AbsLineInfo*)(getaddr_(S, n * sizeof(AbsLineInfo))), n);
```

Java can't overlay `int[]` on `byte[]`, but it can read little-endian
`int`s out of a `byte[]` at array-like speed (a `byteArrayViewVarHandle`).

- **Proposed:** `Proto::code` becomes a byte-backed store (bytes +
  offset) for *all* protos, read through one `fetch(pc)` accessor that
  decodes a little-endian `int32`. In C++ this compiles to a plain load.
  Fixed protos alias the host buffer; others own their bytes.
- `lineinfo` is bytes already. `abslineinfo` gets the same treatment
  through an `(pc, line)` accessor pair.
- **A/B is the gate.** If C++ instruction fetch regresses beyond threshold,
  keep `std::span<Instruction>` in C++, confine the cast to one
  `getaddr`-side function, and record this as a port-time decision
  instead.

**Done when:** `lundump.cpp` has no pointer casts, and `api.lua`'s
fixed-buffer test passes unchanged.

**Status: done** (`0fa14389`).
- `lzio` takes and returns typed views: `luaZ_read(ZIO*, std::span<std::byte>)`
  and `luaZ_getaddr(...) -> std::optional<byte_view>`. The single overlay
  of a fixed block as a typed span is `luaZ_overlay<T>` in `lzio.h`.
- `lundump`/`ldump` get back upstream's `loadVector`/`loadVar`/`getaddr`/
  `checknum` and `dumpVector`/`dumpVar`/`dumpNumInfo` as small templates
  over spans; the macro-expanded `sizeof` blocks are gone. `bytes.h`
  `as_byte_view` bridges `std::byte` to `byte_view`.
- `dumpAlign`/`loadAlign` use `size_t`; the varint counter is `int`.
  `LoadState::fixed` is `bool`.

**Decision: code storage stays `std::span<Instruction>` in C++.**
- **Problem:** the proposal made `Proto::code` byte-backed in C++ too.
  That touches about 150 sites (lcode patches instructions in place, and
  `ltests` reads them) for no C++ gain.
- **Why the C++ change isn't needed:** every access is already index
  form, `code[pc]` and `code[pc] = x`, so Java can choose its storage
  without C++ changing shape.
- **Choice (shape and performance first):** keep the span and confine the
  cast to `luaZ_overlay`. See the port-time note **Instruction storage**.
- **Rejected for Java:** copying a fixed chunk into an `int[]` either
  goes uncharged, which violates the accountability principle, or is
  charged, which breaks `api.lua:575`'s fewer-than-2000-bytes assertion.
  So Java must alias.

**Results:**
- `make full` passes, including `api.lua`'s fixed-buffer tests.
- A/B (min): all.lua 5.17s → 5.12s, b_dump (dump+load ×3000) 0.49s →
  0.49s, b_vm 0.67s → 0.61s (noise; fetch is unchanged). RSS is equal.

### L10. `loadlib` casts behind `os_api`

**Effort:** low · **Risk:** low

Keep upstream's `loadlib` structure (handle userdata, `_CLIBS`, `__gc`)
and move only the native typing behind `os_api`:

```cpp
loadlib.cpp:48   return reinterpret_cast<void*>(lib);
loadlib.cpp:53   lua_CFunction f = reinterpret_cast<lua_CFunction>(os_dlsym(reinterpret_cast<OsLib*>(lib), sym));
loadlib.cpp:158  os_dlclose(reinterpret_cast<OsLib*>(*p));
```

`os_api` gains `os_dlfunc(OsLib*, byte_view) -> lua_CFunction` and
`loadlib` holds `OsLib*` throughout. The `void**` userdata overlay
(`loadlib.cpp:155,166`) is handled by M4.

**Done when:** `loadlib.cpp` has no `reinterpret_cast`.
**Depends:** M4 for the `void**` overlay.

**Status: done** (`c474c74b`). `loadlib.cpp` has no `reinterpret_cast`.
- `os_dlfunc` returns `int (*)(lua_State*)`, declared without including
  `lua.h` in `os_api.h`. `os_dlsym` stays for `lua.cpp`'s readline
  lookup, which is host-launcher code and is rewritten in Java rather
  than transcribed.
- **Remaining:** `static_cast<OsLib*>` on the `_CLIBS` light userdata,
  which Java expresses as a plain downcast `(OsLib) obj`, and
  `static_cast<OsLib**>` on the `__gc` userdata payload, which M4
  replaces.

**Results:** `make full` passes. This is not a hot path, so there was no
A/B.

### L12. Time functions behind `os_api` (added during L4)

**Effort:** low · **Risk:** low

`loslib.cpp` still calls libc time functions directly: `clock`, `time`,
`gmtime_r`, `localtime_r`, `mktime`, `difftime`, and `struct tm`
throughout `os.date`/`os.time`. Everything else in the host OS layer
already goes through `os_api`, and Java reimplements this with
`java.time`.

Approach: `os_api` gains `os_clock`, `os_time_now`, `os_gmtime`,
`os_localtime`, `os_mktime` and `os_difftime`, plus an engine-side
broken-down time struct, so `loslib` never names `struct tm` or
`time_t`. Keep upstream's field handling in `setallfields`/`getfield`.

**Done when:** `loslib.cpp` includes no `<time.h>` and calls no libc time
function.

**Status: done** (`262116ef`).
- `OsDate` is the engine-side broken-down time. It also carries `gmtoff`
  and `zone`, so `strftime`'s `%z`/`%Z` output is byte-identical
  (checked against the previous build).
- **Naming:** the functions are `os_cpuclock`, `os_now`, `os_gmtime`,
  `os_localtime`, `os_mktime` and `os_timediff`. The planned
  `os_clock`/`os_difftime` names collide with `loslib`'s own Lua entry
  points.
- **Decision:** `time_t` is pinned to 64 bits by a `static_assert` in
  `os_api.cpp`, and times cross the seam as `long long`. Upstream's
  `(time_t)t == t` checks (`l_checktime`, `os.time`'s result) could
  never fail on a supported host, so they are gone, and
  `os.time`/`os.date` accept the same values as before.
  `lt::time_out_of_bounds` is now unused.

**Results:** `make full` passes. `os.date` output for `%c`, `%Z`, `%z`,
`!%c` and `*t`, plus `os.time{...}`, was spot-checked against the
previous build. These aren't hot paths, so there was no A/B.

### L11. Cast elimination

**Effort:** medium · **Risk:** low

After L1–L10 (and especially L7), list every remaining cast: C-style casts
(`((int)(size))`, `(StkId)n`, `(lua_Unsigned)n`, `(void**)`) and
`static_cast`s between arithmetic types. Remove each by correcting a
declared type. What legitimately remains:

- `GCObject` hierarchy downcasts (fine in Java)
- the wraparound/shift helpers (L7)
- the identity functions (L5)
- `os_api.cpp`

**Done when:** every remaining cast falls in one of those groups, and the
before/after cast count is recorded in Results.

**Status: done** (`d2dd5624`, `307f8fbb`, `ec3189cd`).
- **Mechanical pass:** about 240 casts removed from `.cpp` and header
  files.
  - **Candidates:** only casts in a value context (argument, initializer,
    assignment, return, comparison), where the implicit conversion is
    the same one the cast performed. Casts on an operand of an
    arithmetic, bitwise, shift or compound-assignment operator were
    never candidates, because removing them changes the type the
    expression is evaluated in.
  - **Check:** each removal had to leave the release (`-O2`) and debug
    (`-Og -DLUA_DEBUG`) object code byte-identical, and each commit
    compares identical across all TUs.
  - Byte-identical code alone is not enough: an earlier run without the
    context rule turned `(lua_Unsigned)a - (lua_Unsigned)b` into a
    signed `a - b` that GCC happened to compile identically. That run
    was discarded.
- **`StkId` is signed (`ptrdiff_t`).**
  - `int` was tried first. It cost 3–5% on C-call paths (pcall
    1.38s → 1.45s, `__index`/calls 0.72s → 0.74s), because every
    `stack[func + idx]` needed a sign extension. `ptrdiff_t` keeps the
    64-bit addressing and is neutral (pcall 1.38s → 1.38s, b_vm
    1.14s → 1.07s). Java uses `int`, because values are bounded by
    `LUAI_MAXSTACK`.
  - Signed indices required two shape fixes, both restoring upstream's
    forms:
    - `NOSLOT` is -1, and upvalue origins count down from
      `UPVALORIGIN = -2`. They were `~0` and `~0 - 256`, which relied
      on unsigned ordering.
    - `lua_checkstack`/`luaD_checkstack` test
      `stack_last - top > n`, as upstream does, instead of
      `top + n < stack_last`. The latter overflows for
      `lua_checkstack(L, INT_MAX)`, which `test_public_api` exercises.

**Decision: what legitimately remains** (the plan's groups, extended).
Each remaining cast is one Java also needs, or is owned by a later task:
- `GCObject` hierarchy downcasts (`static_cast<Table*>` …): Java casts
  the same way.
- **Narrowing to a smaller declared type**, such as `(int)` from a
  `.size()`, a `StkId` difference or a `lua_Integer` that was already
  range-checked, or `(lu_byte)`/`(char)` into byte fields. Java needs an
  explicit `(int)`/`(byte)` at the same places.
- **Integer ↔ float conversions** that set the evaluation type
  (`(lua_Number)i` in float arithmetic). Java needs them too.
- **`lua_Unsigned` conversions.** They mark where Lua's unsigned
  semantics start: the for-loop iteration count, `luaV_shiftl`'s logical
  shifts, `math.random`'s `project`, `string.pack`/`unpack`, and the
  dump varint/zigzag. Java transcribes the operators that follow as
  `>>>`, `divideUnsigned` and `compareUnsigned`.
- **Bit containers:** hashes, seeds, `Instruction` encodes and
  `callstatus` (see L7).
- `(unsigned char)` byte reads, the `& 0xFF` idiom (see L7).
- `(void)x` for unused parameters (Java: nothing).
- **Owned elsewhere:**
  - M2: `lu_mem` accounting sums in `lfunc`, `lgc`, `lstate`, `ltable`
    and the closure sizes.
  - M3: the `(lua_Unsigned)k - 1u` array fast paths.
  - M4: the userdata payload `static_cast<T*>(lua_touserdata)`.
  - Identity: `util_cast.h`, `luaZ_overlay`, `as_byte_view`, and
    `os_api.cpp`.

**Results:**
- **Cast count:** C-style casts to arithmetic or engine types, pointer
  casts and named casts, excluding `ltests`, `test_public_api`,
  `os_api.cpp`, `lua.cpp` and `lhack.h`.

  | Point | Total | Arithmetic |
  |---|---|---|
  | Plan start (`24eee778`) | 985 | 811 |
  | Before L7 (`e8102b82`) | 970 | 808 |
  | After L7 (`7e85b1a7`) | 831 | 669 |
  | After L11 | 573 | 416 |

  (L7's own Results used a looser counter that also counted
  parenthesized macro arguments.)
- `make full` passes. A/B for `StkId` is above; the cast removals are
  code-identical by construction.

---

## Phase M — memory changes

Changes to layout, payloads and accounting, so that a Java object graph
reproduces today's behavior, including GC pacing, `collectgarbage("count")`
and the out-of-memory paths in `memerr.lua` and `test_public_api.cpp`.

### M1. Limit parity audit (no new limits)

**Effort:** low · **Risk:** low

Compare every size limit with upstream 5.5 on a 64-bit host:

- `MAX_SIZE`, `MAXASIZE`, `MAXHSIZE` (`ltable.cpp:52-80`)
- `LUAI_MAXSTACK`, `LUAI_MAXCCALLS`, `MAXUPVAL`, `MAXREGS`
- string, `string.rep` and `table.concat` limits
- the `lhack.h` debug overrides

Any limit that drifted from upstream is restored; nothing here introduces a
cap. Java-side constraints (a `byte[]` holds at most about 2³¹ bytes,
while upstream allows larger strings) are recorded as port-time notes, not
imposed in C++.

**Done when:** a written table of limit → upstream value → our value, all
equal.

**Status: done** (audit; no code change).

Upstream reference: the `master` branch (stock Lua 5.5 C sources), 64-bit host. Debug values are `ltests.h` (upstream) vs `lhack.h` (ours).

| Limit | Upstream | Ours | |
|---|---|---|---|
| `MAX_SIZE` | `LUA_MAXINTEGER` (size_t ≥ lua_Integer) | same | = |
| `MAX_LMEM` | 2⁶³−1 | same | = |
| `MAXABITS` / `MAXASIZE` | 31 / 2³¹ | 30 / 2³⁰ | see below |
| `MAXHBITS` / `MAXHSIZE` | 30 / min(2³⁰, MAX_SIZET/sizeof(Node)) | same | = |
| `LUAI_MAXSTACK` | 1 000 000 (debug 68 000) | same | = |
| `MAXSTACK_BYSIZET` | MAX_SIZET/sizeof(StackValue) − STACKERRSPACE | MAX_SIZET/sizeof(lua_Integer) − STACKERRSPACE | unreachable either way (≫ LUAI_MAXSTACK) |
| `LUAI_MAXCCALLS` | 200 (debug 180) | same | = |
| `MAXUPVAL` | 255 | same | = |
| `MAXVARS` | 200 | same | = |
| `MAX_FSTACK` / `MAXARG_*` | from `SIZE_A/B/C/vB/vC/Bx/Ax/sJ` = 8/8/8/6/10/17/25/25 | same | = |
| `MAXINDEXRK` | `MAXARG_B` (debug 1) | same | = |
| `MAXRESULTS` | 250 | same | = |
| `MAXIWTHABS` | 128 (debug 3) | same | = |
| `LUAI_MAXSHORTLEN` | 40 | same | = |
| `MINSTRTABSIZE` | 128 (debug 2) | same | = |
| string table cap `MAXSTRTB` / `nuse == INT_MAX` | INT_MAX entries | none (`std::vector` buckets) | fewer limits, not more |
| `STRCACHE_N/M` (API string cache) | 53×2 | removed with the C-string API | not a limit |
| `LUA_IDSIZE` | 60 | same | = |
| `LUAL_BUFFERSIZE` | 16·sizeof(void*)·sizeof(lua_Number) = 1024 (debug 23) | 1024 (debug 23) | = |
| `LUA_MAXCAPTURES` | 32 | same | = |
| `MAXCCALLS` (pattern matching) | 200 | same | = |
| `MAX_ITEM` / `MAX_ITEMF` / `MAX_FORMAT` | 120 / 110+DBL_MAX_10_EXP / 32 | same | = |
| `MAXINTSIZE` (pack) | 16 | same | = |
| `MAXUNICODE` / `MAXUTF` | 0x10FFFF / 0x7FFFFFFF | same | = |
| `MAXMISS`, `EXTRA_STACK`, `LUA_MINSTACK` | 10, 5, 20 | same | = |
| `string.rep` / `string.pack` / `luaL_Buffer` growth | against `MAX_SIZE` | same expressions | = |
| `table.create` / `table.unpack` / `table.concat` index ranges | `INT_MAX` | same (signed form) | = |
| `string.byte` / `utf8.codepoint` result count | `INT_MAX` | same | = |

**Decision (`MAXASIZE` 2³¹ → 2³⁰, from L7):** kept. Not Lua-visible: a table whose array part would exceed 2³⁰ entries (≥ 9 GB of values) keeps the excess in the hash part, exactly as upstream does past 2³¹; `#t`, `next`, and indexing semantics are unchanged. It makes the array size an `int` (Java array index) and leaves the C++ and Java limits identical.

### M2. Accounting by named cost constants

**Effort:** medium · **Risk:** medium

GC debt and the sizes passed to the `lua_Alloc` permission hook come from
`sizeof` (46 sites in `lmem.cpp`, plus `luaF_protosize`, `luaH_size`,
`lstate.cpp`, `lfunc.cpp`). Java has no `sizeof`.

- Replace them with one `constexpr` cost table (`cost::Table`,
  `cost::Node`, `cost::Value`, …).
- **Accountability first, exactness second** (see Principles). Each
  constant is a logical per-unit cost that scales with the request (per
  slot, per node, per byte), not a measurement of the host's layout. An
  object with no clean C++ equivalent still gets a charge.
- **Proposed values:** stock Lua 5.5's 64-bit sizes, so that
  `collectgarbage("count")` and GC pacing match stock. That fits the
  invisible-engine principle, and `tests/deep/allocinfo.lua` already
  treats stock as the reference.
- The constants are decoupled from `sizeof`, so M3's layout changes don't
  perturb pacing.
- Verify `gc.lua`, `gengc.lua`, `memerr.lua`, `api.lua` (fixed buffer),
  `allocinfo.lua`, and the `size_deny_alloc`/`counting_alloc` tests in
  `test_public_api.cpp`. Those tests deny or count specific `nsize`
  values, so update them only if they are consumer tests pinned to old
  sizes. If stock values break an immutable test, fall back to today's
  `sizeof` values frozen as constants.

**Done when:** `sizeof` remains only in `static_assert`s.

**Status: done** (`909f3741`).
- `lmem.h` `namespace cost` has one `int` per-unit charge per
  allocation kind:
  - per element: `Byte`, `Ref`, `Instruction`, `Value`, `TValue`,
    `Node`, `LocVar`, `Vardesc`, `Upvaldesc`, `Labeldesc`,
    `AbsLineInfo`
  - per object: `CallInfo`, `TString`, `Udata`, `Table`, `CClosure`,
    `LClosure`, `CFuncInfo`, `UpVal`, `Proto`, `Thread`, `Global`
- **Users:** the typed span allocators, `take_debt`/`free_debt`,
  `objsize`, `luaF_protosize`, `luaH_size`, `luaE_threadsize`,
  `sizeCclosure`/`sizeLclosure`, the GC step unit (`stepsize / cost::Ref`),
  `LUAI_GCSTEPSIZE`, and the `MAX_SIZE - cost::X` and
  `MAX_SIZET / cost::X` guards.
- Accounting sums are `l_mem`, and `lu_mem` is unused (the typedef stays
  with upstream's comment). `MAX_LMEM` is `PTRDIFF_MAX`, and `MAX_SIZE`
  is `LUA_MAXINTEGER` with a `static_assert`.

**Decision: frozen current sizes, not stock 5.5 sizes.**
- The user's accountability guidance puts accountability first and
  exactness second. Today's values already scale (per slot, per node,
  per byte of string or userdata), and no allocation is uncharged.
- Switching to stock values would move GC pacing and
  `collectgarbage("count")` and risk the pinned `nsize` tests, and
  would add nothing to accountability.
- Constants are `int`, so the signed `GCdebt` arithmetic never mixes
  with unsigned.

**Remaining `sizeof`, all outside accounting:**
- `static_assert`s.
- The userdata payload sizes `sizeof(LStream/RanState/GMatchState/
  OsLib*)`, which M4 replaces.
- The chunk-format widths in `ldump`/`lundump` (`sizeof(T)` in
  `loadVector`/`dumpNumInfo`/align). These are the format's
  fixed widths; Java uses `Integer.BYTES`/`Long.BYTES`/`Double.BYTES`.
- `luaZ_overlay`'s element count.
- **Note for M3:** `ltests` `T.querytab`-style size reports still print
  `sizeof(TValue/Node/lua_State)`. Accounting no longer follows them, so
  a layout change shows up there and nowhere else.

**Results:**
- `make full` passes.
- `trace_gc.lua` + `trace_gc2.lua` output is byte-identical before and
  after.
- A/B (min): all.lua 4.90s → 4.86s, b_gc 3.92s → 3.88s,
  b_table 0.41s → 0.40s. RSS is equal.

### M3. Value storage layout: parallel columns

**Effort:** high · **Risk:** medium · **A/B** (time and memory)

`Value {prim, ref}`, `TValue` and `Node` are stored by value inside
arrays. Arrays of small objects in Java mean a header plus a pointer chase
per element. Parallel primitive/reference columns are what perform best
there, and the stack already uses them.

- **Table array part:** `array` (`span<Value>`) → `prim` + `ref`
  columns beside the existing `tags`. The `getArrVal`/`getArrTag`
  interior pointers and `fval2arr(lu_byte* tag)` become `(t, k)`
  get/set accessors. This finishes what `ff278e40` started.
- **Table hash part:** `Node` key/value/next → columns indexed by node.
  This is the highest-risk piece: the collision chains, `lastfree`, and
  `reinserthash`/`exchangehashpart` all touch it. Do it last, after the
  other containers have proven the accessor pattern.
- **`Proto::k`, `Udata::uv`, `CClosure` upvalues, `UpVal::value`**: same
  column form where they are arrays. A lone `UpVal::value` stays a field
  pair.
- **Scalar `TValue`s** (locals, parameters, returns) stay a small struct,
  but copies go through one named operation, so Java can make `TValue`
  immutable or add explicit copies mechanically. Audit the struct-copy
  sites (`*a = *b`, `TValue x = ...`, `setobj*`).
- **One container per commit**, each with an A/B. C++ may lose some cache
  locality (`Node` especially). Record the cost; accept it only where it
  buys the Java layout.

**Done when:** no array stores `Value`/`TValue`/`Node` by value, and
every scalar copy goes through the named copy.

**Status: done**, with a revised target (`122d856d`, `eebae878`,
`223d563a`, `4923c1f5`).
- **Table array part (`122d856d`):** `getArrTag`/`getArrVal` return
  values, not interior pointers. Upstream's names are kept, so its
  comment stays true. `setArrTag` writes a tag, and `fval2arr` takes the
  index instead of a `lu_byte*`.
  - Indices are `lua_Integer`, so the L7-deferred `lua_Unsigned` index
    type is gone.
  - The VM/API/`psetint` fast paths keep their single unsigned range
    test, written `l_ult(l_intsub(k, 1), asize)`.
- **Hash part (`eebae878`):** the GC's four `for (n = gnode(h,0);
  n < limit; n++)` pointer walks are index loops, and `gnodelast` is
  gone. Everything else already used node indices and `next` offsets.
- **Explicit copies (`4923c1f5`):** `TValue`'s copy operations are
  private and defaulted. The only copies are the named friends `getobj`
  (read a copy) and `copyobj` (raw copy), plus the existing `setobj`
  (checked assignment) and the new `copynode`. Moves stay public
  (assigning a fresh value). The compiler now enforces that every
  scalar copy is named. Array growth for `Vardesc` moves elements.

**Decisions:**
- **C++ storage stays as it is; Java gets columns behind the accessors.**
  - The user already tried a physical prim/ref/tag column layout for the
    stack (`6da9b08a`) and went back to `TValue[]` behind the slot
    accessors (`396e7bb5`).
  - M3 follows that precedent: the accessors are the seam, and storage
    is a per-language choice.
  - The table array part was already split (`array` + `tags`,
    `ff278e40`) and stays that way. In Java it becomes
    `long[] prim` + `Object[] ref` + `byte[] tags`.
- **`Node` is not split into columns, in C++ or Java.**
  - A lookup reads key tag, key value and `next` together, so six
    parallel arrays would cost three cache misses where one node record
    costs one. This is the same reason `java.util.HashMap` uses entry
    objects.
  - Java transcribes `Node` as a class with flattened fields that
    extends `TValue`. Then `&gnode(t,i)->val`, which `getgeneric`/
    `luaH_Hgetshortstr` return as `const TValue*`, is the node itself,
    and `copynode` copies fields.
- **`Proto::k`, `Udata::uv`, `CClosure` upvalues and `UpVal::value`
  stay `TValue` arrays and fields.**
  - Java uses `TValue[]` of owned element objects: constants are
    immutable, and upvalue/user-value writes go through `setobj` into
    the element.
  - Because copies are compiler-enforced to be named, an element object
    is never shared by accident.
  - The arrays are small (≤255 upvalues), so columns would buy nothing
    measurable.
- **Cost found on the way:** the first version of the copy change used
  deleted copy operations with memberwise bodies. The slot accessors
  then copied 16 + 1 bytes instead of the whole 24-byte struct, which
  cost b_arith +9% and b_vm +2%. Private defaulted copies keep the same
  enforcement with the original codegen.

**Results:** `make full` passes.

| A/B (min) | all.lua | b_vm | b_call | b_arith | b_table | b_gc |
|---|---|---|---|---|---|---|
| array accessors (`7498236d` → `122d856d`) | 4.82 → 4.84 | 1.07 → 1.09 | | | 0.41 → 0.41 | 3.83 → 3.89 |
| node loops (`122d856d` → `eebae878`) | 4.86 → 4.86 | | | | | 3.93 → 3.91 |
| explicit copies (`eebae878` → `4923c1f5`) | 4.83 → 4.83 | 1.08 → 1.09 | 4.27 → 4.26 | 1.25 → 1.24 | 0.40 → 0.40 | |

RSS is unchanged throughout.

### M4. Userdata payloads that hold native state

**Effort:** medium · **Risk:** medium

Raw-byte userdata is upstream's model and stays the consumer API. Some
library code, though, overlays structs that hold native or engine
references, which a Java `byte[]` can't contain:

- `liolib.cpp:53,65,104,223,576` `LStream` (`OsFile*` + `closef`
  function)
- `lstrlib.cpp:931,955` `GMatchState` (views into strings + `lua_State*`)
- `loadlib.cpp:155,166` `void*` library handle
- `lua.cpp:704` `argv` (host launcher; see the port-time notes)

These need an *object payload*: the `Udata` carries a typed reference
(Java: `Object`) instead of bytes. Pure-data overlays stay bytes: for
example, `lmathlib.cpp:386,439,465` `RanState` (4 × 64-bit) is read
through little-endian `byte_span` accessors, which keeps `math.random`
sequences bit-identical. Design the object-payload API so that Java
consumers can use it for their own objects too. This is a public API
addition.

**Done when:** no `static_cast<T*>` on a userdata payload remains in
library code.

**Status: done** (`350f38ae`).
- **Public API:**
  - `lua.h`: `struct lua_UserObject { virtual ~lua_UserObject(); }`,
    `lua_newuserobject(L, std::unique_ptr<lua_UserObject>, size,
    nuvalue)` and `lua_touserobject(L, idx)`.
  - `lauxlib`: `luaL_testuserobject`/`luaL_checkuserobject` (plus the
    internal `i` variants).
  - In Java, `lua_UserObject` is `Object` and consumers pass their own
    objects.
- **Ownership:** the `Udata` owns the object. It is destroyed when the
  userdata is freed, which is after `__gc`, so finalizers still see
  it. The `unique_ptr` parameter means an allocation error can't leak
  it.
- **Accountability:** the declared `size` is charged (`luaM_takeobj`),
  counted in `objsize`, and released on free (`luaM_freeobj`). Each
  library declares today's payload size (`LSTREAM_SIZE` 16,
  `GMATCHSTATE_SIZE` 584, `LIBHANDLE_SIZE` 8, `RANSTATE_SIZE` 32), so
  `collectgarbage("count")` does not move.
- **Converted:**
  - `luaL_Stream` derives from `lua_UserObject`. External libraries
    that build their own `FILE*` handles now use `lua_newuserobject`.
  - `GMatchState`, the `loadlib` handle (`LibHandle`) and `RanState`.
- **Remaining casts are hierarchy downcasts**
  (`static_cast<LStream*>(lua_UserObject*)`), which Java expresses as
  `(LStream) obj`. `loadlib`'s `static_cast<OsLib*>` on the `_CLIBS`
  *light* userdata stays (L10). `lua.cpp`'s `argv` is host-launcher
  code (port-time note).
- `test_public_api` has a new `userobject` test: identity, user values,
  metatable checks, `__gc` sees the object, the destructor runs on
  collect and on `lua_close`, and the charge appears in and leaves
  `LUA_GCCOUNT`.

**Decision: `RanState` is an object payload, not little-endian bytes.**
The plan kept it as bytes read through `byte_span` accessors. That would
decode and re-encode four 64-bit words on every `math.random` call, in
C++ and in Java. The generator state is the same `uint64_t[4]` either
way, so sequences stay bit-identical. Performance first: object.

**Cost:** `Udata` grows from 72 to 88 bytes of real memory (the owning
pointer and `objsize`). The accounting charge is unchanged
(`cost::Udata` is frozen).

**Results:** `make full` passes.
- A/B (min): all.lua 5.10s → 5.04s; `b_ud` (gmatch ×200k +
  `math.random` ×3M) 2.50s → 2.49s; b_gc 4.33s → 4.28s. RSS is equal.
- `ltests.cpp` `T.nonblock` got a mechanical update
  (`luaL_checkudata` overlay → `luaL_checkuserobject`).

### M5. `lua_getextraspace` → typed slot

**Effort:** low · **Risk:** low

`lua_getextraspace` returns raw per-state bytes, and `lhack.h`'s debug
lock macros pun them as `uint64_t*`/`void**`. Replace it with a typed
per-state user slot (public API change) and an explicit `reentrancy` field
for the debug lock assertions.

**Done when:** `LUA_EXTRASPACE` and the pointer casts in `lhack.h` are
gone.

**Status: done** (`ee01222f`).
- **Public API:** `lua_getextraspace` is replaced by
  `lua_getextra(L)` / `lua_setextra(L, void*)`, a typed per-thread slot
  (Java: `Object`). A new thread starts with the main thread's value,
  matching upstream's `memcpy` of the extra space in `lua_newthread`.
- **Debug lock:** the counter is `global_State::apilock`, and all
  threads of a state share it. This is what upstream's `plock` pointer
  achieved: one counter per state, reached from any thread. The
  `luai_userstatethread`/`free` assertions check the shared
  `global_State` instead of comparing `plock`s.
- `LUA_EXTRASPACE` and the `uint64_t*`/`void**` puns in `lhack.h` are
  gone.
- **`test_public_api`:** the extraspace check, which wrote raw bytes
  with `memcpy`, now uses `lua_setextra`/`lua_getextra` and also checks
  thread inheritance.

**Results:** `make full` passes; the debug build exercises the lock
assertions. Not a hot path, so there was no A/B. `global_State` grows by
the `int` (its charge is frozen as `cost::Global`).

### M6. Byte storage: view, span, owner

**Effort:** medium · **Risk:** low · **A/B** (allocation churn on
`tostring` and formatting paths)

The intent is `byte_view` for read-only access and `byte_span` for
read-write access, both non-owning. Ownership needs its own type, and today
`byte_span` carries it: `byte_span` holds a
`std::shared_ptr<byte_owner>`. That has three costs:

- Every span copy is an atomic reference-count bump.
- `byte_span(out).copy_from(...)` (`lobject.cpp:951-1002`,
  `loslib.cpp:203`) writes through a *copy* that shares storage. Which
  object writes where is hard to read.
- Each scratch buffer (`byte_span buff(N)`, about 35 sites, e.g.
  `lapi.cpp:425`, `lobject.cpp:653,751,874`, `lvm.cpp:827`,
  `lstrlib.cpp:1500-1705`) is a heap allocation plus a `make_shared`, where
  upstream uses a stack array.

None of the current owners needs shared ownership. In each case one object
owns the bytes and others borrow them for a scope:

- `Udata::data`
- `lua_Debug::short_src_out`
- `MBuffer::bytes_`
- `BuffFS::b`
- `luaL_Buffer::bytes`
- the `luaM_alloc_char_span` results

Java gets sharing for free (any reference to a `byte[]` keeps it alive), so
C++ needs only unique ownership plus borrowing.

- **`byte_owner`** owns exactly one buffer and is move-only
  (`std::unique_ptr<char[]>` + size, or the current `span` base with moves
  enabled). It lends `.span()`/`.view()` and its member owners are
  allocated, resized and released through `luaM_alloc_char_span`. Java:
  a `byte[]` field.
- **`byte_span`**: a non-owning pointer + length, writable, with
  `subspan(pos, len)` and `view()`. Java: `(byte[], off, len)`.
- **`byte_view`**: unchanged in role. Its commented-out `offset`/`length`
  fields can go, since `sv` already carries both. Java:
  `(byte[], off, len)`, read-only by convention.
- Struct members above become `byte_owner`. Functions take
  `byte_span`/`byte_view` parameters, never `byte_owner` unless they
  transfer or resize ownership.
- **Scratch buffers** become a `byte_owner` local. For the hot fixed-size
  ones (number → string in `lapi`/`lobject`/`lvm`, `%p` and format items
  in `lstrlib`), A/B a fixed-capacity inline owner against heap
  allocation. In Java both are `new byte[N]`.
- **Java hot-path rule:** in Java, each view or span value is an object. In
  loops (lexer, pattern matching, formatting), advance an index over one
  base view rather than creating a sub-view per step. The code mostly does
  this already ("walk by offset"); keep it that way.

**Done when:** `shared_ptr` is gone from `bytes.h`. No owning `byte_span`
exists: every buffer that outlives its scope is a `byte_owner` member.
Scratch-buffer A/B results are recorded.

**Status: done** (`33428451`, `731cbe00`, `73c17300`).
- **Types:**
  - `byte_span` is now a non-owning pointer + length. Its raw-pointer
    constructor stays private, so spans come only from an owner or from
    `subspan`.
  - `byte_owner` is the move-only owner (`unique_ptr<char[]>` + size).
  - `byte_inline<N>` is a fixed-capacity owner with inline storage, the
    counterpart of upstream's stack arrays.
  - All three share their write/format methods through a CRTP mixin
    (`byte_writes<Self>`), so call sites read the same as before.
  - `shared_ptr` is gone from `bytes.h`.
- **Owners:** `Udata::data`, `MBuffer::bytes_`, `luaL_Buffer::bytes`,
  `BuffFS::b`, `InternalDebug::short_src_out` and `LoadF::buff` are
  `byte_owner`.
  - `luaM_alloc_char_span` borrows the old block (for accounting and
    copying) and returns a new owner, so a failed growth leaves the
    caller's buffer intact, as before.
  - `InternalDebug`'s copy deep-copies `short_src_out`.
- **Scratch buffers:** the compile-time-sized ones (number → string,
  format items and specs, UTF-8 escapes, chunk ids, `%p`) are
  `byte_inline<N>`. Runtime-sized ones (`lvm` concat, the REPL line) are
  `byte_owner`.
- **Bugs found by AddressSanitizer + UBSan** (full suite, deep tests and
  `test_public_api`, release and debug configurations, all clean
  afterwards):
  - `lua.cpp`'s REPL kept a `byte_view` into a destroyed `HostString`
    temporary (`lua_tostring(L, 1)->to_utf8()`), a stack-use-after-scope.
    This predates M6.
  - `copy_from` called `memcpy` with a null pointer for an empty source.
  - Spans from `luaL_buffaddr` now follow upstream's rule (invalid after
    the buffer grows), and no code depended on the old shared-ownership
    behavior.
- **Java:** owner, span and inline buffer all become `byte[]` (+ offset,
  length). Ownership is implicit.

**Results:** `make full` passes. The scratch-buffer A/B:

| A/B (min) | b_fmt (tostring + string.format ×2M) | b_string |
|---|---|---|
| shared span → `byte_owner` (`8ea3f7ed` → `33428451`) | 5.74s → 4.66s (−19%) | 1.21s → 1.02s (−16%) |
| heap owner → `byte_inline` (`33428451` → `731cbe00`) | 4.45s → 3.97s (−11%) | 0.98s → 0.91s (−7%) |

all.lua and RSS are neutral. `ltests.cpp` got a mechanical type-name
update (scratch `byte_span(N)` → `byte_owner(N)`).

---

## Phase C — class changes

Reorganize into namespaces and classes that stand in for the Java
package/class layout. This is a pure staging move: no behavior change, and
inlining and direct dispatch are preserved exactly.

### C0. Conventions (decide before C1)

- **Namespaces:** `lua::x` → `lua.x`, using the layout in C1–C10.
- **Keep upstream file boundaries and function names.** A file's functions
  become methods of the class that file represents. Names keep their C
  prefixes (`Table::luaH_get`), as already done, so upstream fixes stay
  easy to map. Renaming is optional and comes last (C13).
- **Visibility:**
  - `static` (about 1,280) → `private`
  - `LUAI_FUNC` (about 270) → package-private. C++ has no package scope, so
    these are public members and the namespace marks the boundary.
  - `LUA_API` (about 250) → `public` on `LuaState`/`LuaAuxLib`.
- **Free functions** whose first argument is always one type become methods
  on that type. No generic utility classes, and no templates beyond those
  already present.
- **No new virtual dispatch.** Hot methods stay concrete and
  `inline`/`constexpr`. The only virtual methods are the L3 interfaces,
  each of which replaces an existing function pointer.
- **API and tests:** the public API may change shape (`lua_State*`-first →
  `LuaState::method()`), and `test_public_api.cpp`/`tests/libs` follow.
  `ltests.cpp` gets mechanical call-shape updates only, flagged per commit.

**C0 decisions (recorded before C1):**
- **Upstream's prefixes are the class names; functions stay free.**
  - `luaD_call(L, …)` transcribes mechanically to Java's
    `Ldo.luaD_call(L, …)` (a static method). Upstream's prefix already
    names the file that is the class: `luaD_` → ldo, `luaV_` → lvm,
    `luaK_` → lcode, `luaG_` → ldebug, and so on.
  - Converting every first-argument function into a method on that type
    would rewrite about 1,600 internal call sites and change upstream's
    call shape, with no Java benefit. So by "favor shape", free functions
    stay free.
  - The exception is where a file *is* a type, as the branch already did
    for `Table`, `TString`, `Proto` and the closures. Those keep their
    methods.
- **Public API shape is kept.**
  - `lua_*`/`luaL_*` stay free functions over `lua_State*`. Java gets
    `LuaApi.lua_xxx(L, …)`/`LuaAuxLib.luaL_xxx(L, …)` static methods.
  - This avoids rewriting about 2,600 call sites in `test_public_api`,
    `tests/libs` and `ltests.cpp` (which may only take mechanical edits),
    and consumers keep upstream's API.
  - C7 still retires `extern "C"` and the `LUA_API`-style macros.
- **Packages are C++ namespaces** (`lua::bytes`, `lua::object`,
  `lua::gc`, `lua::runtime`, `lua::vm`, `lua::compiler`, `lua::api`,
  `lua::lib`, `lua::platform`, `lua::host`).
  - `packages.h` declares them and brings them into global scope with
    using-directives, the stand-in for Java imports. Wrapping a file in
    its package therefore leaves every call site unchanged.
  - Each namespace is one Java package, and each file is one class in
    it.
  - Mangled names change and nothing else does: same inlining, same
    dispatch.

### C1. `lua::object`

Finish the `GCObject` hierarchy, including `GCListed` from L1. `Table`,
`TString`, `UpVal`, `Proto` and the closure constructors have moved
already. Left to do: the inline helpers in `ltable.h`, the value helpers in
`lobject.h`/`lobject.cpp`, and the leftovers in `lstring.h`/`lfunc.h`.

**Status: done** (`ba0b3f64`).
- `lobject`, `ltable`, `lstring` and `lfunc` (`.h`/`.cpp`) are in
  `lua::object`. Other headers forward-declare object types inside it.
- The `GCObject` hierarchy (`GCListed` from L1) and the type methods
  (`Table::`, `TString::`, `Proto::`, closures) are as before. The inline
  value helpers stay free functions in the package (C0).
- **Results:** `make full` passes. A/B: b_vm 1.11s → 1.11s,
  b_table 0.37s → 0.35s.

### C2. `lua::bytes`

`byte_view`, `byte_span`, `byte_owner`, `Zio` (`lzio.*`), `MBuffer`,
`InternalString` and `literals.h`. A leaf namespace.

**Status: done** (`cf244dd8`). `bytes.h`, `internal_string.h`, `literals.h`
(`lt` becomes `lua::bytes::lt`), `util_cast.h` and `lzio` are in
`lua::bytes`. `HostString` stays with the API (C7). `make full` passes.

### C3. `lua::gc`

`lgc.cpp`'s algorithms become a `GarbageCollector` class. **State stays on
`global_State`.** `GCdebt`, `gcstate` and the list heads are read on
allocation and barrier hot paths, and moving them into a separate object
would add a dereference in Java. `GarbageCollector` operates on
`global_State`'s fields.

**Status: done** (`31954e74`).
- `lgc` is in `lua::gc`. Its functions keep their `luaC_` names (Java:
  static methods of `GarbageCollector`) and still operate on
  `global_State`'s fields, which stay put.
- **Results:** `make full` passes. A/B: b_gc 3.71s → 3.76s min
  (3.82s → 3.80s median).

### C4. `lua::runtime`

`lstate.*` (`LuaState`, `GlobalState`, `CallInfo`), `ldo.*` (`luaD_*`
→ `LuaState` methods), `lmem.*` (`luaM_*` → `LuaState`/`GlobalState`
methods), `ltm.*`, and the scalar `TValue`/`Value`.

**Status: done** (`0205ff78`).
- `lstate` (`global_State`, `CallInfo`), `ldo`, `lmem` (the cost table
  becomes `lua::runtime::cost`) and `ltm` are in `lua::runtime`.
- **Decision: the public API surface stays global.**
  - `lua_State` is defined outside the package, and the `lua_*`
    functions these files define (`lua_newstate`, `lua_close`,
    `lua_newthread`, `lua_resume`, …) sit outside it, via
    `tools/apiout.py`'s close/reopen.
  - Consumers that forward-declare `struct lua_State;` keep working, as
    `tests/libs/lib_decls.h` does and as upstream C-API users do.
  - This follows from C0's "public API shape is kept".
- **Rename:** the stack-to-`TValue` `setobj(L, TValue*, StkId)` overload
  is now `slot2obj`, beside the other slot accessors.
  `lua::object::setobj` hid it inside `lua::object`, and Java static
  methods in different classes can't share a name across the call anyway.
- `ltests.h` got a mechanical update (it includes `packages.h`).
- **Results:** `make full` passes. A/B: b_call 4.29s → 4.28s,
  b_vm 1.07s → 1.07s.

### C5. `lua::vm`

`lvm.cpp` keeps its single dispatch `switch` as `Interpreter::execute`.
`lopcodes.*` → `OpCodes`; `ldebug.*` (`luaG_*`) → `Debug`, kept
distinct from `lua::lib::DebugLib`.

**Status: done** (`eb6a2ade`). `lvm` (`luaV_execute` keeps its single
dispatch `switch`), `lopcodes` and `ldebug` are in `lua::vm`. The public
and internal (`…i`) debug API that `ldebug.cpp` defines stays global, and
`Instruction`'s forward typedef sits with `lua::object`. `make full`
passes. A/B: see the C-phase summary row in C12.

### C6. `lua::compiler`

`llex.*`, `lparser.*`, `lcode.*`, plus `ldump.cpp`/`lundump.cpp`.

**Status: done** (`b223e5ee`). `llex`, `lparser`, `lcode`, `ldump` and
`lundump` are in `lua::compiler`, and `lopnames.h`'s name table is in
`lua::vm`. `lcode.cpp`'s two mid-file includes move up with the others.
`make full` passes.

### C7. `lua::api`

`lapi.cpp` → `LuaState`'s public methods; `lauxlib.cpp` → `LuaAuxLib`.
This is the only namespace that touches `HostString`. Retire `extern "C"`,
`LUA_API`, `LUALIB_API` and `LUAMOD_API`.

**Status: done** (`4c6d0ded`).
- `lapi.h` and the internals of `lapi.cpp`/`lauxlib.cpp` are in
  `lua::api`. The public `lua_*`/`luaL_*` functions, `lua_ident` and
  the `luaL_Buffer` destructor stay global (C0).
- `LUA_API`/`LUALIB_API` (`extern "C"`) are retired, so the public API
  has C++ linkage.
- **Decision: `LUAMOD_API` stays `extern "C"`.** `package.loadlib`/
  `require` find a C module's `luaopen_*` entry point by its unmangled
  name (`dlsym`), so this is host ABI, not engine shape.
- **The user's library seal keeps its intent.** The `makefile` step that
  localizes every mangled symbol in `liblua.a` now also keeps the mangled
  global-scope `lua_*`/`luaL_*`/`luaopen_*` functions. The public API
  stays linkable, and the packaged internals (`_ZN3lua…`) are still
  sealed off. It previously relied on `extern "C"` to tell the two
  apart.
- `HostString` stays a global public type, since it appears in the API
  signatures.
- **Results:** `make full` passes, including `test_public_api` against
  the sealed archive and the `tests/libs` C modules.

### C8. `lua::lib`

One class per library file (`BaseLib`, `StringLib`, `TableLib`, `MathLib`,
`OsLib`, `IoLib`, `CoroutineLib`, `DebugLib`, `Utf8Lib`, `PackageLib`),
with `linit` as the registry. Functions register through `CFuncInfo`.

**Status: done** (`f4c20316`).
- The ten libraries and the `linit` registry are in `lua::lib`. The
  `luaopen_*` entry points and `linit`'s public functions stay global.
- **Name collision:** `loslib`'s own `os_remove`/`os_rename`/
  `os_tmpname`/`os_getenv` (the Lua functions) hide the platform
  functions of the same names inside the package, so its platform calls
  are written `::os_remove(…)` and so on. In Java the two are already
  distinct (`OsLib.os_remove(L)` vs `OsApi.os_remove(path)`).
- `liolib`'s mid-file `<sys/types.h>` include moves up with the others.
- **Results:** `make full` passes.

### C9. `lua::platform`

`os_api.*` and `locale_util.*`: reimplemented, not transcribed, in Java.

**Status: done** (`0e86c46a`). `os_api` and `locale_util` are in
`lua::platform`, and the public `luaL_Stream`'s `OsFile*` names the
package. `make full` passes.

### C10. `lua::host`

`lua.cpp` becomes the launcher class, outside the engine.

**Status: done** (`2fe920ff`). `lua.cpp` is in `lua::host`; `main` stays
global. `make full` passes.

### C11. Macro teardown

Remove `LUAI_FUNC`, `LUAI_DDEC`/`LUAI_DDEF` and any file-scope `static`
helpers that remain. `l_noret` → `[[noreturn]]`. `l_forceinline`,
`l_likely`/`l_unlikely` and `lhack.h` stay as build configuration.

**Status: done** (`c2ebf70d`).
- `LUAI_FUNC`/`LUAI_DDEC`/`LUAI_DDEF` are gone, together with upstream's
  comment describing them. `LUAI_DDEC(x)` becomes `extern x`, and
  `l_noret` becomes `[[noreturn]] void`.
- File-scope `static` helpers now sit inside their package namespace,
  where `static` still means private (Java: `private static`).
- `l_forceinline`, `l_likely`/`l_unlikely` and `lhack.h` stay as build
  configuration.
- **Decision: `LUAI_FUNC`'s internal-visibility hint is retired, not
  moved.**
  - A visibility attribute can't sit on a nested namespace definition
    (`namespace lua::x`).
  - Pragma-based `push(internal)` clashes with public structs that hold
    package types: `lua_Debug` holds `byte_view` values and a
    `CallInfo*`, and `luaL_Stream` holds an `OsFile*`.
  - The archive seal (C7) still localizes packaged internals. The
    executable's `-Wl,-E` dynamic table now also lists them, which only
    a C module could notice, and a C module has no declarations to name
    them with. PIE calls bind locally either way, so there is no
    runtime cost.
- **Phase C A/B** (before C1 `8e276daa` → after C11 `c2ebf70d`, min):
  all.lua 4.68s → 4.66s, b_vm 1.09s → 1.08s, b_call 4.25s → 4.26s,
  b_arith 1.24s → 1.23s, b_table 0.35s → 0.35s, b_gc 3.67s → 3.67s,
  b_fmt 3.84s → 3.83s. RSS is identical.

### C12. Final audit

- Run every **Done when** check from L, M and C again.
- Confirm that every `luaM_free_*` and span release is accounting plus
  dropping the reference, with no reuse of freed storage.
- Write the port checklist that maps each remaining C++ idiom to its one
  Java transcription:
  - `byte_owner` → `byte[]`; `byte_span`/`byte_view` → `byte[]` +
    offset + length (read-only by convention for views)
  - `std::optional` → nullable reference
  - L7 helpers → `>>>` / `compareUnsigned`
  - M3 column accessors
  - M2 cost constants
  - L3 interfaces

**Status: done** (`256b6244`).

**Audit (every "Done when" re-run at the end):**
- **L1:** `T**` appears only in `lua.cpp` (`argv`, readline `dlsym`).
- **L2:** no `InternalString**`. Out-params are result structs.
- **L3:** no `void* ud` and no `static_cast<X*>(ud)`.
- **L4:** `memcpy`/`memmove`/`memset` and `.data()` arithmetic appear
  only in `bytes.h` and `os_api.cpp`.
  - The audit caught `luaZ_read`'s `memcpy` (added in L9). It now goes
    through `bytes.h` `copy_bytes` (`256b6244`).
- **L5:** `point2uint` (the identity hash), `uintptr_t` (`util_cast.h`)
  and `const_cast` (`setpvalue`, `luaZ_overlay`) appear only in the
  recorded identity and overlay functions.
- **L6/M2:** `sizeof` remains in `static_assert`s, the chunk-format
  element widths of `ldump`/`lundump`'s typed I/O templates, and
  `luaZ_overlay`. These are recorded in M2.
- **L8:** no libc ctype call outside `locale_util.cpp`.
- **L9:** `lundump.cpp` has no pointer casts.
- **L10:** `loadlib.cpp` has no `reinterpret_cast`.
- **L12:** `loslib.cpp` uses no libc time function and no `<time.h>`.
- **L11:** casts total 550 (arithmetic 394), down from 985 (arithmetic
  811) at the plan's start. Every remaining cast falls in one of L11's
  recorded groups.
- **M3:** `TValue` copies are compiler-enforced through
  `getobj`/`copyobj`/`setobj`/`copynode`.
- **M4:** no `static_cast` on a userdata *payload*. The one
  `static_cast<OsLib*>(lua_touserdata)` left in `loadlib` reads a
  *light* userdata (L10).
- **M5:** `lhack.h` has no `LUA_EXTRASPACE` or pointer puns.
- **M6:** `bytes.h` has no `shared_ptr`.
- **Frees:** every `luaM_free_*` and every
  `luaM_alloc_*_span(…, 0, …)` is accounting (`free_debt`/
  `_alloc_span_request`) plus `delete`/`delete[]`. Each call site then
  either replaces the field with the new block (`luaD_reallocstack`,
  `luaH_resize`'s array/hash, buffer growth) or destroys the owner
  (`freeobj`, `luaE_freethread`, `luaF_freeproto`).
  - AddressSanitizer + UBSan ran the full suite, deep tests and
    `test_public_api` clean in both configurations (M6). Nothing reads
    freed storage.

**Port checklist (C++ idiom → one Java transcription):**

| C++ | Java |
|---|---|
| package namespace `lua::x` | package `lua.x` |
| file `lxxx.cpp` + prefix `luaX_` | class (`Ldo`, `Lvm`, `Lcode`, …) with static methods. `Table::`/`TString::`/`Proto::` methods stay instance methods |
| file-scope `static` in a package | `private static` |
| public `lua_*`/`luaL_*` (global) | `public static` on `LuaApi`/`LuaAuxLib`; `luaopen_*` on each library class |
| `byte_owner`, `byte_inline<N>` | `byte[]` (`new byte[N]`) |
| `byte_span`, `byte_view` | `byte[]` + offset + length (views read-only by convention); loops advance an index over one base view |
| `HostString` | `String` (consumer text) |
| `std::optional<T*>`/`std::optional<byte_view>` | nullable reference |
| `std::optional<lua_Integer>`/`<lua_Number>`, `IntResult`/`NumResult`, result structs | small result classes (or `OptionalLong`/`OptionalDouble`) |
| `std::unique_ptr<T>` | plain reference |
| `std::span<T>` arrays | `T[]` (length is the array's) |
| `l_intadd`/`sub`/`mul`/`unm`/`bnot`/`band`/`bor`/`bxor` | `+ - * - ~ & \| ^` on `long` |
| `l_ult`, `l_umod`, `(lua_Unsigned)` `/` and `>>` | `Long.compareUnsigned(a,b) < 0`, `Long.remainderUnsigned`, `Long.divideUnsigned`, `>>>` |
| bit containers (hash, seed, `Instruction`, `callstatus`) | `int` with `>>>` where shifted |
| `(unsigned char)c` | `c & 0xFF` |
| `(int)`, `(lu_byte)`, `(char)` narrowing | `(int)`, `(byte)` |
| `StkId` (`ptrdiff_t`) | `int` |
| slot accessors (`getslot`/`setslot`/`copyslot`/`slot2obj`/`slotival`/…) | stack columns `long[] prim`, `Object[] ref`, `byte[] tag` |
| `getArrTag`/`setArrTag`/`getArrVal`/`arr2obj`/`obj2arr`/`farr2val`/`fval2arr` | `Table` array columns `long[]`/`Object[]`/`byte[]` |
| `Node`, `gnode(t,i)`, `&gnode(t,i)->val` | `Node extends TValue` (flattened key fields) in `Node[]`; the node itself |
| `getobj`/`copyobj`/`copynode`/`setobj` | explicit field copies (never share a `TValue`) |
| `cost::X` | `static final int` |
| `ProtectedCall`, `lua_Reader`, `lua_Writer`, `lua_Alloc`, `lua_WarnFunction`, `lua_UserObject` | interfaces / abstract classes; `lua_UserObject` is `Object` |
| `Proto::code` (`std::span<Instruction>`, `luaZ_overlay` for fixed chunks) | code store over `byte[]` read with a little-endian `VarHandle` (aliases fixed buffers; see port-time notes) |
| `GCListed` sentinel list heads | sentinel objects |
| `[[noreturn]]` + `luaD_throw` | `throw` |
| `l_forceinline`, `l_likely`/`l_unlikely`, `lhack.h` | dropped (JIT) |
| `os_api`, `locale_util` | reimplemented on the JVM |
| `as_byte_view`, `copy_bytes`, `luaZ_overlay`, `util::pointertext` | `byte[]` access / identity hash text |

### C13. Renaming (optional)

If wanted, drop the C prefixes (`luaH_get` → `get`) and apply Java class
names (`Table` → `LuaTable`), one namespace per commit, after everything
else.

**Status: not done, by decision.**
- C0 made upstream's prefixes the class mapping (`luaD_` → `Ldo`, …).
  The names stay upstream's, so upstream fixes keep mapping line for
  line.
- Renaming now would cost every call site and that traceability, and
  the port gets nothing from it: a transcriber can rename at the Java
  end with the checklist above.
- If renaming is wanted later, it remains one namespace per commit, as
  planned.

---

## Phase R — review cleanup (after L/M/C)

Added after the plan finished, from the user's review: the Lua script API
comes first, then performance, then the C API. Compact memory layout
matters less than API shape and performance. Now that the code has real
types, use construction and initialization where they simplify, and bring
the code closer to the original's intent and performance.

### R1. Named value predicates and setters
Upstream's `ttisnil`, `isempty`, `ttisinteger`, `iscollectable`,
`setnilvalue`, `setempty`, … were expanded inline by the earlier macro
removal. About 350 sites now spell out `(o->tt_ & NOVARIANT_MASK) == …`.
Restore them as `lobject.h` inline functions with upstream's names (Java:
static methods) and replace the expansions.
**Check:** object code byte-identical (`objcmp.sh`).

**Status: done** (`08c8d548`).
- Restored with upstream's names and in upstream's places. Several
  upstream comments that the macro removal had left orphaned now sit on
  their functions again.
- **Coverage:** about 400 sites use the predicates again. Stack-slot
  tags keep their slot accessors and use `novariant`/`tagisempty`.
- Node-key tests are back too (`keyisnil`, `keyisinteger`,
  `keyisshrstr`, `keyiscollectable`, `keyisdead`, `setnilkey`,
  `setdeadkey`, `gckeyN`).
- The tag helpers return `lu_byte`, so `-Wconversion` keeps seeing the
  masked range.
- **Check:** with asserts compiled out, object code is identical except
  basic-block layout in a few functions (the `bool`-returning
  predicates). A/B b_table (15 runs) 0.42s → 0.41s; b_vm, b_arith,
  b_call and b_gc are neutral.

### R2. `SETARG_*` instruction setters
The 21 `lcode`/`lparser` sites that spell out
`(i & MASK0(…)) | ((Instruction)(v) << POS & MASK1(…))` go back to
upstream's `SETARG_A`/`B`/`C`/`k`/`Bx`/`sJ`/`OP` as `lopcodes.h` inline
functions. **Check:** byte-identical.

**Status: done** (`64146464`).
- `setarg`, `SET_OPCODE` and `SETARG_A`/`B`/`C`/`k`/`Bx`/`sJ` are
  `constexpr` functions that return the updated instruction (Java can't
  take a reference to an array element). The 21 sites read
  `code[pc] = SETARG_C(code[pc], v);`.
- **Check:** release object code with asserts off is identical. Only
  the `-Og` debug objects differ.

### R2b. Macro-expansion remnants (user request)
This is cosmetic only, with no operational change. Remove the
parenthesization left by macro expansion (`(((n)->key_tt …`,
`((fs)->f->code[(&e)->info])`, `(x) = (y)`, …) in favor of direct,
simple code (`n->key_tt`, `fs->f->code[e.info]`). Finish any remaining
macro-expansion replacements with the named helpers. Trivial field
getters that only re-spell `object->field` (`rawtt`, `keytt`) give way
to the field itself.
**Check:** object code identical with asserts off.

**Status: done** (`b3e93d67`, `837e68b9`, `cea3279f`).
- **Parentheses:** about 760 redundant pairs left by macro expansion
  and removed casts are gone. That covers `(ident)` before `->`/`.`/`[`,
  doubled groups, wholly parenthesized arguments and statements,
  parenthesized postfix and cast operands, and whole right-hand
  sides/returns without comparisons.
- **Kept:** parenthesized boolean results (`int fast = (work2do == 0);`)
  are upstream's own style.
- **Remnants replaced:**
  - `luai_num*`'s `((void)L, a + b)` becomes `a + b`.
  - The slot-tag form of `l_isfalse` is upstream's `tagisfalse`.
  - Upstream's `ispow2` is restored under its orphaned comment.
  - `api_check`-style asserts become `assert(e && "msg")` where `e` has
    no top-level `||`/`?:`.
  - The `rawtt`/`keytt` getters added in R1 give way to `o->tt_`/
    `node->key_tt`.
  - `x->marked = x->marked op y` becomes `op=` where `-Wconversion`
    allows.
- **Kept, with names:** upstream-named accessors stay (`gnode`,
  `mainthread`). `getArrTag`/`getArrVal` also stay; they are the M3
  seam for Java's columns.
- **Check:** release object code with asserts off is identical, apart
  from register allocation around the `bool` `tagisfalse` in
  `lvm`/`lfunc`. `make full` passes after each commit.
- **Remaining `(x)` forms are legitimate:** macro parameters in
  `lhack.h`, attribute syntax, and C++ casts/constructors.

### R3. Construction instead of field-by-field initialization
GC objects (`Proto`, `Table`, `Udata`, `UpVal`, closures, `TString`,
`CFuncInfo`), `lua_State` (`preinit_thread`), `CallInfo` and
`global_State` get default member initializers. The creation functions
keep only what depends on arguments or state (`luaC_link`, sizes).
`luaM_malloc_*` already constructs with `new`. **Check:** `make full`,
A/B (no extra work: the same zeroing, now in the constructor).

**Status: done** (`11a409a7`).
- The object, thread, call-info and global-state structs carry their
  defaults as member initializers. The creation functions keep only
  argument- or state-dependent setup.
- **Exceptions:**
  - `GCObject`'s header (`next`/`tt`/`marked`) stays uninitialized,
    because `luaC_link` sets it on every allocation and an out-of-line
    call can't elide duplicate stores.
  - `Table::flags = maskflags`, `global_State`'s GC configuration
    (`currentwhite`, `gcstate`, `gckind`, `gcstp` from `lgc.h`) and the
    "not yet built" `nilvalue` stay explicit in `luaH_new`/
    `lua_newstate`.
- **Check:** `trace_gc`/`trace_gc2` output is byte-identical. A/B (15
  runs, min): all.lua 4.90s → 4.89s, `b_alloc` (tables, closures,
  coroutines) 0.79s → 0.79s; b_gc/b_call are neutral.

### R4. `short_src` back to an inline buffer
`InternalDebug::short_src_out` is a lazily heap-allocated `byte_owner`
with a hand-written copy constructor. Upstream's is `char
short_src[LUA_IDSIZE]`. Make it `byte_inline<LUA_IDSIZE>` (and make
`byte_inline` copyable like the array it stands for), then drop the lazy
allocation and the copy constructor.

**Status: done** (`61353c36`). `short_src_out` is `byte_inline<LUA_IDSIZE>`,
and `byte_inline` is copyable. The custom copy constructor, the move
operations and `funcinfo`'s allocation are gone. `make full` passes.

### R5. API files without namespace fragments
`lapi.cpp`/`lauxlib.cpp` are almost entirely public API, which stays
global (C0). Wrapping them left 37 close/reopen fragments. The API
package *is* the global scope: unwrap both files (their `static` helpers
keep internal linkage).

**Status: done** (`7f538223`). `lapi.cpp`/`lauxlib.cpp` are unwrapped, so
their static helpers sit at global scope with internal linkage, and
`lapi.h` keeps `lua::api`. Java maps both files to `LuaApi`/`LuaAuxLib`
as before. `make full` passes, including the sealed-archive link.

### R6. Chunk I/O without templates
C0 allows no new templates, but L9 added `loadVector`/`loadVar`/
`getaddr`/`checknum` and `dumpVector`/`dumpVar`/`dumpNumInfo`. Replace
them with the typed functions Java would write: blocks of chars,
instructions, line info, absolute line info, and `int`/`Instruction`/
`lua_Integer`/`lua_Number` scalars.

### R7. Reviewed, no change
- **VM `Protect`/`savestate`/`updatetrap` stay expanded** in
  `luaV_execute`. Lambdas capturing `pc`/`base`/`trap` by reference risk
  forcing those locals into memory in the hot loop, and Java lambdas
  can't capture mutable locals anyway.
- **`lastfree` gating for small hash parts stays.** It exists because C
  stores `lastfree` before the node block, but it also decides
  free-slot choice, and so iteration order and rehash timing.
- **Per-type `luaM_alloc_*_span` functions stay** (the user's decision;
  one Java method each).
- **`TValue`'s private copies stay** (compiler-enforced named copies,
  M3).
- **User objects:** they are internal state holders for the libraries.
  C-API extensibility is low priority, so the API stays minimal and
  unchanged.

---

## Port-time notes (no C++ action)

- **Interrupts:** `lua.cpp`'s SIGINT handler already uses upstream's
  host-invoked design. It calls only `lua_sethook`, and the engine never
  installs a handler. In Java, the host calls `lua_sethook` from whichever
  thread observes the interrupt. `trap`/`hook`/`hookmask` transcribe as
  `volatile`.
- **Host launcher:** `lua.cpp` (`argv` as light userdata, readline
  `dlsym`) is rewritten as the Java `main`, not transcribed. It keeps
  upstream's shape until then.
- **Debug lock macros** (`lhack.h` `lua_lock`/`lua_unlock`) become
  assertions on M5's reentrancy field.
- **Branch hints and forced inlining** (`luai_likely`/`luai_unlikely`,
  `l_forceinline`) disappear; the JIT handles them.
- **`os_api.cpp`** keeps its casts and `std::string` temporaries, and is
  reimplemented against the JVM.
- **Java size ceilings:**
  - A `byte[]` holds about 2³¹ bytes, while upstream allows larger strings
    on 64-bit hosts. Avoiding a new user-visible limit means a segmented or
    `long`-indexed store for oversized strings and userdata, behind the same
    `byte_span`/`byte_view` accessors.
  - `MAXASIZE` is 2³¹, one more than the largest Java array length. It is
    unreachable in practice (about 18 GB), but should be noted.
- **`as_userdata(int)`** (`util_cast.h`, `ltests.cpp` only) forges light
  userdata from integers. In Java, light userdata is an opaque reference,
  and the test helper wraps the integer.
- **Instruction storage (from L9):**
  - `Proto.code` is a small class over `byte[] + offset`, read with a
    little-endian `int` `VarHandle`: `code[pc]` → `code.get(pc)`,
    `code[pc] = x` → `code.set(pc, x)`. A fixed-buffer load aliases the
    host's `byte[]`, so nothing is allocated and nothing goes uncharged.
    A compiled proto owns its bytes.
  - Use one representation for every proto. A branch between `int[]` and
    `byte[]` in the fetch loop costs more than the `VarHandle` read.
  - `lineinfo` is already bytes. `abslineinfo` gets `pc(i)`/`line(i)`
    accessors over the same bytes.
  - Measure fetch against an `int[]` prototype at port time.
- **Bit containers:** after L7, the shift and wraparound helpers are the
  only places that need `>>>` or wrapping semantics in Java.
