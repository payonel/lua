/*
** $Id: lapi.c $
** Lua API
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"

#include "util_cast.h"

/* keep in sync with LUA_COPYRIGHT / LUA_AUTHORS (lua.h) */
const char lua_ident[] =
    "$LuaVersion: Lua 5.5.1  Copyright (C) 1994-2026 Lua.org, PUC-Rio $"
    "$LuaAuthors: R. Ierusalimschy, L. H. de Figueiredo, W. Celes $";

/*
** Test for a valid index (one that is not the 'nilvalue').
*/
static inline int isvalid(lua_State* L, const TValue* o)
{
  return o != &G(L)->nilvalue;
}

/* test for pseudo index */
constexpr int ispseudo(int i)
{
  return i <= LUA_REGISTRYINDEX;
}

/* test for upvalue */
constexpr int isupvalue(int i)
{
  return i < LUA_REGISTRYINDEX;
}

/*
** Convert an acceptable index to a pointer to its respective value.
** Non-valid indices return the special nil value 'G(L)->nilvalue'.
*/
static TValue* index2value(lua_State* L, int idx)
{
  CallInfo* ci = L->ci;
  if (idx > 0)
  {
    StkId o = ci->func + idx;
    assert((((StkId)(idx)) <= ci->top - (ci->func + 1)) && "unacceptable index");
    if (o >= L->top)
      return &G(L)->nilvalue;
    else
      return (&(L)->stack.p[o]);
  }
  else if (!ispseudo(idx))
  { /* negative index */
    assert((idx != 0 && ((StkId)(-idx)) <= L->top - (ci->func + 1)) && "invalid index");
    return (&(L)->stack.p[L->top + idx]);
  }
  else if (idx == LUA_REGISTRYINDEX)
    return &G(L)->l_registry;
  else
  { /* upvalues */
    idx = LUA_REGISTRYINDEX - idx;
    assert((idx <= MAXUPVAL + 1) && "upvalue index too large");
    if (slottag(L, ci->func) == ctb(LUA_VCCL))
    { /* C closure? */
      CClosure* func = slotclCval(L, ci->func);
      return (idx <= func->nupvalues) ? &func->upvalue[idx - 1]
                                      : &G(L)->nilvalue;
    }
    else
    { /* light C function or Lua function (through a hook)?) */
      assert((slottag(L, ci->func) == LUA_VLCF) && "caller not a C function");
      return &G(L)->nilvalue; /* no upvalues */
    }
  }
}

/*
** Convert a valid actual index (not a pseudo-index) to its address.
*/
static StkId index2stack(lua_State* L, int idx)
{
  CallInfo* ci = L->ci;
  if (idx > 0)
  {
    StkId o = ci->func + idx;
    assert((o < L->top) && "invalid index");
    return o;
  }
  else
  { /* non-positive index */
    assert((idx != 0 && ((StkId)(-idx)) <= L->top - (ci->func + 1)) && "invalid index");
    assert((!ispseudo(idx)) && "invalid index");
    return L->top + idx;
  }
}

LUA_API int lua_checkstack(lua_State* L, int n)
{
  int res;
  CallInfo* ci;
  lua_lock(L);
  ci = L->ci;
  assert((n >= 0) && "negative 'n'");
  if (L->top + ((StkId)(n)) < L->stack_last) /* stack large enough? */
    res = 1;                          /* yes; check is OK */
  else                                /* need to grow stack */
    res = luaD_growstack(L, n, 0);
  if (res && ci->top < L->top + n)
    ci->top = L->top + n; /* adjust frame top */
  lua_unlock(L);
  return res;
}

LUA_API void lua_xmove(lua_State* from, lua_State* to, int n)
{
  int i;
  if (from == to)
    return;
  lua_lock(to);
  api_checkpop(from, n);
  assert((G(from) == G(to)) && "moving among independent states");
  assert((to->ci->top >= to->top + ((StkId)(n))) && "stack overflow");
  from->top -= n;
  for (i = 0; i < n; i++)
  {
    TValue v = getslot(from, from->top + i); /* cross-stack transport by row copy */
    setslot(to, to->top, &v);
    to->top++; /* stack already checked by previous 'api_check' */
  }
  lua_unlock(to);
}

LUA_API lua_CFunction lua_atpanic(lua_State* L, lua_CFunction panicf)
{
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}

LUA_API lua_Number lua_version(lua_State*)
{
  return LUA_VERSION_NUM;
}

/*
** basic stack manipulation
*/

/*
** convert an acceptable stack index into an absolute index
*/
LUA_API int lua_absindex(lua_State* L, int idx)
{
  return (idx > 0 || ispseudo(idx))
             ? idx
             : ((int)(L->top - L->ci->func)) + idx;
}

LUA_API int lua_gettop(lua_State* L)
{
  return ((int)(L->top - (L->ci->func + 1)));
}

LUA_API void lua_settop(lua_State* L, int idx)
{
  CallInfo* ci;
  StkId func, newtop;
  int diff; /* difference for new top */
  lua_lock(L);
  ci = L->ci;
  func = ci->func;
  if (idx >= 0)
  {
    assert((((StkId)(idx)) <= ci->top - (func + 1)) && "new top too large");
    diff = ((int)(((func + 1) + idx) - L->top));
    for (; diff > 0; diff--)
      setslottag(L, L->top++, LUA_VNIL); /* clear new slots */
  }
  else
  {
    assert((((StkId)(-(idx + 1))) <= (L->top - (func + 1))) && "invalid new top");
    diff = idx + 1; /* will "subtract" index (as it is negative) */
  }
  newtop = L->top + diff;
  if (diff < 0 && L->tbclist >= newtop)
  {
    assert(ci->callstatus & CIST_TBC);
    newtop = luaF_close(L, newtop, CLOSEKTOP, 0);
  }
  L->top = newtop; /* correct top only after closing any upvalue */
  lua_unlock(L);
}

LUA_API void lua_closeslot(lua_State* L, int idx)
{
  StkId level;
  lua_lock(L);
  level = index2stack(L, idx);
  assert(((L->ci->callstatus & CIST_TBC) && (L->tbclist == level)) && "no variable to close at given level");
  level = luaF_close(L, level, CLOSEKTOP, 0);
  setslottag(L, level, LUA_VNIL);
  lua_unlock(L);
}

/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
** Note that we move(copy) only the value inside the stack.
** (We do not move additional fields that may exist.)
*/
static void reverse(lua_State* L, StkId from, StkId to)
{
  for (; from < to; from++, to--)
  {
    TValue temp;
    setobj(L, &temp, (&(L)->stack.p[from]));
    copyslot(L, from, to);
    setslot(L, to, &temp);
  }
}

/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate(lua_State* L, int idx, int n)
{
  StkId p, t, m;
  lua_lock(L);
  t = L->top - 1;        /* end of stack segment being rotated */
  p = index2stack(L, idx); /* start of segment */
  assert((L->tbclist < p) && "moving a to-be-closed slot");
  assert(((n >= 0 ? n : -n) <= (t - p + 1)) && "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1); /* end of prefix */
  reverse(L, p, m);                 /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);             /* reverse the suffix */
  reverse(L, p, t);                 /* reverse the entire segment */
  lua_unlock(L);
}

LUA_API void lua_copy(lua_State* L, int fromidx, int toidx)
{
  TValue *fr, *to;
  lua_lock(L);
  fr = index2value(L, fromidx);
  to = index2value(L, toidx);
  assert((isvalid(L, to)) && "invalid index");
  setobj(L, to, fr);
  if (isupvalue(toidx)) /* function upvalue? */
  {
    TValue* fv = &L->stack.p[L->ci->func];
    luaC_barrier(L, clCvalue(fv), fr);
  }
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
  lua_unlock(L);
}

LUA_API void lua_pushvalue(lua_State* L, int idx)
{
  lua_lock(L);
  setslot(L, L->top, index2value(L, idx));
  api_incr_top(L);
  lua_unlock(L);
}

/*
** access functions (stack -> C)
*/

LUA_API int lua_type(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return (isvalid(L, o) ? (o->tt_ & NOVARIANT_MASK) : LUA_TNONE);
}

LUA_API const char* lua_typename(lua_State* L, int t)
{
  (void) L; /* public API signature; 'L' is not needed here */
  assert((LUA_TNONE <= t && t < LUA_NUMTYPES) && "invalid type");
  return ttypename(t);
}

LUA_API int lua_iscfunction(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return (((o)->tt_ == LUA_VLCF) || (((o)->tt_ == ctb(LUA_VCCL))));
}

LUA_API int lua_islightcfunction(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return ((o)->tt_ == LUA_VLCF);
}

LUA_API int lua_isinteger(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return ((o)->tt_ == LUA_VNUMINT);
}

LUA_API int lua_isnumber(lua_State* L, int idx)
{
  lua_Number n;
  const TValue* o = index2value(L, idx);
  return tonumber(o, &n);
}

LUA_API int lua_isstring(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return ((((o)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) || cvt2str(o));
}

LUA_API int lua_isuserdata(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return (((o)->tt_ == ctb(LUA_VUSERDATA)) || ((o)->tt_ == LUA_VLIGHTUSERDATA));
}

LUA_API int lua_rawequal(lua_State* L, int index1, int index2)
{
  const TValue* o1 = index2value(L, index1);
  const TValue* o2 = index2value(L, index2);
  return (isvalid(L, o1) && isvalid(L, o2)) ? luaV_rawequalobj(o1, o2) : 0;
}

LUA_API void lua_arith(lua_State* L, int op)
{
  lua_lock(L);
  if (op != LUA_OPUNM && op != LUA_OPBNOT)
    api_checkpop(L, 2); /* all other operations expect two operands */
  else
  { /* for unary operations, add fake 2nd operand */
    api_checkpop(L, 1);
    copyslot(L, L->top, L->top - 1);
    api_incr_top(L);
  }
  /* first operand at top - 2, second at top - 1; result go to top - 2 */
  luaO_arith(L, op, (&(L)->stack.p[L->top - 2]), (&(L)->stack.p[L->top - 1]), L->top - 2);
  L->top--; /* pop second operand */
  lua_unlock(L);
}

LUA_API int lua_compare(lua_State* L, int index1, int index2, int op)
{
  const TValue* o1;
  const TValue* o2;
  int i = 0;
  lua_lock(L); /* may call tag method */
  o1 = index2value(L, index1);
  o2 = index2value(L, index2);
  if (isvalid(L, o1) && isvalid(L, o2))
  {
    switch (op)
    {
    case LUA_OPEQ:
      i = luaV_equalobj(L, o1, o2);
      break;
    case LUA_OPLT:
      i = luaV_lessthan(L, o1, o2);
      break;
    case LUA_OPLE:
      i = luaV_lessequal(L, o1, o2);
      break;
    default:
      assert((0) && "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}

LUA_API unsigned lua_numbertocstring(lua_State* L, int idx, char* buff)
{
  const TValue* o = index2value(L, idx);
  if ((((o)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER))
  {
    unsigned len = luaO_tostringbuff(o, buff);
    buff[len++] = '\0'; /* add final zero */
    return len;
  }
  else
    return 0;
}

LUA_API size_t lua_stringtonumber(lua_State* L, const char* s)
{
  size_t sz = luaO_str2num(s, (&(L)->stack.p[L->top]));
  if (sz != 0)
    api_incr_top(L);
  return sz;
}

LUA_API lua_Number lua_tonumberx(lua_State* L, int idx, int* pisnum)
{
  lua_Number n = 0;
  const TValue* o = index2value(L, idx);
  int isnum = tonumber(o, &n);
  if (pisnum)
    *pisnum = isnum;
  return n;
}

LUA_API lua_Integer lua_tointegerx(lua_State* L, int idx, int* pisnum)
{
  lua_Integer res = 0;
  const TValue* o = index2value(L, idx);
  int isnum = tointeger(o, &res);
  if (pisnum)
    *pisnum = isnum;
  return res;
}

LUA_API int lua_toboolean(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return !(((o)->tt_ == LUA_VFALSE) || (((o)->tt_ & NOVARIANT_MASK) == LUA_TNIL));
}

LUA_API const char* lua_tolstring(lua_State* L, int idx, size_t* len)
{
  TValue* o;
  lua_lock(L);
  o = index2value(L, idx);
  if (!(((o)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
  {
    if (!cvt2str(o))
    { /* not convertible? */
      if (len != nullptr)
        *len = 0;
      lua_unlock(L);
      return nullptr;
    }
    luaO_tostring(L, o);
    luaC_checkGC(L);
    o = index2value(L, idx); /* previous call may reallocate the stack */
  }
  lua_unlock(L);
  assert((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
  TString* s = tsvalue(o);
  if (len != nullptr)
  {
    *len = s->content.size();
    return s->content.c_str();
  }
  else
    return getstr(s);
}

LUA_API lua_Unsigned lua_rawlen(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  switch (((o)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VSHRSTR:
    return (lua_Unsigned)tsslen(tsvalue(o));
  case LUA_VUSERDATA:
    return (lua_Unsigned)uvalue(o)->len;
  case LUA_VTABLE:
  {
    lua_Unsigned res;
    lua_lock(L);
    res = luaH_getn(L, hvalue(o));
    lua_unlock(L);
    return res;
  }
  default:
    return 0;
  }
}

LUA_API lua_CFunction lua_tocfunction(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  if (((o)->tt_ == LUA_VLCF))
    return fvalue(o);
  else if (((o)->tt_ == ctb(LUA_VCCL)))
  {
    return clCvalue(o)->f;
  }
  else
    return nullptr; /* not a C function */
}

static inline void* touserdata(const TValue* o)
{
  switch ((o->tt_ & NOVARIANT_MASK))
  {
  case LUA_TUSERDATA:
    return getudatamem(uvalue(o));
  case LUA_TLIGHTUSERDATA:
    assert(o->tt_ == LUA_VLIGHTUSERDATA);
    return pvalue(o);
  default:
    return nullptr;
  }
}

LUA_API void* lua_touserdata(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  return touserdata(o);
}

LUA_API lua_State* lua_tothread(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  if (!((o)->tt_ == ctb(LUA_VTHREAD)))
    return nullptr;
  return thvalue(o);
}

/*
** Returns a pointer to the internal representation of an object.
** Note that ISO C does not allow the conversion of a pointer to
** function to a 'void*', so the conversion here goes through
** a 'size_t'. (As the returned pointer is only informative, this
** conversion should not be a problem.)
*/
LUA_API const void* lua_topointer(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  switch (((o)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VLCF:
    return nullptr;  /* use lua_funcaddress for light C functions */
  case LUA_VUSERDATA:
  case LUA_VLIGHTUSERDATA:
    return touserdata(o);
  default:
  {
    if (((o)->tt_ & BIT_ISCOLLECTABLE))
      return gcvalue(o);
    else
      return nullptr;
  }
  }
}

std::string lua_funcaddress(lua_State* L, int idx)
{
  const TValue* o = index2value(L, idx);
  assert(((o)->tt_ == LUA_VLCF));
  assert(o->tt_ == LUA_VLCF);
  return util::func_to_string(fvalue(o));
}

/*
** push functions (C -> stack)
*/

LUA_API void lua_pushnil(lua_State* L)
{
  lua_lock(L);
  setslottag(L, L->top, LUA_VNIL);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API void lua_pushnumber(lua_State* L, lua_Number n)
{
  lua_lock(L);
  setslotflt(L, L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API void lua_pushinteger(lua_State* L, lua_Integer n)
{
  lua_lock(L);
  setsloti(L, L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}

/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be nullptr in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
LUA_API const char* lua_pushlstring(lua_State* L, const char* s, size_t len)
{
  TString* ts;
  lua_lock(L);
  ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getstr(ts);
}

LUA_API const char* lua_pushstring(lua_State* L, const char* s)
{
  lua_lock(L);
  if (s == nullptr)
    setslottag(L, L->top, LUA_VNIL);
  else
  {
    TString* ts;
    ts = luaS_new(L, s);
    setslots(L, L->top, ts);
    s = getstr(ts); /* internal copy's address */
  }
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return s;
}

/*
** Format 'fmt' taking one argument from the stack per conversion
** directive (the replacement for C varargs in the public API): the
** caller pushes the arguments in order before the call — a string for
** '%s', an integer for '%d'/'%I'/'%c'/'%U', a float for '%f', and any
** value for '%p' (formatted as its address). The arguments are
** consumed and the formatted result is pushed (and also returned).
*/
LUA_API const char* lua_pushfstring(lua_State* L, const char* fmt)
{
  const char* ret;
  FmtArgs args;
  int n = 0; /* number of arguments taken from the stack */
  const char* e;
  lua_lock(L);
  for (e = fmt; (e = strchr(e, '%')) != nullptr; e += 2)
  { /* scan directives, collecting one stack argument for each */
    switch (*(e + 1))
    {
    case 's':
    case 'c':
    case 'd':
    case 'I':
    case 'f':
    case 'p':
    case 'U':
      n++;
      break;
    default: /* '%%' and unknown directives take no argument */
      break;
    }
  }
  api_checknelems(L, n);
  for (e = fmt; (e = strchr(e, '%')) != nullptr; e += 2)
  { /* arguments are at stack slots 'L->top - n' up to 'L->top - 1' */
    StkId slot = L->top - n + args.n;
    switch (*(e + 1))
    {
    case 's':
    { /* a string (a number is converted in place, as 'lua_tolstring') */
      TValue* o = &L->stack.p[slot];
      if ((o->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)
        luaO_tostring(L, o);
      assert(((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING) && "string expected");
      args.str(getstr(tsvalue(o)));
      break;
    }
    case 'c':
    {
      TValue* o = &L->stack.p[slot];
      assert((o->tt_ == LUA_VNUMINT) && "integer expected");
      args.chr(((int)(ivalue(o))));
      break;
    }
    case 'd':
    {
      TValue* o = &L->stack.p[slot];
      assert((o->tt_ == LUA_VNUMINT) && "integer expected");
      args.num(((int)(ivalue(o))));
      break;
    }
    case 'I':
    {
      TValue* o = &L->stack.p[slot];
      assert((o->tt_ == LUA_VNUMINT) && "integer expected");
      args.lint(ivalue(o));
      break;
    }
    case 'f':
    {
      TValue* o = &L->stack.p[slot];
      assert((o->tt_ == LUA_VNUMFLT) && "float expected");
      args.flt(fltvalue(o));
      break;
    }
    case 'p':
    { /* any value, formatted as its address (as 'lua_topointer') */
      args.ptr(lua_topointer(L, args.n - n)); /* negative API index of 'slot' */
      break;
    }
    case 'U':
    {
      TValue* o = &L->stack.p[slot];
      assert((o->tt_ == LUA_VNUMINT) && "integer expected");
      args.utf8(((unsigned long)(ivalue(o))));
      break;
    }
    default:
      break;
    }
  }
  ret = luaO_pushvfstring(L, fmt, args);
  if (ret == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  if (n > 0)
  { /* move result down, removing its arguments */
    copyslot(L, L->top - 1 - n, L->top - 1);
    L->top -= n;
  }
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}

LUA_API void lua_pushcclosure(lua_State* L, lua_CFunction fn, int n)
{
  lua_lock(L);
  if (n == 0)
  {
    setslotf(L, L->top, fn);
    api_incr_top(L);
  }
  else
  {
    int i;
    CClosure* cl;
    api_checkpop(L, n);
    assert((n <= MAXUPVAL) && "upvalue index too large");
    cl = luaF_newCclosure(L, n);
    cl->f = fn;
    for (i = 0; i < n; i++)
    {
      setobj(L, &cl->upvalue[i], (&(L)->stack.p[L->top - n + i]));
      /* does not need barrier because closure is white */
      assert(iswhite(cl));
    }
    L->top -= n;
    setslotclC(L, L->top, cl);
    api_incr_top(L);
    luaC_checkGC(L);
  }
  lua_unlock(L);
}

LUA_API void lua_pushboolean(lua_State* L, int b)
{
  lua_lock(L);
  if (b)
    setslottag(L, L->top, LUA_VTRUE);
  else
    setslottag(L, L->top, LUA_VFALSE);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API void lua_pushlightuserdata(lua_State* L, void* p)
{
  lua_lock(L);
  setslotp(L, L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API int lua_pushthread(lua_State* L)
{
  lua_lock(L);
  setslotth(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (mainthread(G(L)) == L);
}

/*
** get functions (Lua -> stack)
*/

static int auxgetstr(lua_State* L, const TValue* t, const char* k)
{
  lu_byte tag;
  TString* str = luaS_new(L, k);
  if (t->tt_ != ctb(LUA_VTABLE))
    tag = LUA_VNOTABLE;
  else
  {
    tag = luaH_getstr(hvalue(t), str, &L->stack.p[L->top]);
  }
  if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
    api_incr_top(L);
  else
  {
    setslots(L, L->top, str);
    api_incr_top(L);
    tag = luaV_finishget(L, t, (&(L)->stack.p[L->top - 1]), L->top - 1, tag);
  }
  lua_unlock(L);
  return ((tag) & NOVARIANT_MASK);
}

/*
** The following function assumes that the registry cannot be a weak
** table; so, an emergency collection while using the global table
** cannot collect it.
*/
static void getGlobalTable(lua_State* L, TValue* gt)
{
  Table* registry = hvalue((&G(L)->l_registry));
  lu_byte tag = luaH_getint(registry, LUA_RIDX_GLOBALS, gt);
  (void) tag; /* avoid not-used warnings when checks are off */
  assert((((tag) & NOVARIANT_MASK) == LUA_TTABLE) && "global table must exist");
}

LUA_API int lua_getglobal(lua_State* L, const char* name)
{
  TValue gt;
  lua_lock(L);
  getGlobalTable(L, &gt);
  return auxgetstr(L, &gt, name);
}

LUA_API int lua_gettable(lua_State* L, int idx)
{
  lu_byte tag;
  TValue* t;
  lua_lock(L);
  api_checkpop(L, 1);
  t = index2value(L, idx);
  if (t->tt_ != ctb(LUA_VTABLE))
    tag = LUA_VNOTABLE;
  else
  {
    tag = luaH_get(hvalue(t), &L->stack.p[L->top - 1], &L->stack.p[L->top - 1]);
  }
  if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
    tag = luaV_finishget(L, t, (&(L)->stack.p[L->top - 1]), L->top - 1, tag);
  lua_unlock(L);
  return ((tag) & NOVARIANT_MASK);
}

LUA_API int lua_getfield(lua_State* L, int idx, const char* k)
{
  lua_lock(L);
  return auxgetstr(L, index2value(L, idx), k);
}

LUA_API int lua_geti(lua_State* L, int idx, lua_Integer n)
{
  TValue* t;
  lu_byte tag;
  lua_lock(L);
  t = index2value(L, idx);
  if (t->tt_ != ctb(LUA_VTABLE))
    tag = LUA_VNOTABLE;
  else
  {
    Table* h = hvalue(t);
    lua_Unsigned u = (lua_Unsigned)n - 1u;
    if (u < h->asize)
    {
      tag = *getArrTag(h, u);
      if ((tag & NOVARIANT_MASK) != LUA_TNIL)
        farr2val(h, u, tag, &L->stack.p[L->top]);
    }
    else
      tag = luaH_getint(h, n, &L->stack.p[L->top]);
  }
  if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
  {
    TValue key;
    setivalue(&key, n);
    tag = luaV_finishget(L, t, &key, L->top, tag);
  }
  api_incr_top(L);
  lua_unlock(L);
  return ((tag) & NOVARIANT_MASK);
}

static int finishrawget(lua_State* L, lu_byte tag)
{
  if ((((tag) & NOVARIANT_MASK) == LUA_TNIL)) /* avoid copying empty items to the stack */
    setslottag(L, L->top, LUA_VNIL);
  api_incr_top(L);
  lua_unlock(L);
  return ((tag) & NOVARIANT_MASK);
}

static inline Table* gettable(lua_State* L, int idx)
{
  TValue* t = index2value(L, idx);
  assert((((t)->tt_ == ctb(LUA_VTABLE))) && "table expected");
  return hvalue(t);
}

LUA_API int lua_rawget(lua_State* L, int idx)
{
  Table* t;
  lu_byte tag;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  tag = luaH_get(t, (&(L)->stack.p[L->top - 1]), (&(L)->stack.p[L->top - 1]));
  L->top--; /* pop key */
  return finishrawget(L, tag);
}

LUA_API int lua_rawgeti(lua_State* L, int idx, lua_Integer n)
{
  Table* t;
  lu_byte tag;
  lua_lock(L);
  t = gettable(L, idx);
  {
    lua_Unsigned u = (lua_Unsigned)n - 1u;
    if (u < t->asize)
    {
      tag = *getArrTag(t, u);
      if ((tag & NOVARIANT_MASK) != LUA_TNIL)
        farr2val(t, u, tag, &L->stack.p[L->top]);
    }
    else
      tag = luaH_getint(t, n, &L->stack.p[L->top]);
  }
  return finishrawget(L, tag);
}

LUA_API int lua_rawgetp(lua_State* L, int idx, const void* p)
{
  Table* t;
  TValue k;
  lua_lock(L);
  t = gettable(L, idx);
  setpvalue(&k, util::remove_const_cast(p));
  return finishrawget(L, luaH_get(t, &k, (&(L)->stack.p[L->top])));
}

LUA_API void lua_createtable(lua_State* L, int narray, int nrec)
{
  Table* t;
  lua_lock(L);
  t = luaH_new(L);
  setsloth(L, L->top, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, ((unsigned int)(narray)), ((unsigned int)(nrec)));
  luaC_checkGC(L);
  lua_unlock(L);
}

LUA_API int lua_getmetatable(lua_State* L, int objindex)
{
  const TValue* obj;
  Table* mt;
  int res = 0;
  lua_lock(L);
  obj = index2value(L, objindex);
  switch ((obj->tt_ & NOVARIANT_MASK))
  {
  case LUA_TTABLE:
    mt = hvalue(obj)->metatable;
    break;
  case LUA_TUSERDATA:
    mt = uvalue(obj)->metatable;
    break;
  default:
    mt = G(L)->mt[(obj->tt_ & NOVARIANT_MASK)];
    break;
  }
  if (mt != nullptr)
  {
    setsloth(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}

LUA_API int lua_getiuservalue(lua_State* L, int idx, int n)
{
  TValue* o;
  int t;
  lua_lock(L);
  o = index2value(L, idx);
  assert((((o)->tt_ == ctb(LUA_VUSERDATA))) && "full userdata expected");
  Udata* u = uvalue(o);
  if (n <= 0 || n > u->nuvalue)
  {
    setslottag(L, L->top, LUA_VNIL);
    t = LUA_TNONE;
  }
  else
  {
    setslot(L, L->top, &u->uv[n - 1]);
    t = (slottag(L, L->top) & NOVARIANT_MASK);
  }
  api_incr_top(L);
  lua_unlock(L);
  return t;
}

/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr(lua_State* L, const TValue* t, const char* k)
{
  int hres;
  TString* str = luaS_new(L, k);
  api_checkpop(L, 1);
  if (t->tt_ != ctb(LUA_VTABLE))
    hres = HNOTATABLE;
  else
  {
    hres = luaH_psetstr(hvalue(t), str, &L->stack.p[L->top - 1]);
  }
  if (hres == HOK)
  {
    luaV_finishfastset(L, t, (&(L)->stack.p[L->top - 1]));
    L->top--; /* pop value */
  }
  else
  {
    setslots(L, L->top, str); /* push 'str' (to make it a TValue) */
    api_incr_top(L);
    luaV_finishset(L, t, (&(L)->stack.p[L->top - 1]), (&(L)->stack.p[L->top - 2]), hres);
    L->top -= 2; /* pop value and key */
  }
  lua_unlock(L); /* lock done by caller */
}

LUA_API void lua_setglobal(lua_State* L, const char* name)
{
  TValue gt;
  lua_lock(L); /* unlock done in 'auxsetstr' */
  getGlobalTable(L, &gt);
  auxsetstr(L, &gt, name);
}

LUA_API void lua_settable(lua_State* L, int idx)
{
  TValue* t;
  int hres;
  lua_lock(L);
  api_checkpop(L, 2);
  t = index2value(L, idx);
  if (t->tt_ != ctb(LUA_VTABLE))
    hres = HNOTATABLE;
  else
  {
    hres = luaH_pset(hvalue(t), &L->stack.p[L->top - 2], &L->stack.p[L->top - 1]);
  }
  if (hres == HOK)
    luaV_finishfastset(L, t, (&(L)->stack.p[L->top - 1]));
  else
    luaV_finishset(L, t, (&(L)->stack.p[L->top - 2]), (&(L)->stack.p[L->top - 1]), hres);
  L->top -= 2; /* pop index and value */
  lua_unlock(L);
}

LUA_API void lua_setfield(lua_State* L, int idx, const char* k)
{
  lua_lock(L); /* unlock done in 'auxsetstr' */
  auxsetstr(L, index2value(L, idx), k);
}

LUA_API void lua_seti(lua_State* L, int idx, lua_Integer n)
{
  TValue* t;
  int hres;
  lua_lock(L);
  api_checkpop(L, 1);
  t = index2value(L, idx);
  if (t->tt_ != ctb(LUA_VTABLE))
    hres = HNOTATABLE;
  else
  {
    Table* h = hvalue(t);
    lua_Unsigned u = (lua_Unsigned)n - 1u;
    if (u < h->asize)
    {
      lu_byte* tag = getArrTag(h, u);
      if (checknoTM(h->metatable, TM_NEWINDEX) || (*tag & NOVARIANT_MASK) != LUA_TNIL)
      {
        fval2arr(h, u, tag, &L->stack.p[L->top - 1]);
        hres = HOK;
      }
      else
        hres = ~(int)u;
    }
    else
      hres = luaH_psetint(h, n, &L->stack.p[L->top - 1]);
  }
  if (hres == HOK)
    luaV_finishfastset(L, t, (&(L)->stack.p[L->top - 1]));
  else
  {
    TValue temp;
    setivalue(&temp, n);
    luaV_finishset(L, t, &temp, (&(L)->stack.p[L->top - 1]), hres);
  }
  L->top--; /* pop value */
  lua_unlock(L);
}

static void aux_rawset(lua_State* L, int idx, TValue* key, int n)
{
  Table* t;
  lua_lock(L);
  api_checkpop(L, n);
  t = gettable(L, idx);
  luaH_set(L, t, key, (&(L)->stack.p[L->top - 1]));
  invalidateTMcache(t);
  assert((t->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_barrierback(L, const_cast<GCObject*>(static_cast<const GCObject*>(t)), (&(L)->stack.p[L->top - 1]));
  L->top -= n;
  lua_unlock(L);
}

LUA_API void lua_rawset(lua_State* L, int idx)
{
  aux_rawset(L, idx, (&(L)->stack.p[L->top - 2]), 2);
}

LUA_API void lua_rawsetp(lua_State* L, int idx, const void* p)
{
  TValue k;
  setpvalue(&k, util::remove_const_cast(p));
  aux_rawset(L, idx, &k, 1);
}

LUA_API void lua_rawseti(lua_State* L, int idx, lua_Integer n)
{
  Table* t;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  luaH_setint(L, t, n, (&(L)->stack.p[L->top - 1]));
  assert((t->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_barrierback(L, const_cast<GCObject*>(static_cast<const GCObject*>(t)), (&(L)->stack.p[L->top - 1]));
  L->top--;
  lua_unlock(L);
}

LUA_API int lua_setmetatable(lua_State* L, int objindex)
{
  TValue* obj;
  Table* mt;
  lua_lock(L);
  api_checkpop(L, 1);
  obj = index2value(L, objindex);
  if ((slottag(L, L->top - 1) & NOVARIANT_MASK) == LUA_TNIL)
    mt = nullptr;
  else
  {
    assert((slottag(L, L->top - 1) == ctb(LUA_VTABLE)) && "table expected");
    mt = slothval(L, L->top - 1);
  }
  switch ((obj->tt_ & NOVARIANT_MASK))
  {
  case LUA_TTABLE:
  {
    hvalue(obj)->metatable = mt;
    if (mt)
    {
      assert(obj->tt_ & BIT_ISCOLLECTABLE);
      luaC_objbarrier(L, gcvalue(obj), mt);
      luaC_checkfinalizer(L, gcvalue(obj), mt);
    }
    break;
  }
  case LUA_TUSERDATA:
  {
    uvalue(obj)->metatable = mt;
    if (mt)
    {
      luaC_objbarrier(L, uvalue(obj), mt);
      assert(obj->tt_ & BIT_ISCOLLECTABLE);
      luaC_checkfinalizer(L, gcvalue(obj), mt);
    }
    break;
  }
  default:
  {
    G(L)->mt[(obj->tt_ & NOVARIANT_MASK)] = mt;
    break;
  }
  }
  L->top--;
  lua_unlock(L);
  return 1;
}

LUA_API int lua_setiuservalue(lua_State* L, int idx, int n)
{
  TValue* o;
  int res;
  lua_lock(L);
  api_checkpop(L, 1);
  o = index2value(L, idx);
  assert((((o)->tt_ == ctb(LUA_VUSERDATA))) && "full userdata expected");
  Udata* u = uvalue(o);
  if (!((unsigned int)n - 1u < (unsigned int)u->nuvalue))
    res = 0; /* 'n' not in [1, o's nuvalue] */
  else
  {
    setobj(L, &u->uv[n - 1], (&(L)->stack.p[L->top - 1]));
    assert(o->tt_ & BIT_ISCOLLECTABLE);
    luaC_barrierback(L, gcvalue(o), (&(L)->stack.p[L->top - 1]));
    res = 1;
  }
  L->top--;
  lua_unlock(L);
  return res;
}

/*
** 'load' and 'call' functions (run Lua code)
*/

static inline void checkresults(lua_State* L, int na, int nr)
{
  (void) L;
  (void) na;
  (void) nr;
  assert((nr == LUA_MULTRET || (nr <= na || L->ci->top >= L->top + ((StkId)(nr - na)))) && "results from function overflow current stack size");
  assert((LUA_MULTRET <= nr && nr <= MAXRESULTS) && "invalid number of results");
}

LUA_API void lua_callk(lua_State* L, int nargs, int nresults,
                       lua_KContext ctx, lua_KFunction k)
{
  StkId func;
  lua_lock(L);
  assert((k == nullptr || !isLua(L->ci)) && "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  assert((L->status == LUA_OK) && "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  func = L->top - (nargs + 1);
  if (k != nullptr && yieldable(L))
  {                               /* need to prepare continuation? */
    L->ci->k = k;             /* save continuation */
    L->ci->ctx = ctx;         /* save context */
    luaD_call(L, func, nresults); /* do the call */
  }
  else                                   /* no continuation or no yieldable */
    luaD_callnoyield(L, func, nresults); /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}

/*
** Execute a protected call.
*/
struct CallS
{ /* data to 'f_call' */
  StkId func;
  int nresults;
};

static void f_call(lua_State* L, void* ud)
{
  struct CallS* c = ((struct CallS*)(ud));
  luaD_callnoyield(L, c->func, c->nresults);
}

LUA_API int lua_pcallk(lua_State* L, int nargs, int nresults, int errfunc,
                       lua_KContext ctx, lua_KFunction k)
{
  struct CallS c;
  TStatus status;
  StkId func;
  lua_lock(L);
  assert((k == nullptr || !isLua(L->ci)) && "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  assert((L->status == LUA_OK) && "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else
  {
    StkId o = index2stack(L, errfunc);
    assert(((slottag(L, o) & NOVARIANT_MASK) == LUA_TFUNCTION) && "error handler must be a function");
    func = (o);
  }
  c.func = L->top - (nargs + 1); /* function to be called */
  if (k == nullptr || !yieldable(L))
  {                        /* no continuation or no yieldable? */
    c.nresults = nresults; /* do a 'conventional' protected call */
    status = luaD_pcall(L, f_call, &c, (c.func), func);
  }
  else
  { /* prepare continuation (call is already protected by 'resume') */
    CallInfo* ci = L->ci;
    ci->k = k;     /* save continuation */
    ci->ctx = ctx; /* save context */
    /* save information for error recovery */
    ci->funcidx = ((int)((c.func)));
    ci->old_errfunc = L->errfunc;
    L->errfunc = func;
    setoah(ci, L->allowhook);       /* save value of 'allowhook' */
    ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */
    luaD_call(L, c.func, nresults); /* do the call */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->old_errfunc;
    status = LUA_OK; /* if it is here, there were no errors */
  }
  adjustresults(L, nresults);
  lua_unlock(L);
  return ((int)(status));
}

LUA_API int lua_load(lua_State* L, lua_Reader reader, void* data,
                     const char* chunkname, const char* mode)
{
  ZIO z;
  TStatus status;
  lua_lock(L);
  luaC_checkGC(L);
  if (!chunkname)
    chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK)
  {                                            /* no errors? */
    TValue* s = &L->stack.p[L->top - 1];
    LClosure* f = clLvalue(s); /* get new function */
    if (f->nupvalues >= 1)
    { /* does it have an upvalue? */
      /* get global table from registry */
      TValue gt;
      getGlobalTable(L, &gt);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, upvalval(f->upvals[0]), &gt);
      luaC_barrier(L, f->upvals[0], &gt);
    }
  }
  lua_unlock(L);
  return ((int)(status));
}

/*
** Dump a Lua function, calling 'writer' to write its parts. Ensure
** the stack returns with its original size.
*/
LUA_API int lua_dump(lua_State* L, lua_Writer writer, void* data, int strip)
{
  int status;
  StkId otop = (L->top); /* original top */
  TValue* f = (&(L)->stack.p[L->top - 1]);           /* function to be dumped */
  lua_lock(L);
  api_checkpop(L, 1);
  assert((((f)->tt_ == ctb(LUA_VLCL))) && "Lua function expected");
  status = luaU_dump(L, clLvalue(f)->p, writer, data, strip);
  L->top = (static_cast<StkId>(otop)); /* restore top */
  lua_unlock(L);
  return status;
}

LUA_API int lua_status(lua_State* L)
{
  return ((int)(L->status));
}

/*
** Garbage-collection function. The explicit parameters cover all
** options: 'stepsize' is used by LUA_GCSTEP and 'param'/'value' by
** LUA_GCPARAM; other options ignore them (they default in lua.h).
*/
LUA_API int lua_gc(lua_State* L, int what, size_t stepsize, int param, int value)
{
  int res = 0;
  global_State* g = G(L);
  if (g->gcstp & (GCSTPGC | GCSTPCLS)) /* internal stop? */
    return -1;                         /* all options are invalid when stopped */
  lua_lock(L);
  switch (what)
  {
  case LUA_GCSTOP:
  {
    g->gcstp = GCSTPUSR; /* stopped by the user */
    break;
  }
  case LUA_GCRESTART:
  {
    luaE_setdebt(g, 0);
    g->gcstp = 0; /* (other bits must be zero here) */
    break;
  }
  case LUA_GCCOLLECT:
  {
    luaC_fullgc(L, 0);
    break;
  }
  case LUA_GCCOUNT:
  {
    /* GC values are expressed in Kbytes: #bytes/2^10 */
    res = ((int)(gettotalbytes(g) >> 10));
    break;
  }
  case LUA_GCCOUNTB:
  {
    res = ((int)(gettotalbytes(g) & 0x3ff));
    break;
  }
  case LUA_GCSTEP:
  {
    lu_byte oldstp = g->gcstp;
    l_mem n = ((l_mem)(stepsize));
    l_mem newdebt;
    int work = 0; /* true if GC did some work */
    g->gcstp = 0; /* allow GC to run (other bits must be zero here) */
    if (n <= 0)
      newdebt = 0;                      /* force to run one basic step */
    else if (g->GCdebt >= n - MAX_LMEM) /* no overflow? */
      newdebt = g->GCdebt - n;
    else                   /* overflow */
      newdebt = -MAX_LMEM; /* set debt to miminum value */
    luaE_setdebt(g, newdebt);
    {
      if (G(L)->GCdebt <= 0)
      {
        luaC_step(L);
        work = 1;
      }
      condchangemem(L, (void) 0, work = 1, 0);
    }
    if (work && g->gcstate == GCSpause) /* end of cycle? */
      res = 1;                          /* signal it */
    g->gcstp = oldstp;                  /* restore previous state */
    break;
  }
  case LUA_GCISRUNNING:
  {
    res = gcrunning(g);
    break;
  }
  case LUA_GCGEN:
  {
    res = (g->gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
    luaC_changemode(L, KGC_GENMINOR);
    break;
  }
  case LUA_GCINC:
  {
    res = (g->gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
    luaC_changemode(L, KGC_INC);
    break;
  }
  case LUA_GCPARAM:
  {
    assert((0 <= param && param < LUA_GCPN) && "invalid parameter");
    res = ((int)(luaO_applyparam(g->gcparams[param], 100)));
    if (value >= 0)
      g->gcparams[param] = luaO_codeparam(((unsigned int)(value)));
    break;
  }
  default:
    res = -1; /* invalid option */
  }
  lua_unlock(L);
  return res;
}

/*
** miscellaneous functions
*/

LUA_API int lua_error(lua_State* L)
{
  TValue* errobj;
  lua_lock(L);
  errobj = (&(L)->stack.p[L->top - 1]);
  api_checkpop(L, 1);
  /* error object is the memory error message? */
  if ((((errobj)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) &&
      eqshrstr(tsvalue(errobj), G(L)->memerrmsg))
    luaD_throw(L, LUA_ERRMEM); /* raise a memory error */
  else
    luaG_errormsg(L); /* raise a regular error */
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0; /* to avoid warnings */
}

LUA_API int lua_next(lua_State* L, int idx)
{
  Table* t;
  int more;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  more = luaH_next(L, t, L->top - 1);
  if (more)
    api_incr_top(L);
  else          /* no more elements */
    L->top--; /* pop key */
  lua_unlock(L);
  return more;
}

LUA_API void lua_toclose(lua_State* L, int idx)
{
  StkId o;
  lua_lock(L);
  o = index2stack(L, idx);
  assert((L->tbclist < o) && "given index below or equal a marked one");
  luaF_newtbcupval(L, o);        /* create new to-be-closed upvalue */
  L->ci->callstatus |= CIST_TBC; /* mark that function has TBC slots */
  lua_unlock(L);
}

LUA_API void lua_concat(lua_State* L, int n)
{
  lua_lock(L);
  api_checknelems(L, n);
  if (n > 0)
  {
    luaV_concat(L, n);
    luaC_checkGC(L);
  }
  else
  {                                                   /* nothing to concatenate */
    setslots(L, L->top, luaS_newlstr(L, "", 0)); /* push empty string */
    api_incr_top(L);
  }
  lua_unlock(L);
}

LUA_API void lua_len(lua_State* L, int idx)
{
  TValue* t;
  lua_lock(L);
  t = index2value(L, idx);
  luaV_objlen(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API lua_Alloc lua_getallocf(lua_State* L, void** ud)
{
  lua_Alloc f;
  lua_lock(L);
  if (ud)
    *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}

LUA_API void lua_setallocf(lua_State* L, lua_Alloc f, void* ud)
{
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}

LUA_API void lua_setmemlimit(lua_State* L, size_t limit)
{
  G(L)->memlimit = limit;
}

LUA_API size_t lua_getmemlimit(lua_State* L)
{
  return G(L)->memlimit;
}

void lua_setwarnf(lua_State* L, lua_WarnFunction f, void* ud)
{
  lua_lock(L);
  G(L)->ud_warn = ud;
  G(L)->warnf = f;
  lua_unlock(L);
}

void lua_warning(lua_State* L, const char* msg, int tocont)
{
  lua_lock(L);
  luaE_warning(L, msg, tocont);
  lua_unlock(L);
}

LUA_API void* lua_newuserdatauv(lua_State* L, size_t size, int nuvalue)
{
  Udata* u;
  lua_lock(L);
  assert((0 <= nuvalue && nuvalue < SHRT_MAX) && "invalid value");
  u = luaS_newudata(L, size, ((unsigned short)(nuvalue)));
  setslotu(L, L->top, u);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getudatamem(u);
}

static const char* aux_upvalue(TValue* fi, int n, TValue** val,
                               GCObject** owner)
{
  switch (((fi)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VCCL:
  { /* C closure */
    CClosure* f = clCvalue(fi);
    if (!(((unsigned int)(n)) - 1u < ((unsigned int)(f->nupvalues))))
      return nullptr; /* 'n' not in [1, f->nupvalues] */
    *val = &f->upvalue[n - 1];
    if (owner)
    {
      assert((f->tt & NOVARIANT_MASK) >= LUA_TSTRING);
      *owner = const_cast<GCObject*>(static_cast<const GCObject*>(f));
    }
    return "";
  }
  case LUA_VLCL:
  { /* Lua closure */
    LClosure* f = clLvalue(fi);
    TString* name;
    Proto* p = f->p;
    if (!(((unsigned int)(n)) - 1u < ((unsigned int)(p->sizeupvalues))))
      return nullptr; /* 'n' not in [1, p->sizeupvalues] */
    *val = upvalval(f->upvals[n - 1]);
    if (owner)
    {
      assert((f->upvals[n - 1]->tt & NOVARIANT_MASK) >= LUA_TSTRING);
      *owner = const_cast<GCObject*>(static_cast<const GCObject*>(f->upvals[n - 1]));
    }
    name = p->upvalues[n - 1].name;
    return (name == nullptr) ? "(no name)" : getstr(name);
  }
  default:
    return nullptr; /* not a closure */
  }
}

LUA_API const char* lua_getupvalue(lua_State* L, int funcindex, int n)
{
  const char* name;
  TValue* val = nullptr; /* to avoid warnings */
  lua_lock(L);
  name = aux_upvalue(index2value(L, funcindex), n, &val, nullptr);
  if (name)
  {
    setslot(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}

LUA_API const char* lua_setupvalue(lua_State* L, int funcindex, int n)
{
  const char* name;
  TValue* val = nullptr;     /* to avoid warnings */
  GCObject* owner = nullptr; /* to avoid warnings */
  TValue* fi;
  lua_lock(L);
  fi = index2value(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner);
  if (name)
  {
    L->top--;
    setobj(L, val, (&(L)->stack.p[L->top]));
    luaC_barrier(L, owner, val);
  }
  lua_unlock(L);
  return name;
}

static UpVal** getupvalref(lua_State* L, int fidx, int n, LClosure** pf)
{
  static const UpVal* const nullup = nullptr;
  LClosure* f;
  TValue* fi = index2value(L, fidx);
  assert((((fi)->tt_ == ctb(LUA_VLCL))) && "Lua function expected");
  f = clLvalue(fi);
  if (pf)
    *pf = f;
  if (1 <= n && n <= f->p->sizeupvalues)
    return &f->upvals[n - 1]; /* get its upvalue pointer */
  else
    return (UpVal**) &nullup;
}

LUA_API void* lua_upvalueid(lua_State* L, int fidx, int n)
{
  TValue* fi = index2value(L, fidx);
  switch (((fi)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VLCL:
  { /* lua closure */
    return *getupvalref(L, fidx, n, nullptr);
  }
  case LUA_VCCL:
  { /* C closure */
    CClosure* f = clCvalue(fi);
    if (1 <= n && n <= f->nupvalues)
      return &f->upvalue[n - 1];
    /* else */
  } /* FALLTHROUGH */
  case LUA_VLCF:
    return nullptr; /* light C functions have no upvalues */
  default:
  {
    assert((0) && "function expected");
    return nullptr;
  }
  }
}

LUA_API void lua_upvaluejoin(lua_State* L, int fidx1, int n1,
                             int fidx2, int n2)
{
  LClosure* f1;
  UpVal** up1 = getupvalref(L, fidx1, n1, &f1);
  UpVal** up2 = getupvalref(L, fidx2, n2, nullptr);
  assert((*up1 != nullptr && *up2 != nullptr) && "invalid upvalue index");
  *up1 = *up2;
  luaC_objbarrier(L, f1, *up1);
}

/*
** {======================================================================
** Public API veneer: out-of-line wrappers over the primitive API
** functions. These were macros in upstream Lua (F8); they are compiled
** functions here — all cold paths (ryanbean decision, 2026-07-25).
** =======================================================================
*/

LUA_API int lua_upvalueindex(int i)
{
  return LUA_REGISTRYINDEX - i;
}

LUA_API void lua_call(lua_State* L, int nargs, int nresults)
{
  lua_callk(L, nargs, nresults, 0, nullptr);
}

LUA_API int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc)
{
  return lua_pcallk(L, nargs, nresults, errfunc, 0, nullptr);
}

LUA_API int lua_yield(lua_State* L, int nresults)
{
  return lua_yieldk(L, nresults, 0, nullptr);
}

LUA_API void* lua_getextraspace(lua_State* L)
{
  return (void*)((char*)L + sizeof(lua_State));
}

LUA_API lua_Number lua_tonumber(lua_State* L, int idx)
{
  return lua_tonumberx(L, idx, nullptr);
}

LUA_API lua_Integer lua_tointeger(lua_State* L, int idx)
{
  return lua_tointegerx(L, idx, nullptr);
}

LUA_API void lua_pop(lua_State* L, int n)
{
  lua_settop(L, -n - 1);
}

LUA_API void lua_newtable(lua_State* L)
{
  lua_createtable(L, 0, 0);
}

LUA_API void lua_pushcfunction(lua_State* L, lua_CFunction f)
{
  lua_pushcclosure(L, f, 0);
}

LUA_API void lua_register(lua_State* L, const char* name, lua_CFunction f)
{
  lua_pushcfunction(L, f);
  lua_setglobal(L, name);
}

LUA_API int lua_isfunction(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TFUNCTION;
}

LUA_API int lua_istable(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TTABLE;
}

LUA_API int lua_islightuserdata(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TLIGHTUSERDATA;
}

LUA_API int lua_isnil(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TNIL;
}

LUA_API int lua_isboolean(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TBOOLEAN;
}

LUA_API int lua_isthread(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TTHREAD;
}

LUA_API int lua_isnone(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TNONE;
}

LUA_API int lua_isnoneornil(lua_State* L, int idx)
{
  return lua_type(L, idx) <= 0;
}

LUA_API const char* lua_pushliteral(lua_State* L, const char* s)
{
  return lua_pushstring(L, s);
}

LUA_API void lua_pushglobaltable(lua_State* L)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
}

LUA_API const char* lua_tostring(lua_State* L, int idx)
{
  return lua_tolstring(L, idx, nullptr);
}

LUA_API void lua_insert(lua_State* L, int idx)
{
  lua_rotate(L, idx, 1);
}

LUA_API void lua_remove(lua_State* L, int idx)
{
  lua_rotate(L, idx, -1);
  lua_pop(L, 1);
}

LUA_API void lua_replace(lua_State* L, int idx)
{
  lua_copy(L, -1, idx);
  lua_pop(L, 1);
}

LUA_API void* lua_newuserdata(lua_State* L, size_t sz)
{
  return lua_newuserdatauv(L, sz, 1);
}

LUA_API int lua_getuservalue(lua_State* L, int idx)
{
  return lua_getiuservalue(L, idx, 1);
}

LUA_API int lua_setuservalue(lua_State* L, int idx)
{
  return lua_setiuservalue(L, idx, 1);
}

LUA_API int lua_resetthread(lua_State* L)
{
  return lua_closethread(L, nullptr);
}

/* }====================================================================== */
