/*
** $Id: ldebug.c $
** Debug Interface
** See Copyright Notice in lua.h
*/


#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"

#include "lua_internal.h"
#include "literals.h"

namespace lua::vm
{

static inline int LuaClosure(const Closure* f)
{
  return f != nullptr && f->tt == LUA_VLCL;
}

static InternalString tmplainname[] = {
    lt::index_kw, lt::newindex,
    lt::gc, lt::mode, lt::len, lt::eq,
    lt::add, lt::sub, lt::mul, lt::mod, lt::pow,
    lt::div, lt::idiv,
    lt::band, lt::bor, lt::bxor, lt::shl, lt::shr,
    lt::unm, lt::bnot, lt::lt, lt::le,
    lt::concat, lt::call, lt::close_kw};
static_assert(sizeof(tmplainname) / sizeof(tmplainname[0]) == TM_N, "tmplainname must cover every metamethod (ORDER TM)");

struct VarName
{
  std::optional<byte_view> kind;
  const InternalString* name;
};

static VarName funcnamefromcall(lua_State* L, CallInfo* ci);

static int currentpc(lua_State* L, CallInfo* ci)
{
  assert(isLua(ci));
  (void)L;
  return ci->savedpc - 1;
}

/*
** Get a "base line" to find the line corresponding to an instruction.
** Base lines are regularly placed at MAXIWTHABS intervals, so usually
** an integer division gets the right place. When the source file has
** large sequences of empty/comment lines, it may need extra entries,
** so the original estimate needs a correction.
** If the original estimate is -1, the initial 'if' ensures that the
** 'while' will run at least once.
** The assertion that the estimate is a lower bound for the correct base
** is valid as long as the debug info has been generated with the same
** value for MAXIWTHABS or smaller. (Previous releases use a little
** smaller value.)
*/
struct BaseLine
{
  int pc;
  int line;
};

static BaseLine getbaseline(const Proto* f, int pc)
{
  if (f->abslineinfo.size() == 0 || pc < f->abslineinfo[0].pc)
  {
    return {-1, f->linedefined}; /* start from the beginning */
  }
  else
  {
    int i = pc / MAXIWTHABS - 1; /* get an estimate */
    /* estimate must be a lower bound of the correct base */
    assert(i < 0 ||
               (i < (int)f->abslineinfo.size() && f->abslineinfo[i].pc <= pc));
    while (i + 1 < (int)f->abslineinfo.size() && pc >= f->abslineinfo[i + 1].pc)
      i++; /* low estimate; adjust it */
    return {f->abslineinfo[i].pc, f->abslineinfo[i].line};
  }
}

/*
** Get the line corresponding to instruction 'pc' in function 'f';
** first gets a base line and from there does the increments until
** the desired instruction.
*/
int luaG_getfuncline(const Proto* f, int pc)
{
  if (f->lineinfo.data() == nullptr) /* no debug information? */
    return -1;
  else
  {
    BaseLine base = getbaseline(f, pc);
    int basepc = base.pc;
    int baseline = base.line;
    while (basepc++ < pc)
    { /* walk until given instruction */
      assert(f->lineinfo[basepc] != ABSLINEINFO);
      baseline += f->lineinfo[basepc]; /* correct line */
    }
    return baseline;
  }
}

static int getcurrentline(lua_State* L, CallInfo* ci)
{
  return luaG_getfuncline(ci_func(L, ci)->p, currentpc(L, ci));
}

/*
** Set 'trap' for all active Lua frames.
** This function can be called during a signal, under "reasonable"
** assumptions. A new 'ci' is completely linked in the list before it
** becomes part of the "active" list, and we assume that pointers are
** atomic; see comment in next function.
** (A compiler doing interprocedural optimizations could, theoretically,
** reorder memory writes in such a way that the list could be
** temporarily broken while inserting a new element. We simply assume it
** has no good reasons to do that.)
*/
static void settraps(CallInfo* ci)
{
  for (; ci != nullptr; ci = ci->previous)
    if (isLua(ci))
      ci->trap = 1;
}

/*
** This function can be called during a signal, under "reasonable"
** assumptions.
** Fields 'basehookcount' and 'hookcount' (set by 'resethookcount')
** are for debug only, and it is no problem if they get arbitrary
** values (causes at most one wrong hook call). 'hookmask' is an atomic
** value. We assume that pointers are atomic too (e.g., gcc ensures that
** for all platforms where it runs). Moreover, 'hook' is always checked
** before being called (see 'luaD_hook').
*/
}

void lua_sethook(lua_State* L, lua_Hook func, int mask, int count)
{
  if (func == nullptr || mask == 0)
  { /* turn off hooks? */
    mask = 0;
    func = nullptr;
  }
  L->hook = func;
  L->basehookcount = count;
  resethookcount(L);
  L->hookmask = (lu_byte)mask | (L->hookmask & LUA_MASKBUDGET);
  if (mask)
    settraps(L->ci); /* to trace inside 'luaV_execute' */
}

/*
** A new budget group for 'co', made in protected mode on 'co', so that a
** failure raises nothing on it (see 'lua_setbudget').
*/
struct NewGroup : ProtectedCall
{
  lua_Integer n = 0;
  lua_Budget f = nullptr;
  BudgetGroup* bg = nullptr;
  void run(lua_State* co) override
  {
    BudgetGroup* g1 = luaM_malloc_BudgetGroup(co);
    luaC_link(co, g1, LUA_VBUDGET);
    g1->budget = (l_mem)n;
    g1->left = (l_mem)n;
    g1->f = f;
    g1->root = co;
    if (luaC_newowner(co, g1))
      bg = g1; /* (otherwise too many groups: the new one is garbage) */
  }
};

/*
** Set the budget of thread 'co'. A thread with no budget group gets a new
** one, which it belongs to and passes to the threads it creates (and,
** while they run, to the ones it resumes); a thread that has one updates
** it in place: the limit and the callback are those of every thread of the
** group, and its steps start anew (in the group's own callback, when the
** callback ends). 'n' 0 removes the thread from its group. The group is in
** effect at once, by the rule of a resume ('luaE_enterbudget'), unless the
** thread is suspended: then at its next resume. So a thread never runs
** without its budget once it has been set. Returns LUA_OK, or LUA_ERRMEM
** (no memory for a new group) or LUA_ERRRUN (too many groups at once), and
** then nothing changed: no error is raised, on 'co' or elsewhere.
*/
int lua_setbudget(lua_State* co, lua_Integer n, lua_Budget f)
{
  lua_lock(co);
  global_State* g = co->l_G;
  if (n <= 0 || f == nullptr)
  { /* no budget */
    co->bown = nullptr;
  }
  else if (co->bown != nullptr)
  { /* the group of the thread, updated */
    BudgetGroup* bg = co->bown;
    bg->budget = (l_mem)n;
    bg->f = f;
    if (!(g->budgetL != nullptr && g->budgetg == bg)) /* (its callback refills it as it ends) */
      bg->left = (l_mem)n;
  }
  else
  {
    NewGroup ng;
    ng.n = n;
    ng.f = f;
    TStatus status = luaD_rawrunprotected(co, &ng);
    if (status != LUA_OK || ng.bg == nullptr)
    {
      lua_unlock(co);
      return (status != LUA_OK) ? LUA_ERRMEM : LUA_ERRRUN;
    }
    co->bown = ng.bg;
  }
  if (co->status != LUA_YIELD) /* (a suspended thread gets it at its next resume) */
    luaE_enterbudget(co, co->resumer); /* as at a resume: no thread runs without its budget */
  lua_unlock(co);
  return LUA_OK;
}

/* the thread the budget group of 'co' was set on (nothing once freed) */
lua_State* lua_budgetroot(lua_State* co)
{
  return (co->bown != nullptr) ? co->bown->root : nullptr;
}

/*
** The value for the callback of the group of 'co' (its own group, or the
** one in effect): set it by popping a value from 'L', or push it onto 'L'.
*/
void lua_setbudgetvalue(lua_State* L, lua_State* co)
{
  lua_lock(L);
  BudgetGroup* bg = co->bown;
  assert(bg != nullptr && "no budget");
  bg->cb = getslot(L, L->top - 1);
  luaC_barrier(L, bg, &bg->cb);
  L->top--;
  lua_unlock(L);
}

void lua_pushbudgetvalue(lua_State* L, lua_State* co, int effective)
{
  lua_lock(L);
  BudgetGroup* bg = effective ? co->beff : co->bown;
  if (bg != nullptr)
    setslot(L, L->top, &bg->cb);
  else
    setslottag(L, L->top, LUA_VNIL);
  api_incr_top(L);
  lua_unlock(L);
}

/*
** A thread to run the budget callback's Lua code on (kept by its owner,
** such as the debug library: anchored there), so that the thread that
** spent the budget keeps its stack and C calls as they are.
*/
lua_State* lua_budgetthread(lua_State* L)
{
  return L->l_G->budgetth;
}

/*
** The room a callback needs is reserved in its thread (the collector does
** not trim it): the stack and a number of frames, so that the callback
** allocates neither, up to a depth that no sane callback reaches.
*/
constexpr int BUDGET_STACK = 1000;
constexpr int BUDGET_FRAMES = 40;

void lua_setbudgetthread(lua_State* L, lua_State* B)
{
  bool ok = luaD_growstack(B, BUDGET_STACK, 0);
  while (ok && B->nci < BUDGET_FRAMES)
    ok = luaE_extendCI(B, 0) != nullptr;
  if (!ok)
  { /* (not the state's thread of callbacks yet: the next budget set tries again) */
    lua_lock(L); /* (as for any throw) */
    luaD_throw(L, LUA_ERRMEM);
  }
  L->l_G->budgetth = B;
}

/* push onto 'L' the function of the frame 'ar' of thread 'L1' */
void lua_pushframefunction(lua_State* L, lua_State* L1, const InternalDebug& ar)
{
  lua_lock(L);
  TValue f = getslot(L1, ar.i_ci->func);
  setslot(L, L->top, &f);
  api_incr_top(L);
  lua_unlock(L);
}

/* do '__gc' of the budget group of 'L' wait to be called? */
int lua_hasdrain(lua_State* L)
{ /* (and can they be called here: where the chain can freeze, outside hooks
     and the '__gc' being called (hooks off), outside the budget callback;
     the same as at an instruction boundary, see 'luaG_traceexec') */
  return luaC_hasdrain(L) && L->allowhook && L->l_G->budgetL == nullptr && luaD_canfreeze(L);
}

/* push the function that calls them (see 'luaC_pushdrain') */
void lua_pushdrain(lua_State* L)
{
  lua_lock(L);
  luaC_pushdrain(L);
  lua_unlock(L);
}

/* is 'L' running budgeted code, or the budget callback? */
int lua_budgeted(lua_State* L)
{
  return L->beff != nullptr || L->l_G->budgetL != nullptr;
}

lua_Integer lua_getbudget(lua_State* co, lua_Integer* left)
{
  global_State* g = co->l_G;
  BudgetGroup* bg = co->bown;
  if (bg == nullptr)
  {
    if (left != nullptr)
      *left = 0;
    return 0;
  }
  if (left != nullptr) /* (in the callback, it refills when it ends) */
    *left = (g->budgetL != nullptr && g->budgetg == bg) ? bg->budget : bg->left;
  return bg->budget;
}


lua_Hook lua_gethook(lua_State* L)
{
  return L->hook;
}


int lua_gethookmask(lua_State* L)
{
  return L->hookmask & ~LUA_MASKBUDGET;
}


int lua_gethookcount(lua_State* L)
{
  return L->basehookcount;
}


std::optional<InternalDebug> lua_getstacki(lua_State* L, int level)
{
  CallInfo* ci;
  if (level < 0)
    return std::nullopt; /* invalid (negative) level */
  lua_lock(L);
  for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
    level--;
  if (level == 0 && ci != &L->base_ci)
  { /* level found? */
    lua_unlock(L);
    InternalDebug ar{};
    ar.i_ci = ci;
    return ar;
  }
  lua_unlock(L);
  return std::nullopt; /* no such level */
}


std::optional<lua_Debug> lua_getstack(lua_State* L, int level)
{
  std::optional<InternalDebug> iar = lua_getstacki(L, level);
  if (!iar)
    return std::nullopt;
  lua_Debug ar{};
  ar.i_ci = iar->i_ci;
  return ar;
}

namespace lua::vm
{

static const InternalString* upvalname(const Proto* p, int uv)
{
  assert(uv < (int)p->upvalues.size());
  TString* s = p->upvalues[uv].name;
  if (s == nullptr)
    return &lt::qmark_i;
  else
    return &s->data;
}

static LocalSlot findvararg(lua_State* L, CallInfo* ci, int n)
{
  StkId fv = ci->func;
  LClosure* cl = slotclLval(L, fv);
  if (cl->p->flag & PF_VAHID)
  {
    int nextra = ci->nextraargs;
    if (n >= -nextra)
    { /* 'n' is negative */
      return {&lt::vararg_i, ci->func - nextra - (n + 1)}; /* generic name for any vararg */
    }
  }
  return {nullptr, 0}; /* no such vararg */
}

LocalSlot luaG_findlocal(lua_State* L, CallInfo* ci, int n)
{
  StkId base = ci->func + 1;
  const InternalString* name = nullptr;
  if (isLua(ci))
  {
    if (n < 0) /* access to vararg values? */
      return findvararg(L, ci, n);
    else
      name = ci_func(L, ci)->p->luaF_getlocalname(n, currentpc(L, ci));
  }
  if (name == nullptr)
  { /* no 'standard' name? */
    StkId limit = (ci == L->ci) ? L->top : ci->next->func;
    if (limit - base >= n && n > 0)
    { /* is 'n' inside 'ci' stack? */
      /* generic name for any valid slot */
      name = isLua(ci) ? &lt::temporary_i : &lt::C_temporary_i;
    }
    else
      return {nullptr, 0}; /* no name */
  }
  return {name, base + (n - 1)};
}

}

const InternalString* lua_getlocali(lua_State* L, const InternalDebug* ar, int n)
{
  const InternalString* name;
  lua_lock(L);
  if (ar == nullptr)
  {                                      /* information about non-active function? */
    if (slottag(L, L->top - 1) != ctb(LUA_VLCL)) /* not a Lua function? */
      name = nullptr;
    else /* consider live variables at function start (parameters) */
    {
      StkId fv = L->top - 1;
      name = slotclLval(L, fv)->p->luaF_getlocalname(n, 0);
    }
  }
  else
  {                   /* active function; get information through 'ar' */
    LocalSlot local = luaG_findlocal(L, ar->i_ci, n);
    name = local.name;
    if (name)
    {
      copyslot(L, L->top, local.pos);
      api_incr_top(L);
    }
  }
  lua_unlock(L);
  return name;
}


std::optional<HostString> lua_getlocal(lua_State* L, const lua_Debug* ar, int n)
{
  InternalDebug iar{};
  if (ar != nullptr)
    iar.i_ci = ar->i_ci;
  const InternalString* name = lua_getlocali(L, (ar != nullptr) ? &iar : nullptr, n);
  if (name == nullptr)
    return std::nullopt;
  return HostString(name->view());
}


const InternalString* lua_setlocali(lua_State* L, const InternalDebug* ar, int n)
{
  const InternalString* name;
  lua_lock(L);
  LocalSlot local = luaG_findlocal(L, ar->i_ci, n);
  name = local.name;
  if (name)
  {
    api_checkpop(L, 1);
    copyslot(L, local.pos, L->top - 1);
    L->top--; /* pop value */
  }
  lua_unlock(L);
  return name;
}


std::optional<HostString> lua_setlocal(lua_State* L, const lua_Debug& ar, int n)
{
  InternalDebug iar{};
  iar.i_ci = ar.i_ci;
  const InternalString* name = lua_setlocali(L, &iar, n);
  if (name == nullptr)
    return std::nullopt;
  return HostString(name->view());
}

namespace lua::vm
{

static void funcinfo(InternalDebug* ar, Closure* cl)
{
  if (!LuaClosure(cl))
  {
    ar->source = lt::C;
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = lt::C_char;
  }
  else
  {
    const Proto* p = static_cast<LClosure*>(cl)->p;
    if (p->source)
      ar->source = p->source->data.view();
    else
      ar->source = lt::literal_1;
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? lt::main : lt::Lua;
  }
  luaO_chunkid(ar->short_src_out, ar->source);
}

static int nextline(const Proto* p, int currentline, int pc)
{
  if (p->lineinfo[pc] != ABSLINEINFO)
    return currentline + p->lineinfo[pc];
  else
    return luaG_getfuncline(p, pc);
}

static void collectvalidlines(lua_State* L, Closure* f)
{
  if (!LuaClosure(f))
  {
    setslottag(L, L->top, LUA_VNIL);
    api_incr_top(L);
  }
  else
  {
    const Proto* p = static_cast<LClosure*>(f)->p;
    int currentline = p->linedefined;
    Table* t = Table::luaH_new(L);      /* new table to store active lines */
    setsloth(L, L->top, t); /* push it on stack */
    api_incr_top(L);
    if (p->lineinfo.data() != nullptr)
    { /* proto with debug information? */
      int i;
      TValue v;
      v.tt_ = LUA_VTRUE;     /* boolean 'true' to be the value of all indices */
      if (!isvararg(p)) /* regular function? */
        i = 0;            /* consider all instructions */
      else
      { /* vararg function */
        assert(GET_OPCODE(p->code[0]) == OP_VARARGPREP);
        currentline = nextline(p, currentline, 0);
        i = 1; /* skip first instruction (OP_VARARGPREP) */
      }
      for (; i < (int)p->lineinfo.size(); i++)
      {                                            /* for each instruction */
        currentline = nextline(p, currentline, i); /* get its line */
        t->luaH_setint(L, currentline, &v);        /* table[line] = true */
      }
    }
  }
}

static VarName getfuncname(lua_State* L, CallInfo* ci)
{
  /* calling function is a known function? */
  if (ci != nullptr && !(ci->callstatus & CIST_TAIL))
    return funcnamefromcall(L, ci->previous);
  else
    return {std::nullopt, nullptr}; /* no way to find a name */
}

static int auxgetinfo(lua_State* L, byte_view what, size_t wpos,
                      InternalDebug* ar, Closure* f, CallInfo* ci)
{
  int status = 1;
  for (; wpos < what.size(); wpos++)
  {
    switch (what[wpos])
    {
    case 'S':
    {
      funcinfo(ar, f);
      break;
    }
    case 'l':
    {
      ar->currentline = (ci && isLua(ci)) ? getcurrentline(L, ci) : -1;
      break;
    }
    case 'u':
    {
      ar->nups = (int)((f == nullptr) ? 0 : (LuaClosure(f) ? static_cast<LClosure*>(f)->upvals.size() : static_cast<CClosure*>(f)->upvalue.size()));
      if (!LuaClosure(f))
      {
        ar->isvararg = 1;
        ar->nparams = 0;
      }
      else
      {
        ar->isvararg = isvararg(static_cast<LClosure*>(f)->p) ? 1 : 0;
        ar->nparams = static_cast<LClosure*>(f)->p->numparams;
      }
      break;
    }
    case 't':
    {
      if (ci != nullptr)
      {
        ar->istailcall = !!(ci->callstatus & CIST_TAIL);
        ar->extraargs = (ci->callstatus & MAX_CCMT) >> CIST_CCMT;
      }
      else
      {
        ar->istailcall = 0;
        ar->extraargs = 0;
      }
      break;
    }
    case 'n':
    {
      VarName fn = getfuncname(L, ci);
      ar->namewhat = fn.kind;
      ar->name = (fn.kind.has_value() && fn.name != nullptr) ? fn.name->view() : (std::optional<byte_view>)std::nullopt;
      break;
    }
    case 'r':
    {
      if (ci == nullptr || !(ci->callstatus & CIST_HOOKED))
        ar->ftransfer = ar->ntransfer = 0;
      else
      {
        ar->ftransfer = L->transferinfo.ftransfer;
        ar->ntransfer = L->transferinfo.ntransfer;
      }
      break;
    }
    case 'L':
    case 'f': /* handled by lua_getinfo */
      break;
    default:
      status = 0; /* invalid option */
    }
  }
  return status;
}

}

std::optional<InternalDebug> lua_getinfoi(lua_State* L, byte_view what, const InternalDebug& ar)
{
  int status;
  Closure* cl;
  CallInfo* ci;
  StkId func;
  size_t wpos = 0;
  lua_lock(L);
  if (sv_byteat(what, 0) == '>')
  {
    ci = nullptr;
    func = L->top - 1;
    assert(novariant(slottag(L, func)) == LUA_TFUNCTION && "function expected");
    wpos = 1;   /* skip the '>' */
    L->top--;
  }
  else
  {
    ci = ar.i_ci;
    func = ci->func;
    assert(novariant(slottag(L, func)) == LUA_TFUNCTION);
  }
  if (slottag(L, func) == ctb(LUA_VLCL) || slottag(L, func) == ctb(LUA_VCCL))
  {
    cl = slotclval(L, func);
    assert(novariant(cl->tt) == LUA_TFUNCTION);
  }
  else
    cl = nullptr;
  InternalDebug moredetail = ar;
  status = auxgetinfo(L, what, wpos, &moredetail, cl, ci);
  if (what.find('f', wpos) != byte_view::npos)
  {
    copyslot(L, L->top, func);
    api_incr_top(L);
  }
  if (what.find('L', wpos) != byte_view::npos)
    collectvalidlines(L, cl);
  lua_unlock(L);
  if (!status)
    return std::nullopt; /* invalid option */
  return moredetail;
}


std::optional<lua_Debug> lua_getinfo(lua_State* L, const HostString& options, const lua_Debug& arin)
{
  byte_view what = options.to_utf8();
  InternalDebug iar{};
  iar.event = arin.event;
  iar.i_ci = arin.i_ci;
  std::optional<InternalDebug> res = lua_getinfoi(L, what, iar);
  if (!res)
    return std::nullopt; /* invalid option */
  lua_Debug ar = arin;
  ar.i_ci = res->i_ci;
  if (what.find('S') != byte_view::npos)
  {
    ar.source = res->source;
    ar.short_src = HostString::from_bytes(res->short_src_out.str_view());
    ar.what = HostString(res->what);
    ar.linedefined = res->linedefined;
    ar.lastlinedefined = res->lastlinedefined;
  }
  if (what.find('l') != byte_view::npos)
    ar.currentline = res->currentline;
  if (what.find('u') != byte_view::npos)
  {
    ar.nups = res->nups;
    ar.nparams = res->nparams;
    ar.isvararg = res->isvararg;
  }
  if (what.find('t') != byte_view::npos)
  {
    ar.istailcall = res->istailcall;
    ar.extraargs = res->extraargs;
  }
  if (what.find('n') != byte_view::npos)
  {
    ar.namewhat = res->namewhat.has_value() ? HostString::from_bytes(res->namewhat.value()) : HostString{};
    ar.name = res->name.has_value() ? HostString::from_bytes(res->name.value()) : HostString{};
  }
  if (what.find('r') != byte_view::npos)
  {
    ar.ftransfer = res->ftransfer;
    ar.ntransfer = res->ntransfer;
  }
  return ar;
}

namespace lua::vm
{

/*
** {======================================================
** Symbolic Execution
** =======================================================
*/

static int filterpc(int pc, int jmptarget)
{
  if (pc < jmptarget) /* is code conditional (inside a jump)? */
    return -1;        /* cannot know who sets that register */
  else
    return pc; /* current position sets that register */
}

/*
** Try to find last instruction before 'lastpc' that modified register 'reg'.
*/
static int findsetreg(const Proto* p, int lastpc, int reg)
{
  int pc;
  int setreg = -1;   /* keep last instruction that changed 'reg' */
  int jmptarget = 0; /* any code before this address is conditional */
  if (testMMMode(GET_OPCODE(p->code[lastpc])))
    lastpc--; /* previous instruction was not actually executed */
  for (pc = 0; pc < lastpc; pc++)
  {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    int a = GETARG_A(i);
    int change; /* true if current instruction changed 'reg' */
    switch (op)
    {
    case OP_LOADNIL:
    { /* set registers from 'a' to 'a+b' */
      int b = GETARG_B(i);
      change = (a <= reg && reg <= a + b);
      break;
    }
    case OP_TFORCALL:
    { /* affect all regs above its base */
      change = (reg >= a + 2);
      break;
    }
    case OP_CALL:
    case OP_TAILCALL:
    { /* affect all registers above base */
      change = (reg >= a);
      break;
    }
    case OP_JMP:
    { /* doesn't change registers, but changes 'jmptarget' */
      int b = GETARG_sJ(i);
      int dest = pc + 1 + b;
      /* jump does not skip 'lastpc' and is larger than current one? */
      if (dest <= lastpc && dest > jmptarget)
        jmptarget = dest; /* update 'jmptarget' */
      change = 0;
      break;
    }
    default: /* any instruction that sets A */
      change = (testAMode(op) && reg == a);
      break;
    }
    if (change)
      setreg = filterpc(pc, jmptarget);
  }
  return setreg;
}

/*
** Find a "name" for the constant 'c'.
*/
static VarName kname(const Proto* p, int index)
{
  const TValue* kvalue = &p->k[index];
  if (ttisstring(kvalue))
    return {lt::constant, &tsvalue(kvalue)->data};
  else
    return {std::nullopt, &lt::qmark_i};
}

struct RegName
{
  VarName var;
  int pc;
};

static RegName basicgetobjname(const Proto* p, int pc, int reg)
{
  const InternalString* name = p->luaF_getlocalname(reg + 1, pc);
  if (name) /* is a local? */
    return {{lt::local, name}, pc};
  /* else try symbolic execution */
  pc = findsetreg(p, pc, reg);
  if (pc != -1)
  { /* could find instruction? */
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    switch (op)
    {
    case OP_MOVE:
    {
      int b = GETARG_B(i); /* move from 'b' to 'a' */
      if (b < GETARG_A(i))
        return basicgetobjname(p, pc, b); /* get name for 'b' */
      break;
    }
    case OP_GETUPVAL:
    {
      return {{lt::upvalue, upvalname(p, GETARG_B(i))}, pc};
    }
    case OP_LOADK:
      return {kname(p, GETARG_Bx(i)), pc};
    case OP_LOADKX:
      return {kname(p, GETARG_Ax(p->code[pc + 1])), pc};
    default:
      break;
    }
  }
  return {{std::nullopt, name}, pc}; /* could not find reasonable name */
}

/*
** Find a "name" for the register 'c'.
*/
static const InternalString* rname(const Proto* p, int pc, int c)
{
  VarName what = basicgetobjname(p, pc, c).var; /* search for 'c' */
  if (!what.kind.has_value() || what.kind.value() != lt::constant) /* did not find a constant name? */
    return &lt::qmark_i;
  return what.name;
}

/*
** Check whether table being indexed by instruction 'i' is the
** environment '_ENV'
*/
static byte_view isEnv(const Proto* p, int pc, Instruction i, int isup)
{
  int t = GETARG_B(i);       /* table index */
  const InternalString* name = nullptr; /* name of indexed variable */
  if (isup)                  /* is 't' an upvalue? */
    name = upvalname(p, t);
  else
  { /* 't' is a register */
    VarName what = basicgetobjname(p, pc, t).var;
    name = what.name;
    /* 'name' must be the name of a local variable (at the current
       level or an upvalue) */
    if (!what.kind.has_value() || (what.kind.value() != lt::local && what.kind.value() != lt::upvalue))
      name = nullptr; /* cannot be the variable _ENV */
  }
  return (name != nullptr && name->view() == lt::_ENV) ? lt::global : lt::field;
}

/*
** Extend 'basicgetobjname' to handle table accesses
*/
static VarName getobjname(const Proto* p, int lastpc, int reg)
{
  RegName basic = basicgetobjname(p, lastpc, reg);
  lastpc = basic.pc;
  if (basic.var.kind.has_value())
    return basic.var;
  else if (lastpc != -1)
  { /* could find instruction? */
    Instruction i = p->code[lastpc];
    OpCode op = GET_OPCODE(i);
    switch (op)
    {
    case OP_GETTABUP:
    {
      int k = GETARG_C(i); /* key index */
      return {isEnv(p, lastpc, i, 1), kname(p, k).name};
    }
    case OP_GETTABLE:
    case OP_GETVARG:
    {
      int k = GETARG_C(i); /* key index */
      return {isEnv(p, lastpc, i, 0), rname(p, lastpc, k)};
    }
    case OP_GETI:
    {
      return {lt::field, &lt::integer_index_i};
    }
    case OP_GETFIELD:
    {
      int k = GETARG_C(i); /* key index */
      return {isEnv(p, lastpc, i, 0), kname(p, k).name};
    }
    case OP_SELF:
    {
      int k = GETARG_C(i); /* key index */
      return {lt::method, kname(p, k).name};
    }
    default:
      break;
    }
  }
  return {std::nullopt, basic.var.name}; /* could not find reasonable name */
}

/*
** Try to find a name for a function based on the code that called it.
** (Only works when function was called by a Lua function.)
** Returns what the name is (e.g., "for iterator", "method",
** "metamethod") and sets '*name' to point to the name.
*/
static VarName funcnamefromcode(const Proto* p, int pc)
{
  TMS tm = (TMS) 0;            /* (initial value avoids warnings) */
  Instruction i = p->code[pc]; /* calling instruction */
  switch (GET_OPCODE(i))
  {
  case OP_CALL:
  case OP_TAILCALL:
    return getobjname(p, pc, GETARG_A(i)); /* get function name */
  case OP_TFORCALL:
  { /* for iterator */
    return {lt::for_iterator_i.view(), &lt::for_iterator_i};
  }
  /* other instructions can do calls through metamethods */
  case OP_SELF:
  case OP_GETTABUP:
  case OP_GETTABLE:
  case OP_GETI:
  case OP_GETFIELD:
    tm = TM_INDEX;
    break;
  case OP_SETTABUP:
  case OP_SETTABLE:
  case OP_SETI:
  case OP_SETFIELD:
    tm = TM_NEWINDEX;
    break;
  case OP_MMBIN:
  case OP_MMBINI:
  case OP_MMBINK:
  {
    tm = (TMS)GETARG_C(i);
    break;
  }
  case OP_UNM:
    tm = TM_UNM;
    break;
  case OP_BNOT:
    tm = TM_BNOT;
    break;
  case OP_LEN:
    tm = TM_LEN;
    break;
  case OP_CONCAT:
    tm = TM_CONCAT;
    break;
  case OP_EQ:
    tm = TM_EQ;
    break;
  /* no cases for OP_EQI and OP_EQK, as they don't call metamethods */
  case OP_LT:
  case OP_LTI:
  case OP_GTI:
    tm = TM_LT;
    break;
  case OP_LE:
  case OP_LEI:
  case OP_GEI:
    tm = TM_LE;
    break;
  case OP_CLOSE:
  case OP_RETURN:
    tm = TM_CLOSE;
    break;
  default:
    return {std::nullopt, nullptr}; /* cannot find a reasonable name */
  }
  return {lt::metamethod, &tmplainname[tm]};
}

/*
** Try to find a name for a function based on how it was called.
*/
static VarName funcnamefromcall(lua_State* L, CallInfo* ci)
{
  if (ci->callstatus & CIST_HOOKED)
  { /* was it called inside a hook? */
    return {lt::hook, &lt::qmark_i};
  }
  else if (ci->callstatus & CIST_FIN)
  { /* was it called as a finalizer? */
    return {lt::metamethod, &lt::mm_gc_i}; /* report it as such */
  }
  else if (isLua(ci))
    return funcnamefromcode(ci_func(L, ci)->p, currentpc(L, ci));
  else
    return {std::nullopt, nullptr};
}

/* }====================================================== */

static byte_view formatvarinfo(lua_State* L, std::optional<byte_view> kind, const InternalString* name)
{
  if (!kind.has_value())
    return lt::empty_str; /* no information */
  else
    return luaO_pushfstring(L, lt::fmt_paren_s_quoted_s, FmtArgs().str(kind.value()).str(name->view()))->view();
}

static byte_view varinfo(lua_State* L, StkId src)
{
  CallInfo* ci = L->ci;
  const InternalString* name = nullptr; /* to avoid warnings */
  std::optional<byte_view> kind = std::nullopt;
  if (isLua(ci))
  {
    if (src <= UPVALORIGIN)
    {
      name = upvalname(ci_func(L, ci)->p, (int)(UPVALORIGIN - src));
      kind = lt::upvalue;
    }
    else if (src != NOSLOT)
    {
      StkId base = ci->func + 1;
      if (src >= base && src < ci->top)
      {

        VarName obj = getobjname(ci_func(L, ci)->p, currentpc(L, ci), (int)(src - base));
        kind = obj.kind;
        name = obj.name;
      }
    }
  }
  return formatvarinfo(L, kind, name);
}

/*
** Raise a type error
*/
[[noreturn]] static void typeerror(lua_State* L, const TValue* o, byte_view op, byte_view extra)
{
  byte_view t = luaT_objtypename(L, o);
  luaG_runerror(L, lt::attempt_to_s_a_s_value_s, FmtArgs().str(op).str(t).str(extra));
}

/*
** Raise a type error with "standard" information about the faulty
** object 'o' (using 'varinfo').
*/
[[noreturn]] void luaG_typeerror(lua_State* L, const TValue* o, byte_view op, StkId src)
{
  typeerror(L, o, op, varinfo(L, src));
}

/*
** Raise an error for calling a non-callable object. Try to find a name
** for the object based on how it was called ('funcnamefromcall'); if it
** cannot get a name there, try 'varinfo'.
*/
[[noreturn]] void luaG_callerror(lua_State* L, const TValue* o, StkId src)
{
  CallInfo* ci = L->ci;
  VarName fn = funcnamefromcall(L, ci);
  byte_view extra = fn.kind.has_value() ? formatvarinfo(L, fn.kind, fn.name) : varinfo(L, src);
  typeerror(L, o, lt::call, extra);
}

[[noreturn]] void luaG_forerror(lua_State* L, const TValue* o, byte_view what)
{
  luaG_runerror(L, lt::bad_for_s_number_expected_got_s, FmtArgs().str(what).str(luaT_objtypename(L, o)));
}

[[noreturn]] void luaG_concaterror(lua_State* L, const TValue* p1, const TValue* p2, StkId src1, StkId src2)
{
  if (ttisstring(p1) || cvt2str(p1))
  {
    p1 = p2;
    src1 = src2;
  }
  luaG_typeerror(L, p1, lt::concatenate, src1);
}

[[noreturn]] void luaG_opinterror(lua_State* L, const TValue* p1, const TValue* p2, byte_view msg, StkId src1, StkId src2)
{
  if (!ttisnumber(p1)) /* first operand is wrong? */
  {
    p2 = p1;           /* now second is wrong */
    src2 = src1;
  }
  luaG_typeerror(L, p2, msg, src2);
}

/*
** Error when both values are convertible to numbers, but not to integers
*/
[[noreturn]] void luaG_tointerror(lua_State* L, const TValue* p1, const TValue* p2, StkId src1, StkId src2)
{
  IntResult temp;
  (void)p2;
  if (!luaV_tointegerns(p1, &temp, LUA_FLOORN2I))
    src2 = src1;
  luaG_runerror(L, lt::number_s_has_no_integer_representation, FmtArgs().str(varinfo(L, src2)));
}

[[noreturn]] void luaG_ordererror(lua_State* L, const TValue* p1, const TValue* p2)
{
  byte_view t1 = luaT_objtypename(L, p1);
  byte_view t2 = luaT_objtypename(L, p2);
  if (t1 == t2)
    luaG_runerror(L, lt::attempt_to_compare_two_s_values, FmtArgs().str(t1));
  else
    luaG_runerror(L, lt::attempt_to_compare_s_with_s, FmtArgs().str(t1).str(t2));
}

[[noreturn]] void luaG_errnnil(lua_State* L, LClosure* cl, int k)
{
  const InternalString* globalname = &lt::qmark_i; /* default name if k == 0 */
  if (k > 0)
    globalname = kname(cl->p, k - 1).name;
  luaG_runerror(L, lt::global_s_already_defined, FmtArgs().str(globalname->view()));
}

/* add src:line information to 'msg' */
byte_view luaG_addinfo(lua_State* L, byte_view msg, TString* src, int line)
{
  if (src == nullptr) /* no debug information? */
    return luaO_pushfstring(L, lt::fmt_unknown_loc_s, FmtArgs().str(msg))->view();

  byte_inline<LUA_IDSIZE> bytes;
  luaO_chunkid(bytes, src->data.view());
  byte_view sv = bytes.str_view();
  return luaO_pushfstring(L, lt::fmt_s_d_s, FmtArgs().str(sv).num(line).str(msg))->view();
}

[[noreturn]] void luaG_errormsg(lua_State* L)
{
  if (L->errfunc != 0)
  { /* is there an error handling function? */
    StkId errfunc = (static_cast<StkId>(L->errfunc));
    assert(novariant(slottag(L, errfunc)) == LUA_TFUNCTION);
    copyslot(L, L->top, L->top - 1); /* move argument */
    copyslot(L, L->top - 1, errfunc);  /* push function */
    L->top++;                           /* assume EXTRA_STACK */
    luaD_callerrh(L);                   /* call it */
  }
  luaG_throwerror(L);
}

/* raise the error object at the top */
[[noreturn]] void luaG_throwerror(lua_State* L)
{
  if (tagisempty(slottag(L, L->top - 1)))
  { /* error object is nil? */
    /* change it to a proper message */
    byte_view errstr = lt::no_error_object;
    setslots(L, L->top - 1, TString::luaS_createstr(L, errstr));
  }
  luaD_throw(L, LUA_ERRRUN);
}

[[noreturn]] void luaG_runerror(lua_State* L, byte_view fmt,
                      const FmtArgs& args)
{
  CallInfo* ci = L->ci;
  const InternalString* msg;
  luaC_checkGC(L); /* error message uses memory */
  msg = luaO_pushvfstring(L, fmt, args);
  if (msg == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  if (isLua(ci))
  { /* Lua function? */
    /* add source:line information */
    luaG_addinfo(L, msg->view(), ci_func(L, ci)->p->source, getcurrentline(L, ci));
    copyslot(L, L->top - 2, L->top - 1); /* remove 'msg' */
    L->top--;
  }
  luaG_errormsg(L);
}

[[noreturn]] void luaG_runerror(lua_State* L, byte_view fmt)
{
  luaG_runerror(L, fmt, FmtArgs());
}

/*
** Check whether new instruction 'newpc' is in a different line from
** previous instruction 'oldpc'. More often than not, 'newpc' is only
** one or a few instructions after 'oldpc' (it must be after, see
** caller), so try to avoid calling 'luaG_getfuncline'. If they are
** too far apart, there is a good chance of a ABSLINEINFO in the way,
** so it goes directly to 'luaG_getfuncline'.
*/
static int changedline(const Proto* p, int oldpc, int newpc)
{
  if (p->lineinfo.data() == nullptr) /* no debug information? */
    return 0;
  if (newpc - oldpc < MAXIWTHABS / 2)
  {                /* not too far apart? */
    int delta = 0; /* line difference */
    int pc = oldpc;
    for (;;)
    {
      int lineinfo = p->lineinfo[++pc];
      if (lineinfo == ABSLINEINFO)
        break; /* cannot compute delta; fall through */
      delta += lineinfo;
      if (pc == newpc)
        return (delta != 0); /* delta computed successfully */
    }
  }
  /* either instructions are too far apart or there is an absolute line
     info in the way; compute line difference explicitly */
  return (luaG_getfuncline(p, oldpc) != luaG_getfuncline(p, newpc));
}

/*
** Traces Lua calls. If code is running the first instruction of a function,
** and function is not vararg, and it is not coming from an yield,
** calls 'luaD_hookcall'. (Vararg functions will call 'luaD_hookcall'
** after adjusting its variable arguments; otherwise, they could call
** a line/count hook before the call hook. Functions coming from
** an yield already called 'luaD_hookcall' before yielding.)
*/
int luaG_tracecall(lua_State* L)
{
  CallInfo* ci = L->ci;
  Proto* p = ci_func(L, ci)->p;
  ci->trap = 1; /* ensure hooks will be checked */
  if (ci->savedpc == 0)
  { /* first instruction (not resuming)? */
    if (isvararg(p))
    {
      luaE_spend(L, 1); /* (VARARGPREP runs untraced) */
      return 0;         /* hooks will start at VARARGPREP instruction */
    }
    else if (!(ci->callstatus & (CIST_HOOKYIELD | CIST_FROZE))) /* not yielded or frozen? */
      luaD_hookcall(L, ci);                      /* check 'call' hook */
  }
  return 1; /* keep 'trap' on */
}

/*
** The budget is spent before the instruction at 'savedpc - 1': call the
** budget callback, and freeze there if it asks to (the instruction runs
** when it thaws).
*/
static void budgetpoint(lua_State* L, CallInfo* ci, const Proto* p)
{
  if (!luaP_isIT(p->code[ci->savedpc - 1])) /* top not being used? */
    L->top = ci->top;                 /* correct top */
  lua_State* co = luaD_budget(L);
  if (co != nullptr)
  {
    ci->callstatus |= CIST_FROZE;
    luaD_freeze(L, co);
  }
}

/*
** At this instruction boundary of a budgeted thread, call the '__gc' that
** wait for its group (a restartable point: a freeze inside them goes on
** here). The thread's hooks are off while they run.
*/
static void drainpoint(lua_State* L, CallInfo* ci, const Proto* p)
{
  if (!luaP_isIT(p->code[ci->savedpc - 1])) /* top not being used? */
    L->top = ci->top;                 /* correct top */
  StkId top = L->top;
  StkId ci_top = ci->top;
  /* (the call of the '__gc' needs room: if it cannot be had here without an
     error, they wait for another point; the program sees no error) */
  if (L->stack_last - L->top <= LUA_MINSTACK + 1 && !luaD_growstack(L, LUA_MINSTACK + 1, 0))
    return;
  if ((ci->next == nullptr && luaE_extendCI(L, 0) == nullptr) || getCcalls(L) + 2 >= LUAI_MAXCCALLS)
    return;
  if (ci->top < L->top + LUA_MINSTACK + 1)
    ci->top = L->top + LUA_MINSTACK + 1;
  ci->callstatus |= CIST_FROZE; /* (if it freezes, it goes on at this instruction) */
  luaC_pushdrain(L);
  luaD_call(L, L->top - 1, 0);
  ci->callstatus &= ~CIST_FROZE;
  ci->top = ci_top;
  L->top = top;
}

int luaG_traceexec(lua_State* L, int pc)
{
  CallInfo* ci = L->ci;
  lu_byte mask = (lu_byte)L->hookmask;
  const Proto* p = ci_func(L, ci)->p;
  int counthook;
  if (!(mask & (LUA_MASKLINE | LUA_MASKCOUNT | LUA_MASKBUDGET)))
  {                   /* no hooks? */
    ci->trap = 0; /* don't need to stop again */
    return 0;         /* turn off 'trap' */
  }
  pc++;                 /* reference is always next instruction */
  ci->savedpc = pc; /* save 'pc' */
  if ((mask & LUA_MASKBUDGET) && !(ci->callstatus & CIST_HOOKYIELD))
  { /* a step of the budget (after its callback, if it is spent) */
    global_State* g = L->l_G;
    BudgetGroup* bg = L->beff;
    /* (the first point after a thaw calls no '__gc': if no step was taken
       since, the VM passed no other point; else that one was the first) */
    bool first = bg->nodrain && bg->left == bg->budget;
    bg->nodrain = false;
    if (!first && bg->fnz.next != nullptr && L->allowhook && g->budgetL == nullptr && luaD_canfreeze(L))
      drainpoint(L, ci, p);
    if (L->beff != nullptr && L->beff->left <= 0 && g->budgetL == nullptr)
      budgetpoint(L, ci, p);
    if (L->beff != nullptr) /* (the callback may have removed it) */
      L->beff->left--;
  }
  if (!(mask & (LUA_MASKLINE | LUA_MASKCOUNT)))
    return 1; /* only the budget */
  /* A hook function that freezes is finished after a thaw ('unroll'), and
     this instruction is traced again: 'hookdone' says what of it has been
     done already (the count, which also stays as it is, and the line
     hook), and the hooks left run now. */
  bool refrozen = (ci->hookdone != 0); /* (not a hook that yielded: that is CIST_HOOKYIELD alone) */
  if (refrozen)
    ci->callstatus &= ~CIST_HOOKYIELD; /* (it kept the budget from counting the instruction again) */
  counthook = 0;
  if ((mask & LUA_MASKCOUNT) && !(ci->hookdone & HD_COUNT))
  {
    counthook = (--L->hookcount == 0);
    ci->hookdone |= HD_COUNT;
  }
  if (counthook)
    resethookcount(L); /* reset count */
  else if (!(mask & LUA_MASKLINE))
  { /* no line hook and count != 0; nothing to be done now */
    ci->hookdone = 0;
    return 1;
  }
  if ((ci->callstatus & CIST_HOOKYIELD) && !refrozen)
  {                                    /* hook yielded last time? */
    ci->callstatus &= ~CIST_HOOKYIELD; /* erase mark */
    ci->hookdone = 0;
    return 1;                          /* do not call hook again (VM yielded, so it did not move) */
  }
  ci->callstatus &= ~CIST_HOOKYIELD;
  if (!luaP_isIT(p->code[ci->savedpc - 1])) /* top not being used? */
    L->top = ci->top;                 /* correct top */
  ci->hookev = HE_INSTR;
  if (counthook)
  {
    ci->callstatus |= CIST_FROZE | CIST_HOOKYIELD; /* (if the hook function freezes) */
    luaD_hook(L, LUA_HOOKCOUNT, -1, 0, 0); /* call count hook */
    ci->callstatus &= ~(CIST_FROZE | CIST_HOOKYIELD);
  }
  if (mask & LUA_MASKLINE)
  {
    /* 'L->oldpc' may be invalid; use zero in this case */
    int oldpc = (L->oldpc < (int)p->code.size()) ? L->oldpc : 0;
    int npci = pc - 1;
    if (!(ci->hookdone & HD_LINE) &&
        (npci <= oldpc || /* call hook when jump back (loop), */
         changedline(p, oldpc, npci)))
    { /* or when enter new line */
      int newline = luaG_getfuncline(p, npci);
      ci->hookdone |= HD_LINE;
      ci->callstatus |= CIST_FROZE | CIST_HOOKYIELD;
      luaD_hook(L, LUA_HOOKLINE, newline, 0, 0); /* call line hook */
      ci->callstatus &= ~(CIST_FROZE | CIST_HOOKYIELD);
    }
    L->oldpc = npci; /* 'pc' of last call to line hook */
  }
  ci->hookdone = 0;
  if (L->status == LUA_YIELD)
  { /* did hook yield? */
    if (counthook)
      L->hookcount = 1;               /* undo decrement to zero */
    ci->callstatus |= CIST_HOOKYIELD; /* mark that it yielded */
    luaD_throw(L, LUA_YIELD);
  }
  return 1; /* keep 'trap' on */
}

}
