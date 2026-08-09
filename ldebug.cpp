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

static inline int LuaClosure(const Closure* f)
{
  return f != nullptr && f->tt == LUA_VLCL;
}

static const InternalString* const strlocal = lit<"local">();
static const InternalString* const strupval = lit<"upvalue">();
static const InternalString* const strconstant = lit<"constant">();

static const InternalString* const tmplainname[] = {
    lit<"index">(), lit<"newindex">(),
    lit<"gc">(), lit<"mode">(), lit<"len">(), lit<"eq">(),
    lit<"add">(), lit<"sub">(), lit<"mul">(), lit<"mod">(), lit<"pow">(),
    lit<"div">(), lit<"idiv">(),
    lit<"band">(), lit<"bor">(), lit<"bxor">(), lit<"shl">(), lit<"shr">(),
    lit<"unm">(), lit<"bnot">(), lit<"lt">(), lit<"le">(),
    lit<"concat">(), lit<"call">(), lit<"close">()};
static_assert(sizeof(tmplainname) / sizeof(tmplainname[0]) == TM_N,
              "tmplainname must cover every metamethod (ORDER TM)");

static const InternalString* funcnamefromcall(lua_State* L, CallInfo* ci,
                                              const InternalString** name);

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
static int getbaseline(const Proto* f, int pc, int* basepc)
{
  if (f->sizeabslineinfo == 0 || pc < f->abslineinfo[0].pc)
  {
    *basepc = -1; /* start from the beginning */
    return f->linedefined;
  }
  else
  {
    int i = pc / MAXIWTHABS - 1; /* get an estimate */
    /* estimate must be a lower bound of the correct base */
    assert(i < 0 ||
               (i < f->sizeabslineinfo && f->abslineinfo[i].pc <= pc));
    while (i + 1 < f->sizeabslineinfo && pc >= f->abslineinfo[i + 1].pc)
      i++; /* low estimate; adjust it */
    *basepc = f->abslineinfo[i].pc;
    return f->abslineinfo[i].line;
  }
}

/*
** Get the line corresponding to instruction 'pc' in function 'f';
** first gets a base line and from there does the increments until
** the desired instruction.
*/
int luaG_getfuncline(const Proto* f, int pc)
{
  if (f->lineinfo == nullptr) /* no debug information? */
    return -1;
  else
  {
    int basepc;
    int baseline = getbaseline(f, pc, &basepc);
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
LUA_API void lua_sethook(lua_State* L, lua_Hook func, int mask, int count)
{
  if (func == nullptr || mask == 0)
  { /* turn off hooks? */
    mask = 0;
    func = nullptr;
  }
  L->hook = func;
  L->basehookcount = count;
  resethookcount(L);
  L->hookmask = ((lu_byte)(mask));
  if (mask)
    settraps(L->ci); /* to trace inside 'luaV_execute' */
}

LUA_API lua_Hook lua_gethook(lua_State* L)
{
  return L->hook;
}

LUA_API int lua_gethookmask(lua_State* L)
{
  return L->hookmask;
}

LUA_API int lua_gethookcount(lua_State* L)
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

LUA_API std::optional<lua_Debug> lua_getstack(lua_State* L, int level)
{
  std::optional<InternalDebug> iar = lua_getstacki(L, level);
  if (!iar)
    return std::nullopt;
  lua_Debug ar{};
  ar.i_ci = iar->i_ci;
  return ar;
}

static const InternalString* upvalname(const Proto* p, int uv)
{
  assert(uv < p->sizeupvalues);
  TString* s = p->upvalues[uv].name;
  if (s == nullptr)
    return lit<"?">();
  else
    return &s->data;
}

static const InternalString* findvararg(lua_State* L, CallInfo* ci, int n, StkId* pos)
{
  StkId fv = ci->func;
  LClosure* cl = slotclLval(L, fv);
  if (cl->p->flag & PF_VAHID)
  {
    int nextra = ci->nextraargs;
    if (n >= -nextra)
    { /* 'n' is negative */
      *pos = ci->func - nextra - (n + 1);
      return lit<"(vararg)">(); /* generic name for any vararg */
    }
  }
  return nullptr; /* no such vararg */
}

const InternalString* luaG_findlocal(lua_State* L, CallInfo* ci, int n, StkId* pos)
{
  StkId base = ci->func + 1;
  const InternalString* name = nullptr;
  if (isLua(ci))
  {
    if (n < 0) /* access to vararg values? */
      return findvararg(L, ci, n, pos);
    else
      name = luaF_getlocalname(ci_func(L, ci)->p, n, currentpc(L, ci));
  }
  if (name == nullptr)
  { /* no 'standard' name? */
    StkId limit = (ci == L->ci) ? L->top : ci->next->func;
    if (limit - base >= ((StkId)(n)) && n > 0)
    { /* is 'n' inside 'ci' stack? */
      /* generic name for any valid slot */
      name = isLua(ci) ? lit<"(temporary)">() : lit<"(C temporary)">();
    }
    else
      return nullptr; /* no name */
  }
  if (pos)
    *pos = base + (n - 1);
  return name;
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
      name = luaF_getlocalname(slotclLval(L, fv)->p, n, 0);
    }
  }
  else
  {                   /* active function; get information through 'ar' */
    StkId pos = 0; /* to avoid warnings */
    name = luaG_findlocal(L, ar->i_ci, n, &pos);
    if (name)
    {
      copyslot(L, L->top, pos);
      api_incr_top(L);
    }
  }
  lua_unlock(L);
  return name;
}

LUA_API std::optional<HostString> lua_getlocal(lua_State* L, const lua_Debug* ar, int n)
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
  StkId pos = 0; /* to avoid warnings */
  const InternalString* name;
  lua_lock(L);
  name = luaG_findlocal(L, ar->i_ci, n, &pos);
  if (name)
  {
    api_checkpop(L, 1);
    copyslot(L, pos, L->top - 1);
    L->top--; /* pop value */
  }
  lua_unlock(L);
  return name;
}

LUA_API std::optional<HostString> lua_setlocal(lua_State* L, const lua_Debug* ar, int n)
{
  InternalDebug iar{};
  iar.i_ci = ar->i_ci;
  const InternalString* name = lua_setlocali(L, &iar, n);
  if (name == nullptr)
    return std::nullopt;
  return HostString(name->view());
}

static void funcinfo(InternalDebug* ar, Closure* cl)
{
  if (!LuaClosure(cl))
  {
    ar->source = lit<"=[C]">()->view();
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = lit<"C">()->view();
  }
  else
  {
    const Proto* p = static_cast<LClosure*>(cl)->p;
    if (p->source)
      ar->source = p->source->data.view();
    else
      ar->source = lit<"=?">()->view();
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? lit<"main">()->view() : lit<"Lua">()->view();
  }
  luaO_chunkid(ar->short_src, ar->source);
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
    Table* t = luaH_new(L);      /* new table to store active lines */
    setsloth(L, L->top, t); /* push it on stack */
    api_incr_top(L);
    if (p->lineinfo != nullptr)
    { /* proto with debug information? */
      int i;
      TValue v;
      ((&v)->tt_ = LUA_VTRUE);     /* boolean 'true' to be the value of all indices */
      if (!(isvararg(p))) /* regular function? */
        i = 0;            /* consider all instructions */
      else
      { /* vararg function */
        assert(GET_OPCODE(p->code[0]) == OP_VARARGPREP);
        currentline = nextline(p, currentline, 0);
        i = 1; /* skip first instruction (OP_VARARGPREP) */
      }
      for (; i < p->sizelineinfo; i++)
      {                                            /* for each instruction */
        currentline = nextline(p, currentline, i); /* get its line */
        luaH_setint(L, t, currentline, &v);        /* table[line] = true */
      }
    }
  }
}

static const InternalString* getfuncname(lua_State* L, CallInfo* ci,
                                         const InternalString** name)
{
  /* calling function is a known function? */
  if (ci != nullptr && !(ci->callstatus & CIST_TAIL))
    return funcnamefromcall(L, ci->previous, name);
  else
    return nullptr; /* no way to find a name */
}

static int auxgetinfo(lua_State* L, std::string_view what, size_t wpos,
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
      ar->nups = (f == nullptr) ? 0 : f->nupvalues;
      if (!LuaClosure(f))
      {
        ar->isvararg = 1;
        ar->nparams = 0;
      }
      else
      {
        ar->isvararg = (isvararg(static_cast<LClosure*>(f)->p)) ? 1 : 0;
        ar->nparams = static_cast<LClosure*>(f)->p->numparams;
      }
      break;
    }
    case 't':
    {
      if (ci != nullptr)
      {
        ar->istailcall = !!(ci->callstatus & CIST_TAIL);
        ar->extraargs =
            ((unsigned char)((ci->callstatus & MAX_CCMT) >> CIST_CCMT));
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
      const InternalString* name = nullptr;
      const InternalString* namewhat = getfuncname(L, ci, &name);
      ar->namewhat = (namewhat != nullptr) ? namewhat->view() : std::string_view{};
      ar->name = (namewhat != nullptr && name != nullptr) ? name->view() : std::string_view{};
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

std::optional<InternalDebug> lua_getinfoi(lua_State* L, std::string_view what,
                                          const InternalDebug& arin)
{
  int status;
  Closure* cl;
  CallInfo* ci;
  StkId func;
  InternalDebug ar = arin;
  size_t wpos = 0;
  lua_lock(L);
  if (sv_byteat(what, 0) == '>')
  {
    ci = nullptr;
    func = L->top - 1;
    assert((slottag(L, func) & NOVARIANT_MASK) == LUA_TFUNCTION && "function expected");
    wpos = 1;   /* skip the '>' */
    L->top--;
  }
  else
  {
    ci = ar.i_ci;
    func = ci->func;
    assert((slottag(L, func) & NOVARIANT_MASK) == LUA_TFUNCTION);
  }
  if (slottag(L, func) == ctb(LUA_VLCL) || slottag(L, func) == ctb(LUA_VCCL))
  {
    cl = slotclval(L, func);
    assert((cl->tt & NOVARIANT_MASK) == LUA_TFUNCTION);
  }
  else
    cl = nullptr;
  status = auxgetinfo(L, what, wpos, &ar, cl, ci);
  if (what.find('f', wpos) != std::string_view::npos)
  {
    copyslot(L, L->top, func);
    api_incr_top(L);
  }
  if (what.find('L', wpos) != std::string_view::npos)
    collectvalidlines(L, cl);
  lua_unlock(L);
  if (!status)
    return std::nullopt; /* invalid option */
  return ar;
}

LUA_API std::optional<lua_Debug> lua_getinfo(lua_State* L, const HostString& options,
                                             const lua_Debug& arin)
{
  std::string_view what = options.to_utf8();
  InternalDebug iar{};
  iar.event = arin.event;
  iar.i_ci = arin.i_ci;
  std::optional<InternalDebug> res = lua_getinfoi(L, what, iar);
  if (!res)
    return std::nullopt; /* invalid option */
  lua_Debug ar = arin;
  ar.i_ci = res->i_ci;
  if (what.find('S') != std::string_view::npos)
  {
    ar.source = res->source;
    ar.short_src = HostString(res->short_src);
    ar.what = HostString(res->what);
    ar.linedefined = res->linedefined;
    ar.lastlinedefined = res->lastlinedefined;
  }
  if (what.find('l') != std::string_view::npos)
    ar.currentline = res->currentline;
  if (what.find('u') != std::string_view::npos)
  {
    ar.nups = res->nups;
    ar.nparams = res->nparams;
    ar.isvararg = res->isvararg;
  }
  if (what.find('t') != std::string_view::npos)
  {
    ar.istailcall = res->istailcall;
    ar.extraargs = res->extraargs;
  }
  if (what.find('n') != std::string_view::npos)
  {
    ar.namewhat = (res->namewhat.data() != nullptr) ? HostString(res->namewhat) : HostString{};
    ar.name = (res->name.data() != nullptr) ? HostString(res->name) : HostString{};
  }
  if (what.find('r') != std::string_view::npos)
  {
    ar.ftransfer = res->ftransfer;
    ar.ntransfer = res->ntransfer;
  }
  return ar;
}

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
static const InternalString* kname(const Proto* p, int index,
                                   const InternalString** name)
{
  TValue* kvalue = &p->k[index];
  if ((((kvalue)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
  {
    *name = &tsvalue(kvalue)->data;
    return strconstant;
  }
  else
  {
    *name = lit<"?">();
    return nullptr;
  }
}

static const InternalString* basicgetobjname(const Proto* p, int* ppc, int reg,
                                             const InternalString** name)
{
  int pc = *ppc;
  *name = luaF_getlocalname(p, reg + 1, pc);
  if (*name) /* is a local? */
    return strlocal;
  /* else try symbolic execution */
  *ppc = pc = findsetreg(p, pc, reg);
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
        return basicgetobjname(p, ppc, b, name); /* get name for 'b' */
      break;
    }
    case OP_GETUPVAL:
    {
      *name = upvalname(p, GETARG_B(i));
      return strupval;
    }
    case OP_LOADK:
      return kname(p, GETARG_Bx(i), name);
    case OP_LOADKX:
      return kname(p, GETARG_Ax(p->code[pc + 1]), name);
    default:
      break;
    }
  }
  return nullptr; /* could not find reasonable name */
}

/*
** Find a "name" for the register 'c'.
*/
static void rname(const Proto* p, int pc, int c, const InternalString** name)
{
  const InternalString* what = basicgetobjname(p, &pc, c, name); /* search for 'c' */
  if (what != strconstant) /* did not find a constant name? */
    *name = lit<"?">();
}

/*
** Check whether table being indexed by instruction 'i' is the
** environment '_ENV'
*/
static const InternalString* isEnv(const Proto* p, int pc, Instruction i, int isup)
{
  int t = GETARG_B(i);       /* table index */
  const InternalString* name; /* name of indexed variable */
  if (isup)                  /* is 't' an upvalue? */
    name = upvalname(p, t);
  else
  { /* 't' is a register */
    const InternalString* what = basicgetobjname(p, &pc, t, &name);
    /* 'name' must be the name of a local variable (at the current
       level or an upvalue) */
    if (what != strlocal && what != strupval)
      name = nullptr; /* cannot be the variable _ENV */
  }
  return (name != nullptr && name->view() == LUA_ENV->view())
             ? lit<"global">()
             : lit<"field">();
}

/*
** Extend 'basicgetobjname' to handle table accesses
*/
static const InternalString* getobjname(const Proto* p, int lastpc, int reg,
                                        const InternalString** name)
{
  const InternalString* kind = basicgetobjname(p, &lastpc, reg, name);
  if (kind != nullptr)
    return kind;
  else if (lastpc != -1)
  { /* could find instruction? */
    Instruction i = p->code[lastpc];
    OpCode op = GET_OPCODE(i);
    switch (op)
    {
    case OP_GETTABUP:
    {
      int k = GETARG_C(i); /* key index */
      kname(p, k, name);
      return isEnv(p, lastpc, i, 1);
    }
    case OP_GETTABLE:
    case OP_GETVARG:
    {
      int k = GETARG_C(i); /* key index */
      rname(p, lastpc, k, name);
      return isEnv(p, lastpc, i, 0);
    }
    case OP_GETI:
    {
      *name = lit<"integer index">();
      return lit<"field">();
    }
    case OP_GETFIELD:
    {
      int k = GETARG_C(i); /* key index */
      kname(p, k, name);
      return isEnv(p, lastpc, i, 0);
    }
    case OP_SELF:
    {
      int k = GETARG_C(i); /* key index */
      kname(p, k, name);
      return lit<"method">();
    }
    default:
      break;
    }
  }
  return nullptr; /* could not find reasonable name */
}

/*
** Try to find a name for a function based on the code that called it.
** (Only works when function was called by a Lua function.)
** Returns what the name is (e.g., "for iterator", "method",
** "metamethod") and sets '*name' to point to the name.
*/
static const InternalString* funcnamefromcode(const Proto* p, int pc,
                                              const InternalString** name)
{
  TMS tm = (TMS) 0;            /* (initial value avoids warnings) */
  Instruction i = p->code[pc]; /* calling instruction */
  switch (GET_OPCODE(i))
  {
  case OP_CALL:
  case OP_TAILCALL:
    return getobjname(p, pc, GETARG_A(i), name); /* get function name */
  case OP_TFORCALL:
  { /* for iterator */
    *name = lit<"for iterator">();
    return lit<"for iterator">();
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
    tm = ((TMS)(GETARG_C(i)));
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
    return nullptr; /* cannot find a reasonable name */
  }
  *name = tmplainname[tm];
  return lit<"metamethod">();
}

/*
** Try to find a name for a function based on how it was called.
*/
static const InternalString* funcnamefromcall(lua_State* L, CallInfo* ci,
                                              const InternalString** name)
{
  if (ci->callstatus & CIST_HOOKED)
  { /* was it called inside a hook? */
    *name = lit<"?">();
    return lit<"hook">();
  }
  else if (ci->callstatus & CIST_FIN)
  { /* was it called as a finalizer? */
    *name = lit<"__gc">();
    return lit<"metamethod">(); /* report it as such */
  }
  else if (isLua(ci))
    return funcnamefromcode(ci_func(L, ci)->p, currentpc(L, ci), name);
  else
    return nullptr;
}

/* }====================================================== */

static const InternalString* formatvarinfo(lua_State* L,
                                           const InternalString* kind,
                                           const InternalString* name)
{
  if (kind == nullptr)
    return lit<"">(); /* no information */
  else
    return luaO_pushfstring(L, lit<" (%s '%s')">()->view(), FmtArgs().str(kind->view()).str(name->view()));
}

static const InternalString* varinfo(lua_State* L, StkId src)
{
  CallInfo* ci = L->ci;
  const InternalString* name = nullptr; /* to avoid warnings */
  const InternalString* kind = nullptr;
  if (isLua(ci))
  {
    if (src >= UPVALORIGIN && src != NOSLOT)
    {
      name = upvalname(ci_func(L, ci)->p, (int)(src - UPVALORIGIN));
      kind = strupval;
    }
    else if (src != NOSLOT)
    {
      StkId base = ci->func + 1;
      if (src >= base && src < ci->top)
        kind = getobjname(ci_func(L, ci)->p, currentpc(L, ci),
                          (int)(src - base), &name);
    }
  }
  return formatvarinfo(L, kind, name);
}

/*
** Raise a type error
*/
static l_noret typeerror(lua_State* L, const TValue* o, std::string_view op, const InternalString* extra)
{
  const InternalString* t = luaT_objtypename(L, o);
  luaG_runerror(L, lit<"attempt to %s a %s value%s">()->view(), FmtArgs().str(op).str(t->view()).str(extra->view()));
}

/*
** Raise a type error with "standard" information about the faulty
** object 'o' (using 'varinfo').
*/
l_noret luaG_typeerror(lua_State* L, const TValue* o, std::string_view op, StkId src)
{
  typeerror(L, o, op, varinfo(L, src));
}

/*
** Raise an error for calling a non-callable object. Try to find a name
** for the object based on how it was called ('funcnamefromcall'); if it
** cannot get a name there, try 'varinfo'.
*/
l_noret luaG_callerror(lua_State* L, const TValue* o, StkId src)
{
  CallInfo* ci = L->ci;
  const InternalString* name = nullptr; /* to avoid warnings */
  const InternalString* kind = funcnamefromcall(L, ci, &name);
  const InternalString* extra = kind ? formatvarinfo(L, kind, name) : varinfo(L, src);
  typeerror(L, o, lit<"call">()->view(), extra);
}

l_noret luaG_forerror(lua_State* L, const TValue* o, std::string_view what)
{
  luaG_runerror(L, lit<"bad 'for' %s (number expected, got %s)">()->view(), FmtArgs().str(what).str(luaT_objtypename(L, o)->view()));
}

l_noret luaG_concaterror(lua_State* L, const TValue* p1, const TValue* p2,
                         StkId src1, StkId src2)
{
  if ((((p1)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) || cvt2str(p1))
  {
    p1 = p2;
    src1 = src2;
  }
  luaG_typeerror(L, p1, lit<"concatenate">()->view(), src1);
}

l_noret luaG_opinterror(lua_State* L, const TValue* p1, const TValue* p2, std::string_view msg, StkId src1, StkId src2)
{
  if (!(((p1)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)) /* first operand is wrong? */
  {
    p2 = p1;           /* now second is wrong */
    src2 = src1;
  }
  luaG_typeerror(L, p2, msg, src2);
}

/*
** Error when both values are convertible to numbers, but not to integers
*/
l_noret luaG_tointerror(lua_State* L, const TValue* p1, const TValue* p2,
                        StkId src1, StkId src2)
{
  lua_Integer temp;
  (void)p2;
  if (!luaV_tointegerns(p1, &temp, LUA_FLOORN2I))
    src2 = src1;
  luaG_runerror(L, lit<"number%s has no integer representation">()->view(),
                FmtArgs().str(varinfo(L, src2)->view()));
}

l_noret luaG_ordererror(lua_State* L, const TValue* p1, const TValue* p2)
{
  const InternalString* t1 = luaT_objtypename(L, p1);
  const InternalString* t2 = luaT_objtypename(L, p2);
  if (*t1 == *t2)
    luaG_runerror(L, lit<"attempt to compare two %s values">()->view(), FmtArgs().str(t1->view()));
  else
    luaG_runerror(L, lit<"attempt to compare %s with %s">()->view(), FmtArgs().str(t1->view()).str(t2->view()));
}

l_noret luaG_errnnil(lua_State* L, LClosure* cl, int k)
{
  const InternalString* globalname = lit<"?">(); /* default name if k == 0 */
  if (k > 0)
    kname(cl->p, k - 1, &globalname);
  luaG_runerror(L, lit<"global '%s' already defined">()->view(), FmtArgs().str(globalname->view()));
}

/* add src:line information to 'msg' */
const InternalString* luaG_addinfo(lua_State* L, std::string_view msg, TString* src, int line)
{
  if (src == nullptr) /* no debug information? */
    return luaO_pushfstring(L, lit<"?:?: %s">()->view(), FmtArgs().str(msg));
  else
  {
    char buff[LUA_IDSIZE];
    luaO_chunkid(buff, src->data.view());
    return luaO_pushfstring(L, lit<"%s:%d: %s">()->view(), FmtArgs().str(std::string_view{buff, strlen(buff)}).num(line).str(msg));
  }
}

l_noret luaG_errormsg(lua_State* L)
{
  if (L->errfunc != 0)
  { /* is there an error handling function? */
    StkId errfunc = (static_cast<StkId>(L->errfunc));
    assert((slottag(L, errfunc) & NOVARIANT_MASK) == LUA_TFUNCTION);
    copyslot(L, L->top, L->top - 1); /* move argument */
    copyslot(L, L->top - 1, errfunc);  /* push function */
    L->top++;                           /* assume EXTRA_STACK */
    luaD_callnoyield(L, L->top - 2, 1); /* call it */
  }
  if ((slottag(L, L->top - 1) & NOVARIANT_MASK) == LUA_TNIL)
  { /* error object is nil? */
    /* change it to a proper message */
    const InternalString* errstr = lit<"<no error object>">();
    setslots(L, L->top - 1, luaS_createstr(L, errstr->view(), 0, errstr->size()));
  }
  luaD_throw(L, LUA_ERRRUN);
}

l_noret luaG_runerror(lua_State* L, std::string_view fmt,
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

l_noret luaG_runerror(lua_State* L, std::string_view fmt)
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
  if (p->lineinfo == nullptr) /* no debug information? */
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
      return 0;                                  /* hooks will start at VARARGPREP instruction */
    else if (!(ci->callstatus & CIST_HOOKYIELD)) /* not yielded? */
      luaD_hookcall(L, ci);                      /* check 'call' hook */
  }
  return 1; /* keep 'trap' on */
}

int luaG_traceexec(lua_State* L, int pc)
{
  CallInfo* ci = L->ci;
  lu_byte mask = ((lu_byte)(L->hookmask));
  const Proto* p = ci_func(L, ci)->p;
  int counthook;
  if (!(mask & (LUA_MASKLINE | LUA_MASKCOUNT)))
  {                   /* no hooks? */
    ci->trap = 0; /* don't need to stop again */
    return 0;         /* turn off 'trap' */
  }
  pc++;                 /* reference is always next instruction */
  ci->savedpc = pc; /* save 'pc' */
  counthook = (mask & LUA_MASKCOUNT) && (--L->hookcount == 0);
  if (counthook)
    resethookcount(L); /* reset count */
  else if (!(mask & LUA_MASKLINE))
    return 1; /* no line hook and count != 0; nothing to be done now */
  if (ci->callstatus & CIST_HOOKYIELD)
  {                                    /* hook yielded last time? */
    ci->callstatus &= ~CIST_HOOKYIELD; /* erase mark */
    return 1;                          /* do not call hook again (VM yielded, so it did not move) */
  }
  if (!luaP_isIT(p->code[ci->savedpc - 1])) /* top not being used? */
    L->top = ci->top;                 /* correct top */
  if (counthook)
    luaD_hook(L, LUA_HOOKCOUNT, -1, 0, 0); /* call count hook */
  if (mask & LUA_MASKLINE)
  {
    /* 'L->oldpc' may be invalid; use zero in this case */
    int oldpc = (L->oldpc < p->sizecode) ? L->oldpc : 0;
    int npci = pc - 1;
    if (npci <= oldpc || /* call hook when jump back (loop), */
        changedline(p, oldpc, npci))
    { /* or when enter new line */
      int newline = luaG_getfuncline(p, npci);
      luaD_hook(L, LUA_HOOKLINE, newline, 0, 0); /* call line hook */
    }
    L->oldpc = npci; /* 'pc' of last call to line hook */
  }
  if (L->status == LUA_YIELD)
  { /* did hook yield? */
    if (counthook)
      L->hookcount = 1;               /* undo decrement to zero */
    ci->callstatus |= CIST_HOOKYIELD; /* mark that it yielded */
    luaD_throw(L, LUA_YIELD);
  }
  return 1; /* keep 'trap' on */
}
