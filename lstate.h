/*
** $Id: lstate.h $
** Global State
** See Copyright Notice in lua.h
*/

#pragma once

#include <vector>

#include "lua.h"

/* Some header files included here need this definition */
struct CallInfo;

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"

#include "lhack.h"

/*
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray (with two exceptions explained below):
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
**
** The exceptions to that "gray rule" are:
** - TOUCHED2 objects in generational mode stay in a gray list (because
** they must be visited again at the end of the cycle), but they are
** marked black because assignments to them must activate barriers (to
** move them back to TOUCHED1).
** - Open upvalues are kept gray to avoid barriers, but they stay out
** of gray lists. (They don't even have a 'gclist' field.)
*/

/*
** About 'nCcalls':  This count has two parts: the lower 16 bits counts
** the number of recursive invocations in the C stack; the higher
** 16 bits counts the number of non-yieldable calls in the stack.
** (They are together so that we can change and save both with one
** instruction.)
*/

/* true if this thread does not have non-yieldable calls in the stack */

/* real number of C calls */

/* Increment the number of non-yieldable calls */

/* Decrement the number of non-yieldable calls */

/* Non-yieldable call increment */
constexpr int nyci = (0x10000 | 1);

struct lua_longjmp; /* defined in ldo.c */

/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#include <signal.h>
typedef sig_atomic_t l_signalT;

/*
** Extra stack space to handle TM calls and some other extras. This
** space is not included in 'stack_last'. It is used only to avoid stack
** checks, either because the element will be promptly popped or because
** there will be a stack check soon after the push. Function frames
** never use this extra space, so it does not need to be kept clean.
*/
constexpr int EXTRA_STACK = 5;

constexpr int BASIC_STACK_SIZE = (2 * LUA_MINSTACK);

/* kinds of Garbage Collection */
constexpr int KGC_INC = 0; /* incremental gc */
constexpr int KGC_GENMINOR = 1; /* generational gc in minor (regular) mode */
constexpr int KGC_GENMAJOR = 2; /* generational in major mode */

struct CallInfo
{
  StkId func;                       /* function index in the stack */
  StkId top;                        /* top for this function */
  struct CallInfo *previous, *next; /* dynamic call link */
  int savedpc;
  volatile l_signalT trap; /* function is tracing lines/counts */
  int nextraargs;          /* # of extra arguments in vararg functions */
  lua_KFunction k; /* continuation in case of yields */
  StkId old_errfunc;
  lua_KContext ctx; /* context info. in case of yields */
  int funcidx;      /* called-function index */
  int nyield;       /* number of values yielded */
  int nres;         /* number of values returned */
  l_uint32 callstatus;
};

/*
** Maximum expected number of results from a function
** (must fit in CIST_NRESULTS).
*/
constexpr int MAXRESULTS = 250;

/*
** Bits in CallInfo status
*/
/* bits 0-7 are the expected number of results from this function + 1 */
constexpr l_uint32 CIST_NRESULTS = 0xffu;

/* bits 8-11 count call metamethods (and their extra arguments) */
constexpr int CIST_CCMT = 8; /* the offset, not the mask */
constexpr l_uint32 MAX_CCMT = (0xfu << CIST_CCMT);

/* Bits 12-14 are used for CIST_RECST (see below) */
constexpr int CIST_RECST = 12; /* the offset, not the mask */

/* call is running a C function (still in first 16 bits) */
constexpr l_uint32 CIST_C = (1u << (CIST_RECST + 3));
/* call is on a fresh "luaV_execute" frame */
constexpr l_uint32 CIST_FRESH = (((l_uint32)(CIST_C)) << 1);
/* function is closing tbc variables */
constexpr l_uint32 CIST_CLSRET = (CIST_FRESH << 1);
/* function has tbc variables to close */
constexpr l_uint32 CIST_TBC = (CIST_CLSRET << 1);
/* original value of 'allowhook' */
constexpr l_uint32 CIST_OAH = (CIST_TBC << 1);
/* call is running a debug hook */
constexpr l_uint32 CIST_HOOKED = (CIST_OAH << 1);
/* doing a yieldable protected call */
constexpr l_uint32 CIST_YPCALL = (CIST_HOOKED << 1);
/* call was tail called */
constexpr l_uint32 CIST_TAIL = (CIST_YPCALL << 1);
/* last hook called yielded */
constexpr l_uint32 CIST_HOOKYIELD = (CIST_TAIL << 1);
/* function "called" a finalizer */
constexpr l_uint32 CIST_FIN = (CIST_HOOKYIELD << 1);

constexpr int get_nresults(l_uint32 cs)
{
  return (int)(cs & CIST_NRESULTS) - 1;
}

/*
** Field CIST_RECST stores the "recover status", used to keep the error
** status while closing to-be-closed variables in coroutines, so that
** Lua can correctly resume after an yield from a __close method called
** because of an error.  (Three bits are enough for error status.)
*/
static l_forceinline lu_byte getcistrecst(const CallInfo* ci)
{
  return (lu_byte)((ci->callstatus >> CIST_RECST) & 7);
}
static l_forceinline void setcistrecst(CallInfo* ci, int st)
{
  assert((st & 7) == st); /* status must fit in three bits */
  ci->callstatus = (ci->callstatus & ~(7u << CIST_RECST)) | ((l_uint32)st << CIST_RECST);
}

/* active function is a Lua function */
static l_forceinline int isLua(const CallInfo* ci)
{
  return !(ci->callstatus & CIST_C);
}

/* call is running Lua code (not a hook) */
static l_forceinline int isLuacode(const CallInfo* ci)
{
  return !(ci->callstatus & (CIST_C | CIST_HOOKED));
}

static l_forceinline void setoah(CallInfo* ci, int v)
{
  ci->callstatus = v ? (ci->callstatus | CIST_OAH) : (ci->callstatus & ~CIST_OAH);
}
static l_forceinline lu_byte getoah(const CallInfo* ci)
{
  return (ci->callstatus & CIST_OAH) ? 1 : 0;
}

/*
** 'per thread' state
*/
struct global_State;
struct lua_State : GCListed
{
  lu_byte allowhook;
  TStatus status;
  StkId top; /* first free slot in the stack */
  global_State* l_G;
  CallInfo* ci;    /* call info for current function */
  StkId stack_last; /* end of stack (last element + 1) */
  std::span<TValue> stack;    /* stack base */
  UpVal* openupval;    /* list of open upvalues in this stack */
  StkId tbclist;
  std::span<StkId> tbc;
  int ntbc;
  struct lua_State* twups;      /* list of threads with open upvalues */
  struct lua_longjmp* errorJmp; /* current error recover point */
  CallInfo base_ci;             /* CallInfo for first level (C host) */
  volatile lua_Hook hook;
  StkId errfunc; /* current error handling function (stack index) */
  l_uint32 nCcalls;  /* number of nested non-yieldable or C calls */
  int oldpc;         /* last pc traced */
  int nci;           /* number of items in 'ci' list */
  int basehookcount;
  int hookcount;
  volatile l_signalT hookmask;
  struct
  {                /* info about transferred values (for call/return hooks) */
    int ftransfer; /* offset of first value transferred */
    int ntransfer; /* number of values transferred */
  } transferinfo;
  void* extra;
};

static l_forceinline void setthvalue(lua_State* L, TValue* obj, lua_State* x)
{
  assert(x->tt == LUA_VTHREAD);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VTHREAD);
  checkliveness(L, obj);
}

static l_forceinline lua_State* thvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VTHREAD));
  assert(o->value_.ref->tt == LUA_VTHREAD);
  return static_cast<lua_State*>(o->value_.ref);
}

static l_forceinline lu_byte slottag(lua_State* L, StkId idx)
{
  return L->stack[idx].tt_;
}

static l_forceinline void setslottag(lua_State* L, StkId idx, lu_byte tt)
{
  L->stack[idx].tt_ = tt;
}

static l_forceinline TValue getslot(lua_State* L, StkId idx)
{
  return L->stack[idx];
}

static l_forceinline void setslot(lua_State* L, StkId idx, const TValue* v)
{
  L->stack[idx] = *v;
  checkliveness(L, v);
}

static l_forceinline void copyslot(lua_State* L, StkId dst, StkId src)
{
  L->stack[dst] = L->stack[src];
}

static l_forceinline void copyslot(lua_State* to, StkId dst, lua_State* from, StkId src)
{
  to->stack[dst] = from->stack[src];
}

static l_forceinline void setobj(lua_State* L, TValue* dst, StkId src)
{
  *dst = L->stack[src];
  checkliveness(L, dst);
}

static l_forceinline lua_Integer slotival(lua_State* L, StkId idx)
{
  return ivalue(&L->stack[idx]);
}

static l_forceinline lua_Number slotfltval(lua_State* L, StkId idx)
{
  return fltvalue(&L->stack[idx]);
}

static l_forceinline lua_Number slotnval(lua_State* L, StkId idx)
{
  return nvalue(&L->stack[idx]);
}

static l_forceinline void* slotpval(lua_State* L, StkId idx)
{
  return pvalue(&L->stack[idx]);
}

static l_forceinline lua_CFunction slotfval(lua_State* L, StkId idx)
{
  return fvalue(&L->stack[idx]);
}

static l_forceinline GCObject* slotgcval(lua_State* L, StkId idx)
{
  return gcvalue(&L->stack[idx]);
}

static l_forceinline TString* slottsval(lua_State* L, StkId idx)
{
  return tsvalue(&L->stack[idx]);
}

static l_forceinline Table* slothval(lua_State* L, StkId idx)
{
  return hvalue(&L->stack[idx]);
}

static l_forceinline Udata* slotuval(lua_State* L, StkId idx)
{
  return uvalue(&L->stack[idx]);
}

static l_forceinline lua_State* slotthval(lua_State* L, StkId idx)
{
  return thvalue(&L->stack[idx]);
}

static l_forceinline Closure* slotclval(lua_State* L, StkId idx)
{
  return clvalue(&L->stack[idx]);
}

static l_forceinline LClosure* slotclLval(lua_State* L, StkId idx)
{
  return clLvalue(&L->stack[idx]);
}

static l_forceinline CClosure* slotclCval(lua_State* L, StkId idx)
{
  return clCvalue(&L->stack[idx]);
}

static l_forceinline void setslotnil(lua_State* L, StkId idx)
{
  L->stack[idx].tt_ = LUA_VNIL;
}

static l_forceinline void setslotempty(lua_State* L, StkId idx)
{
  L->stack[idx].tt_ = LUA_VEMPTY;
}

static l_forceinline void setslotbt(lua_State* L, StkId idx)
{
  L->stack[idx].tt_ = LUA_VTRUE;
}

static l_forceinline void setslotbf(lua_State* L, StkId idx)
{
  L->stack[idx].tt_ = LUA_VFALSE;
}

static l_forceinline void setsloti(lua_State* L, StkId idx, lua_Integer x)
{
  L->stack[idx].value_.prim = x;
  L->stack[idx].tt_ = LUA_VNUMINT;
}

static l_forceinline void setslotflt(lua_State* L, StkId idx, lua_Number x)
{
  L->stack[idx].value_.prim = std::bit_cast<lua_Integer>(x);
  L->stack[idx].tt_ = LUA_VNUMFLT;
}

static l_forceinline void chgsloti(lua_State* L, StkId idx, lua_Integer x)
{
  assert(L->stack[idx].tt_ == LUA_VNUMINT);
  L->stack[idx].value_.prim = x;
}

static l_forceinline void chgslotflt(lua_State* L, StkId idx, lua_Number x)
{
  assert(L->stack[idx].tt_ == LUA_VNUMFLT);
  L->stack[idx].value_.prim = std::bit_cast<lua_Integer>(x);
}

static l_forceinline void setslotp(lua_State* L, StkId idx, void* x)
{
  L->stack[idx].value_.ref = static_cast<GCObject*>(x);
  L->stack[idx].tt_ = LUA_VLIGHTUSERDATA;
}

static l_forceinline void setslotf(lua_State* L, StkId idx, CFuncInfo* cfi)
{
  L->stack[idx].value_.ref = cfi;
  L->stack[idx].tt_ = ctb(LUA_VLCF);
}

static l_forceinline void setslots(lua_State* L, StkId idx, TString* x)
{
  assert((x->tt & NOVARIANT_MASK) == LUA_TSTRING);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  checkliveness(L, &L->stack[idx]);
}

static l_forceinline void setsloth(lua_State* L, StkId idx, Table* x)
{
  assert(x->tt == LUA_VTABLE);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  checkliveness(L, &L->stack[idx]);
}

static l_forceinline void setslotu(lua_State* L, StkId idx, Udata* x)
{
  assert(x->tt == LUA_VUSERDATA);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  checkliveness(L, &L->stack[idx]);
}

static l_forceinline void setslotclL(lua_State* L, StkId idx, LClosure* x)
{
  assert(x->tt == LUA_VLCL);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  checkliveness(L, &L->stack[idx]);
}

static l_forceinline void setslotclC(lua_State* L, StkId idx, CClosure* x)
{
  assert(x->tt == LUA_VCCL);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  checkliveness(L, &L->stack[idx]);
}

static l_forceinline void setslotth(lua_State* L, StkId idx, lua_State* x)
{
  assert(x->tt == LUA_VTHREAD);
  L->stack[idx].value_.ref = x;
  L->stack[idx].tt_ = ctb(x->tt);
  TValue v = getslot(L, idx);
  checkliveness(L, &v);
}

/* }================================================================== */

/* true if this thread does not have non-yieldable calls in the stack */
static l_forceinline int yieldable(const lua_State* L)
{
  return (L->nCcalls & 0xffff0000) == 0;
}

/* real number of C calls */
static l_forceinline int getCcalls(const lua_State* L)
{
  return (int)(L->nCcalls & 0xffff);
}

static l_forceinline void incnny(lua_State* L)
{
  L->nCcalls += 0x10000;
}

static l_forceinline void decnny(lua_State* L)
{
  L->nCcalls -= 0x10000;
}

static l_forceinline int stacksize(const lua_State* th)
{
  return (int)th->stack_last;
}

/*
** 'global state', shared by all threads of this state
*/
struct global_State
{
  lua_Alloc* frealloc; /* function to reallocate memory */
  l_mem GCtotalbytes; /* number of bytes currently allocated + debt */
  l_mem GCdebt;       /* bytes counted but not yet allocated */
  size_t memlimit;
  l_mem GCmarked;     /* number of objects marked in a GC cycle */
  l_mem GCmajorminor; /* auxiliary counter to control major-minor shifts */
  struct StringTable
  {
    std::vector<TString*> buckets;
    size_t count = 0;
  };
  StringTable strt;
  TValue l_registry;
  TValue nilvalue;   /* a nil value */
  unsigned int seed; /* randomized seed for hashes */
  lu_byte gcparams[LUA_GCPN];
  lu_byte currentwhite;
  lu_byte gcstate;     /* state of garbage collector */
  lu_byte gckind;      /* kind of GC running */
  lu_byte gcstopem;    /* stops emergency collections */
  lu_byte gcstp;       /* control whether GC is running */
  lu_byte gcemergency; /* true if this is an emergency collection */
  GCObject allgc;      /* list of all collectable objects */
  GCObject* sweepgc;  /* current position of sweep in list */
  GCObject finobj;     /* list of collectable objects with finalizers */
  GCListed gray;       /* list of gray objects */
  GCListed grayagain;  /* list of objects to be traversed atomically */
  GCListed weak;       /* list of tables with weak values */
  GCListed ephemeron;  /* list of ephemeron tables (weak keys) */
  GCListed allweak;    /* list of all-weak tables */
  GCObject tobefnz;    /* list of userdata to be GC */
  GCObject* fixedgc;   /* list of objects not to be collected */
  /* fields for generational collector */
  GCObject* survival;                        /* start of objects that survived one GC cycle */
  GCObject* old1;                            /* start of old1 objects */
  GCObject* reallyold;                       /* objects more than one cycle old ("really old") */
  GCObject* firstold1;                       /* first OLD1 object in the list (if any) */
  GCObject* finobjsur;                       /* list of survival objects with finalizers */
  GCObject* finobjold1;                      /* list of old1 objects with finalizers */
  GCObject* finobjrold;                      /* list of really old objects with finalizers */
  struct lua_State* twups;                   /* list of threads with open upvalues */
  lua_CFunction panic;                       /* to be called in unprotected errors */
  TString* memerrmsg;                        /* message for memory-allocation errors */
  TString* tmname[TM_N];                     /* array with tag-method names */
  struct Table* mt[LUA_NUMTYPES];            /* metatables for basic types */
  lua_WarnFunction* warnf;                    /* warning function */
  InternalString locale_name;
  lua_State mainth;                          /* main thread of this state */
};

/*
** 'g->nilvalue' being a nil value flags that the state was completely
** build.
*/

/* actual number of total memory allocated */

LUAI_FUNC void luaE_setdebt(global_State* g, l_mem debt);
LUAI_FUNC void luaE_freethread(lua_State* L, lua_State* L1);
LUAI_FUNC lu_mem luaE_threadsize(lua_State* L);
LUAI_FUNC CallInfo* luaE_extendCI(lua_State* L, int err);
LUAI_FUNC void luaE_shrinkCI(lua_State* L);
static l_forceinline lua_State* mainthread(global_State* g)
{
  return &g->mainth;
}

static l_forceinline int completestate(global_State* g)
{
  return (g->nilvalue.tt_ & NOVARIANT_MASK) == LUA_TNIL;
}

/* actual number of total memory allocated */
static l_forceinline l_mem gettotalbytes(global_State* g)
{
  return g->GCtotalbytes - g->GCdebt;
}

static l_forceinline const TValue* gfasttm(global_State* g, Table* mt, TMS e)
{
  return checknoTM(mt, e) ? nullptr : luaT_gettm(mt, e, g->tmname[e]);
}

LUAI_FUNC void luaE_checkcstack(lua_State* L);
LUAI_FUNC void luaE_incCstack(lua_State* L);
LUAI_FUNC void luaE_warning(lua_State* L, byte_view msg, int tocont);
LUAI_FUNC void luaE_warnerror(lua_State* L, byte_view where);
LUAI_FUNC TStatus luaE_resetthread(lua_State* L, TStatus status);
