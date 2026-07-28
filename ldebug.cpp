/*
** $Id: ldebug.c $
** Debug Interface
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

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

static inline int LuaClosure(const Closure* f)
{
  return f != nullptr && f->tt == LUA_VLCL;
}

static const char strlocal[] = "local";
static const char strupval[] = "upvalue";

static const char* funcnamefromcall(lua_State* L, CallInfo* ci,
                                    const char** name);

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

LUA_API int lua_getstack(lua_State* L, int level, lua_Debug* ar)
{
  int status;
  CallInfo* ci;
  if (level < 0)
    return 0; /* invalid (negative) level */
  lua_lock(L);
  for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
    level--;
  if (level == 0 && ci != &L->base_ci)
  { /* level found? */
    status = 1;
    ar->i_ci = ci;
  }
  else
    status = 0; /* no such level */
  lua_unlock(L);
  return status;
}

static const char* upvalname(const Proto* p, int uv)
{
  assert(uv < p->sizeupvalues);
  TString* s = p->upvalues[uv].name;
  if (s == nullptr)
    return "?";
  else
    return getstr(s);
}

static const char* findvararg(lua_State* L, CallInfo* ci, int n, StkId* pos)
{
  StkId fv = ci->func;
  LClosure* cl = slotclLval(L, fv);
  if (cl->p->flag & PF_VAHID)
  {
    int nextra = ci->nextraargs;
    if (n >= -nextra)
    { /* 'n' is negative */
      *pos = ci->func - nextra - (n + 1);
      return "(vararg)"; /* generic name for any vararg */
    }
  }
  return nullptr; /* no such vararg */
}

const char* luaG_findlocal(lua_State* L, CallInfo* ci, int n, StkId* pos)
{
  StkId base = ci->func + 1;
  const char* name = nullptr;
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
      name = isLua(ci) ? "(temporary)" : "(C temporary)";
    }
    else
      return nullptr; /* no name */
  }
  if (pos)
    *pos = base + (n - 1);
  return name;
}

LUA_API const char* lua_getlocal(lua_State* L, const lua_Debug* ar, int n)
{
  const char* name;
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

LUA_API const char* lua_setlocal(lua_State* L, const lua_Debug* ar, int n)
{
  StkId pos = 0; /* to avoid warnings */
  const char* name;
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

static void funcinfo(lua_Debug* ar, Closure* cl)
{
  if (!LuaClosure(cl))
  {
    ar->source = "=[C]";
    ar->srclen = (sizeof("=[C]") / sizeof(char) - 1);
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = "C";
  }
  else
  {
    const Proto* p = static_cast<LClosure*>(cl)->p;
    if (p->source)
    {
      ar->source = ((void)((ar->srclen) = (p->source)->content.size()), (p->source)->content.c_str());
    }
    else
    {
      ar->source = "=?";
      ar->srclen = (sizeof("=?") / sizeof(char) - 1);
    }
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? "main" : "Lua";
  }
  luaO_chunkid(ar->short_src, ar->source, ar->srclen);
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

static const char* getfuncname(lua_State* L, CallInfo* ci, const char** name)
{
  /* calling function is a known function? */
  if (ci != nullptr && !(ci->callstatus & CIST_TAIL))
    return funcnamefromcall(L, ci->previous, name);
  else
    return nullptr; /* no way to find a name */
}

static int auxgetinfo(lua_State* L, const char* what, lua_Debug* ar,
                      Closure* f, CallInfo* ci)
{
  int status = 1;
  for (; *what; what++)
  {
    switch (*what)
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
      ar->namewhat = getfuncname(L, ci, &ar->name);
      if (ar->namewhat == nullptr)
      {
        ar->namewhat = ""; /* not found */
        ar->name = nullptr;
      }
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

LUA_API int lua_getinfo(lua_State* L, const char* what, lua_Debug* ar)
{
  int status;
  Closure* cl;
  CallInfo* ci;
  TValue func; /* row copy of the function value */
  lua_lock(L);
  if (*what == '>')
  {
    ci = nullptr;
    func = getslot(L, L->top - 1);
    assert(((((&func)->tt_ & NOVARIANT_MASK) == LUA_TFUNCTION)) && "function expected");
    what++;     /* skip the '>' */
    L->top--; /* pop function */
  }
  else
  {
    ci = ar->i_ci;
    func = getslot(L, ci->func);
    assert((((&func)->tt_ & NOVARIANT_MASK) == LUA_TFUNCTION));
  }
  if ((&func)->tt_ == ctb(LUA_VLCL) || (&func)->tt_ == ctb(LUA_VCCL))
  {
    assert((gcvalue(&func)->tt & NOVARIANT_MASK) == LUA_TFUNCTION);
    cl = clvalue(&func);
  }
  else
    cl = nullptr;
  status = auxgetinfo(L, what, ar, cl, ci);
  if (strchr(what, 'f'))
  {
    setslot(L, L->top, &func);
    api_incr_top(L);
  }
  if (strchr(what, 'L'))
    collectvalidlines(L, cl);
  lua_unlock(L);
  return status;
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
static const char* kname(const Proto* p, int index, const char** name)
{
  TValue* kvalue = &p->k[index];
  if ((((kvalue)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
  {
    *name = getstr(tsvalue(kvalue));
    return "constant";
  }
  else
  {
    *name = "?";
    return nullptr;
  }
}

static const char* basicgetobjname(const Proto* p, int* ppc, int reg,
                                   const char** name)
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
static void rname(const Proto* p, int pc, int c, const char** name)
{
  const char* what = basicgetobjname(p, &pc, c, name); /* search for 'c' */
  if (!(what && *what == 'c'))                         /* did not find a constant name? */
    *name = "?";
}

/*
** Check whether table being indexed by instruction 'i' is the
** environment '_ENV'
*/
static const char* isEnv(const Proto* p, int pc, Instruction i, int isup)
{
  int t = GETARG_B(i); /* table index */
  const char* name;    /* name of indexed variable */
  if (isup)            /* is 't' an upvalue? */
    name = upvalname(p, t);
  else
  { /* 't' is a register */
    const char* what = basicgetobjname(p, &pc, t, &name);
    /* 'name' must be the name of a local variable (at the current
       level or an upvalue) */
    if (what != strlocal && what != strupval)
      name = nullptr; /* cannot be the variable _ENV */
  }
  return (name && strcmp(name, LUA_ENV) == 0) ? "global" : "field";
}

/*
** Extend 'basicgetobjname' to handle table accesses
*/
static const char* getobjname(const Proto* p, int lastpc, int reg,
                              const char** name)
{
  const char* kind = basicgetobjname(p, &lastpc, reg, name);
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
      *name = "integer index";
      return "field";
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
      return "method";
    }
    default:
      break; /* go through to return nullptr */
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
static const char* funcnamefromcode(lua_State* L, const Proto* p,
                                    int pc, const char** name)
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
    *name = "for iterator";
    return "for iterator";
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
  *name = getstr(G(L)->tmname[tm]) + 2;
  return "metamethod";
}

/*
** Try to find a name for a function based on how it was called.
*/
static const char* funcnamefromcall(lua_State* L, CallInfo* ci,
                                    const char** name)
{
  if (ci->callstatus & CIST_HOOKED)
  { /* was it called inside a hook? */
    *name = "?";
    return "hook";
  }
  else if (ci->callstatus & CIST_FIN)
  { /* was it called as a finalizer? */
    *name = "__gc";
    return "metamethod"; /* report it as such */
  }
  else if (isLua(ci))
    return funcnamefromcode(L, ci_func(L, ci)->p, currentpc(L, ci), name);
  else
    return nullptr;
}

/* }====================================================== */

/*
** Check whether pointer 'o' points to some value in the stack frame of
** the current function and, if so, returns its index.  Because 'o' may
** not point to a value in this stack, we cannot compare it with the
** region boundaries (undefined behavior in ISO C).
*/
static int instack(lua_State* L, CallInfo* ci, const TValue* o)
{
  int pos;
  StkId base = ci->func + 1;
  for (pos = 0; base + pos < ci->top; pos++)
  {
    if (o == (&(L)->stack.p[base + pos]))
      return pos;
  }
  return -1; /* not found */
}

/*
** Checks whether value 'o' came from an upvalue. (That can only happen
** with instructions OP_GETTABUP/OP_SETTABUP, which operate directly on
** upvalues.)
*/
static const char* getupvalname(lua_State* L, CallInfo* ci, const TValue* o,
                                const char** name)
{
  LClosure* c = ci_func(L, ci);
  int i;
  for (i = 0; i < c->nupvalues; i++)
  {
    if (upvalval(c->upvals[i]) == o)
    {
      *name = upvalname(c->p, i);
      return strupval;
    }
  }
  return nullptr;
}

static const char* formatvarinfo(lua_State* L, const char* kind,
                                 const char* name)
{
  if (kind == nullptr)
    return ""; /* no information */
  else
    return luaO_pushfstring(L, " (%s '%s')", FmtArgs().str(kind).str(name));
}

/*
** Build a string with a "description" for the value 'o', such as
** "variable 'x'" or "upvalue 'y'". 'src' is the value's origin: its
** stack index when it lives in the current frame, or -1. An explicit
** origin drives attribution directly; the pointer-identity fallback
** below it is transitional (callers not yet threading origins).
*/
static const char* varinfo(lua_State* L, const TValue* o, StkId src)
{
  CallInfo* ci = L->ci;
  const char* name = nullptr; /* to avoid warnings */
  const char* kind = nullptr;
  if (isLua(ci))
  {
    if (src != NOSLOT)
    { /* explicit origin: a register of the current frame */
      StkId base = ci->func + 1;
      if (src >= base && src < ci->top)
        kind = getobjname(ci_func(L, ci)->p, currentpc(L, ci),
                          (int)(src - base), &name);
    }
    else
    {
      kind = getupvalname(L, ci, o, &name); /* check whether 'o' is an upvalue */
      if (!kind)
      {                           /* not an upvalue? */
        int reg = instack(L, ci, o); /* try a register */
        if (reg >= 0)             /* is 'o' a register? */
          kind = getobjname(ci_func(L, ci)->p, currentpc(L, ci), reg, &name);
      }
    }
  }
  return formatvarinfo(L, kind, name);
}

/*
** Raise a type error
*/
static l_noret typeerror(lua_State* L, const TValue* o, const char* op,
                         const char* extra)
{
  const char* t = luaT_objtypename(L, o);
  luaG_runerror(L, "attempt to %s a %s value%s", FmtArgs().str(op).str(t).str(extra));
}

/*
** Raise a type error with "standard" information about the faulty
** object 'o' (using 'varinfo').
*/
l_noret luaG_typeerror(lua_State* L, const TValue* o, const char* op, StkId src)
{
  typeerror(L, o, op, varinfo(L, o, src));
}

/*
** Raise an error for calling a non-callable object. Try to find a name
** for the object based on how it was called ('funcnamefromcall'); if it
** cannot get a name there, try 'varinfo'.
*/
l_noret luaG_callerror(lua_State* L, const TValue* o, StkId src)
{
  CallInfo* ci = L->ci;
  const char* name = nullptr; /* to avoid warnings */
  const char* kind = funcnamefromcall(L, ci, &name);
  const char* extra = kind ? formatvarinfo(L, kind, name) : varinfo(L, o, src);
  typeerror(L, o, "call", extra);
}

l_noret luaG_forerror(lua_State* L, const TValue* o, const char* what)
{
  luaG_runerror(L, "bad 'for' %s (number expected, got %s)",
                FmtArgs().str(what).str(luaT_objtypename(L, o)));
}

l_noret luaG_concaterror(lua_State* L, const TValue* p1, const TValue* p2,
                         StkId src1, StkId src2)
{
  if ((((p1)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) || cvt2str(p1))
  {
    p1 = p2;
    src1 = src2;
  }
  luaG_typeerror(L, p1, "concatenate", src1);
}

l_noret luaG_opinterror(lua_State* L, const TValue* p1,
                        const TValue* p2, const char* msg,
                        StkId src1, StkId src2)
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
  if (!luaV_tointegerns(p1, &temp, LUA_FLOORN2I))
  {
    p2 = p1;
    src2 = src1;
  }
  luaG_runerror(L, "number%s has no integer representation",
                FmtArgs().str(varinfo(L, p2, src2)));
}

l_noret luaG_ordererror(lua_State* L, const TValue* p1, const TValue* p2)
{
  const char* t1 = luaT_objtypename(L, p1);
  const char* t2 = luaT_objtypename(L, p2);
  if (strcmp(t1, t2) == 0)
    luaG_runerror(L, "attempt to compare two %s values", FmtArgs().str(t1));
  else
    luaG_runerror(L, "attempt to compare %s with %s", FmtArgs().str(t1).str(t2));
}

l_noret luaG_errnnil(lua_State* L, LClosure* cl, int k)
{
  const char* globalname = "?"; /* default name if k == 0 */
  if (k > 0)
    kname(cl->p, k - 1, &globalname);
  luaG_runerror(L, "global '%s' already defined", FmtArgs().str(globalname));
}

/* add src:line information to 'msg' */
const char* luaG_addinfo(lua_State* L, const char* msg, TString* src,
                         int line)
{
  if (src == nullptr) /* no debug information? */
    return luaO_pushfstring(L, "?:?: %s", FmtArgs().str(msg));
  else
  {
    char buff[LUA_IDSIZE];
    size_t idlen;
    const char* id = ((void)((idlen) = (src)->content.size()), (src)->content.c_str());
    luaO_chunkid(buff, id, idlen);
    return luaO_pushfstring(L, "%s:%d: %s", FmtArgs().str(buff).num(line).str(msg));
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
    setslots(L, L->top - 1, (luaS_newlstr(L, "" "<no error object>", (sizeof("<no error object>") / sizeof(char)) - 1)));
  }
  luaD_throw(L, LUA_ERRRUN);
}

l_noret luaG_runerror(lua_State* L, const char* fmt, const FmtArgs& args)
{
  CallInfo* ci = L->ci;
  const char* msg;
  luaC_checkGC(L); /* error message uses memory */
  msg = luaO_pushvfstring(L, fmt, args);
  if (msg == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  if (isLua(ci))
  { /* Lua function? */
    /* add source:line information */
    luaG_addinfo(L, msg, ci_func(L, ci)->p->source, getcurrentline(L, ci));
    copyslot(L, L->top - 2, L->top - 1); /* remove 'msg' */
    L->top--;
  }
  luaG_errormsg(L);
}

l_noret luaG_runerror(lua_State* L, const char* fmt)
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

/*
** Traces the execution of a Lua function. Called before the execution
** of each opcode, when debug is on. 'L->oldpc' stores the last
** instruction traced, to detect line changes. When entering a new
** function, 'npci' will be zero and will test as a new line whatever
** the value of 'oldpc'.  Some exceptional conditions may return to
** a function without setting 'oldpc'. In that case, 'oldpc' may be
** invalid; if so, use zero as a valid value. (A wrong but valid 'oldpc'
** at most causes an extra call to a line hook.)
** This function is not "Protected" when called, so it should correct
** 'L->top' before calling anything that can run the GC.
*/
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
