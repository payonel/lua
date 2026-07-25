/*
** $Id: lgc.h $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#pragma once

#include <stddef.h>

#include "lobject.h"
#include "lstate.h"

/*
** Collectable objects may have one of three colors: white, which means
** the object is not marked; gray, which means the object is marked, but
** its references may be not marked; and black, which means that the
** object and all its references are marked.  The main invariant of the
** garbage collector, while marking objects, is that a black object can
** never point to a white one. Moreover, any gray object must be in a
** "gray list" (gray, grayagain, weak, allweak, ephemeron) so that it
** can be visited again before finishing the collection cycle. (Open
** upvalues are an exception to this rule, as they are attached to
** a corresponding thread.)  These lists have no meaning when the
** invariant is not being enforced (e.g., sweep phase).
*/

/*
** Possible states of the Garbage Collector
*/
constexpr int GCSpropagate = 0;
constexpr int GCSenteratomic = 1;
constexpr int GCSatomic = 2;
constexpr int GCSswpallgc = 3;
constexpr int GCSswpfinobj = 4;
constexpr int GCSswptobefnz = 5;
constexpr int GCSswpend = 6;
constexpr int GCScallfin = 7;
constexpr int GCSpause = 8;

static l_forceinline int issweepphase(global_State* g)
{
  return GCSswpallgc <= g->gcstate && g->gcstate <= GCSswpend;
}

/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep phase may break
** the invariant, as objects turned white may point to still-black
** objects. The invariant is restored when sweep ends and all objects
** are white again.
*/

static l_forceinline int keepinvariant(global_State* g)
{
  return g->gcstate <= GCSatomic;
}

/*
** some useful bit tricks
*/
constexpr int bitmask(int b)
{
  return 1 << b;
}

constexpr int bit2mask(int b1, int b2)
{
  return bitmask(b1) | bitmask(b2);
}

/*
** Layout for bit use in 'marked' field. First three bits are
** used for object "age" in generational mode. Last bit is used
** by tests.
*/
constexpr int WHITE0BIT = 3; /* object is white (type 0) */
constexpr int WHITE1BIT = 4; /* object is white (type 1) */
constexpr int BLACKBIT = 5; /* object is black */
constexpr int FINALIZEDBIT = 6; /* object has been marked for finalization */

constexpr int TESTBIT = 7;

constexpr int WHITEBITS = bit2mask(WHITE0BIT, WHITE1BIT);

static l_forceinline int iswhite(const GCObject* x)
{
  return x->marked & WHITEBITS;
}
static l_forceinline int isblack(const GCObject* x)
{
  return x->marked & bitmask(BLACKBIT);
}
/* neither white nor black */
static l_forceinline int isgray(const GCObject* x)
{
  return !(x->marked & (WHITEBITS | bitmask(BLACKBIT)));
}

static l_forceinline int tofinalize(const GCObject* x)
{
  return x->marked & bitmask(FINALIZEDBIT);
}

static l_forceinline lu_byte otherwhite(global_State* g)
{
  return (lu_byte)(g->currentwhite ^ WHITEBITS);
}
constexpr int isdeadm(lu_byte ow, lu_byte m)
{
  return m & ow;
}
static l_forceinline int isdead(global_State* g, const GCObject* v)
{
  return isdeadm(otherwhite(g), v->marked);
}

static l_forceinline void changewhite(GCObject* x)
{
  x->marked = (lu_byte)(x->marked ^ WHITEBITS);
}
static l_forceinline void nw2black(GCObject* x)
{
  assert(!iswhite(x));
  x->marked = (lu_byte)(x->marked | bitmask(BLACKBIT));
}

static l_forceinline lu_byte luaC_white(global_State* g)
{
  return (lu_byte)(g->currentwhite & WHITEBITS);
}

/* object age in generational mode */
constexpr int G_NEW = 0; /* created in current cycle */
constexpr int G_SURVIVAL = 1; /* created in previous cycle */
constexpr int G_OLD0 = 2; /* marked old by frw. barrier in this cycle */
constexpr int G_OLD1 = 3; /* first full cycle as old */
constexpr int G_OLD = 4; /* really old object (not to be visited) */
constexpr int G_TOUCHED1 = 5; /* old object touched this cycle */
constexpr int G_TOUCHED2 = 6; /* old object touched in previous cycle */

constexpr int AGEBITS = 7; /* all age bits (111) */

static l_forceinline int getage(const GCObject* o)
{
  return o->marked & AGEBITS;
}
static l_forceinline void setage(GCObject* o, int a)
{
  o->marked = (lu_byte)((o->marked & ~AGEBITS) | a);
}
static l_forceinline int isold(const GCObject* o)
{
  return getage(o) > G_SURVIVAL;
}

/*
** In generational mode, objects are created 'new'. After surviving one
** cycle, they become 'survival'. Both 'new' and 'survival' can point
** to any other object, as they are traversed at the end of the cycle.
** We call them both 'young' objects.
** If a survival object survives another cycle, it becomes 'old1'.
** 'old1' objects can still point to survival objects (but not to
** new objects), so they still must be traversed. After another cycle
** (that, being old, 'old1' objects will "survive" no matter what)
** finally the 'old1' object becomes really 'old', and then they
** are no more traversed.
**
** To keep its invariants, the generational mode uses the same barriers
** also used by the incremental mode. If a young object is caught in a
** forward barrier, it cannot become old immediately, because it can
** still point to other young objects. Instead, it becomes 'old0',
** which in the next cycle becomes 'old1'. So, 'old0' objects is
** old but can point to new and survival objects; 'old1' is old
** but cannot point to new objects; and 'old' cannot point to any
** young object.
**
** If any old object ('old0', 'old1', 'old') is caught in a back
** barrier, it becomes 'touched1' and goes into a gray list, to be
** visited at the end of the cycle.  There it evolves to 'touched2',
** which can point to survivals but not to new objects. In yet another
** cycle then it becomes 'old' again.
**
** The generational mode must also control the colors of objects,
** because of the barriers.  While the mutator is running, young objects
** are kept white. 'old', 'old1', and 'touched2' objects are kept black,
** as they cannot point to new objects; exceptions are threads and open
** upvalues, which age to 'old1' and 'old' but are kept gray. 'old0'
** objects may be gray or black, as in the incremental mode. 'touched1'
** objects are kept gray, as they must be visited again at the end of
** the cycle.
*/

/*
** {======================================================
** Default Values for GC parameters
** =======================================================
*/

/*
** Minor collections will shift to major ones after LUAI_MINORMAJOR%
** bytes become old.
*/
constexpr int LUAI_MINORMAJOR = 70;

/*
** Major collections will shift to minor ones after a collection
** collects at least LUAI_MAJORMINOR% of the new bytes.
*/
constexpr int LUAI_MAJORMINOR = 50;

/*
** A young (minor) collection will run after creating LUAI_GENMINORMUL%
** new bytes.
*/
constexpr int LUAI_GENMINORMUL = 20;

/* incremental */

/* Number of bytes must be LUAI_GCPAUSE% before starting new cycle */
constexpr int LUAI_GCPAUSE = 250;

/*
** Step multiplier: The collector handles LUAI_GCMUL% work units for
** each new allocated word. (Each "work unit" corresponds roughly to
** sweeping one object or traversing one slot.)
*/
constexpr int LUAI_GCMUL = 200;

/* How many bytes to allocate before next GC step */
constexpr size_t LUAI_GCSTEPSIZE = (200 * sizeof(Table));

/* }====================================================== */

/*
** Control when GC is running:
*/
constexpr int GCSTPUSR = 1; /* bit true when GC stopped by user */
constexpr int GCSTPGC = 2; /* bit true when GC stopped by itself */
constexpr int GCSTPCLS = 4; /* bit true when closing Lua state */
static l_forceinline int gcrunning(global_State* g)
{
  return g->gcstp == 0;
}

/*
** Does one step of collection when debt becomes zero. 'pre'/'pos'
** allows some adjustments to be done only when needed. macro
** 'condchangemem' is used only for heavy tests (forcing a full
** GC cycle on every opportunity)
*/

#if !defined(HARDMEMTESTS)
#define condchangemem(L, pre, pos, emg) ((void) 0)
#else
#define condchangemem(L, pre, pos, emg) \
  {                                     \
    if (gcrunning(G(L)))                \
    {                                   \
      pre;                              \
      luaC_fullgc(L, emg);              \
      pos;                              \
    }                                   \
  }
#endif

LUAI_FUNC void luaC_fix(lua_State* L, GCObject* o);
LUAI_FUNC void luaC_freeallobjects(lua_State* L);
LUAI_FUNC void luaC_step(lua_State* L);

static l_forceinline void luaC_checkGC(lua_State* L)
{
  if (G(L)->GCdebt <= 0)
    luaC_step(L);
  condchangemem(L, (void) 0, (void) 0, 0);
}
LUAI_FUNC void luaC_runtilstate(lua_State* L, int state, int fast);
LUAI_FUNC void luaC_fullgc(lua_State* L, int isemergency);
LUAI_FUNC GCObject* luaC_newobj(lua_State* L, lu_byte tt, size_t sz);
LUAI_FUNC GCObject* luaC_newobjdt(lua_State* L, lu_byte tt, size_t sz);
LUAI_FUNC void luaC_barrier_(lua_State* L, GCObject* o, GCObject* v);
LUAI_FUNC void luaC_barrierback_(lua_State* L, GCObject* o);

/*
** GC barriers. 'p' is the parent object, 'v'/'o' the newly referenced
** value/object. Forward barrier marks the new object; back barrier
** re-grays the parent.
*/
static l_forceinline void luaC_objbarrier(lua_State* L, GCObject* p, GCObject* o)
{
  if (isblack(p) && iswhite(o))
    luaC_barrier_(L, p, o);
}

static l_forceinline void luaC_barrier(lua_State* L, GCObject* p, const TValue* v)
{
  if (v->tt_ & BIT_ISCOLLECTABLE)
    luaC_objbarrier(L, p, v->value_.gc);
}

static l_forceinline void luaC_objbarrierback(lua_State* L, GCObject* p, GCObject* o)
{
  if (isblack(p) && iswhite(o))
    luaC_barrierback_(L, p);
}

static l_forceinline void luaC_barrierback(lua_State* L, GCObject* p, const TValue* v)
{
  if (v->tt_ & BIT_ISCOLLECTABLE)
    luaC_objbarrierback(L, p, v->value_.gc);
}
LUAI_FUNC void luaC_checkfinalizer(lua_State* L, GCObject* o, Table* mt);
LUAI_FUNC void luaC_changemode(lua_State* L, int newmode);
