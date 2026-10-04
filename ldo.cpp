/*
** $Id: ldo.c $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"

#include "lhack.h"
#include "lauxlib.h"
#include "literals.h"

constexpr int errorstatus(TStatus s)
{
  return s > LUA_YIELD;
}

/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/* chained list of long jump buffers */
struct lua_longjmp
{
  struct lua_longjmp* previous;
  int level;
  volatile TStatus status; /* error code */
};

struct LuaError
{
  TStatus status;
  int level;
};

static void LUAI_TRY(lua_State* L, lua_longjmp* c, ProtectedCall* f)
{
  try
  {
    f->run(L); /* call function protected */
  }
  catch (LuaError& e)
  {              /* Lua error */
    if (e.level != c->level) /* not the correct level? */
      throw;     /* rethrow to upper level */
    c->status = e.status;
  }
  catch (...)
  {                 /* non-Lua exception */
    c->status = -1; /* create some error code */
  }
}

void luaD_seterrorobj(lua_State* L, TStatus errcode, StkId oldtop)
{
  if (errcode == LUA_ERRMEM)
  {                                          /* memory error? */
    setslots(L, oldtop, L->l_G->memerrmsg); /* reuse preregistered msg. */
  }
  else
  {
    assert(errorstatus(errcode));        /* must be a real error */
    assert((slottag(L, L->top - 1) & NOVARIANT_MASK) != LUA_TNIL); /* with a non-nil object */
    copyslot(L, oldtop, L->top - 1);      /* move it to 'oldtop' */
  }
  L->top = oldtop + 1; /* top goes back to old top plus error object */
}

l_noret luaD_throw(lua_State* L, TStatus errcode)
{
  if (L->errorJmp)
  {                       /* thread has an error handler? */
    throw LuaError{errcode, L->errorJmp->level}; /* jump to it */
  }
  else
  { /* thread has no error handler */
    global_State* g = L->l_G;
    lua_State* mainth = mainthread(g);
    errcode = luaE_resetthread(L, errcode); /* close all upvalues */
    L->status = errcode;
    if (mainth->errorJmp)
    {                                              /* main thread has a handler? */
      TValue v = getslot(L, L->top - 1);
      setslot(mainth, mainth->top++, &v);
      luaD_throw(mainth, errcode);                 /* re-throw in main thread */
    }
    else
    { /* no handler at all; abort */
      if (g->panic)
      { /* panic function? */
        lua_unlock(L);
        g->panic(L); /* call panic function (last chance to jump out) */
      }
      abort();
    }
  }
}

l_noret luaD_throwbaselevel(lua_State* L, TStatus errcode)
{
  if (L->errorJmp)
  {
    /* unroll error entries up to the first level */
    while (L->errorJmp->previous != nullptr)
      L->errorJmp = L->errorJmp->previous;
  }
  luaD_throw(L, errcode);
}

TStatus luaD_rawrunprotected(lua_State* L, ProtectedCall* f)
{
  l_uint32 oldnCcalls = L->nCcalls;
  lua_longjmp lj;
  lj.status = LUA_OK;
  lj.previous = L->errorJmp; /* chain new error handler */
  lj.level = (lj.previous != nullptr) ? lj.previous->level + 1 : 1;
  L->errorJmp = &lj;
  LUAI_TRY(L, &lj, f);   /* call 'f' catching errors */
  L->errorJmp = lj.previous; /* restore old error handler */
  L->nCcalls = oldnCcalls;
  return lj.status;
}

/* }====================================================== */

/*
** {==================================================================
** Stack reallocation
** ===================================================================
*/

/* some stack space for error handling */
constexpr int STACKERRSPACE = 200;

/* maximum stack size that respects size_t */
constexpr size_t MAXSTACK_BYSIZET = ((MAX_SIZET / sizeof(lua_Integer)) - STACKERRSPACE);

/*
** Minimum between LUAI_MAXSTACK and MAXSTACK_BYSIZET
** (Maximum size for the stack must respect size_t.)
*/
constexpr int MAXSTACK = (LUAI_MAXSTACK < MAXSTACK_BYSIZET ? LUAI_MAXSTACK : MAXSTACK_BYSIZET);

/* stack size with extra space for error handling */
constexpr int ERRORSTACKSIZE = (MAXSTACK + STACKERRSPACE);

/* raise a stack error while running the message handler */
l_noret luaD_errerr(lua_State* L)
{
  byte_view errstr = lt::error_in_error_handling;
  TString* msg = TString::luaS_createstr(L, errstr);
  setslots(L, L->top, msg);
  L->top++; /* assume EXTRA_STACK */
  luaD_throw(L, LUA_ERRERR);
}

/*
** Check whether stacks have enough space to run a simple function (such
** as a finalizer): At least BASIC_STACK_SIZE in the Lua stack, two
** available CallInfos, and two "slots" in the C stack.
*/
int luaD_checkminstack(lua_State* L)
{
  if (getCcalls(L) >= LUAI_MAXCCALLS - 2)
    return 0; /* not enough C-stack slots */
  if (L->ci->next == nullptr && luaE_extendCI(L, 0) == nullptr)
    return 0; /* unable to allocate first ci */
  if (L->ci->next->next == nullptr && luaE_extendCI(L, 0) == nullptr)
    return 0; /* unable to allocate second ci */
  if (L->top + BASIC_STACK_SIZE <= L->stack_last)
    return 1; /* enough (BASIC_STACK_SIZE) free slots in the Lua stack */
  else        /* try to grow stack to a size with enough free slots */
    return luaD_growstack(L, BASIC_STACK_SIZE, 0);
}

static void correctstack(lua_State* L)
{
  for (CallInfo* ci = L->ci; ci != nullptr; ci = ci->previous)
  {
    if (isLua(ci))
      ci->trap = 1; /* signal to update 'trap' in 'luaV_execute' */
  }
}

int luaD_reallocstack(lua_State* L, int newsize, int raiseerror)
{
  int oldsize = stacksize(L);
  int i;
  size_t oldn = (oldsize + EXTRA_STACK);
  size_t newn = (newsize + EXTRA_STACK);
  size_t keep = (oldn < newn) ? oldn : newn;
  lu_byte oldgcstop = L->l_G->gcstopem;
  assert(newsize <= MAXSTACK || newsize == ERRORSTACKSIZE);
  L->l_G->gcstopem = 1; /* stop emergency collection */
  std::span<TValue> newstk = luaM_alloc_TValue_span(L->l_G, L, {}, newn, false);
  L->l_G->gcstopem = oldgcstop; /* restore emergency collection */
  if (l_unlikely(newstk.data() == nullptr))
  {
    correctstack(L);
    if (raiseerror)
      luaD_throw(L, LUA_ERRMEM);
    else
      return 0; /* do not raise an error */
  }
  for (size_t si = 0; si < keep; si++)
    newstk[si] = L->stack[si];
  luaM_alloc_TValue_span(L->l_G, L, L->stack, 0, false);
  L->stack = newstk;
  correctstack(L);
  L->stack_last = newsize;
  for (i = oldsize + EXTRA_STACK; i < newsize + EXTRA_STACK; i++)
    setslottag(L, i, LUA_VNIL); /* erase new segment */
  return 1;
}

/*
** Try to grow the stack by at least 'n' elements. When 'raiseerror'
** is true, raises any error; otherwise, return 0 in case of errors.
*/
int luaD_growstack(lua_State* L, int n, int raiseerror)
{
  int size = stacksize(L);
  if (l_unlikely(size > MAXSTACK))
  {
    /* if stack is larger than maximum, thread is already using the
       extra space reserved for errors, that is, thread is handling
       a stack error; cannot grow further than that. */
    assert(stacksize(L) == ERRORSTACKSIZE);
    if (raiseerror)
      luaD_errerr(L); /* stack error inside message handler */
    return 0;         /* if not 'raiseerror', just signal it */
  }
  else if (n < MAXSTACK)
  {                                   /* avoids arithmetic overflows */
    int newsize = size + (size >> 1); /* tentative new size (size * 1.5) */
    int needed = L->top + n;
    if (newsize > MAXSTACK) /* cannot cross the limit */
      newsize = MAXSTACK;
    if (newsize < needed) /* but must respect what was asked for */
      newsize = needed;
    if (l_likely(newsize <= MAXSTACK))
      return luaD_reallocstack(L, newsize, raiseerror);
  }
  /* else stack overflow */
  /* add extra size to be able to handle the error message */
  luaD_reallocstack(L, ERRORSTACKSIZE, raiseerror);
  if (raiseerror)
    luaG_runerror(L, lt::stack_overflow);
  return 0;
}

/*
** Compute how much of the stack is being used, by computing the
** maximum top of all call frames in the stack and the current top.
*/
static int stackinuse(lua_State* L)
{
  CallInfo* ci;
  int res;
  StkId lim = L->top;
  for (ci = L->ci; ci != nullptr; ci = ci->previous)
  {
    if (lim < ci->top)
      lim = ci->top;
  }
  assert(lim <= L->stack_last + EXTRA_STACK);
  res = ((int)(lim)) + 1; /* part of stack in use */
  if (res < LUA_MINSTACK)
    res = LUA_MINSTACK; /* ensure a minimum size */
  return res;
}

/*
** If stack size is more than 3 times the current use, reduce that size
** to twice the current use. (So, the final stack size is at most 2/3 the
** previous size, and half of its entries are empty.)
** As a particular case, if stack was handling a stack overflow and now
** it is not, 'max' (limited by MAXSTACK) will be smaller than
** stacksize (equal to ERRORSTACKSIZE in this case), and so the stack
** will be reduced to a "regular" size.
*/
void luaD_shrinkstack(lua_State* L)
{
  int inuse = stackinuse(L);
  int max = (inuse > MAXSTACK / 3) ? MAXSTACK : inuse * 3;
  /* if thread is currently not handling a stack overflow and its
     size is larger than maximum "reasonable" size, shrink it */
  if (inuse <= MAXSTACK && stacksize(L) > max)
  {
    int nsize = (inuse > MAXSTACK / 2) ? MAXSTACK : inuse * 2;
    luaD_reallocstack(L, nsize, 0); /* ok if that fails */
  }
  luaE_shrinkCI(L); /* shrink CI list */
}

void luaD_inctop(lua_State* L)
{
  L->top++;
  luaD_checkstack(L, 1);
}

/* }================================================================== */

/*
** Call a hook for the given event. Make sure there is a hook to be
** called. (Both 'L->hook' and 'L->hookmask', which trigger this
** function, can be changed asynchronously by signals.)
*/
void luaD_hook(lua_State* L, int event, int line,
               int ftransfer, int ntransfer)
{
  lua_Hook hook = L->hook;
  if (hook && L->allowhook)
  { /* make sure there is a hook */
    CallInfo* ci = L->ci;
    StkId top = L->top;     /* preserve original 'top' */
    StkId ci_top = ci->top; /* idem for 'ci->top' */
    lua_Debug ar;
    ar.event = event;
    ar.currentline = line;
    ar.i_ci = ci;
    L->transferinfo.ftransfer = ftransfer;
    L->transferinfo.ntransfer = ntransfer;
    if (isLua(ci) && L->top < ci->top)
      L->top = ci->top;           /* protect entire activation register */
    luaD_checkstack(L, LUA_MINSTACK); /* ensure minimum stack size */
    if (ci->top < L->top + LUA_MINSTACK)
      ci->top = L->top + LUA_MINSTACK;
    L->allowhook = 0; /* cannot call hooks inside a hook */
    ci->callstatus |= CIST_HOOKED;
    lua_unlock(L);
    (*hook)(L, &ar);
    lua_lock(L);
    assert(!L->allowhook);
    L->allowhook = 1;
    ci->top = ci_top;
    L->top = top;
    ci->callstatus &= ~CIST_HOOKED;
  }
}

/*
** Executes a call hook for Lua functions. This function is called
** whenever 'hookmask' is not zero, so it checks whether call hooks are
** active.
*/
void luaD_hookcall(lua_State* L, CallInfo* ci)
{
  L->oldpc = 0; /* set 'oldpc' for new function */
  if (L->hookmask & LUA_MASKCALL)
  { /* is call hook on? */
    int event = (ci->callstatus & CIST_TAIL) ? LUA_HOOKTAILCALL
                                             : LUA_HOOKCALL;
    Proto* p = ci_func(L, ci)->p;
    ci->savedpc++; /* hooks assume 'pc' is already incremented */
    luaD_hook(L, event, -1, 1, p->numparams);
    ci->savedpc--; /* correct 'pc' */
  }
}

/*
** Executes a return hook for Lua and C functions and sets/corrects
** 'oldpc'. (Note that this correction is needed by the line hook, so it
** is done even when return hooks are off.)
*/
static void rethook(lua_State* L, CallInfo* ci, int nres)
{
  if (L->hookmask & LUA_MASKRET)
  {                                   /* is return hook on? */
    StkId firstres = L->top - nres; /* index of first result */
    int delta = 0;                    /* correction for vararg functions */
    int ftransfer;
    if (isLua(ci))
    {
      Proto* p = ci_func(L, ci)->p;
      if (p->flag & PF_VAHID)
        delta = ci->nextraargs + p->numparams + 1;
    }
    ci->func += delta; /* if vararg, back to virtual 'func' */
    ftransfer = ((int)(firstres - ci->func));
    luaD_hook(L, LUA_HOOKRET, -1, ftransfer, nres); /* call it */
    ci->func -= delta;
  }
  if (isLua(ci = ci->previous))
    L->oldpc = ci->savedpc - 1; /* set 'oldpc' */
}

/*
** Check whether 'func' has a '__call' metafield. If so, put it in the
** stack, below original 'func', so that 'luaD_precall' can call it.
** Raise an error if there is no '__call' metafield.
** Bits CIST_CCMT in status count how many _call metamethods were
** invoked and how many corresponding extra arguments were pushed.
** (This count will be saved in the 'callstatus' of the call).
**  Raise an error if this counter overflows.
*/
static l_uint32 tryfuncTM(lua_State* L, StkId func, l_uint32 status)
{
  const TValue* tm;
  StkId p;
  tm = luaT_gettmbyobj(L, func, TM_CALL);
  if (l_unlikely((((tm)->tt_ & NOVARIANT_MASK) == LUA_TNIL))) /* no metamethod? */
    luaG_callerror(L, func);
  for (p = L->top; p > func; p--) /* open space for metamethod */
    copyslot(L, p, p - 1);
  L->top++;                          /* stack space pre-allocated by the caller */
  setslot(L, func, tm);               /* metamethod is the new function to be called */
  if ((status & MAX_CCMT) == MAX_CCMT) /* is counter full? */
    luaG_runerror(L, lt::call_chain_too_long);
  return status + (1u << CIST_CCMT); /* increment counter */
}

/* Generic case for 'moveresult' */
static inline void genmoveresults(lua_State* L, StkId res, int nres,
                              int wanted)
{
  StkId firstresult = L->top - nres; /* index of first result */
  int i;
  if (nres > wanted)         /* extra results? */
    nres = wanted;           /* don't need them */
  for (i = 0; i < nres; i++) /* move all results to correct place */
    copyslot(L, res + i, firstresult + i);
  for (; i < wanted; i++) /* complete wanted number of results */
    setslottag(L, res + i, LUA_VNIL);
  L->top = res + wanted; /* top points after the last result */
}

/*
** Given 'nres' results at 'firstResult', move 'fwanted-1' of them
** to 'res'.  Handle most typical cases (zero results for commands,
** one result for expressions, multiple results for tail calls/single
** parameters) separated. The flag CIST_TBC in 'fwanted', if set,
** forces the switch to go to the default case.
*/
static inline void moveresults(lua_State* L, StkId res, int nres,
                           l_uint32 fwanted)
{
  switch (fwanted)
  {           /* handle typical cases separately */
  case 0 + 1: /* no values needed */
    L->top = res;
    return;
  case 1 + 1:                             /* one value needed */
    if (nres == 0)                        /* no results? */
      setslottag(L, res, LUA_VNIL);             /* adjust with nil */
    else                                  /* at least one result */
      copyslot(L, res, L->top - nres); /* move it to proper place */
    L->top = res + 1;
    return;
  case LUA_MULTRET + 1:
    genmoveresults(L, res, nres, nres); /* we want all results */
    break;
  default:
  { /* two/more results and/or to-be-closed variables */
    int wanted = get_nresults(fwanted);
    if (fwanted & CIST_TBC)
    { /* to-be-closed variables? */
      L->ci->nres = nres;
      L->ci->callstatus |= CIST_CLSRET; /* in case of yields */
      res = luaF_close(L, res, CLOSEKTOP, 1);
      L->ci->callstatus &= ~CIST_CLSRET;
      if (L->hookmask)
      { /* if needed, call hook after '__close's */
        StkId savedres = res;
        rethook(L, L->ci, nres);
        res = savedres;
      }
      if (wanted == LUA_MULTRET)
        wanted = nres; /* we want all results */
    }
    genmoveresults(L, res, nres, wanted);
    break;
  }
  }
}

/*
** Finishes a function call: calls hook if necessary, moves current
** number of results to proper place, and returns to previous call
** info. If function has to close variables, hook must be called after
** that.
*/
void luaD_poscall(lua_State* L, CallInfo* ci, int nres)
{
  l_uint32 fwanted = ci->callstatus & (CIST_TBC | CIST_NRESULTS);
  if (l_unlikely(L->hookmask) && !(fwanted & CIST_TBC))
    rethook(L, ci, nres);
  /* move results to proper place */
  moveresults(L, ci->func, nres, fwanted);
  /* function cannot be in any of these cases when returning */
  assert(!(ci->callstatus &
               (CIST_HOOKED | CIST_YPCALL | CIST_FIN | CIST_CLSRET)));
  L->ci = ci->previous; /* back to caller (after closing variables) */
}

static inline CallInfo* next_ci(lua_State* L)
{
  return L->ci->next ? L->ci->next : luaE_extendCI(L, 1);
}

/*
** Allocate and initialize CallInfo structure. At this point, the
** only valid fields in the call status are number of results,
** CIST_C (if it's a C function), and number of extra arguments.
** (All these bit-fields fit in 16-bit values.)
*/
static inline CallInfo* prepCallInfo(lua_State* L, StkId func, l_uint32 status,
                                 StkId top)
{
  CallInfo* ci = L->ci = next_ci(L); /* new frame */
  ci->func = func;
  assert((status & ~(CIST_NRESULTS | CIST_C | MAX_CCMT)) == 0);
  ci->callstatus = status;
  ci->top = top;
  return ci;
}

/*
** precall for C functions
*/
static inline int precallC(lua_State* L, StkId func, l_uint32 status,
                       lua_CFunction f)
{
  int n; /* number of returns */
  CallInfo* ci;
  checkstackp(L, LUA_MINSTACK, func); /* ensure minimum stack size */
  L->ci = ci = prepCallInfo(L, func, status | CIST_C,
                            L->top + LUA_MINSTACK);
  assert(ci->top <= L->stack_last);
  if (l_unlikely(L->hookmask & LUA_MASKCALL))
  {
    int narg = ((int)(L->top - func)) - 1;
    luaD_hook(L, LUA_HOOKCALL, -1, 1, narg);
  }
  lua_unlock(L);
  n = (*f)(L); /* do the actual call */
  lua_lock(L);
  api_checknelems(L, n);
  luaD_poscall(L, ci, n);
  return n;
}

/*
** Prepare a function for a tail call, building its call info on top
** of the current call info. 'narg1' is the number of arguments plus 1
** (so that it includes the function itself). Return the number of
** results, if it was a C function, or -1 for a Lua function.
*/
int luaD_pretailcall(lua_State* L, CallInfo* ci, StkId func,
                     int narg1, int delta)
{
  l_uint32 status = LUA_MULTRET + 1;
  for (;;)
  {
    switch (slottag(L, func) & WITHVARIANT_MASK)
    {
    case LUA_VCCL: /* C closure */
    {
      CClosure* ccl = slotclCval(L, func);
      return precallC(L, func, status, ccl->f);
    }
    case LUA_VLCF: /* light C function */
      return precallC(L, func, status, slotfval(L, func));
    case LUA_VLCL:
    { /* Lua function */
      Proto* p = slotclLval(L, func)->p;
      int fsize = p->maxstacksize; /* frame size */
      int nfixparams = p->numparams;
      int i;
      checkstackp(L, fsize - delta, func);
      ci->func -= delta;        /* restore 'func' (if vararg) */
      for (i = 0; i < narg1; i++) /* move down function and arguments */
        copyslot(L, ci->func + i, func + i);
      func = ci->func; /* moved-down function */
      for (; narg1 <= nfixparams; narg1++)
        setslottag(L, func + narg1, LUA_VNIL); /* complete missing arguments */
      ci->top = func + 1 + fsize;  /* top for new function */
      assert(ci->top <= L->stack_last);
      ci->savedpc = 0; /* starting point */
      ci->callstatus |= CIST_TAIL;
      L->top = func + narg1; /* set top */
      return -1;
    }
    default:
    {                                      /* not a function */
      checkstackp(L, 1, func);             /* space for metamethod */
      status = tryfuncTM(L, func, status); /* try '__call' metamethod */
      narg1++;
      continue; /* try again */
    }
    }
  }
}

CallInfo* luaD_precall(lua_State* L, StkId func, int nresults)
{
  l_uint32 status = nresults + 1;
  assert(status <= MAXRESULTS + 1);
  for (;;)
  {
    switch (slottag(L, func) & WITHVARIANT_MASK)
    {
    case LUA_VCCL: /* C closure */
    {
      CClosure* ccl = slotclCval(L, func);
      precallC(L, func, status, ccl->f);
      return nullptr;
    }
    case LUA_VLCF: /* light C function */
      precallC(L, func, status, slotfval(L, func));
      return nullptr;
    case LUA_VLCL:
    { /* Lua function */
      CallInfo* ci;
      Proto* p = slotclLval(L, func)->p;
      int narg = ((int)(L->top - func)) - 1; /* number of real arguments */
      int nfixparams = p->numparams;
      int fsize = p->maxstacksize; /* frame size */
      checkstackp(L, fsize, func);
      L->ci = ci = prepCallInfo(L, func, status, func + 1 + fsize);
      ci->savedpc = 0; /* starting point */
      for (; narg < nfixparams; narg++)
        setslottag(L, L->top++, LUA_VNIL); /* complete missing arguments */
      assert(ci->top <= L->stack_last);
      return ci;
    }
    default:
    {                                      /* not a function */
      checkstackp(L, 1, func);             /* space for metamethod */
      status = tryfuncTM(L, func, status); /* try '__call' metamethod */
      continue;                            /* try again with metamethod */
    }
    }
  }
}

/*
** Call a function (C or Lua) through C. 'inc' can be 1 (increment
** number of recursive invocations in the C stack) or nyci (the same
** plus increment number of non-yieldable calls).
** This function can be called with some use of EXTRA_STACK, so it should
** check the stack before doing anything else. 'luaD_precall' already
** does that.
*/
static inline void ccall(lua_State* L, StkId func, int nResults, l_uint32 inc)
{
  CallInfo* ci;
  L->nCcalls += inc;
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS))
  {
    checkstackp(L, 0, func); /* free any use of EXTRA_STACK */
    luaE_checkcstack(L);
  }
  if ((ci = luaD_precall(L, func, nResults)) != nullptr)
  {                               /* Lua function? */
    ci->callstatus |= CIST_FRESH; /* mark that it is a "fresh" execute */
    luaV_execute(L, ci);          /* call it */
  }
  L->nCcalls -= inc;
}

/*
** External interface for 'ccall'
*/
void luaD_call(lua_State* L, StkId func, int nResults)
{
  ccall(L, func, nResults, 1);
}

/*
** Similar to 'luaD_call', but does not allow yields during the call.
*/
void luaD_callnoyield(lua_State* L, StkId func, int nResults)
{
  ccall(L, func, nResults, nyci);
}

/*
** Finish the job of 'lua_pcallk' after it was interrupted by an yield.
** (The caller, 'finishCcall', does the final call to 'adjustresults'.)
** The main job is to complete the 'luaD_pcall' called by 'lua_pcallk'.
** If a '__close' method yields here, eventually control will be back
** to 'finishCcall' (when that '__close' method finally returns) and
** 'finishpcallk' will run again and close any still pending '__close'
** methods. Similarly, if a '__close' method errs, 'precover' calls
** 'unroll' which calls ''finishCcall' and we are back here again, to
** close any pending '__close' methods.
** Note that, up to the call to 'luaF_close', the corresponding
** 'CallInfo' is not modified, so that this repeated run works like the
** first one (except that it has at least one less '__close' to do). In
** particular, field CIST_RECST preserves the error status across these
** multiple runs, changing only if there is a new error.
*/
static TStatus finishpcallk(lua_State* L, CallInfo* ci)
{
  TStatus status = getcistrecst(ci); /* get original status */
  if (l_likely(status == LUA_OK))    /* no error? */
    status = LUA_YIELD;              /* was interrupted by an yield */
  else
  { /* error */
    StkId func = (static_cast<StkId>(ci->funcidx));
    L->allowhook = getoah(ci);             /* restore 'allowhook' */
    func = luaF_close(L, func, status, 1); /* can yield or raise an error */
    luaD_seterrorobj(L, status, func);
    luaD_shrinkstack(L);      /* restore stack size in case of overflow */
    setcistrecst(ci, LUA_OK); /* clear original status */
  }
  ci->callstatus &= ~CIST_YPCALL;
  L->errfunc = ci->old_errfunc;
  /* if it is here, there were errors or yields; unlike 'lua_pcallk',
     do not change status */
  return status;
}

/*
** Completes the execution of a C function interrupted by an yield.
** The interruption must have happened while the function was either
** closing its tbc variables in 'moveresults' or executing
** 'lua_callk'/'lua_pcallk'. In the first case, it just redoes
** 'luaD_poscall'. In the second case, the call to 'finishpcallk'
** finishes the interrupted execution of 'lua_pcallk'.  After that, it
** calls the continuation of the interrupted function and finally it
** completes the job of the 'luaD_call' that called the function.  In
** the call to 'adjustresults', we do not know the number of results
** of the function called by 'lua_callk'/'lua_pcallk', so we are
** conservative and use LUA_MULTRET (always adjust).
*/
static void finishCcall(lua_State* L, CallInfo* ci)
{
  int n; /* actual number of results from C function */
  if (ci->callstatus & CIST_CLSRET)
  { /* was closing TBC variable? */
    assert(ci->callstatus & CIST_TBC);
    n = ci->nres; /* just redo 'luaD_poscall' */
    /* don't need to reset CIST_CLSRET, as it will be set again anyway */
  }
  else
  {
    TStatus status = LUA_YIELD;   /* default if there were no errors */
    lua_KFunction kf = ci->k; /* continuation function */
    /* must have a continuation and must be able to call it */
    assert(kf != nullptr && yieldable(L));
    if (ci->callstatus & CIST_YPCALL) /* was inside a 'lua_pcallk'? */
      status = finishpcallk(L, ci);   /* finish it */
    adjustresults(L, LUA_MULTRET);    /* finish 'lua_callk' */
    lua_unlock(L);
    n = (*kf)(L, (status), ci->ctx); /* call continuation */
    lua_lock(L);
    api_checknelems(L, n);
  }
  luaD_poscall(L, ci, n); /* finish 'luaD_call' */
}

/*
** Executes "full continuation" (everything in the stack) of a
** previously interrupted coroutine until the stack is empty (or another
** interruption long-jumps out of the loop).
*/
static void unroll(lua_State* L)
{
  CallInfo* ci;
  while ((ci = L->ci) != &L->base_ci)
  {                       /* something in the stack */
    if (!isLua(ci))       /* C function? */
      finishCcall(L, ci); /* complete its execution */
    else
    {                      /* Lua function */
      luaV_finishOp(L);    /* finish interrupted instruction */
      luaV_execute(L, ci); /* execute down to higher C 'boundary' */
    }
  }
}

/*
** Try to find a suspended protected call (a "recover point") for the
** given thread.
*/
static CallInfo* findpcall(lua_State* L)
{
  CallInfo* ci;
  for (ci = L->ci; ci != nullptr; ci = ci->previous)
  { /* search for a pcall */
    if (ci->callstatus & CIST_YPCALL)
      return ci;
  }
  return nullptr; /* no pending pcall */
}

/*
** Signal an error in the call to 'lua_resume', not in the execution
** of the coroutine itself. (Such errors should not be handled by any
** coroutine error handler and should not kill the coroutine.)
*/
static int resume_error(lua_State* L, byte_view msg, int narg)
{
  api_checkpop(L, narg);
  L->top -= narg;                           /* remove args from the stack */
  setslots(L, L->top, TString::luaS_createstr(L, msg)); /* push error message */
  api_incr_top(L);
  lua_unlock(L);
  return LUA_ERRRUN;
}

/*
** Do the work for 'lua_resume' in protected mode. Most of the work
** depends on the status of the coroutine: initial state, suspended
** inside a hook, or regularly suspended (optionally with a continuation
** function), plus erroneous cases: non-suspended coroutine or dead
** coroutine.
*/
static void resume(lua_State* L, int n)
{
  StkId firstArg = L->top - n; /* first argument */
  CallInfo* ci = L->ci;
  if (L->status == LUA_OK)                  /* starting a coroutine? */
    ccall(L, firstArg - 1, LUA_MULTRET, 0); /* just call its body */
  else
  { /* resuming from previous yield */
    assert(L->status == LUA_YIELD);
    L->status = LUA_OK; /* mark that it is running (again) */
    if (isLua(ci))
    { /* yielded inside a hook? */
      /* undo increment made by 'luaG_traceexec': instruction was not
         executed yet */
      assert(ci->callstatus & CIST_HOOKYIELD);
      ci->savedpc--;
      L->top = firstArg; /* discard arguments */
      luaV_execute(L, ci); /* just continue running Lua code */
    }
    else
    { /* 'common' yield */
      if (ci->k != nullptr)
      { /* does it have a continuation function? */
        lua_unlock(L);
        n = (*ci->k)(L, LUA_YIELD, ci->ctx); /* call continuation */
        lua_lock(L);
        api_checknelems(L, n);
      }
      luaD_poscall(L, ci, n); /* finish 'luaD_call' */
    }
    unroll(L); /* run continuation */
  }
}

struct Unroll : ProtectedCall
{
  void run(lua_State* L) override { unroll(L); }
};

struct Resume : ProtectedCall
{
  int nargs;
  void run(lua_State* L) override { resume(L, nargs); }
};

/*
** Unrolls a coroutine in protected mode while there are recoverable
** errors, that is, errors inside a protected call. (Any error
** interrupts 'unroll', and this loop protects it again so it can
** continue.) Stops with a normal end (status == LUA_OK), an yield
** (status == LUA_YIELD), or an unprotected error ('findpcall' doesn't
** find a recover point).
*/
static TStatus precover(lua_State* L, TStatus status)
{
  CallInfo* ci;
  while (errorstatus(status) && (ci = findpcall(L)) != nullptr)
  {
    L->ci = ci;               /* go down to recovery functions */
    setcistrecst(ci, status); /* status to finish 'pcall' */
    Unroll u;
    status = luaD_rawrunprotected(L, &u);
  }
  return status;
}

LUA_API lua_ResumeResult lua_resume(lua_State* L, lua_State* from, int nargs)
{
  TStatus status;
  lua_lock(L);
  if (L->status == LUA_OK)
  {                           /* may be starting a coroutine */
    if (L->ci != &L->base_ci) /* not in base level? */
      return {resume_error(L, lt::cannot_resume_non_suspended_coroutine, nargs), 1};
    else if (((int)(L->top - (L->ci->func + 1))) == nargs) /* no function? */
      return {resume_error(L, lt::cannot_resume_dead_coroutine, nargs), 1};
  }
  else if (L->status != LUA_YIELD) /* ended with errors? */
    return {resume_error(L, lt::cannot_resume_dead_coroutine, nargs), 1};
  L->nCcalls = (from != nullptr) ? getCcalls(from) : 0;
  if (getCcalls(L) >= LUAI_MAXCCALLS)
    return {resume_error(L, lt::C_stack_overflow, nargs), 1};
  L->nCcalls++;
  api_checkpop(L, (L->status == LUA_OK) ? nargs + 1 : nargs);
  Resume r;
  r.nargs = nargs;
  status = luaD_rawrunprotected(L, &r);
  /* continue running after recoverable errors */
  status = precover(L, status);
  if (l_likely(!errorstatus(status)))
    assert(status == L->status); /* normal end or yield */
  else
  {                                        /* unrecoverable error */
    L->status = status;                    /* mark thread as 'dead' */
    luaD_seterrorobj(L, status, L->top); /* push error message */
    L->ci->top = L->top;
  }
  int nresults = (status == LUA_YIELD) ? L->ci->nyield
                                       : ((int)(L->top - (L->ci->func + 1)));
  lua_unlock(L);
  return {(status), nresults};
}

LUA_API int lua_isyieldable(lua_State* L)
{
  return yieldable(L);
}

LUA_API int lua_yieldk(lua_State* L, int nresults, lua_KContext ctx,
                       lua_KFunction k)
{
  CallInfo* ci;
  lua_lock(L);
  ci = L->ci;
  api_checkpop(L, nresults);
  if (l_unlikely(!yieldable(L)))
  {
    if (L != mainthread(L->l_G))
      luaG_runerror(L, lt::attempt_to_yield_across_a_C_call_boundary);
    else
      luaG_runerror(L, lt::attempt_to_yield_from_outside_a_coroutine);
  }
  L->status = LUA_YIELD;
  ci->nyield = nresults; /* save number of results */
  if (isLua(ci))
  { /* inside a hook? */
    assert(!isLuacode(ci));
    assert((nresults == 0) && "hooks cannot yield values");
    assert((k == nullptr) && "hooks cannot continue after yielding");
  }
  else
  {
    if ((ci->k = k) != nullptr) /* is there a continuation? */
      ci->ctx = ctx;         /* save context */
    luaD_throw(L, LUA_YIELD);
  }
  assert(ci->callstatus & CIST_HOOKED); /* must be inside a hook */
  lua_unlock(L);
  return 0; /* return to 'luaD_hook' */
}

/*
** Auxiliary structure to call 'luaF_close' in protected mode.
*/
struct CloseP : ProtectedCall
{
  StkId level;
  TStatus status;
  void run(lua_State* L) override;
};

/*
** Auxiliary function to call 'luaF_close' in protected mode.
*/
static void closepaux(lua_State* L, CloseP* pcl)
{
  luaF_close(L, pcl->level, pcl->status, 0);
}

void CloseP::run(lua_State* L)
{
  closepaux(L, this);
}

/*
** Calls 'luaF_close' in protected mode. Return the original status
** or, in case of errors, the new status.
*/
TStatus luaD_closeprotected(lua_State* L, StkId level, TStatus status)
{
  CallInfo* old_ci = L->ci;
  lu_byte old_allowhooks = L->allowhook;
  for (;;)
  { /* keep closing upvalues until no more errors */
    struct CloseP pcl;
    pcl.level = (static_cast<StkId>(level));
    pcl.status = status;
    status = luaD_rawrunprotected(L, &pcl);
    if (l_likely(status == LUA_OK)) /* no more errors? */
      return pcl.status;
    else
    { /* an error occurred; restore saved state and repeat */
      L->ci = old_ci;
      L->allowhook = old_allowhooks;
    }
  }
}

/*
** Call the C function 'func' in protected mode, restoring basic
** thread information ('allowhook', etc.) and in particular
** its stack level in case of errors.
*/
TStatus luaD_pcall(lua_State* L, ProtectedCall* func, StkId old_top,
                   StkId ef)
{
  TStatus status;
  CallInfo* old_ci = L->ci;
  lu_byte old_allowhooks = L->allowhook;
  StkId old_errfunc = L->errfunc;
  L->errfunc = ef;
  status = luaD_rawrunprotected(L, func);
  if (l_unlikely(status != LUA_OK))
  { /* an error occurred? */
    L->ci = old_ci;
    L->allowhook = old_allowhooks;
    status = luaD_closeprotected(L, old_top, status);
    luaD_seterrorobj(L, status, (static_cast<StkId>(old_top)));
    luaD_shrinkstack(L); /* restore stack size in case of overflow */
  }
  L->errfunc = old_errfunc;
  return status;
}

/*
** Execute a protected parser.
*/
struct SParser : ProtectedCall
{ /* data to 'f_parser' */
  ZIO* z;
  MBuffer buff;
  Dyndata dyd;  /* dynamic structures used by the parser */
  byte_view mode;
  byte_view name;
  void run(lua_State* L) override;
};

static void checkmode(lua_State* L, byte_view mode, byte_view x)
{
  if (mode.find(x[0]) == byte_view::npos)
  {
    luaO_pushfstring(L, lt::attempt_to_load_a_s_chunk_mode_is_s,
                     FmtArgs().str(x).str(mode));
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}

/*
** Before the first call to the reader function, Lua reserves a slot
** with a table for anchoring stuff.
*/
static void f_parser(lua_State* L, SParser* p)
{
  LClosure* cl;
  byte_view mode = p->mode;
  int c;
  Table* anchor;
  StkId otop = (L->top); /* original top */
  luaD_checkstack(L, 2);
  anchor = Table::luaH_new(L);               /* create the anchor table */
  setsloth(L, L->top++, anchor); /* anchor the anchor table */
  c = (p->z->pos < p->z->chunk.size())
          ? ((unsigned char)(p->z->chunk[p->z->pos++]))
          : luaZ_fill(p->z); /* read first character */
  if (c == LUA_SIGNATURE[0])
  {
    int fixed = 0;
    if (mode.find('B') != byte_view::npos)
      fixed = 1;
    else
      checkmode(L, mode, lt::binary);
    cl = luaU_undump(L, p->z, anchor, p->name, fixed);
  }
  else
  {
    checkmode(L, mode, lt::text);
    cl = luaY_parser(L, p->z, anchor, &p->buff, &p->dyd, p->name, c);
  }
  L->top = (static_cast<StkId>(otop)); /* restore stack */
  setslotclL(L, L->top++, cl); /* push closure */
  assert(cl->upvals.size() == cl->p->upvalues.size());
  cl->luaF_initupvals(L);
}

void SParser::run(lua_State* L)
{
  f_parser(L, this);
}

/*
** Anchor an object in a table in the stack.  First, anchor the object
** temporarily in the stack, as luaH_set may call an emergency GC.
** Then, add it in the table with itself as its key.
*/
void luaD_anchorobj(lua_State* L, Table* anchor, GCObject* obj)
{
  TValue v;
  setgcovalue(L, &v, obj);
  setslot(L, L->top, &v); /* temporary anchor in the stack */
  L->top++;
  anchor->luaH_set(L, &v, &v);
  /* Because this is a new key, luaH_set will call the GC barrier, so
     we don't need to call the barrier again here */
  L->top--;
}

TStatus luaD_protectedparser(lua_State* L, ZIO* z, std::optional<byte_view> name, std::optional<byte_view> mode)
{
  struct SParser p;
  TStatus status;
  incnny(L); /* cannot yield during parsing */
  p.z = z;
  p.buff.attach(L);
  p.name = name.value_or(lt::qmark);
  p.mode = mode.value_or(lt::bt);
  p.dyd.actvar.arr = nullptr;
  p.dyd.actvar.size = 0;
  p.dyd.gt.arr = nullptr;
  p.dyd.gt.size = 0;
  p.dyd.label.arr = nullptr;
  p.dyd.label.size = 0;
  status = luaD_pcall(L, &p, (L->top), L->errfunc);
  luaM_alloc_Vardesc_span(L->l_G, L, std::span<Vardesc>(p.dyd.actvar.arr, p.dyd.actvar.size), 0, false);
  luaM_alloc_Labeldesc_span(L->l_G, L, std::span<Labeldesc>(p.dyd.gt.arr, p.dyd.gt.size), 0, false);
  luaM_alloc_Labeldesc_span(L->l_G, L, std::span<Labeldesc>(p.dyd.label.arr, p.dyd.label.size), 0, false);
  decnny(L);
  return status;
}
