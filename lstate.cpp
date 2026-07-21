/*
** $Id: lstate.c $
** Global State
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <new>
#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"

static inline LX* fromstate(lua_State* L)
{
  return (LX*)L;
}

/*
** set GCdebt to a new value keeping the real number of allocated
** objects (GCtotalobjs - GCdebt) invariant and avoiding overflows in
** 'GCtotalobjs'.
*/
void luaE_setdebt(global_State* g, l_mem debt)
{
  l_mem tb = gettotalbytes(g);
  lua_assert(tb > 0);
  if (debt > MAX_LMEM - tb)
    debt = MAX_LMEM - tb; /* will make GCtotalbytes == MAX_LMEM */
  g->GCtotalbytes = tb + debt;
  g->GCdebt = debt;
}

CallInfo* luaE_extendCI(lua_State* L, int err)
{
  CallInfo* ci;
  ci = (((CallInfo*)(luaM_realloc_(L, NULL, ((size_t)(0)) * sizeof(CallInfo), ((size_t)(1)) * sizeof(CallInfo)))));
  if (l_unlikely(ci == NULL))
  { /* allocation failed? */
    if (err)
      luaD_throw(L, LUA_ERRMEM); /* raise the error */
    return NULL;     /* else only report it */
  }
  ci->next = L->ci->next;
  ci->previous = L->ci;
  L->ci->next = ci;
  if (ci->next)
    ci->next->previous = ci;
  ci->u.l.trap = 0;
  L->nci++;
  return ci;
}

/*
** free all CallInfo structures not in use by a thread
*/
static void freeCI(lua_State* L)
{
  CallInfo* ci = L->ci;
  CallInfo* next = ci->next;
  ci->next = NULL;
  while ((ci = next) != NULL)
  {
    next = ci->next;
    luaM_free_(L, (ci), sizeof(*(ci)));
    L->nci--;
  }
}

/*
** free half of the CallInfo structures not in use by a thread,
** keeping the first one.
*/
void luaE_shrinkCI(lua_State* L)
{
  CallInfo* ci = L->ci->next; /* first free CallInfo */
  CallInfo* next;
  if (ci == NULL)
    return; /* no extra elements */
  while ((next = ci->next) != NULL)
  {                               /* two extra elements? */
    CallInfo* next2 = next->next; /* next's next */
    ci->next = next2;             /* remove next from the list */
    L->nci--;
    luaM_free_(L, (next), sizeof(*(next))); /* free next */
    if (next2 == NULL)
      break; /* no more elements */
    else
    {
      next2->previous = ci;
      ci = next2; /* continue */
    }
  }
}

/*
** Called when 'getCcalls(L)' larger or equal to LUAI_MAXCCALLS.
** If equal, raises an overflow error. If value is larger than
** LUAI_MAXCCALLS (which means it is handling an overflow) but
** not much larger, does not report an error (to allow overflow
** handling to work).
*/
void luaE_checkcstack(lua_State* L)
{
  if (getCcalls(L) == LUAI_MAXCCALLS)
    luaG_runerror(L, "C stack overflow");
  else if (getCcalls(L) >= (LUAI_MAXCCALLS / 10 * 11))
    luaD_errerr(L); /* error while handling stack error */
}

LUAI_FUNC void luaE_incCstack(lua_State* L)
{
  L->nCcalls++;
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS))
    luaE_checkcstack(L);
}

static void resetCI(lua_State* L)
{
  CallInfo* ci = L->ci = &L->base_ci;
  ci->func = 0;
  (((&(L)->stack.p[ci->func]))->tt_ = LUA_VNIL);                 /* 'function' entry for basic 'ci' */
  ci->top = ci->func + 1 + LUA_MINSTACK; /* +1 for 'function' entry */
  ci->u.c.k = NULL;
  ci->callstatus = CIST_C;
  L->status = LUA_OK;
  L->errfunc = 0; /* stack unwind can "throw away" the error function */
}

static void stack_init(lua_State* L1, lua_State* L)
{
  int i;
  /* initialize stack array */
  L1->stack.p = ((StackValue*)(luaM_malloc_(L, ((size_t)(BASIC_STACK_SIZE + EXTRA_STACK)) * sizeof(StackValue), 0)));
  L1->tbclist = 0;
  for (i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
    (((&(L1)->stack.p[i]))->tt_ = LUA_VNIL); /* erase new stack */
  L1->stack_last = BASIC_STACK_SIZE;
  /* initialize first ci */
  resetCI(L1);
  L1->top = 1; /* +1 for 'function' entry */
}

static void freestack(lua_State* L)
{
  luaM_free_(L, (L->tbc), (((size_t)(L->sizetbc))) * sizeof(*(L->tbc))); /* free tbc list */
  L->tbc = NULL;
  L->ntbc = L->sizetbc = 0;
  if (L->stack.p == NULL)
    return;            /* stack not completely built yet */
  L->ci = &L->base_ci; /* free the entire 'ci' list */
  freeCI(L);
  lua_assert(L->nci == 0);
  /* free stack */
  luaM_free_(L, (L->stack.p), (((size_t)(stacksize(L) + EXTRA_STACK))) * sizeof(*(L->stack.p)));
}

/*
** Create registry table and its predefined values
*/
static void init_registry(lua_State* L, global_State* g)
{
  /* create registry */
  TValue aux;
  Table* registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);
  /* registry[1] = false */
  ((&aux)->tt_ = LUA_VFALSE);
  luaH_setint(L, registry, 1, &aux);
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  setthvalue(L, &aux, L);
  luaH_setint(L, registry, LUA_RIDX_MAINTHREAD, &aux);
  /* registry[LUA_RIDX_GLOBALS] = new table (table of globals) */
  sethvalue(L, &aux, luaH_new(L));
  luaH_setint(L, registry, LUA_RIDX_GLOBALS, &aux);
}

/*
** open parts of the state that may cause memory-allocation errors.
*/
static void f_luaopen(lua_State* L, void*)
{
  global_State* g = G(L);
  stack_init(L, L); /* init stack */
  init_registry(L, g);
  luaS_init(L);
  luaT_init(L);
  luaX_init(L);
  g->gcstp = 0;              /* allow gc */
  ((&g->nilvalue)->tt_ = LUA_VNIL); /* now state is complete */
  luai_userstateopen(L);
}

/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
static void preinit_thread(lua_State* L, global_State* g)
{
  L->l_G = g;
  L->stack.p = NULL;
  L->tbclist = 0;
  L->tbc = NULL;
  L->ntbc = 0;
  L->sizetbc = 0;
  L->ci = NULL;
  L->nci = 0;
  L->twups = L; /* thread has no upvalues */
  L->nCcalls = 0;
  L->errorJmp = NULL;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->status = LUA_OK;
  L->errfunc = 0;
  L->oldpc = 0;
  L->base_ci.previous = L->base_ci.next = NULL;
}

lu_mem luaE_threadsize(lua_State* L)
{
  lu_mem sz = ((lu_mem)(sizeof(LX))) + ((unsigned int)(L->nci)) * sizeof(CallInfo);
  if (L->stack.p != NULL)
    sz += ((unsigned int)(stacksize(L) + EXTRA_STACK)) * sizeof(StackValue);
  sz += ((unsigned int)(L->sizetbc)) * sizeof(StkId);
  return sz;
}

static void close_state(lua_State* L)
{
  global_State* g = G(L);
  if (!completestate(g))    /* closing a partially built state? */
    luaC_freeallobjects(L); /* just collect its objects */
  else
  { /* closing a fully built state */
    resetCI(L);
    luaD_closeprotected(L, 1, LUA_OK); /* close all upvalues */
    L->top = 1;         /* empty the stack to run finalizers */
    luaC_freeallobjects(L);            /* collect all objects */
    luai_userstateclose(L);
  }
  G(L)->strt.~unordered_map();
  freestack(L);
  lua_assert(gettotalbytes(g) == sizeof(global_State));
  (*g->frealloc)(g->ud, g, sizeof(global_State), 0); /* free main block */
}

LUA_API lua_State* lua_newthread(lua_State* L)
{
  global_State* g = G(L);
  GCObject* o;
  lua_State* L1;
  lua_lock(L);
  luaC_checkGC(L);
  /* create new thread */
  o = luaC_newobjdt(L, LUA_TTHREAD, sizeof(LX));
  L1 = (lua_assert((o)->tt == LUA_VTHREAD), static_cast<lua_State*>(o));
  /* anchor it on L stack */
  setthvalue(L, (&(L)->stack.p[L->top]), L1);
  api_incr_top(L);
  preinit_thread(L1, g);
  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  /* initialize L1 extra space */
  *((void**)(lua_getextraspace(L1))) = lua_getextraspace(mainthread(g));
  luai_userstatethread(L, L1);
  stack_init(L1, L); /* init stack */
  lua_unlock(L);
  return L1;
}

void luaE_freethread(lua_State* L, lua_State* L1)
{
  LX* l = fromstate(L1);
  luaF_closeupval(L1, 0); /* close all upvalues */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free_(L, (l), sizeof(*(l)));
}

TStatus luaE_resetthread(lua_State* L, TStatus status)
{
  resetCI(L);
  if (status == LUA_YIELD)
    status = LUA_OK;
  status = luaD_closeprotected(L, 1, status);
  if (status != LUA_OK) /* errors? */
    luaD_seterrorobj(L, status, 1);
  else
    L->top = 1;
  luaD_reallocstack(L, ((int)(L->ci->top)), 0);
  return status;
}

LUA_API int lua_closethread(lua_State* L, lua_State* from)
{
  TStatus status;
  lua_lock(L);
  L->nCcalls = (from) ? getCcalls(from) : 0;
  status = luaE_resetthread(L, L->status);
  if (L == from) /* closing itself? */
    luaD_throwbaselevel(L, status);
  lua_unlock(L);
  return ((int)(status));
}

LUA_API lua_State* lua_newstate(lua_Alloc f, void* ud, unsigned seed)
{
  int i;
  lua_State* L;
  global_State* g = ((global_State*)((*f)(ud, NULL, LUA_TTHREAD, sizeof(global_State))));
  if (g == NULL)
    return NULL;
  L = &g->mainth.l;
  g->mainth.extra = NULL;  /* initialize to 0 */
  L->tt = LUA_VTHREAD;
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked = luaC_white(g);
  preinit_thread(L, g);
  g->allgc = (lua_assert((((L)->tt) & NOVARIANT_MASK) >= LUA_TSTRING), const_cast<GCObject*>(static_cast<const GCObject*>(L))); /* by now, only object is the main thread */
  L->next = NULL;
  incnny(L); /* main thread is always non yieldable */
  g->frealloc = f;
  g->ud = ud;
  g->warnf = NULL;
  g->ud_warn = NULL;
  g->seed = seed;
  g->gcstp = GCSTPGC; /* no GC while building state */
  new (&g->strt) decltype(g->strt)();
  ((&g->l_registry)->tt_ = LUA_VNIL);
  g->panic = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_INC;
  g->gcstopem = 0;
  g->gcemergency = 0;
  g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->firstold1 = g->survival = g->old1 = g->reallyold = NULL;
  g->finobjsur = g->finobjold1 = g->finobjrold = NULL;
  g->sweepgc = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->twups = NULL;
  g->GCtotalbytes = sizeof(global_State);
  g->GCmarked = 0;
  g->GCdebt = 0;
  g->memlimit = 0;
  setivalue(&g->nilvalue, 0); /* to signal that state is not yet built */
  ((g)->gcparams[LUA_GCPPAUSE] = luaO_codeparam(LUAI_GCPAUSE));
  ((g)->gcparams[LUA_GCPSTEPMUL] = luaO_codeparam(LUAI_GCMUL));
  ((g)->gcparams[LUA_GCPSTEPSIZE] = luaO_codeparam(LUAI_GCSTEPSIZE));
  ((g)->gcparams[LUA_GCPMINORMUL] = luaO_codeparam(LUAI_GENMINORMUL));
  ((g)->gcparams[LUA_GCPMINORMAJOR] = luaO_codeparam(LUAI_MINORMAJOR));
  ((g)->gcparams[LUA_GCPMAJORMINOR] = luaO_codeparam(LUAI_MAJORMINOR));
  for (i = 0; i < LUA_NUMTYPES; i++)
    g->mt[i] = NULL;
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK)
  {
    /* memory allocation error: free partial state */
    close_state(L);
    L = NULL;
  }
  return L;
}

LUA_API void lua_close(lua_State* L)
{
  lua_lock(L);
  L = mainthread(G(L)); /* only the main thread can be closed */
  close_state(L);
}

void luaE_warning(lua_State* L, const char* msg, int tocont)
{
  lua_WarnFunction wf = G(L)->warnf;
  if (wf != NULL)
    wf(G(L)->ud_warn, msg, tocont);
}

/*
** Generate a warning from an error message
*/
void luaE_warnerror(lua_State* L, const char* where)
{
  TValue* errobj = (&(L)->stack.p[L->top - 1]); /* error object */
  const char* msg = ((((errobj)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
                        ? getstr((lua_assert(((errobj)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((errobj)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((errobj)->value_).gc))))
                        : "error object is not a string";
  /* produce warning "error in %s (%s)" (where, msg) */
  luaE_warning(L, "error in ", 1);
  luaE_warning(L, where, 1);
  luaE_warning(L, " (", 1);
  luaE_warning(L, msg, 1);
  luaE_warning(L, ")", 0);
}
