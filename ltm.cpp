/*
** $Id: ltm.c $
** Tag methods
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"


LUAI_DDEF const InternalString* const luaT_typenames_[LUA_TOTALTYPES] = {
    lit<"no value">(),
    lit<"nil">(), lit<"boolean">(), lit<"userdata">(), lit<"number">(),
    lit<"string">(), lit<"table">(), lit<"function">(), lit<"userdata">(),
    lit<"thread">(),
    lit<"upvalue">(), lit<"proto">() /* these last cases are used for tests only */
};

void luaT_init(lua_State* L)
{
  static const char* const luaT_eventname[] = {/* ORDER TM */
                                               "__index", "__newindex",
                                               "__gc", "__mode", "__len", "__eq",
                                               "__add", "__sub", "__mul", "__mod", "__pow",
                                               "__div", "__idiv",
                                               "__band", "__bor", "__bxor", "__shl", "__shr",
                                               "__unm", "__bnot", "__lt", "__le",
                                               "__concat", "__call", "__close"};
  int i;
  for (i = 0; i < TM_N; i++)
  {
    G(L)->tmname[i] = luaS_new(L, luaT_eventname[i]);
    assert((((G(L)->tmname[i])->tt) & NOVARIANT_MASK) >= LUA_TSTRING);
    luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(G(L)->tmname[i]))); /* never collect these names */
  }
}

/*
** function to be used with macro "fasttm": optimized for absence of
** tag methods
*/
const TValue* luaT_gettm(Table* events, TMS event, TString* ename)
{
  const TValue* tm = luaH_Hgetshortstr(events, ename);
  assert(event <= TM_EQ);
  if (notm(tm))
  {                                          /* no tag method? */
    events->flags |= ((lu_byte)(1u << event)); /* cache this fact */
    return nullptr;
  }
  else
    return tm;
}

const TValue* luaT_gettmbyobj(lua_State* L, const TValue* o, TMS event)
{
  Table* mt;
  switch ((o->tt_ & NOVARIANT_MASK))
  {
  case LUA_TTABLE:
    mt = hvalue(o)->metatable;
    break;
  case LUA_TUSERDATA:
    mt = uvalue(o)->metatable;
    break;
  default:
    mt = G(L)->mt[(o->tt_ & NOVARIANT_MASK)];
  }
  return (mt ? luaH_Hgetshortstr(mt, G(L)->tmname[event]) : &G(L)->nilvalue);
}

/* slot form */
const TValue* luaT_gettmbyobj(lua_State* L, StkId o, TMS event)
{
  TValue v = getslot(L, o);
  return luaT_gettmbyobj(L, &v, event);
}

/*
** Return the name of the type of an object. For tables and userdata
** with metatable, use their '__name' metafield, if present.
*/
const InternalString* luaT_objtypename(lua_State* L, const TValue* o)
{
  Table* mt;
  if ((((o)->tt_ == ctb(LUA_VTABLE)) && (mt = hvalue(o)->metatable) != nullptr) ||
      (((o)->tt_ == ctb(LUA_VUSERDATA)) && (mt = uvalue(o)->metatable) != nullptr))
  {
    const TValue* name = luaH_Hgetshortstr(mt, luaS_new(L, lit<"__name">()));
    if ((((name)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))           /* is '__name' a string? */
    {
      return &tsvalue(name)->data; /* use it as type name */
    }
  }
  return ttypename((o->tt_ & NOVARIANT_MASK)); /* else use standard type name */
}

void luaT_callTM(lua_State* L, const TValue* f, const TValue* p1,
                 const TValue* p2, const TValue* p3)
{
  StkId func = L->top;
  setslot(L, func, f);      /* push function (assume EXTRA_STACK) */
  setslot(L, func + 1, p1); /* 1st argument */
  setslot(L, func + 2, p2); /* 2nd argument */
  setslot(L, func + 3, p3); /* 3rd argument */
  L->top = func + 4;
  /* metamethod may yield only when called from Lua code */
  if (isLuacode(L->ci))
    luaD_call(L, func, 0);
  else
    luaD_callnoyield(L, func, 0);
}

lu_byte luaT_callTMres(lua_State* L, const TValue* f, const TValue* p1,
                       const TValue* p2, StkId res)
{
  StkId result = (res);
  StkId func = L->top;
  setslot(L, func, f);      /* push function (assume EXTRA_STACK) */
  setslot(L, func + 1, p1); /* 1st argument */
  setslot(L, func + 2, p2); /* 2nd argument */
  L->top += 3;
  /* metamethod may yield only when called from Lua code */
  if (isLuacode(L->ci))
    luaD_call(L, func, 1);
  else
    luaD_callnoyield(L, func, 1);
  res = (static_cast<StkId>(result));
  copyslot(L, res, --L->top); /* move result to its place */
  return ((slottag(L, res) & WITHVARIANT_MASK));     /* return tag of the result */
}

static int callbinTM(lua_State* L, const TValue* p1, const TValue* p2,
                     StkId res, TMS event)
{
  const TValue* tm = luaT_gettmbyobj(L, p1, event); /* try first operand */
  if (notm(tm))
    tm = luaT_gettmbyobj(L, p2, event); /* try second operand */
  if (notm(tm))
    return -1; /* tag method not found */
  else         /* call tag method and return the tag of the result */
    return luaT_callTMres(L, tm, p1, p2, res);
}

void luaT_trybinTM(lua_State* L, const TValue* p1, const TValue* p2,
                   StkId res, TMS event, StkId src1, StkId src2)
{
  if (l_unlikely(callbinTM(L, p1, p2, res, event) < 0))
  {
    switch (event)
    {
    case TM_BAND:
    case TM_BOR:
    case TM_BXOR:
    case TM_SHL:
    case TM_SHR:
    case TM_BNOT:
    {
      if ((((p1)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) && (((p2)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER))
        luaG_tointerror(L, p1, p2, src1, src2);
      else
        luaG_opinterror(L, p1, p2, lit<"perform bitwise operation on">(), src1, src2);
    }
    /* calls never return, but to avoid warnings: */ /* FALLTHROUGH */
    default:
      luaG_opinterror(L, p1, p2, lit<"perform arithmetic on">(), src1, src2);
    }
  }
}

/*
** The use of 'p1' after 'callbinTM' is safe because, when a tag
** method is not found, 'callbinTM' cannot change the stack.
*/
void luaT_tryconcatTM(lua_State* L)
{
  StkId p1 = L->top - 2; /* first argument */
  TValue v1 = getslot(L, p1);
  TValue v2 = getslot(L, p1 + 1);
  if (l_unlikely(callbinTM(L, &v1, &v2, p1, TM_CONCAT) < 0))
    luaG_concaterror(L, &v1, &v2, p1, p1 + 1); /* no call was made; rows still valid */
}

void luaT_trybinassocTM(lua_State* L, const TValue* p1, const TValue* p2,
                        int flip, StkId res, TMS event, StkId src1)
{
  if (flip)
    luaT_trybinTM(L, p2, p1, res, event, NOSLOT, src1);
  else
    luaT_trybinTM(L, p1, p2, res, event, src1, NOSLOT);
}

void luaT_trybiniTM(lua_State* L, const TValue* p1, lua_Integer i2,
                    int flip, StkId res, TMS event, StkId src1)
{
  TValue aux;
  setivalue(&aux, i2);
  luaT_trybinassocTM(L, p1, &aux, flip, res, event, src1);
}

/*
** Slot-form overloads: operands are named by stack index; the row is
** materialized here, once, at the storage boundary. (In the managed
** port these bodies read the operand columns directly.)
*/
void luaT_trybinTM(lua_State* L, StkId p1, StkId p2, StkId res, TMS event)
{
  TValue v1 = getslot(L, p1);
  TValue v2 = getslot(L, p2);
  luaT_trybinTM(L, &v1, &v2, res, event, p1, p2);
}

void luaT_trybinassocTM(lua_State* L, StkId p1, const TValue* p2,
                        int flip, StkId res, TMS event)
{
  TValue v1 = getslot(L, p1);
  luaT_trybinassocTM(L, &v1, p2, flip, res, event, p1);
}

void luaT_trybiniTM(lua_State* L, StkId p1, lua_Integer i2,
                    int flip, StkId res, TMS event)
{
  TValue v1 = getslot(L, p1);
  luaT_trybiniTM(L, &v1, i2, flip, res, event, p1);
}

/*
** Calls an order tag method.
*/
int luaT_callorderTM(lua_State* L, const TValue* p1, const TValue* p2,
                     TMS event)
{
  int tag = callbinTM(L, p1, p2, L->top, event); /* try original event */
  if (tag >= 0)                                    /* found tag method? */
    return !((tag) == LUA_VFALSE || ((tag) & NOVARIANT_MASK) == LUA_TNIL);
  luaG_ordererror(L, p1, p2); /* no metamethod found */
  return 0;                   /* to avoid warnings */
}

int luaT_callorderiTM(lua_State* L, const TValue* p1, int v2,
                      int flip, int isfloat, TMS event)
{
  TValue aux;
  const TValue* p2;
  if (isfloat)
  {
    setfltvalue(&aux, ((lua_Number)(v2)));
  }
  else
    setivalue(&aux, v2);
  if (flip)
  { /* arguments were exchanged? */
    p2 = p1;
    p1 = &aux; /* correct them */
  }
  else
    p2 = &aux;
  return luaT_callorderTM(L, p1, p2, event);
}

int luaT_callorderiTM(lua_State* L, StkId p1, int v2,
                      int flip, int isfloat, TMS event)
{
  TValue v1 = getslot(L, p1);
  return luaT_callorderiTM(L, &v1, v2, flip, isfloat, event);
}

/*
** Create a vararg table at the top of the stack, with 'n' elements
** starting at 'f'.
*/
static void createvarargtab(lua_State* L, StkId f, int n)
{
  int i;
  TValue key, value;
  Table* t = luaH_new(L);
  setsloth(L, L->top, t);
  L->top++;
  luaH_resize(L, t, ((unsigned int)(n)), 1);
  setsvalue(L, &key, luaS_new(L, lit<"n">())); /* key is "n" */
  setivalue(&value, n);                 /* value is n */
  /* No need to anchor the key: Due to the resize, the next operation
     cannot trigger a garbage collection */
  luaH_set(L, t, &key, &value); /* t.n = n */
  for (i = 0; i < n; i++)
  {
    TValue v = getslot(L, f + i); /* original slot anchors the value */
    luaH_setint(L, t, i + 1, &v);
  }
  luaC_checkGC(L);
}

/*
** initial stack:  func arg1 ... argn extra1 ...
**                 ^ ci->func                    ^ L->top
** final stack: func nil ... nil extra1 ... func arg1 ... argn
**                                          ^ ci->func
*/
static void buildhiddenargs(lua_State* L, CallInfo* ci, const Proto* p,
                            int totalargs, int nfixparams, int nextra)
{
  int i;
  ci->nextraargs = nextra;
  luaD_checkstack(L, p->maxstacksize + 1);
  /* copy function to the top of the stack, after extra arguments */
  copyslot(L, L->top++, ci->func);
  /* move fixed parameters to after the copied function */
  for (i = 1; i <= nfixparams; i++)
  {
    copyslot(L, L->top++, ci->func + i);
    setslottag(L, ci->func + i, LUA_VNIL); /* erase original parameter (for GC) */
  }
  ci->func += totalargs + 1; /* 'func' now lives after hidden arguments */
  ci->top += totalargs + 1;
}

void luaT_adjustvarargs(lua_State* L, CallInfo* ci, const Proto* p)
{
  int totalargs = ((int)(L->top - ci->func)) - 1;
  int nfixparams = p->numparams;
  int nextra = totalargs - nfixparams; /* number of extra arguments */
  if (p->flag & PF_VATAB)
  { /* does it need a vararg table? */
    assert(!(p->flag & PF_VAHID));
    createvarargtab(L, ci->func + nfixparams + 1, nextra);
    /* move table to proper place (last parameter) */
    copyslot(L, ci->func + nfixparams + 1, L->top - 1);
  }
  else
  { /* no table */
    assert(p->flag & PF_VAHID);
    buildhiddenargs(L, ci, p, totalargs, nfixparams, nextra);
    /* set vararg parameter to nil */
    setslottag(L, ci->func + nfixparams + 1, LUA_VNIL);
    assert(L->top <= ci->top && ci->top <= L->stack_last);
  }
}

void luaT_getvararg(lua_State* L, CallInfo* ci, StkId ra, TValue* rc)
{
  int nextra = ci->nextraargs;
  lua_Integer n;
  if (tointegerns(rc, &n))
  { /* integral value? */
    if (((lua_Unsigned)(n)) - 1 < ((unsigned int)(nextra)))
    {
      StkId slot = ci->func - nextra + ((int)(n)) - 1;
      copyslot(L, ra, slot);
      return;
    }
  }
  else if ((((rc)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
  { /* string value? */
    TString* ts = tsvalue(rc);
    size_t len = ts->data.size();
    const char* s = ts->data.c_str();
    if (len == 1 && s[0] == 'n')
    { /* key is "n"? */
      setsloti(L, ra, nextra);
      return;
    }
  }
  setslottag(L, ra, LUA_VNIL); /* else produce nil */
}

/* slot form */
void luaT_getvararg(lua_State* L, CallInfo* ci, StkId ra, StkId rc)
{
  TValue v = getslot(L, rc);
  luaT_getvararg(L, ci, ra, &v);
}

/*
** Get the number of extra arguments in a vararg function. If vararg
** table has been optimized away, that number is in the call info.
** Otherwise, get the field 'n' from the vararg table and check that it
** has a proper value (non-negative integer not larger than the stack
** limit).
*/
static int getnumargs(lua_State* L, CallInfo* ci, Table* h)
{
  if (h == nullptr) /* no vararg table? */
    return ci->nextraargs;
  else
  {
    TValue res;
    if (luaH_getshortstr(h, luaS_new(L, lit<"n">()), &res) != LUA_VNUMINT ||
        ((lua_Unsigned)(ivalue(&res))) > ((unsigned int)(INT_MAX / 2)))
      luaG_runerror(L, lit<"vararg table has no proper 'n'">());
    assert((&res)->tt_ == LUA_VNUMINT);
    return ((int)(ivalue(&res)));
  }
}

/*
** Get 'wanted' vararg arguments and put them in 'where'. 'vatab' is
** the register of the vararg table or -1 if there is no vararg table.
*/
void luaT_getvarargs(lua_State* L, CallInfo* ci, StkId where, int wanted,
                     int vatab)
{
  Table* h;
  if (vatab < 0)
    h = nullptr;
  else
  {
    StkId vt = ci->func + vatab + 1;
    h = slothval(L, vt);
  }
  int nargs = getnumargs(L, ci, h); /* number of available vararg args. */
  int i, touse;                     /* 'touse' is minimum between 'wanted' and 'nargs' */
  if (wanted < 0)
  {
    touse = wanted = nargs;       /* get all extra arguments available */
    checkstackp(L, nargs, where); /* ensure stack space */
    L->top = where + nargs;     /* next instruction will need top */
  }
  else
    touse = (nargs > wanted) ? wanted : nargs;
  if (h == nullptr)
  {                             /* no vararg table? */
    for (i = 0; i < touse; i++) /* get vararg values from the stack */
      copyslot(L, where + i, ci->func - nargs + i);
  }
  else
  { /* get vararg values from vararg table */
    for (i = 0; i < touse; i++)
    {
      TValue res;
      lu_byte tag = luaH_getint(h, i + 1, &res);
      if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        setslottag(L, where + i, LUA_VNIL);
      else
        setslot(L, where + i, &res);
    }
  }
  for (; i < wanted; i++) /* complete required results with nil */
    setslottag(L, where + i, LUA_VNIL);
}
