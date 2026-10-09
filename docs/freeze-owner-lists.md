# G3: owners of finalizable objects, deferred user finalizers (read-only analysis)

Branch lua-c++, HEAD 963b6f8d. Nothing was modified. One scratch program (sizeof/offsetof of GCObject) was compiled in the scratchpad only.
No budget group exists in the code yet: the only "budget" state is global (lstate.h:604-610).

## 1. Inventory

### Fields (lstate.h)
- `allgc` (lstate.h:579), `sweepgc` (580), `finobj` (582), `tobefnz` (588), `fixedgc` (589).
- Generational age pointers for allgc: `survival, old1, reallyold, firstold1` (592-595).
- Generational age pointers for finobj: `finobjsur, finobjold1, finobjrold` (595-597). The finobj list is segmented like allgc: new | survival | old1 | really old.
- Finalizer thread `finth` (613); created in `init_finthread` (lstate.cpp:205-221), marked as a root in `restartcollection` (lgc.cpp:482-483).
- `runafewfinalizers` does NOT exist in this tree. Finalizers run from `GCScallfin` (lgc.cpp:1840-1855), `finishgencycle` (1438-1439) and `luaC_freeallobjects` (1696).

### Sites in lgc.cpp / lgc.h
| Site | What it does |
|---|---|
| lgc.h:83, 105 | `FINALIZEDBIT = 6`, `tofinalize(x)` |
| `luaC_link` 307-330 | New object: `marked = white`, `tt`, `hash` (identity-hash types only), linked at head of `allgc` |
| `luaC_checkfinalizer` 1183-1209 | Registration. Returns if already marked, no `__gc` (`gfasttm`), or closing (`GCSTPCLS`). In a sweep phase: `makewhite` and fix `sweepgc`; otherwise `correctpointers`. Then it **searches allgc linearly** for the predecessor, unlinks, pushes at head of `finobj`, sets FINALIZEDBIT. Called only from lapi.cpp:1511 and 1522 (`lua_setmetatable`, table and userdata cases). |
| `markbeingfnz` 417-422 | Marks every object in `tobefnz`. Called from `restartcollection` (lgc.cpp:487) and `atomic` (1731). |
| `separatetobefnz` 1137-1157 | Walks `finobj` from head up to `finobjold1`. A white object (or any object if `all`) is unlinked and appended at the END of `tobefnz` (`findlast`, O(n) once per call). Fixes `finobjsur` if it points at the removed object. Called from `atomic` (1730) and `luaC_freeallobjects` (1684). |
| `udata2finalize` 1040-1056 | Pops the head of `tobefnz`, relinks it at the head of `allgc`, clears FINALIZEDBIT. If sweeping: `makewhite`. Else if age is OLD1: `firstold1 = o`. |
| `finthready` 1079-1093 | The finalizer thread is idle (`F->ci == &F->base_ci`, not frozen) and both stacks have room. |
| `GCTM` 1095-1115 | `udata2finalize`, then looks up `__gc`. If present: `gcstp |= GCSTPGC`, pushes tm and the object on `finth`, `luaD_runfinalizer(F, L)` (ldo.cpp:465). |
| `callallpendingfinalizers` 1120-1125 | `while (tobefnz.next && finthready(L)) GCTM(L)` |
| `singlestep` 1788-1853 | `GCSswpallgc` -> sweep allgc, then `GCSswpfinobj` (list `&g->finobj`), then `GCSswptobefnz` (list `&g->tobefnz`), then `GCSswpend`, then `GCScallfin`. `GCScallfin` runs one `GCTM` per step while `tobefnz.next && !gcemergency && finthready`, otherwise goes to `GCSpause`. |
| `sweeplist` 983-1004 | Frees dead objects, recolors the live ones to the current white and age `G_NEW`. Used for all three incremental sweeps. |
| `markold` 1478-1493 | Called with `(finobj.next, finobjrold)` and `(tobefnz.next, nullptr)` in `youngcollection` (1488-1489). |
| `youngcollection` 1496-1535 | Sweeps allgc in two segments with `sweepgen`, then the same two segments for `finobj` (1505-1512), then `sweepgen(&tobefnz, nullptr)` (1514). Rotates the finobj pointers: `finobjrold = finobjold1; finobjold1 = psurvival->next; finobjsur = finobj.next`. |
| `atomic2gen` 1540-1560 | `sweep2old(&finobj)`, all three finobj pointers set to `finobj.next`; `sweep2old(&tobefnz)`. |
| `minor2inc` 1449-1458 | Zeroes the three finobj pointers and the allgc ones. |
| `entergen` / `fullgen` / `luaC_changemode` 1578-1630 | `entergen` runs the cycle to atomic, then `atomic2gen`. |
| `finishgencycle` 1432-1440 | `correctgraylists`, then `callallpendingfinalizers` if `tobefnz` is non-empty and not emergency. |
| `correctgraylist` 1361-1393 | Gray lists only; it does not touch finobj. |
| `luaC_freeallobjects` 1678-1701 | `GCSTPCLS`, `separatetobefnz(all=1)`, runs all pending finalizers, `deletelist(allgc)`. Asserts `finobj` is empty. |
| ltests.cpp:735-745 | Checks `finobj` with the three pointers and asserts `tobefnz` objects are `tofinalize`, of type table or userdata. `GCSswpfinobj`/`GCSswptobefnz` names are in ltests.cpp:1069-1077 and literals.h:828-829. |

### How an object with `__gc` moves

**Incremental mode**
1. `luaC_link` puts it in `allgc`.
2. `setmetatable` with a `__gc` field calls `luaC_checkfinalizer`: allgc -> head of `finobj`, FINALIZEDBIT on.
3. In the atomic phase, `separatetobefnz` finds it white and appends it to `tobefnz`; `markbeingfnz` plus `propagateall` resurrect it and everything it references.
4. Sweep: `GCSswpfinobj` and `GCSswptobefnz` only recolor live objects. Objects in `tobefnz` are marked, so they survive.
5. `GCScallfin` calls `GCTM`, which calls `udata2finalize`: `tobefnz` -> head of `allgc`, FINALIZEDBIT cleared. The object is collected at a later cycle, with no second `__gc` (unless `setmetatable` is called again).
6. Objects can stay in `tobefnz` across cycles (`finthready` false). `restartcollection` marks them again (comment at 487).

**Generational mode**
- `finobj` is a mirror of allgc with ages. Pointers `finobjsur`, `finobjold1`, `finobjrold` split it. `separatetobefnz` only walks the young part, `[finobj.next, finobjold1)`.
- Old objects are never white in a minor cycle, so they are never separated. They are found in a major cycle (`minor2inc`, then incremental, then `atomic2gen` again).
- `tobefnz` objects keep their age and are aged/marked as a whole list (`markold`, `sweepgen`, `sweep2old`).
- `udata2finalize` puts an object of ANY age at the head of allgc. This is the existing precedent for "old object in the new segment": it only fixes `firstold1` for OLD1.

## 2. Design A: one finobj list set per owner

Each owner (kernel plus each group) gets `finobj`, `finobjsur`, `finobjold1`, `finobjrold`, and its own deferred list.

Code sites that change:
1. **State**: a group struct/object with four finobj words and a queue head/tail, plus a registry `g->groups` to iterate (new field, lstate.h). About 25 lines.
2. **`luaC_checkfinalizer`**: pick the list from the owner of the running thread; one more load. About 5 lines.
3. **`separatetobefnz`**: loop over owners; per owner do what it does now. The `finobjsur` fix-up becomes per owner. User objects go to the group queue. About 30 lines.
4. **`markbeingfnz`**: also walk every group queue. About 8 lines.
5. **Incremental sweep**: `GCSswpfinobj` and `GCSswptobefnz` now run over a set of lists. `sweepstep` has a single cursor `sweepgc`, so a second cursor (owner index) and a loop are needed in `singlestep`, or the group lists are chained into one pass. About 30 lines. Group lists can change during a sweep (a group is created or freed), so the cursor must stay valid.
6. **Generational**: `markold` x2, the finobj part of `youngcollection` (1505-1512), `atomic2gen` (1545-1549), `minor2inc` (1452), `ltests` `lua_checkmemory`. Each becomes a loop over owners. About 40 lines.
7. **`luaC_freeallobjects`** and the ltests checks. About 20 lines.
8. **Group free**: the group's finobj list must be moved out (see section 5). About 40 lines.

Total: roughly 200 lines in about 10 functions, mostly in the generational code. Risks:
- Per-owner segment pointers must stay correct whenever an object is unlinked. Today the only place is `separatetobefnz` (the `finobjsur` fix). With many lists, each unlink or insert must fix the right owner's pointers. A missed fix gives silent corruption, found only by ltests asserts.
- `luaC_checkfinalizer` when the object is already in some list is fine, but "object changes owner" never happens, since FINALIZEDBIT blocks re-registration.
- At group death, a young group's finobj list holds objects of any age (old objects registered by a young group). Relinking them into allgc needs the udata2finalize treatment (only OLD1 is fixed up there).
- The `sweepgc` cursor and the `GCSsw*` states are per global list. More lists means more state and more places to get wrong when a group is created or freed in the middle of a sweep. I could not verify this by running anything.
- Per-group memory: four pointers (32 bytes) and more with queue head/tail.

## 3. Design B: one finobj list set plus an owner tag

### The header (lobject.h:269-275, lgc.h:73-85)
`GCObject { next(8), tt(1), marked(1), hash(4) }`, size 16.

Compiled check (scratchpad only, `sizeof`/`offsetof` of GCObject against the repo headers): `GCObject 16 hash@12 marked@9 tt@8`. So **bytes 10-11 are free padding on LP64** (also bytes 6-7 on a 32-bit build, by arithmetic, not compiled).

Bits of `marked`: 0-2 age (`AGEBITS = 7`, lgc.h:145), 3 WHITE0, 4 WHITE1, 5 BLACK, 6 FINALIZED, 7 TESTBIT. **No bit is free.**

`TESTBIT` (lgc.h:85) is used only in ltests.cpp:601-670 (gray-list consistency check). `ltests.o` is in `OBJ_NAMES` (makefile:18), so I could not confirm whether it is conditional on the `TESTS` variable. Taking bit 7 would make the test checker unable to run. Not recommended; use the padding.

So: add a `uint16_t owner` between `marked` and `hash` (0 = kernel). Cost: one store in `luaC_link` (zero) and one in `luaC_checkfinalizer` (store the current group id). The object does not grow. Only tables and userdata can be finalizable (ltests.cpp:743), so the field is meaningful only for those, but there is no free space in `Table`/`Udata` that I checked (Table: `lu_byte flags` then spans; layout not verified), and the common header is the simpler place.

Limits: 16 bits means at most 65534 concurrent groups, plus one value for "orphan". That is a new hard limit (the port-prep principle says no new Lua limits); it could be made 32-bit only by growing every object by 8 bytes (16 -> 24), which I do not recommend.

### Other places for the owner
- Side table keyed by object (`unordered_map<GCObject*, id>`): allocates at registration (an error path in `setmetatable`), needs removal in `freeobj` for every finalizable object, and is awkward to serialize. Rejected.
- Derive from the metatable: not possible, a metatable is shared between owners.
- Derive from list position: that is Design A.

### `separatetobefnz` under B
The loop is the same. For a white object it reads `owner`:
- 0 (kernel): append to `tobefnz` as now.
- live group: append to that group's queue (tail pointer in the group, to avoid `findlast`), and mark the object.
- orphan (group already freed): do nothing, `p = curr`. It stays white in `finobj` and the existing finobj sweep frees it if nothing resurrects it (sweeplist frees white objects in any list). If something resurrects it, it stays harmlessly in `finobj` with an orphan tag.

All generational pointers stay as they are. This is the main advantage.

## 4. The deferred queue

### Reusing `next`
Yes. An object in a queue is out of `finobj`, so `next` is free. Appending to a queue relinks pointers only: no allocation. The queue needs a head and tail in the group. `tobefnz` uses `findlast` (O(n)); a tail pointer avoids that.

### Staying alive
Objects in a queue must be marked every cycle, exactly like `tobefnz`. The restartable point later calls `udata2finalize`-like code that pops from the group queue and relinks to allgc.

- Marking by the group's own traversal ("strong refs from the group") has two problems: (1) in the atomic phase the group may already be black when `separatetobefnz` appends, so the new items must be marked explicitly; (2) in generational mode an old black group pointing to a young queue item without a barrier breaks the invariant, so the group would need to be made touched (`luaC_barrierback_`). I could not test this.
- **Recommended**: walk all registered groups' queues and mark every item unconditionally, in `restartcollection` and in `atomic` after `separatetobefnz`, regardless of whether the group is itself alive. It costs O(queued) per cycle, the same as `tobefnz`. It avoids deciding group death before resurrection is complete (a group object could be resurrected after `separatetobefnz` by a kernel finalizer, so "dead group" cannot be decided at that point).

### Sweeping and generational aging
Queued objects are marked (black or aged), so they MUST be visited by the same passes as `tobefnz`: the incremental sweep (otherwise they stay black into the next cycle and their white referents are freed: use-after-free), `markold`, `sweepgen`, `sweep2old`, and the ltests checks. With one list per group this means an iterator over the registry in each of those 5 places, or one extra global list. About 40-60 lines.

### Weak tables
- Pass 1 `clearbyvalues` (lgc.cpp:1722-1723) runs before `separatetobefnz`: weak-value entries pointing to a newly dead user object are removed, as for stock finalizable objects.
- Pass 2 `clearbykeys` (1735-1736) runs after the queue is marked: the object is marked, so weak KEYS referring to it are kept (stock "resurrected" rule). The second `clearbyvalues(weak, origweak)` (1738) handles tables found during resurrection.
- Objects already in a queue from earlier cycles are marked at `restartcollection`, so no weak entry is removed for them. User code cannot reach them anyway.
- `iscleared` (228-239) is unchanged.

### GC states and the finalizer thread
- `GCScallfin` (1840-1855) must only run kernel `tobefnz` items. User queues are never touched there: the collector only moves objects.
- The pending test `g->tobefnz.next` in `callallpendingfinalizers`, `finishgencycle` and `GCScallfin` stays kernel-only; no change if user items live in separate queues.
- `g->finth` is one thread with one frozen flag (`finthready`, lgc.cpp:1079). A user finalizer run at a restartable point should run on a thread of the group (its own finalizer thread, budgeted and freezable), not on `g->finth`. `GCTM`'s body can be factored to take the queue, the thread and `L`. `GCTM` also sets `GCSTPGC` and asserts `!gcemergency` (1098-1108): a restartable-point call is outside the collector and should not need `GCSTPGC`; decide this when writing it.
- The existing `GCTM`/`udata2finalize` only ever run in `GCScallfin`, `finishgencycle` and `luaC_freeallobjects`, i.e. at the end of a cycle. A pop from a restartable point can occur in ANY collector state (propagate, atomic is excluded, sweep) and in any mode. `udata2finalize` handles sweep (`makewhite`) and OLD1, but this was never exercised mid-mark. It must be tested in every `gcstate` in both modes, with the ltests invariant checker on. Not verified.
- `luaC_freeallobjects`: at close, `separatetobefnz(all=1)` would put user objects in group queues. Policy needed: drop without `__gc` (consistent with rule (b)) or run them on a group thread. The `assert(g->finobj.next == nullptr)` stays true.

## 5. Group freed (or its threads freed)

With the recommended unconditional marking, at the cycle in which a group is found dead its queue items are all still marked. So in the group's `freeobj` case (lgc.cpp:918):
1. Unregister the group from the registry.
2. For each queued item, clear FINALIZEDBIT and relink at the head of `allgc` exactly like `udata2finalize` (1040-1056): `makewhite` in a sweep phase, `firstold1` for OLD1. No `__gc` is called. They are collected normally at the next cycle (one cycle later than ideal).
3. If an item were unmarked (dead), it could be freed directly; with unconditional marking this does not occur.
4. Do not relink a dead object into allgc after the sweep has freed its referents: it would be traversed with dangling pointers. This is why unmarked-but-relinked must be avoided.

Objects registered but not dead (still in `finobj`):
- Under B they carry the group id. At group free, **walk `finobj` once and set `owner = ORPHAN`** (O(finobj), once per group death). This also makes id reuse safe. Without the walk, a reused id would hand the old group's objects to a new group (silent wrong behavior).
- An orphan white object is skipped by `separatetobefnz` and freed by the finobj sweep; an orphan live object stays in `finobj` with FINALIZEDBIT set and is never finalized (`luaC_checkfinalizer` returns early since `tofinalize`).
- Under A the same objects must be physically spliced from the group's finobj list into allgc or kernel finobj with age handling (the main cost of A).

Freeing a thread of a group: the group registry only holds queues and counts, so threads need no special handling; only the "effective group" of the running thread must not dangle (not designed here).

## 6. Recommendation

**Design B with `owner` in the header padding, plus per-group deferred queues** (strongly marked as roots like `tobefnz`, swept/aged by iterating a registry).

- **Correctness**: all generational age pointers are untouched; `separatetobefnz` keeps its single walk; no new splice code except in group free. Both modes use the same rule. Orphans are handled by existing sweeps.
- **Code size**: about 100-150 lines (header field, registration store, `separatetobefnz` branch, queue mark/sweep/age loops, group free) versus about 200 plus subtle pointer work for A.
- **Hot paths**: `luaC_link` gets one 16-bit store; `luaC_checkfinalizer` gets one 16-bit store, and its allgc linear search (1198) is unchanged, so no new cost beyond that. The mark, sweep and age loops over group queues only cost when queues are non-empty.
- **Persistence**: the owner is a small id in the object plus a registry of group ids; the queue is a plain list. Serializable. A (lists per owner) is also serializable but has many more list heads and segment pointers.
- **Weak spots**: the 65534-group cap (flag it as a new limit, or choose Design A, which has none); id reuse needs the orphan walk; the queue sweep/age loops are the main risk; user-finalizer pop mid-cycle in all `gcstate`s is untested.

Top risks, in order:
1. Queued objects not swept/aged in some path (incremental sweep, `markold`, `sweepgen`, `sweep2old`, `minor2inc`): use-after-free or stale black objects.
2. Deciding "group is dead" too early (before resurrection finishes), or marking queues only via the group (generational barrier).
3. Relinking dead (unmarked) objects into allgc after their referents were freed.
4. Stale ids after group free (reuse) and the 16-bit cap.
5. Pop at a restartable point during mark/sweep phases, in both modes; only ever exercised at end of cycle today.

Unverified: nothing was built or run beyond the `sizeof` check; line numbers are from HEAD 963b6f8d; `Table`/`Udata` spare bytes not checked; whether `TESTS` makes ltests optional.
