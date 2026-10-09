/*
** $Id: lstate.c $
** Global State
** See Copyright Notice in lua.h
*/


#include <new>
#include <stddef.h>
#include <stdlib.h>
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
#include "literals.h"

namespace lua::runtime
{

/*
** set GCdebt to a new value keeping the real number of allocated
** objects (GCtotalobjs - GCdebt) invariant and avoiding overflows in
** 'GCtotalobjs'.
*/
void luaE_setdebt(global_State* g, l_mem debt)
{
  l_mem tb = gettotalbytes(g);
  assert(tb > 0);
  if (debt > MAX_LMEM - tb)
    debt = MAX_LMEM - tb; /* will make GCtotalbytes == MAX_LMEM */
  g->GCtotalbytes = tb + debt;
  g->GCdebt = debt;
}

CallInfo* luaE_extendCI(lua_State* L, int err)
{
  CallInfo* ci;
  ci = luaM_alloc_CallInfo(L);
  if (l_unlikely(ci == nullptr))
  { /* allocation failed? */
    if (err)
      luaD_throw(L, LUA_ERRMEM); /* raise the error */
    return nullptr;     /* else only report it */
  }
  ci->next = L->ci->next;
  ci->previous = L->ci;
  L->ci->next = ci;
  if (ci->next)
    ci->next->previous = ci;
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
  ci->next = nullptr;
  while ((ci = next) != nullptr)
  {
    next = ci->next;
    luaM_free_CallInfo(L, ci);
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
  if (ci == nullptr)
    return; /* no extra elements */
  while ((next = ci->next) != nullptr)
  {                               /* two extra elements? */
    CallInfo* next2 = next->next; /* next's next */
    ci->next = next2;             /* remove next from the list */
    L->nci--;
    luaM_free_CallInfo(L, next); /* free next */
    if (next2 == nullptr)
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
    luaG_runerror(L, lt::C_stack_overflow);
  else if (getCcalls(L) >= (LUAI_MAXCCALLS / 10 * 11))
    luaD_errerr(L); /* error while handling stack error */
}

void luaE_incCstack(lua_State* L)
{
  L->nCcalls++;
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS))
    luaE_checkcstack(L);
}

static void resetCI(lua_State* L)
{
  CallInfo* ci = L->ci = &L->base_ci;
  ci->func = 0;
  setslottag(L, ci->func, LUA_VNIL);                 /* 'function' entry for basic 'ci' */
  ci->top = ci->func + 1 + LUA_MINSTACK; /* +1 for 'function' entry */
  ci->k = nullptr;
  ci->callstatus = CIST_C;
  L->status = LUA_OK;
  L->errfunc = 0; /* stack unwind can "throw away" the error function */
}

static void stack_init(lua_State* L1, lua_State* L)
{
  int i;
  size_t nslots = BASIC_STACK_SIZE + EXTRA_STACK;
  std::span<TValue> stk = luaM_alloc_TValue_span(L->l_G, L, {}, nslots, false);
  if (l_unlikely(stk.data() == nullptr))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  L1->stack = stk;
  L1->tbclist = 0;
  for (i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
    setslottag(L1, i, LUA_VNIL); /* erase new stack */
  L1->stack_last = BASIC_STACK_SIZE;
  /* initialize first ci */
  resetCI(L1);
  L1->top = 1; /* +1 for 'function' entry */
}

static void freestack(lua_State* L)
{
  luaM_alloc_StkId_span(L->l_G, L, L->tbc, 0, false);
  L->tbc = {};
  L->ntbc = 0;
  if (L->stack.data() == nullptr)
    return;            /* stack not completely built yet */
  L->ci = &L->base_ci; /* free the entire 'ci' list */
  freeCI(L);
  assert(L->nci == 0);
  {
    luaM_alloc_TValue_span(L->l_G, L, L->stack, 0, false);
  }
}

/*
** Create registry table and its predefined values
*/
static void init_registry(lua_State* L, global_State* g)
{
  /* create registry */
  TValue aux;
  Table* registry = Table::luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
  registry->luaH_resize(L, LUA_RIDX_LAST, 0);
  /* registry[1] = false */
  aux.tt_ = LUA_VFALSE;
  registry->luaH_setint(L, 1, &aux);
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  setthvalue(L, &aux, L);
  registry->luaH_setint(L, LUA_RIDX_MAINTHREAD, &aux);
  /* registry[LUA_RIDX_GLOBALS] = new table (table of globals) */
  sethvalue(L, &aux, Table::luaH_new(L));
  registry->luaH_setint(L, LUA_RIDX_GLOBALS, &aux);
}

/*
** open parts of the state that may cause memory-allocation errors.
*/
static void preinit_thread(lua_State* L, global_State* g);

static void f_luaopen(lua_State* L)
{
  global_State* g = L->l_G;
  stack_init(L, L); /* init stack */
  init_registry(L, g);
  luaS_init(L);
  luaT_init(L);
  luaX_init(L);
  g->gcstp = 0;              /* allow gc */
  setnilvalue(&g->nilvalue); /* now state is complete */
  luai_userstateopen(L);
}

struct LuaOpen : ProtectedCall
{
  void run(lua_State* L) override { f_luaopen(L); }
};

/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
static void preinit_thread(lua_State* L, global_State* g)
{
  L->l_G = g;
}

l_mem luaE_threadsize(lua_State* L)
{
  l_mem sz = cost::Thread + L->nci * cost::CallInfo;
  if (L->stack.data() != nullptr)
    sz += (stacksize(L) + EXTRA_STACK) * cost::TValue;
  sz += L->tbc.size() * cost::Ref;
  return sz;
}

static void close_state(lua_State* L)
{
  global_State* g = L->l_G;
  if (L->beff != nullptr)
    luaE_setbeff(L, nullptr); /* closing: no budget points */
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
  g->groupbyid = luaM_alloc_BudgetGroup_ptr_span(g, L, g->groupbyid, 0, false);
  freestack(L);
  assert(gettotalbytes(g) == cost::Global);
  g->frealloc->permit(cost::Global, 0, LUA_TTHREAD); /* free main block */
  delete g;
}

}

lua_State* lua_newthread(lua_State* L)
{
  global_State* g = L->l_G;
  lua_State* L1;
  lua_lock(L);
  luaC_checkGC(L);
  /* create new thread */
  L1 = luaM_malloc_lua_State(L);
  luaC_link(L, L1, LUA_TTHREAD);
  /* anchor it on L stack */
  setslotth(L, L->top, L1);
  api_incr_top(L);
  preinit_thread(L1, g);
  L1->hookmask = (lu_byte)(L->hookmask & ~LUA_MASKBUDGET); /* (its budget is set at a resume) */
  L1->bown = L->beff; /* it belongs to the budget group in effect */
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  /* initialize L1 extra space */
  L1->extra = mainthread(g)->extra;
  luai_userstatethread(L, L1);
  stack_init(L1, L); /* init stack */
  lua_unlock(L);
  return L1;
}

namespace lua::runtime
{

/* put 'bg' (or nothing) in effect for 'th', with the budget bit of its mask */
void luaE_setbeff(lua_State* th, BudgetGroup* bg)
{
  th->beff = bg;
  if (bg != nullptr)
  {
    th->hookmask |= LUA_MASKBUDGET;
    for (CallInfo* ci = th->ci; ci != nullptr; ci = ci->previous)
      if (isLua(ci))
        ci->trap = 1; /* to count inside 'luaV_execute' */
  }
  else
    th->hookmask &= ~LUA_MASKBUDGET;
}

void luaE_freethread(lua_State* L, lua_State* L1)
{
  /* (a group's reference to the thread it was set on is weak: the collector
     clears it when the thread is dead, see 'atomic') */
  luaF_closeupval(L1, 0); /* close all upvalues */
  assert(L1->openupval == nullptr);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free_lua_State(L, L1);
}

/*
** The start of a close of the variables of a thread: it goes back to the
** base, with no status, so that a '__close' can run. Returns the status
** to close with.
*/
TStatus luaE_beginclose(lua_State* L)
{
  TStatus status = L->status;
  resetCI(L);
  return (status == LUA_YIELD) ? LUA_OK : status;
}

/* its end: the error is at 1 (or there is none) */
void luaE_endclose(lua_State* L, TStatus status)
{
  if (status != LUA_OK) /* errors? */
    luaD_seterrorobj(L, status, 1);
  else
    L->top = 1;
  luaD_reallocstack(L, (int)L->ci->top, 0);
}

TStatus luaE_resetthread(lua_State* L, TStatus status)
{
  resetCI(L);
  if (status == LUA_YIELD)
    status = LUA_OK;
  status = luaD_closeprotected(L, 1, status);
  luaE_endclose(L, status);
  return status;
}


/*
** After the budget callback left, with a pending exit, the thread it ran
** on: close its pending variables (with no error) and go back to the base,
** with no change to its stack, which is reserved. The error of a
** '__close', if any, is at 1.
*/
TStatus luaE_resetcallbackthread(lua_State* L)
{
  L->errorJmp = nullptr;
  L->nfz = 0;
  resetCI(L);
  TStatus status = luaD_closeprotected(L, 1, LUA_OK);
  if (status != LUA_OK)
    luaD_seterrorobj(L, status, 1);
  else
    L->top = 1;
  return status;
}

}

int lua_closethread(lua_State* L, lua_State* from)
{
  TStatus status;
  lua_lock(L);
  if (L->frozen)
  { /* a frozen thread is read-only: it only thaws */
    setslots(L, L->top, TString::luaS_createstr(L, lt::cannot_close_a_frozen_coroutine));
    L->top++;
    lua_unlock(L);
    return LUA_ERRRUN;
  }
  L->nCcalls = from ? getCcalls(from) : 0;
  /* the '__close' of the thread run as a call that cannot freeze, in the
     chain of the thread that closes it (see 'luaD_closeprotected') */
  struct Plain
  {
    lua_State* L;
    bool on;
    ~Plain()
    {
      if (on)
      {
        L->nfz--;
        L->resumer = nullptr;
        luaE_leavebudget(L);
      }
    }
  } plain{L, L != from};
  if (plain.on)
  {
    L->resumer = from;
    L->resumedk = 0;
    L->nfz++;
    luaE_enterbudget(L, from);
  }
  status = luaE_resetthread(L, L->status);
  if (L == from) /* closing itself? */
    luaD_throwbaselevel(L, status);
  lua_unlock(L);
  return status;
}


lua_State* lua_newstate(lua_Alloc* f, unsigned seed)
{
  lua_State* L;
  if (!f->permit(0, cost::Global, LUA_TTHREAD))
    return nullptr;
  global_State* g = new global_State();
  if (g == nullptr)
    return nullptr;
  L = &g->mainth;
  L->tt = LUA_VTHREAD;
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked = luaC_white(g);
  preinit_thread(L, g);
  assert(novariant(L->tt) >= LUA_TSTRING);
  g->allgc.next = L; /* by now, only object is the main thread */
  L->next = nullptr;
  incnny(L); /* main thread is always non yieldable */
  g->frealloc = f;
  g->seed = seed;
  g->nexthash = seed;
  L->hash = luaC_newhash(g);
  g->gcstp = GCSTPGC; /* no GC while building state */
  g->gcstate = GCSpause;
  g->gckind = KGC_INC;
  setivalue(&g->nilvalue, 0); /* to signal that state is not yet built */
  g->gcparams[LUA_GCPPAUSE] = luaO_codeparam(LUAI_GCPAUSE);
  g->gcparams[LUA_GCPSTEPMUL] = luaO_codeparam(LUAI_GCMUL);
  g->gcparams[LUA_GCPSTEPSIZE] = luaO_codeparam(LUAI_GCSTEPSIZE);
  g->gcparams[LUA_GCPMINORMUL] = luaO_codeparam(LUAI_GENMINORMUL);
  g->gcparams[LUA_GCPMINORMAJOR] = luaO_codeparam(LUAI_MINORMAJOR);
  g->gcparams[LUA_GCPMAJORMINOR] = luaO_codeparam(LUAI_MAJORMINOR);
  LuaOpen open;
  if (luaD_rawrunprotected(L, &open) != LUA_OK)
  {
    /* memory allocation error: free partial state */
    close_state(L);
    L = nullptr;
  }
  return L;
}


void lua_close(lua_State* L)
{
  lua_lock(L);
  L = mainthread(L->l_G); /* only the main thread can be closed */
  close_state(L);
}

namespace lua::runtime
{

void luaE_warning(lua_State* L, byte_view msg, int tocont)
{
  lua_WarnFunction* wf = L->l_G->warnf;
  if (wf != nullptr)
  {
    lua_unlock(L);
    wf->warn(L, HostString{msg}, tocont);
    lua_lock(L);
  }
}

/*
** Generate a warning from an error message
*/
void luaE_warnerror(lua_State* L, byte_view where)
{
  StkId errobj = L->top - 1; /* error object */
  byte_view msg = (novariant(slottag(L, errobj)) == LUA_TSTRING)
                             ? slottsval(L, errobj)->data.view()
                             : lt::error_object_is_not_a_string;
  /* produce warning "error in %s (%s)" (where, msg) */
  luaE_warning(L, lt::error_in, 1);
  luaE_warning(L, where, 1);
  luaE_warning(L, lt::sp_lparen, 1);
  luaE_warning(L, msg, 1);
  luaE_warning(L, lt::rparen, 0);
}

}
