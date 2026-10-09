/*
** $Id: lstate.h $
** Global State
** See Copyright Notice in lua.h
*/

#pragma once

#include <signal.h>
#include <vector>

#include "lua.h"

/* Some header files included here need this definition */
namespace lua::runtime
{
struct CallInfo;
}

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"

#include "lhack.h"

namespace lua::runtime
{

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
constexpr int nyci = 0x10000 | 1;

struct lua_longjmp; /* defined in ldo.c */

/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
typedef sig_atomic_t l_signalT;

/*
** Extra stack space to handle TM calls and some other extras. This
** space is not included in 'stack_last'. It is used only to avoid stack
** checks, either because the element will be promptly popped or because
** there will be a stack check soon after the push. Function frames
** never use this extra space, so it does not need to be kept clean.
*/
constexpr int EXTRA_STACK = 5;

constexpr int BASIC_STACK_SIZE = 2 * LUA_MINSTACK;

/* kinds of Garbage Collection */
constexpr int KGC_INC = 0; /* incremental gc */
constexpr int KGC_GENMINOR = 1; /* generational gc in minor (regular) mode */
constexpr int KGC_GENMAJOR = 2; /* generational in major mode */

struct CallInfo
{
  StkId func = 0;                       /* function index in the stack */
  StkId top = 0;                        /* top for this function */
  CallInfo* previous = nullptr; /* dynamic call link */
  CallInfo* next = nullptr;
  int savedpc = 0;
  volatile l_signalT trap = 0; /* function is tracing lines/counts */
  int nextraargs = 0;          /* # of extra arguments in vararg functions */
  lua_KFunction k = nullptr; /* continuation in case of yields */
  StkId old_errfunc = 0;
  lua_KContext ctx = 0; /* context info. in case of yields */
  int funcidx = 0;      /* called-function index */
  int nyield = 0;       /* number of values yielded */
  int nres = 0;         /* number of values returned */
  l_uint32 callstatus = 0;
  l_uint32 nccalls = 0; /* CIST_CCOUNT: 'nCcalls' while it runs its own code */
  lu_byte apiop = 0;  /* CIST_APIK: how to finish the API call (APIK_*) */
  StkId apifirst = 0; /* CIST_APIK, a concatenation: its first value */
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
constexpr l_uint32 MAX_CCMT = 0xfu << CIST_CCMT;

/* Bits 12-14 are used for CIST_RECST (see below) */
constexpr int CIST_RECST = 12; /* the offset, not the mask */

/* call is running a C function (still in first 16 bits) */
constexpr l_uint32 CIST_C = 1u << (CIST_RECST + 3);
/* call is on a fresh "luaV_execute" frame */
constexpr l_uint32 CIST_FRESH = (l_uint32)CIST_C << 1;
/* function is closing tbc variables */
constexpr l_uint32 CIST_CLSRET = CIST_FRESH << 1;
/* function has tbc variables to close */
constexpr l_uint32 CIST_TBC = CIST_CLSRET << 1;
/* original value of 'allowhook' */
constexpr l_uint32 CIST_OAH = CIST_TBC << 1;
/* call is running a debug hook */
constexpr l_uint32 CIST_HOOKED = CIST_OAH << 1;
/* doing a yieldable protected call */
constexpr l_uint32 CIST_YPCALL = CIST_HOOKED << 1;
/* call was tail called */
constexpr l_uint32 CIST_TAIL = CIST_YPCALL << 1;
/* last hook called yielded */
constexpr l_uint32 CIST_HOOKYIELD = CIST_TAIL << 1;
/* function "called" a finalizer */
constexpr l_uint32 CIST_FIN = CIST_HOOKYIELD << 1;
/* C function is in an API call with a continuation ('lua_getik'...):
   a metamethod the call runs may yield */
constexpr l_uint32 CIST_APIK = CIST_FIN << 1;
/* Lua function frozen at a budget point: the instruction before 'savedpc'
   has not run yet (see 'lua_freeze') */
constexpr l_uint32 CIST_FROZE = CIST_APIK << 1;
/* C function is in a call that cannot yield ('lua_callk' or 'lua_pcallk'
   in a thread that cannot yield): after a thaw, its end counts down the
   non-yieldable calls */
constexpr l_uint32 CIST_NYCALL = CIST_FROZE << 1;
/* C function whose calls with continuations can freeze but not yield, as
   in a thread that cannot yield (a library function that keeps the
   yieldability of stock Lua: see 'lua_freezeonly') */
constexpr l_uint32 CIST_FZONLY = CIST_NYCALL << 1;
/* function raised an error and is calling the message handler (a call
   that cannot yield): when the handler ends after a thaw, the error goes
   on (see 'unroll') */
constexpr l_uint32 CIST_ERRH = CIST_FZONLY << 1;
/* frame entered through C ('ccall'): 'nccalls' has the count it runs
   with (frames entered from Lua code run with their caller's) */
constexpr l_uint32 CIST_CCOUNT = CIST_ERRH << 1;

/* the budget, in 'hookmask' (not a hook: the hook API never shows it) */
constexpr int LUA_MASKBUDGET = 1 << 6;

/* how a frozen thread was entered (field 'frozen') */
constexpr lu_byte FZ_RESUME = 1; /* by 'lua_resume' or 'lua_thaw' */
constexpr lu_byte FZ_CALL = 2;   /* by a 'lua_call' of the host */
constexpr lu_byte FZ_PCALL = 3;  /* by a 'lua_pcall' of the host */

/*
** How 'finishCcall' finishes an API call with a continuation whose
** metamethod yielded, so that the stack is as the call leaves it.
*/
constexpr lu_byte APIK_PUSH = 1;    /* the result is on the top: done */
constexpr lu_byte APIK_OVERKEY = 2; /* the result replaces the key below it */
constexpr lu_byte APIK_POP1 = 3;    /* pop the value set */
constexpr lu_byte APIK_POP2 = 4;    /* pop the value set and its key */
constexpr lu_byte APIK_BOOL = 5;    /* the result, as a boolean, is on the top */
constexpr lu_byte APIK_ARITH = 6;   /* the result replaces both operands */
constexpr lu_byte APIK_CONCAT = 7;  /* the result replaces two values; go on */

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
  return (ci->callstatus >> CIST_RECST) & 7;
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
}

struct lua_State : GCListed
{
  lu_byte allowhook = 1;
  TStatus status = LUA_OK;
  StkId top = 0; /* first free slot in the stack */
  global_State* l_G = nullptr;
  CallInfo* ci = nullptr;    /* call info for current function */
  StkId stack_last = 0; /* end of stack (last element + 1) */
  std::span<TValue> stack;    /* stack base */
  UpVal* openupval = nullptr;    /* list of open upvalues in this stack */
  StkId tbclist = 0;
  std::span<StkId> tbc;
  int ntbc = 0;
  lua_State* twups = this;      /* list of threads with open upvalues */
  lua_longjmp* errorJmp = nullptr; /* current error recover point */
  CallInfo base_ci;             /* CallInfo for first level (C host) */
  volatile lua_Hook hook = nullptr;
  StkId errfunc = 0; /* current error handling function (stack index) */
  l_uint32 nCcalls = 0;  /* number of nested non-yieldable or C calls */
  int oldpc = 0;         /* last pc traced */
  int nci = 0;           /* number of items in 'ci' list */
  int basehookcount = 0;
  int hookcount = 0;
  volatile l_signalT hookmask = 0;
  struct
  {                /* info about transferred values (for call/return hooks) */
    int ftransfer = 0; /* offset of first value transferred */
    int ntransfer = 0; /* number of values transferred */
  } transferinfo;
  void* extra = nullptr;
  /* freezing ('lua_freeze') */
  lua_State* resumer = nullptr;     /* thread resuming this one, while it runs */
  lua_State* frozenchild = nullptr; /* frozen while resuming this thread */
  int nfz = 0;              /* number of calls in the stack that cannot freeze */
  lu_byte resumedk = 0;     /* resumed by a call with a continuation */
  BudgetGroup* bown = nullptr; /* the budget group it belongs to, if any */
  BudgetGroup* beff = nullptr; /* the budget group in effect while it runs */
  lu_byte frozen = 0;       /* frozen: how it was entered (FZ_*); 0: not */
  StkId frozenfunc = 0;     /* frozen in a host call: the function called, */
  StkId frozenerrfunc = 0;  /* its message handler, */
  int frozennres = 0;       /* its number of results, */
  l_uint32 frozencc = 0;    /* its 'nCcalls' where it froze */
  l_uint32 ccbase = 0;      /* its 'nCcalls' when resumed (see 'forgetcalls') */
};

namespace lua::runtime
{

static l_forceinline void setthvalue(lua_State* L, TValue* obj, lua_State* x)
{
  assert(x->tt == LUA_VTHREAD);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VTHREAD);
  checkliveness(L, obj);
}

static l_forceinline lua_State* thvalue(const TValue* o)
{
  assert(ttisthread(o));
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
  return getobj(&L->stack[idx]);
}

static l_forceinline void setslot(lua_State* L, StkId idx, const TValue* v)
{
  copyobj(&L->stack[idx], v);
  checkliveness(L, v);
}

static l_forceinline void copyslot(lua_State* L, StkId dst, StkId src)
{
  copyobj(&L->stack[dst], &L->stack[src]);
}

static l_forceinline void copyslot(lua_State* to, StkId dst, lua_State* from, StkId src)
{
  copyobj(&to->stack[dst], &from->stack[src]);
}

static l_forceinline void slot2obj(lua_State* L, TValue* dst, StkId src)
{
  copyobj(dst, &L->stack[src]);
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
  setnilvalue(&L->stack[idx]);
}

static l_forceinline void setslotempty(lua_State* L, StkId idx)
{
  setempty(&L->stack[idx]);
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
  assert(ttisinteger(&L->stack[idx]));
  L->stack[idx].value_.prim = x;
}

static l_forceinline void chgslotflt(lua_State* L, StkId idx, lua_Number x)
{
  assert(ttisfloat(&L->stack[idx]));
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
  assert(novariant(x->tt) == LUA_TSTRING);
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
  return L->nCcalls & 0xffff;
}

static l_forceinline void incnny(lua_State* L)
{
  L->nCcalls += 0x10000;
}

static l_forceinline void decnny(lua_State* L)
{
  L->nCcalls -= 0x10000;
}

/* number of non-yieldable calls */
static l_forceinline l_uint32 getnny(const lua_State* L)
{
  return L->nCcalls >> 16;
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
  lua_Alloc* frealloc = nullptr; /* function to reallocate memory */
  int apilock = 0;
  l_mem GCtotalbytes = cost::Global; /* number of bytes currently allocated + debt */
  l_mem GCdebt = 0;       /* bytes counted but not yet allocated */
  l_mem GCmarked = 0;     /* number of objects marked in a GC cycle */
  l_mem GCmajorminor = 0; /* auxiliary counter to control major-minor shifts */
  struct StringTable
  {
    std::vector<TString*> buckets;
    size_t count = 0;
  };
  StringTable strt;
  TValue l_registry{Value{0, nullptr}, LUA_VNIL};
  TValue nilvalue{Value{0, nullptr}, LUA_VNIL};   /* a nil value */
  unsigned int seed = 0; /* randomized seed for hashes */
  unsigned int nexthash = 0; /* the hash of the next object created */
  int gcparams[LUA_GCPN] = {};
  lu_byte currentwhite = 0;
  lu_byte gcstate = 0;     /* state of garbage collector */
  lu_byte gckind = 0;      /* kind of GC running */
  lu_byte gcstopem = 0;    /* stops emergency collections */
  lu_byte gcstp = 0;       /* control whether GC is running */
  lu_byte gcemergency = 0; /* true if this is an emergency collection */
  GCObject allgc{};      /* list of all collectable objects */
  GCObject* sweepgc = nullptr;  /* current position of sweep in list */
  GCObject finobj{};     /* list of collectable objects with finalizers */
  GCListed gray{};       /* list of gray objects */
  GCListed grayagain{};  /* list of objects to be traversed atomically */
  GCListed weak{};       /* list of tables with weak values */
  GCListed ephemeron{};  /* list of ephemeron tables (weak keys) */
  GCListed allweak{};    /* list of all-weak tables */
  GCObject tobefnz{};    /* list of userdata to be GC */
  GCObject* fixedgc = nullptr;   /* list of objects not to be collected */
  /* fields for generational collector */
  GCObject* survival = nullptr;                        /* start of objects that survived one GC cycle */
  GCObject* old1 = nullptr;                            /* start of old1 objects */
  GCObject* reallyold = nullptr;                       /* objects more than one cycle old ("really old") */
  GCObject* firstold1 = nullptr;                       /* first OLD1 object in the list (if any) */
  GCObject* finobjsur = nullptr;                       /* list of survival objects with finalizers */
  GCObject* finobjold1 = nullptr;                      /* list of old1 objects with finalizers */
  GCObject* finobjrold = nullptr;                      /* list of really old objects with finalizers */
  lua_State* twups = nullptr;                   /* list of threads with open upvalues */
  lua_CFunction panic = nullptr;                       /* to be called in unprotected errors */
  TString* memerrmsg = nullptr;                        /* message for memory-allocation errors */
  TString* tmname[TM_N] = {};                     /* array with tag-method names */
  Table* mt[LUA_NUMTYPES] = {};            /* metatables for basic types */
  lua_WarnFunction* warnf = nullptr;                    /* warning function */
  /* the budget callback ('lua_setbudget'); the budget is in each group */
  lua_State* budgetL = nullptr;      /* thread running it, while it runs */
  BudgetGroup* budgetg = nullptr;    /* the group whose callback runs (kept by the collector) */
  lua_State* budgetth = nullptr;     /* a thread for the callback (see 'lua_budgetthread') */
  int budgetnfz = 0;                 /* its 'nfz' when the callback began */
  lua_State* freezetarget = nullptr; /* coroutine being frozen (unwinding) */
  bool freezekill = false;           /* ... or whose chain is being destroyed, */
  TValue killerr;                    /* with this error (no collection in between) */
  lua_State* finth = nullptr;        /* the thread that runs finalizers */
  InternalString locale_name;
  lua_State mainth;                          /* main thread of this state */
};

/*
** 'g->nilvalue' being a nil value flags that the state was completely
** build.
*/

/* actual number of total memory allocated */

void luaE_setdebt(global_State* g, l_mem debt);
void luaE_freethread(lua_State* L, lua_State* L1);
l_mem luaE_threadsize(lua_State* L);
CallInfo* luaE_extendCI(lua_State* L, int err);
void luaE_shrinkCI(lua_State* L);
static l_forceinline lua_State* mainthread(global_State* g)
{
  return &g->mainth;
}

static l_forceinline int completestate(global_State* g)
{
  return ttype(&g->nilvalue) == LUA_TNIL;
}

/* put 'bg' (or nothing) in effect for 'th', with the budget bit of its mask */
void luaE_setbeff(lua_State* th, BudgetGroup* bg);

/*
** At a resume of 'L' by 'from' (maybe none): the resumer's budget group is
** in effect if it has one, otherwise the thread's own.
*/
static l_forceinline void luaE_enterbudget(lua_State* L, lua_State* from)
{
  BudgetGroup* bg = (from != nullptr && from->beff != nullptr) ? from->beff : L->bown;
  if (L->beff != bg)
    luaE_setbeff(L, bg);
}

/* when the resume ends (not in a freeze): back to the thread's own */
static l_forceinline void luaE_leavebudget(lua_State* L)
{
  if (L->beff != L->bown)
    luaE_setbeff(L, L->bown);
}

/* spend 'n' steps of the budget, if there is one */
static l_forceinline void luaE_spend(lua_State* L, l_mem n)
{
  if (L->hookmask & LUA_MASKBUDGET)
    L->beff->left -= n;
}

/* C work of 'n' steps: true when the budget is spent (see 'lua_spend') */
static l_forceinline bool luaE_spent(lua_State* L, l_mem n)
{
  if (!(L->hookmask & LUA_MASKBUDGET))
    return false;
  BudgetGroup* bg = L->beff;
  bg->left -= n;
  return bg->left <= 0;
}

/* is the budget spent? */
static l_forceinline bool luaE_budgetout(lua_State* L)
{
  return (L->hookmask & LUA_MASKBUDGET) && L->beff->left <= 0;
}

/* see 'lua_freezeonly' */
static l_forceinline void luaE_freezeonly(lua_State* L)
{
  L->ci->callstatus |= CIST_FZONLY;
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

void luaE_checkcstack(lua_State* L);
void luaE_incCstack(lua_State* L);
void luaE_warning(lua_State* L, byte_view msg, int tocont);
void luaE_warnerror(lua_State* L, byte_view where);
TStatus luaE_resetthread(lua_State* L, TStatus status);
TStatus luaE_resetcallbackthread(lua_State* L);

}
