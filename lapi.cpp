/*
** $Id: lapi.c $
** Lua API
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"

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
#include "literals.h"


const byte_view lua_ident = lt::lua_ident_str;

namespace lua::api
{

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

struct IndexAddr
{
  StkId slot;
  TValue* row;
};

static IndexAddr resolveindex(lua_State* L, int idx)
{
  CallInfo* ci = L->ci;
  if (idx > 0)
  {
    StkId o = ci->func + idx;
    assert(idx <= ci->top - (ci->func + 1) && "unacceptable index");
    if (o >= L->top)
      return {0, &L->l_G->nilvalue};
    else
      return {o, nullptr};
  }
  else if (!ispseudo(idx))
  { /* negative index */
    assert(idx != 0 && -idx <= L->top - (ci->func + 1) && "invalid index");
    return {L->top + idx, nullptr};
  }
  else if (idx == LUA_REGISTRYINDEX)
    return {0, &L->l_G->l_registry};
  else
  { /* upvalues */
    idx = LUA_REGISTRYINDEX - idx;
    assert(idx <= MAXUPVAL + 1 && "upvalue index too large");
    if (slottag(L, ci->func) == ctb(LUA_VCCL))
    { /* C closure? */
      CClosure* func = slotclCval(L, ci->func);
      return {0, (idx <= (int)func->upvalue.size()) ? &func->upvalue[idx - 1]
                                          : &L->l_G->nilvalue};
    }
    else
    { /* light C function or Lua function (through a hook)?) */
      assert(slottag(L, ci->func) == ctb(LUA_VLCF) && "caller not a C function");
      return {0, &L->l_G->nilvalue}; /* no upvalues */
    }
  }
}

static TValue index2val(lua_State* L, int idx)
{
  IndexAddr a = resolveindex(L, idx);
  return (a.row != nullptr) ? getobj(a.row) : getslot(L, a.slot);
}

static int isvalidindex(lua_State* L, int idx)
{
  IndexAddr a = resolveindex(L, idx);
  return a.row != &L->l_G->nilvalue;
}

static void setindexval(lua_State* L, int idx, const TValue* v)
{
  IndexAddr a = resolveindex(L, idx);
  if (a.row == nullptr)
    setslot(L, a.slot, v);
  else
  {
    assert(a.row != &L->l_G->nilvalue && "invalid index");
    setobj(L, a.row, v);
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
    assert(o < L->top && "invalid index");
    return o;
  }
  else
  { /* non-positive index */
    assert(idx != 0 && -idx <= L->top - (ci->func + 1) && "invalid index");
    assert(!ispseudo(idx) && "invalid index");
    return L->top + idx;
  }
}

}

int lua_checkstack(lua_State* L, int n)
{
  int res;
  CallInfo* ci;
  lua_lock(L);
  ci = L->ci;
  assert(n >= 0 && "negative 'n'");
  if (L->stack_last - L->top > n) /* stack large enough? */
    res = 1;                          /* yes; check is OK */
  else                                /* need to grow stack */
    res = luaD_growstack(L, n, 0);
  if (res && ci->top < L->top + n)
    ci->top = L->top + n; /* adjust frame top */
  lua_unlock(L);
  return res;
}


void lua_xmove(lua_State* from, lua_State* to, int n)
{
  int i;
  if (from == to)
    return;
  lua_lock(to);
  api_checkpop(from, n);
  assert(from->l_G == to->l_G && "moving among independent states");
  assert(to->ci->top >= to->top + n && "stack overflow");
  from->top -= n;
  for (i = 0; i < n; i++)
  {
    copyslot(to, to->top, from, from->top + i);
    to->top++; /* stack already checked by previous 'api_check' */
  }
  lua_unlock(to);
}


lua_CFunction lua_atpanic(lua_State* L, lua_CFunction panicf)
{
  lua_CFunction old;
  lua_lock(L);
  old = L->l_G->panic;
  L->l_G->panic = panicf;
  lua_unlock(L);
  return old;
}


lua_Number lua_version(lua_State*)
{
  return LUA_VERSION_NUM;
}

namespace lua::api
{

/*
** basic stack manipulation
*/

/*
** convert an acceptable stack index into an absolute index
*/
}

int lua_absindex(lua_State* L, int idx)
{
  return (idx > 0 || ispseudo(idx))
             ? idx
             : (int)(L->top - L->ci->func) + idx;
}


int lua_gettop(lua_State* L)
{
  return (int)(L->top - (L->ci->func + 1));
}


void lua_settop(lua_State* L, int idx)
{
  CallInfo* ci;
  StkId func, newtop;
  int diff; /* difference for new top */
  lua_lock(L);
  ci = L->ci;
  func = ci->func;
  if (idx >= 0)
  {
    assert(idx <= ci->top - (func + 1) && "new top too large");
    diff = (int)(((func + 1) + idx) - L->top);
    for (; diff > 0; diff--)
      setslottag(L, L->top++, LUA_VNIL); /* clear new slots */
  }
  else
  {
    assert(-(idx + 1) <= (L->top - (func + 1)) && "invalid new top");
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


void lua_closeslot(lua_State* L, int idx)
{
  StkId level;
  lua_lock(L);
  level = index2stack(L, idx);
  assert((L->ci->callstatus & CIST_TBC) && (L->tbclist == level) && "no variable to close at given level");
  level = luaF_close(L, level, CLOSEKTOP, 0);
  setslottag(L, level, LUA_VNIL);
  lua_unlock(L);
}

namespace lua::api
{

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
    TValue temp = getslot(L, from);
    copyslot(L, from, to);
    setslot(L, to, &temp);
  }
}

/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
}

void lua_rotate(lua_State* L, int idx, int n)
{
  StkId p, t, m;
  lua_lock(L);
  t = L->top - 1;        /* end of stack segment being rotated */
  p = index2stack(L, idx); /* start of segment */
  assert(L->tbclist < p && "moving a to-be-closed slot");
  assert((n >= 0 ? n : -n) <= (t - p + 1) && "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1); /* end of prefix */
  reverse(L, p, m);                 /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);             /* reverse the suffix */
  reverse(L, p, t);                 /* reverse the entire segment */
  lua_unlock(L);
}


void lua_copy(lua_State* L, int fromidx, int toidx)
{
  lua_lock(L);
  TValue fr = index2val(L, fromidx);
  assert(isvalidindex(L, toidx) && "invalid index");
  setindexval(L, toidx, &fr);
  if (isupvalue(toidx)) /* function upvalue? */
  {
    StkId fv = L->ci->func;
    luaC_barrier(L, slotclCval(L, fv), &fr);
  }
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
  lua_unlock(L);
}


void lua_pushvalue(lua_State* L, int idx)
{
  lua_lock(L);
  TValue v = index2val(L, idx);
  setslot(L, L->top, &v);
  api_incr_top(L);
  lua_unlock(L);
}

namespace lua::api
{

/*
** access functions (stack -> C)
*/

}

int lua_type(lua_State* L, int idx)
{
  return (isvalidindex(L, idx) ? novariant(index2val(L, idx).tt_)
                               : LUA_TNONE);
}


HostString lua_typename(lua_State* L, int t)
{
  (void) L;
  assert(LUA_TNONE <= t && t < LUA_NUMTYPES && "invalid type");
  byte_view n = ttypename(t);
  return HostString(n);
}


int lua_iscfunction(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return (ttislcf(&o) || ttisCclosure(&o));
}


int lua_islightcfunction(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return ttislcf(&o);
}


int lua_isinteger(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return ttisinteger(&o);
}


int lua_isnumber(lua_State* L, int idx)
{
  NumResult n;
  TValue o = index2val(L, idx);
  return tonumber(&o, &n);
}


int lua_isstring(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return (ttisstring(&o) || cvt2str(&o));
}


int lua_isuserdata(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return (ttisfulluserdata(&o) || ttislightuserdata(&o));
}


int lua_rawequal(lua_State* L, int index1, int index2)
{
  TValue o1 = index2val(L, index1);
  TValue o2 = index2val(L, index2);
  return (isvalidindex(L, index1) && isvalidindex(L, index2))
             ? luaV_rawequalobj(&o1, &o2)
             : 0;
}


void lua_arith(lua_State* L, int op)
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
  luaO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
  L->top--; /* pop second operand */
  lua_unlock(L);
}


int lua_compare(lua_State* L, int index1, int index2, int op)
{
  int i = 0;
  lua_lock(L); /* may call tag method */
  TValue o1 = index2val(L, index1);
  TValue o2 = index2val(L, index2);
  if (isvalidindex(L, index1) && isvalidindex(L, index2))
  {
    switch (op)
    {
    case LUA_OPEQ:
      i = luaV_equalobj(L, &o1, &o2);
      break;
    case LUA_OPLT:
      i = luaV_lessthan(L, &o1, &o2);
      break;
    case LUA_OPLE:
      i = luaV_lessequal(L, &o1, &o2);
      break;
    default:
      assert(0 && "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}


std::optional<byte_view> lua_numbertocstringi(lua_State* L, int idx, byte_span buff)
{
  TValue o = index2val(L, idx);
  if (ttisnumber(&o))
  {
    size_t len = luaO_tostringbuff(&o, buff);
    return byte_view(buff.data(), len);
  }
  else
    return std::nullopt;
}


std::optional<HostString> lua_numbertocstring(lua_State* L, int idx)
{
  byte_inline<LUA_N2SBUFFSZ> buff;
  std::optional<byte_view> s = lua_numbertocstringi(L, idx, buff);
  if (!s.has_value())
    return std::nullopt;
  return HostString(*s);
}


size_t lua_stringtonumber(lua_State* L, const HostString& s)
{
  TValue v;
  size_t sz = luaO_str2num(s.to_utf8(), &v);
  if (sz != 0)
  {
    setslot(L, L->top, &v);
    api_incr_top(L);
  }
  return sz;
}


std::optional<lua_Number> lua_tonumberx(lua_State* L, int idx)
{
  NumResult n = {0};
  TValue o = index2val(L, idx);
  if (!tonumber(&o, &n))
    return std::nullopt;
  return n.v;
}


std::optional<lua_Integer> lua_tointegerx(lua_State* L, int idx)
{
  IntResult res = {0};
  TValue o = index2val(L, idx);
  if (!tointeger(&o, &res))
    return std::nullopt;
  return res.v;
}


int lua_toboolean(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return !(ttisfalse(&o) || ttisnil(&o));
}


std::optional<byte_view> lua_tolstring(lua_State* L, int idx)
{
  lua_lock(L);
  TValue o = index2val(L, idx);
  if (!ttisstring(&o))
  {
    if (!cvt2str(&o))
    {
      lua_unlock(L);
      return std::nullopt;
    }
    luaO_tostring(L, &o);
    setindexval(L, idx, &o);
    luaC_checkGC(L);
  }
  lua_unlock(L);
  assert(ttisstring(&o));
  TString* s = tsvalue(&o);
  return s->data.view();
}


const InternalString* lua_toistring(lua_State* L, int idx)
{
  lua_lock(L);
  TValue o = index2val(L, idx);
  if (!ttisstring(&o))
  {
    if (!cvt2str(&o))
    { /* not convertible? */
      lua_unlock(L);
      return nullptr;
    }
    luaO_tostring(L, &o);
    setindexval(L, idx, &o);
    luaC_checkGC(L);
  }
  lua_unlock(L);
  assert(ttisstring(&o));
  return &tsvalue(&o)->data;
}


lua_Unsigned lua_rawlen(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  switch (ttypetag(&o))
  {
  case LUA_VSHRSTR:
    return tsslen(tsvalue(&o));
  case LUA_VUSERDATA:
  {
    Udata* pu = uvalue(&o);
    return pu->zb ? 0 : pu->data.size();
  }
  case LUA_VTABLE:
  {
    lua_Unsigned res;
    lua_lock(L);
    res = hvalue(&o)->luaH_getn(L);
    lua_unlock(L);
    return res;
  }
  default:
    return 0;
  }
}


lua_CFunction lua_tocfunction(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  if (ttislcf(&o))
    return fvalue(&o);
  else if (ttisCclosure(&o))
  {
    return clCvalue(&o)->f;
  }
  else
    return nullptr; /* not a C function */
}

namespace lua::api
{

static inline void* touserdata(const TValue* o)
{
  switch (ttype(o))
  {
  case LUA_TUSERDATA:
    return getudatamem(uvalue(o));
  case LUA_TLIGHTUSERDATA:
    assert(ttislightuserdata(o));
    return pvalue(o);
  default:
    return nullptr;
  }
}

}

void* lua_touserdata(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  return touserdata(&o);
}


lua_State* lua_tothread(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  if (!ttisthread(&o))
    return nullptr;
  return thvalue(&o);
}

namespace lua::api
{

/*
** Returns a pointer to the internal representation of an object.
** Note that ISO C does not allow the conversion of a pointer to
** function to a 'void*', so the conversion here goes through
** a 'size_t'. (As the returned pointer is only informative, this
** conversion should not be a problem.)
*/
}

const void* lua_topointer(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  switch (ttypetag(&o))
  {
  case LUA_VLCF:
    return o.value_.ref;
  case LUA_VUSERDATA:
  case LUA_VLIGHTUSERDATA:
    return touserdata(&o);
  default:
  {
    if (iscollectable(&o))
      return gcvalue(&o);
    else
      return nullptr;
  }
  }
}


InternalString lua_funcaddressi(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  assert(ttislcf(&o));
  return util::func_to_string(fvalue(&o));
}


byte_view lua_typenamei(lua_State*, int t)
{
  assert(LUA_TNONE <= t && t < LUA_NUMTYPES && "invalid type");
  return ttypename(t);
}

namespace lua::api
{

/*
** push functions (C -> stack)
*/

}

void lua_pushnil(lua_State* L)
{
  lua_lock(L);
  setslottag(L, L->top, LUA_VNIL);
  api_incr_top(L);
  lua_unlock(L);
}


void lua_pushnumber(lua_State* L, lua_Number n)
{
  lua_lock(L);
  setslotflt(L, L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


void lua_pushinteger(lua_State* L, lua_Integer n)
{
  lua_lock(L);
  setsloti(L, L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


byte_view lua_pushlstring(lua_State* L, byte_view s, size_t off, size_t len)
{
  TString* ts;
  lua_lock(L);
  if (len == 0)
  {
    s = lt::empty_str;
    off = 0;
    len = s.size();
  }
  ts = TString::luaS_createstr(L, s.substr(off, len));
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return ts->data.view();
}


void lua_pushstring(lua_State* L, const std::optional<HostString>& s)
{
  if (!s)
  {
    lua_pushnil(L);
    return;
  }
  lua_lock(L);
  byte_view bytes = s->to_utf8();
  TString* ts = TString::luaS_createstr(L, bytes);
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


void lua_pushistringref(lua_State* L, byte_view bytes)
{
  lua_lock(L);
  TString* ts = TString::luaS_createstr(L, bytes);
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


void lua_pushistring(lua_State* L, InternalString&& s)
{
  lua_lock(L);
  TString* ts = TString::luaS_take(L, std::move(s));
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


void lua_pushistringext(lua_State* L, InternalString&& s)
{
  lua_lock(L);
  TString* ts = TString::luaS_takeexternal(L, std::move(s));
  setslots(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


HostString lua_pushfstring(lua_State* L, const HostString& fmt)
{
  return HostString(lua_pushfstringi(L, fmt.to_utf8())->view());
}


const InternalString* lua_pushfstringi(lua_State* L, byte_view fmt)
{
  const InternalString* ret;
  FmtArgs args;
  lua_lock(L);
  int n = luaO_fmtargcount(fmt);
  api_checknelems(L, n);
  FmtScan sc{fmt};
  while (sc.next())
  {
    StkId slot = L->top - n + args.n;
    switch (sc.spec)
    {
    case 's':
    {
      StkId o = slot;
      if (novariant(slottag(L, o)) == LUA_TNUMBER)
      {
        TValue v = getslot(L, o);
        luaO_tostring(L, &v);
        setslot(L, o, &v);
      }
      assert(novariant(slottag(L, o)) == LUA_TSTRING && "string expected");
      args.str(slottsval(L, o)->data.view());
      break;
    }
    case 'c':
    {
      StkId o = slot;
      assert(slottag(L, o) == LUA_VNUMINT && "integer expected");
      args.chr((int)slotival(L, o));
      break;
    }
    case 'd':
    {
      StkId o = slot;
      assert(slottag(L, o) == LUA_VNUMINT && "integer expected");
      args.num((int)slotival(L, o));
      break;
    }
    case 'I':
    {
      StkId o = slot;
      assert(slottag(L, o) == LUA_VNUMINT && "integer expected");
      args.lint(slotival(L, o));
      break;
    }
    case 'f':
    {
      StkId o = slot;
      assert(slottag(L, o) == LUA_VNUMFLT && "float expected");
      args.flt(slotfltval(L, o));
      break;
    }
    case 'p':
    {
      args.ptr(lua_topointer(L, args.n - n));
      break;
    }
    case 'U':
    {
      StkId o = slot;
      assert(slottag(L, o) == LUA_VNUMINT && "integer expected");
      args.utf8(slotival(L, o));
      break;
    }
    default:
      break;
    }
  }
  const InternalString* is = luaO_pushvfstring(L, fmt, args);
  if (is == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  ret = is;
  if (n > 0)
  {
    copyslot(L, L->top - 1 - n, L->top - 1);
    L->top -= n;
  }
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


void lua_pushcclosure(lua_State* L, lua_CFunction fn, int n)
{
  lua_lock(L);
  if (n == 0)
  {
    CFuncInfo* cfi = lua_createcfuncinfo(L, fn);
    setslotf(L, L->top, cfi);
    api_incr_top(L);
  }
  else
  {
    int i;
    CClosure* cl;
    api_checkpop(L, n);
    assert(n <= MAXUPVAL && "upvalue index too large");
    cl = CClosure::luaF_newCclosure(L, n);
    cl->f = fn;
    for (i = 0; i < n; i++)
    {
      slot2obj(L, &cl->upvalue[i], L->top - n + i);
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


void lua_pushboolean(lua_State* L, int b)
{
  lua_lock(L);
  if (b)
    setslottag(L, L->top, LUA_VTRUE);
  else
    setslottag(L, L->top, LUA_VFALSE);
  api_incr_top(L);
  lua_unlock(L);
}


void lua_pushlightuserdata(lua_State* L, void* p)
{
  lua_lock(L);
  setslotp(L, L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


int lua_pushthread(lua_State* L)
{
  lua_lock(L);
  setslotth(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (mainthread(L->l_G) == L);
}

namespace lua::api
{

/*
** get functions (Lua -> stack)
*/

static int auxgetstr(lua_State* L, const TValue* t, byte_view k)
{
  lu_byte tag;
  TValue res;
  TString* str = TString::luaS_createstr(L, k);
  if (!ttistable(t))
    tag = LUA_VNOTABLE;
  else
  {
    tag = hvalue(t)->luaH_getstr(str, &res);
  }
  if (!tagisempty(tag))
  {
    setslot(L, L->top, &res);
    api_incr_top(L);
  }
  else
  {
    setslots(L, L->top, str);
    api_incr_top(L);
    TValue key = getslot(L, L->top - 1);
    tag = luaV_finishget(L, t, &key, L->top - 1, tag, NOSLOT);
  }
  lua_unlock(L);
  return novariant(tag);
}

/*
** The following function assumes that the registry cannot be a weak
** table; so, an emergency collection while using the global table
** cannot collect it.
*/
static void getGlobalTable(lua_State* L, TValue* gt)
{
  Table* registry = hvalue(&L->l_G->l_registry);
  lu_byte tag = registry->luaH_getint(LUA_RIDX_GLOBALS, gt);
  (void) tag; /* avoid not-used warnings when checks are off */
  assert(novariant(tag) == LUA_TTABLE && "global table must exist");
}

}

int lua_getglobal(lua_State* L, const HostString& name)
{
  return lua_getglobali(L, name.to_utf8());
}


int lua_getglobali(lua_State* L, byte_view name)
{
  TValue gt;
  lua_lock(L);
  getGlobalTable(L, &gt);
  return auxgetstr(L, &gt, name);
}


int lua_gettable(lua_State* L, int idx)
{
  lu_byte tag;
  lua_lock(L);
  api_checkpop(L, 1);
  TValue t = index2val(L, idx);
  if (!ttistable(&t))
    tag = LUA_VNOTABLE;
  else
  {
    TValue key = getslot(L, L->top - 1);
    TValue res;
    tag = hvalue(&t)->luaH_get(&key, &res);
    if (!tagisempty(tag))
      setslot(L, L->top - 1, &res);
  }
  if (tagisempty(tag))
  {
    TValue key = getslot(L, L->top - 1);
    tag = luaV_finishget(L, &t, &key, L->top - 1, tag, NOSLOT);
  }
  lua_unlock(L);
  return novariant(tag);
}


int lua_getfield(lua_State* L, int idx, const HostString& k)
{
  lua_lock(L);
  TValue t = index2val(L, idx);
  return auxgetstr(L, &t, k.to_utf8());
}


int lua_geti(lua_State* L, int idx, lua_Integer n)
{
  lu_byte tag;
  lua_lock(L);
  TValue t = index2val(L, idx);
  TValue res;
  if (!ttistable(&t))
    tag = LUA_VNOTABLE;
  else
  {
    Table* h = hvalue(&t);
    lua_Integer u = l_intsub(n, 1);
    if (l_ult(u, h->asize()))
    {
      tag = getArrTag(h, u);
      if (!tagisempty(tag))
        farr2val(h, u, tag, &res);
    }
    else
      tag = h->luaH_getint(n, &res);
  }
  if (tagisempty(tag))
  {
    TValue key;
    setivalue(&key, n);
    tag = luaV_finishget(L, &t, &key, L->top, tag, NOSLOT);
  }
  else
    setslot(L, L->top, &res);
  api_incr_top(L);
  lua_unlock(L);
  return novariant(tag);
}

namespace lua::api
{

static int finishrawget(lua_State* L, lu_byte tag)
{
  if (tagisempty(tag)) /* avoid copying empty items to the stack */
    setslottag(L, L->top, LUA_VNIL);
  api_incr_top(L);
  lua_unlock(L);
  return novariant(tag);
}

static inline Table* gettable(lua_State* L, int idx)
{
  TValue t = index2val(L, idx);
  assert(ttistable(&t) && "table expected");
  return hvalue(&t);
}

}

int lua_rawget(lua_State* L, int idx)
{
  Table* t;
  lu_byte tag;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  {
    TValue key = getslot(L, L->top - 1);
    TValue res;
    tag = t->luaH_get(&key, &res);
    if (!tagisempty(tag))
      setslot(L, L->top - 1, &res);
  }
  L->top--; /* pop key */
  return finishrawget(L, tag);
}


int lua_rawgeti(lua_State* L, int idx, lua_Integer n)
{
  Table* t;
  lu_byte tag;
  lua_lock(L);
  t = gettable(L, idx);
  {
    TValue res;
    lua_Integer u = l_intsub(n, 1);
    if (l_ult(u, t->asize()))
    {
      tag = getArrTag(t, u);
      if (!tagisempty(tag))
        farr2val(t, u, tag, &res);
    }
    else
      tag = t->luaH_getint(n, &res);
    if (!tagisempty(tag))
      setslot(L, L->top, &res);
  }
  return finishrawget(L, tag);
}


int lua_rawgetp(lua_State* L, int idx, const void* p)
{
  Table* t;
  TValue k;
  lua_lock(L);
  t = gettable(L, idx);
  setpvalue(&k, p);
  TValue res;
  lu_byte tag = t->luaH_get(&k, &res);
  if (!tagisempty(tag))
    setslot(L, L->top, &res);
  return finishrawget(L, tag);
}


void lua_createtable(lua_State* L, int narray, int nrec)
{
  Table* t;
  lua_lock(L);
  t = Table::luaH_new(L);
  setsloth(L, L->top, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    t->luaH_resize(L, narray, nrec);
  luaC_checkGC(L);
  lua_unlock(L);
}


int lua_getmetatable(lua_State* L, int objindex)
{
  Table* mt;
  int res = 0;
  lua_lock(L);
  TValue obj = index2val(L, objindex);
  switch (ttype(&obj))
  {
  case LUA_TTABLE:
    mt = hvalue(&obj)->metatable;
    break;
  case LUA_TUSERDATA:
    mt = uvalue(&obj)->metatable;
    break;
  default:
    mt = L->l_G->mt[ttype(&obj)];
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


int lua_getiuservalue(lua_State* L, int idx, int n)
{
  int t;
  lua_lock(L);
  TValue o = index2val(L, idx);
  assert(ttisfulluserdata(&o) && "full userdata expected");
  Udata* u = uvalue(&o);
  if (n <= 0 || n > (int)u->uv.size())
  {
    setslottag(L, L->top, LUA_VNIL);
    t = LUA_TNONE;
  }
  else
  {
    setslot(L, L->top, &u->uv[n - 1]);
    t = novariant(slottag(L, L->top));
  }
  api_incr_top(L);
  lua_unlock(L);
  return t;
}

namespace lua::api
{

/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr(lua_State* L, const TValue* t, byte_view k)
{
  int hres;
  TString* str = TString::luaS_createstr(L, k);
  api_checkpop(L, 1);
  if (!ttistable(t))
    hres = HNOTATABLE;
  else
  {
    TValue val = getslot(L, L->top - 1);
    hres = hvalue(t)->luaH_psetstr(str, &val);
  }
  if (hres == HOK)
  {
    TValue val = getslot(L, L->top - 1);
    luaV_finishfastset(L, t, &val);
    L->top--; /* pop value */
  }
  else
  {
    setslots(L, L->top, str); /* push 'str' (to make it a TValue) */
    api_incr_top(L);
    TValue key = getslot(L, L->top - 1);
    TValue val = getslot(L, L->top - 2);
    luaV_finishset(L, t, &key, &val, hres, NOSLOT);
    L->top -= 2; /* pop value and key */
  }
  lua_unlock(L); /* lock done by caller */
}

}

void lua_setglobal(lua_State* L, const HostString& name)
{
  lua_setglobali(L, name.to_utf8());
}


void lua_setglobali(lua_State* L, byte_view name)
{
  TValue gt;
  lua_lock(L); /* unlock done in 'auxsetstr' */
  getGlobalTable(L, &gt);
  auxsetstr(L, &gt, name);
}


void lua_settable(lua_State* L, int idx)
{
  int hres;
  lua_lock(L);
  api_checkpop(L, 2);
  TValue t = index2val(L, idx);
  TValue key = getslot(L, L->top - 2);
  TValue val = getslot(L, L->top - 1);
  if (!ttistable(&t))
    hres = HNOTATABLE;
  else
  {
    hres = hvalue(&t)->luaH_pset(&key, &val);
  }
  if (hres == HOK)
    luaV_finishfastset(L, &t, &val);
  else
    luaV_finishset(L, &t, &key, &val, hres, NOSLOT);
  L->top -= 2; /* pop index and value */
  lua_unlock(L);
}


void lua_setfield(lua_State* L, int idx, const HostString& k)
{
  lua_lock(L); /* unlock done in 'auxsetstr' */
  TValue t = index2val(L, idx);
  auxsetstr(L, &t, k.to_utf8());
}


int lua_getfieldi(lua_State* L, int idx, byte_view k)
{
  lua_lock(L);
  TValue t = index2val(L, idx);
  return auxgetstr(L, &t, k);
}


void lua_setfieldi(lua_State* L, int idx, byte_view k)
{
  lua_lock(L); /* unlock done in 'auxsetstr' */
  TValue t = index2val(L, idx);
  auxsetstr(L, &t, k);
}


void lua_seti(lua_State* L, int idx, lua_Integer n)
{
  int hres;
  lua_lock(L);
  api_checkpop(L, 1);
  TValue t = index2val(L, idx);
  TValue val = getslot(L, L->top - 1);
  if (!ttistable(&t))
    hres = HNOTATABLE;
  else
  {
    Table* h = hvalue(&t);
    lua_Integer u = l_intsub(n, 1);
    if (l_ult(u, h->asize()))
    {
      lu_byte tag = getArrTag(h, u);
      if (checknoTM(h->metatable, TM_NEWINDEX) || !tagisempty(tag))
      {
        fval2arr(h, u, &val);
        hres = HOK;
      }
      else
        hres = ~(int)u;
    }
    else
      hres = h->luaH_psetint(n, &val);
  }
  if (hres == HOK)
    luaV_finishfastset(L, &t, &val);
  else
  {
    TValue temp;
    setivalue(&temp, n);
    luaV_finishset(L, &t, &temp, &val, hres, NOSLOT);
  }
  L->top--; /* pop value */
  lua_unlock(L);
}

namespace lua::api
{

static void aux_rawset(lua_State* L, int idx, TValue* key, int n)
{
  Table* t;
  lua_lock(L);
  api_checkpop(L, n);
  t = gettable(L, idx);
  TValue val = getslot(L, L->top - 1);
  t->luaH_set(L, key, &val);
  invalidateTMcache(t);
  assert(novariant(t->tt) >= LUA_TSTRING);
  luaC_barrierback(L, t, &val);
  L->top -= n;
  lua_unlock(L);
}

}

void lua_rawset(lua_State* L, int idx)
{
  TValue key = getslot(L, L->top - 2);
  aux_rawset(L, idx, &key, 2);
}


void lua_rawsetp(lua_State* L, int idx, const void* p)
{
  TValue k;
  setpvalue(&k, p);
  aux_rawset(L, idx, &k, 1);
}


void lua_rawseti(lua_State* L, int idx, lua_Integer n)
{
  Table* t;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  TValue val = getslot(L, L->top - 1);
  t->luaH_setint(L, n, &val);
  assert(novariant(t->tt) >= LUA_TSTRING);
  luaC_barrierback(L, t, &val);
  L->top--;
  lua_unlock(L);
}


int lua_setmetatable(lua_State* L, int objindex)
{
  Table* mt;
  lua_lock(L);
  api_checkpop(L, 1);
  TValue obj = index2val(L, objindex);
  if (tagisempty(slottag(L, L->top - 1)))
    mt = nullptr;
  else
  {
    assert(slottag(L, L->top - 1) == ctb(LUA_VTABLE) && "table expected");
    mt = slothval(L, L->top - 1);
  }
  switch (ttype(&obj))
  {
  case LUA_TTABLE:
  {
    hvalue(&obj)->metatable = mt;
    if (mt)
    {
      assert(iscollectable(&obj));
      luaC_objbarrier(L, gcvalue(&obj), mt);
      luaC_checkfinalizer(L, gcvalue(&obj), mt);
    }
    break;
  }
  case LUA_TUSERDATA:
  {
    uvalue(&obj)->metatable = mt;
    if (mt)
    {
      luaC_objbarrier(L, uvalue(&obj), mt);
      assert(iscollectable(&obj));
      luaC_checkfinalizer(L, gcvalue(&obj), mt);
    }
    break;
  }
  default:
  {
    L->l_G->mt[ttype(&obj)] = mt;
    break;
  }
  }
  L->top--;
  lua_unlock(L);
  return 1;
}


int lua_setiuservalue(lua_State* L, int idx, int n)
{
  int res;
  lua_lock(L);
  api_checkpop(L, 1);
  TValue o = index2val(L, idx);
  assert(ttisfulluserdata(&o) && "full userdata expected");
  Udata* u = uvalue(&o);
  if (!(n >= 1 && (size_t)n <= u->uv.size()))
    res = 0;
  else
  {
    TValue v = getslot(L, L->top - 1);
    setobj(L, &u->uv[n - 1], &v);
    assert(iscollectable(&o));
    luaC_barrierback(L, gcvalue(&o), &v);
    res = 1;
  }
  L->top--;
  lua_unlock(L);
  return res;
}

namespace lua::api
{

/*
** 'load' and 'call' functions (run Lua code)
*/

static inline void checkresults(lua_State* L, int na, int nr)
{
  (void) L;
  (void) na;
  (void) nr;
  assert((nr == LUA_MULTRET || (nr <= na || L->ci->top >= L->top + (nr - na))) && "results from function overflow current stack size");
  assert(LUA_MULTRET <= nr && nr <= MAXRESULTS && "invalid number of results");
}

}

void lua_callk(lua_State* L, int nargs, int nresults,
                       lua_KContext ctx, lua_KFunction k)
{
  StkId func;
  lua_lock(L);
  assert((k == nullptr || !isLua(L->ci)) && "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  assert(L->status == LUA_OK && "cannot do calls on non-normal thread");
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

namespace lua::api
{

/*
** Execute a protected call.
*/
struct CallS : ProtectedCall
{ /* data to 'f_call' */
  StkId func;
  int nresults;
  void run(lua_State* L) override;
};

static void f_call(lua_State* L, CallS* c)
{
  luaD_callnoyield(L, c->func, c->nresults);
}

void CallS::run(lua_State* L)
{
  f_call(L, this);
}

}

int lua_pcallk(lua_State* L, int nargs, int nresults, int errfunc,
                       lua_KContext ctx, lua_KFunction k)
{
  struct CallS c;
  TStatus status;
  StkId func;
  lua_lock(L);
  assert((k == nullptr || !isLua(L->ci)) && "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  assert(L->status == LUA_OK && "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else
  {
    StkId o = index2stack(L, errfunc);
    assert(novariant(slottag(L, o)) == LUA_TFUNCTION && "error handler must be a function");
    func = o;
  }
  c.func = L->top - (nargs + 1); /* function to be called */
  if (k == nullptr || !yieldable(L))
  {                        /* no continuation or no yieldable? */
    c.nresults = nresults; /* do a 'conventional' protected call */
    status = luaD_pcall(L, &c, c.func, func);
  }
  else
  { /* prepare continuation (call is already protected by 'resume') */
    CallInfo* ci = L->ci;
    ci->k = k;     /* save continuation */
    ci->ctx = ctx; /* save context */
    /* save information for error recovery */
    ci->funcidx = (int)c.func;
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
  return status;
}


int lua_loadi(lua_State* L, lua_Reader* reader, std::optional<byte_view> chunkname, std::optional<byte_view> mode)
{
  ZIO z;
  TStatus status;
  lua_lock(L);
  luaC_checkGC(L);
  luaZ_init(L, &z, reader);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK)
  {                                            /* no errors? */
    StkId s = L->top - 1;
    LClosure* f = slotclLval(L, s); /* get new function */
    if (f->upvals.size() >= 1)
    { /* does it have an upvalue? */
      /* get global table from registry */
      TValue gt;
      getGlobalTable(L, &gt);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      upvalsetval(L, f->upvals[0], &gt);
      luaC_barrier(L, f->upvals[0], &gt);
    }
  }
  lua_unlock(L);
  return status;
}


int lua_load(lua_State* L, lua_Reader* reader,
                     const std::optional<HostString>& chunkname,
                     const std::optional<HostString>& mode)
{
  std::optional<byte_view> name = chunkname.has_value() ? std::optional<byte_view>(chunkname->to_utf8()) : std::nullopt;
  std::optional<byte_view> m = mode.has_value() ? std::optional<byte_view>(mode->to_utf8()) : std::nullopt;
  return lua_loadi(L, reader, name, m);
}

namespace lua::api
{

/*
** Dump a Lua function, calling 'writer' to write its parts. Ensure
** the stack returns with its original size.
*/
}

int lua_dump(lua_State* L, lua_Writer* writer, int strip)
{
  int status;
  StkId otop = L->top; /* original top */
  StkId f = L->top - 1;           /* function to be dumped */
  lua_lock(L);
  api_checkpop(L, 1);
  assert(slottag(L, f) == ctb(LUA_VLCL) && "Lua function expected");
  status = luaU_dump(L, slotclLval(L, f)->p, writer, strip);
  L->top = (static_cast<StkId>(otop)); /* restore top */
  lua_unlock(L);
  return status;
}


int lua_status(lua_State* L)
{
  return L->status;
}


int lua_gc(lua_State* L, int what, size_t stepsize, int param, int value)
{
  int res = 0;
  global_State* g = L->l_G;
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
    res = (int)(gettotalbytes(g) >> 10);
    break;
  }
  case LUA_GCCOUNTB:
  {
    res = gettotalbytes(g) & 0x3ff;
    break;
  }
  case LUA_GCSTEP:
  {
    lu_byte oldstp = g->gcstp;
    l_mem n = stepsize;
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
      if (L->l_G->GCdebt <= 0)
      {
        luaC_step(L);
        work = 1;
      }
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
    assert(0 <= param && param < LUA_GCPN && "invalid parameter");
    res = (int)luaO_applyparam(g->gcparams[param], 100);
    if (value >= 0)
      g->gcparams[param] = luaO_codeparam(value);
    break;
  }
  default:
    res = -1; /* invalid option */
  }
  lua_unlock(L);
  return res;
}

namespace lua::api
{

/*
** miscellaneous functions
*/

}

int lua_error(lua_State* L)
{
  lua_lock(L);
  TValue errobj = getslot(L, L->top - 1);
  api_checkpop(L, 1);
  /* error object is the memory error message? */
  if (ttisstring(&errobj) &&
      eqshrstr(tsvalue(&errobj), L->l_G->memerrmsg))
    luaD_throw(L, LUA_ERRMEM); /* raise a memory error */
  else
    luaG_errormsg(L); /* raise a regular error */
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0; /* to avoid warnings */
}


int lua_next(lua_State* L, int idx)
{
  Table* t;
  int more;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx);
  more = t->luaH_next(L, L->top - 1);
  if (more)
    api_incr_top(L);
  else          /* no more elements */
    L->top--; /* pop key */
  lua_unlock(L);
  return more;
}


void lua_toclose(lua_State* L, int idx)
{
  StkId o;
  lua_lock(L);
  o = index2stack(L, idx);
  assert(L->tbclist < o && "given index below or equal a marked one");
  luaF_newtbcupval(L, o);        /* create new to-be-closed upvalue */
  L->ci->callstatus |= CIST_TBC; /* mark that function has TBC slots */
  lua_unlock(L);
}


void lua_concat(lua_State* L, int n)
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
    setslots(L, L->top, TString::luaS_createstr(L, lt::empty_str)); /* push empty string */
    api_incr_top(L);
  }
  lua_unlock(L);
}


void lua_len(lua_State* L, int idx)
{
  lua_lock(L);
  TValue t = index2val(L, idx);
  luaV_objlen(L, L->top, &t, NOSLOT);
  api_incr_top(L);
  lua_unlock(L);
}


lua_Alloc* lua_getallocf(lua_State* L)
{
  lua_lock(L);
  lua_Alloc* a = L->l_G->frealloc;
  lua_unlock(L);
  return a;
}


void lua_setallocf(lua_State* L, lua_Alloc* f)
{
  lua_lock(L);
  L->l_G->frealloc = f;
  lua_unlock(L);
}


void lua_setmemlimit(lua_State* L, size_t limit)
{
  L->l_G->memlimit = limit;
}


size_t lua_getmemlimit(lua_State* L)
{
  return L->l_G->memlimit;
}


void lua_setwarnf(lua_State* L, lua_WarnFunction* f)
{
  lua_lock(L);
  L->l_G->warnf = f;
  lua_unlock(L);
}


void lua_warning(lua_State* L, const HostString& msg, int tocont)
{
  lua_warningi(L, msg.to_utf8(), tocont);
}


void lua_warningi(lua_State* L, byte_view msg, int tocont)
{
  lua_lock(L);
  luaE_warning(L, msg, tocont);
  lua_unlock(L);
}


int lua_reservebytes(lua_State* L, size_t sz)
{
  return luaM_reserve(L, sz);
}


void lua_releasebytes(lua_State* L, size_t sz)
{
  luaM_release(L, sz);
}


void lua_newuserobject(lua_State* L, std::unique_ptr<lua_UserObject> obj, size_t size, int nuvalue)
{
  Udata* u;
  lua_lock(L);
  assert(0 <= nuvalue && nuvalue < SHRT_MAX && "invalid value");
  u = Udata::luaS_newudata(L, 0, nuvalue);
  setslotu(L, L->top, u);
  api_incr_top(L);
  luaM_takeobj(L, size);
  u->objsize = size;
  u->obj = std::move(obj);
  luaC_checkGC(L);
  lua_unlock(L);
}


lua_UserObject* lua_touserobject(lua_State* L, int idx)
{
  TValue o = index2val(L, idx);
  if (!checktype(&o, LUA_TUSERDATA))
    return nullptr;
  return uvalue(&o)->obj.get();
}


void* lua_newuserdatauv(lua_State* L, size_t size, int nuvalue)
{
  Udata* u;
  lua_lock(L);
  assert(0 <= nuvalue && nuvalue < SHRT_MAX && "invalid value");
  u = Udata::luaS_newudata(L, size, nuvalue);
  setslotu(L, L->top, u);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getudatamem(u);
}

namespace lua::api
{

struct UpvalAddr
{
  TValue* row;
  UpVal* uv;
  GCObject* owner;
};

static const InternalString* aux_upvalue(TValue* fi, int n, UpvalAddr* addr)
{
  switch (ttypetag(fi))
  {
  case LUA_VCCL:
  { /* C closure */
    CClosure* f = clCvalue(fi);
    if (!(n >= 1 && (size_t)n <= f->upvalue.size()))
      return nullptr; /* 'n' not in [1, f->nupvalues] */
    assert(novariant(f->tt) >= LUA_TSTRING);
    *addr = {&f->upvalue[n - 1], nullptr, f};
    return &lt::empty_str_i;
  }
  case LUA_VLCL:
  { /* Lua closure */
    LClosure* f = clLvalue(fi);
    TString* name;
    Proto* p = f->p;
    if (!(n >= 1 && (size_t)n <= p->upvalues.size()))
      return nullptr; /* 'n' not in [1, p->sizeupvalues] */
    assert(novariant(f->upvals[n - 1]->tt) >= LUA_TSTRING);
    *addr = {nullptr, f->upvals[n - 1], f->upvals[n - 1]};
    name = p->upvalues[n - 1].name;
    return (name == nullptr) ? &lt::no_name_i : &name->data;
  }
  default:
    return nullptr; /* not a closure */
  }
}

}

const InternalString* lua_getupvaluei(lua_State* L, int funcindex, int n)
{
  const InternalString* name;
  UpvalAddr addr = {{}, nullptr, nullptr};
  lua_lock(L);
  TValue fi = index2val(L, funcindex);
  name = aux_upvalue(&fi, n, &addr);
  if (name)
  {
    TValue v = (addr.uv != nullptr) ? upvalgetval(addr.uv) : getobj(addr.row);
    setslot(L, L->top, &v);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


std::optional<HostString> lua_getupvalue(lua_State* L, int funcindex, int n)
{
  const InternalString* name = lua_getupvaluei(L, funcindex, n);
  if (name == nullptr)
    return std::nullopt;
  return HostString(name->view());
}


const InternalString* lua_setupvaluei(lua_State* L, int funcindex, int n)
{
  const InternalString* name;
  UpvalAddr addr = {{}, nullptr, nullptr};
  lua_lock(L);
  TValue fi = index2val(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(&fi, n, &addr);
  if (name)
  {
    L->top--;
    TValue v = getslot(L, L->top);
    if (addr.uv != nullptr)
      upvalsetval(L, addr.uv, &v);
    else
    {
      setobj(L, addr.row, &v);
    }
    luaC_barrier(L, addr.owner, &v);
  }
  lua_unlock(L);
  return name;
}


std::optional<HostString> lua_setupvalue(lua_State* L, int funcindex, int n)
{
  const InternalString* name = lua_setupvaluei(L, funcindex, n);
  if (name == nullptr)
    return std::nullopt;
  return HostString(name->view());
}

namespace lua::api
{

static UpVal* getupval(lua_State* L, int fidx, int n)
{
  TValue fi = index2val(L, fidx);
  assert(ttisLclosure(&fi) && "Lua function expected");
  LClosure* f = clLvalue(&fi);
  if (1 <= n && n <= (int)f->p->upvalues.size())
    return f->upvals[n - 1]; /* get its upvalue */
  else
    return nullptr;
}

}

void* lua_upvalueid(lua_State* L, int fidx, int n)
{
  TValue fi = index2val(L, fidx);
  switch (ttypetag(&fi))
  {
  case LUA_VLCL:
  { /* lua closure */
    return getupval(L, fidx, n);
  }
  case LUA_VCCL:
  { /* C closure */
    CClosure* f = clCvalue(&fi);
    if (1 <= n && n <= (int)f->upvalue.size())
      return &f->upvalue[n - 1];
    /* else */
  } /* FALLTHROUGH */
  case LUA_VLCF:
    return nullptr; /* light C functions have no upvalues */
  default:
  {
    assert(0 && "function expected");
    return nullptr;
  }
  }
}


void lua_upvaluejoin(lua_State* L, int fidx1, int n1,
                             int fidx2, int n2)
{
  UpVal* uv1 = getupval(L, fidx1, n1);
  UpVal* uv2 = getupval(L, fidx2, n2);
  TValue fi1 = index2val(L, fidx1);
  LClosure* f1 = clLvalue(&fi1);
  assert(uv1 != nullptr && uv2 != nullptr && "invalid upvalue index");
  f1->upvals[n1 - 1] = uv2;
  luaC_objbarrier(L, f1, uv2);
}


int lua_upvalueindex(int i)
{
  return LUA_REGISTRYINDEX - i;
}


void lua_call(lua_State* L, int nargs, int nresults)
{
  lua_callk(L, nargs, nresults, 0, nullptr);
}


int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc)
{
  return lua_pcallk(L, nargs, nresults, errfunc, 0, nullptr);
}


int lua_yield(lua_State* L, int nresults)
{
  return lua_yieldk(L, nresults, 0, nullptr);
}


void* lua_getextra(lua_State* L)
{
  return L->extra;
}


void lua_setextra(lua_State* L, void* p)
{
  L->extra = p;
}


lua_Number lua_tonumber(lua_State* L, int idx)
{
  return lua_tonumberx(L, idx).value_or(0);
}


lua_Integer lua_tointeger(lua_State* L, int idx)
{
  return lua_tointegerx(L, idx).value_or(0);
}


void lua_pop(lua_State* L, int n)
{
  lua_settop(L, -n - 1);
}


void lua_newtable(lua_State* L)
{
  lua_createtable(L, 0, 0);
}


void lua_pushcfunction(lua_State* L, lua_CFunction f)
{
  lua_pushcclosure(L, f, 0);
}


CFuncInfo* lua_createcfuncinfo(lua_State* L, lua_CFunction fn, std::optional<HostString> name)
{
  CFuncInfo* cfi = CFuncInfo::luaF_newCFuncInfo(L, fn);
  if (name.has_value())
    cfi->name = name->to_utf8();
  return cfi;
}


void lua_pushcfuncinfo(lua_State* L, CFuncInfo* cfi)
{
  lua_lock(L);
  setslotf(L, L->top, cfi);
  api_incr_top(L);
  lua_unlock(L);
}


void lua_register(lua_State* L, const HostString& name, lua_CFunction f)
{
  lua_pushcfunction(L, f);
  lua_setglobal(L, name);
}


int lua_isfunction(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TFUNCTION;
}


int lua_istable(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TTABLE;
}


int lua_islightuserdata(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TLIGHTUSERDATA;
}


int lua_isnil(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TNIL;
}


int lua_isboolean(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TBOOLEAN;
}


int lua_isthread(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TTHREAD;
}


int lua_isnone(lua_State* L, int idx)
{
  return lua_type(L, idx) == LUA_TNONE;
}


int lua_isnoneornil(lua_State* L, int idx)
{
  return lua_type(L, idx) <= 0;
}


void lua_pushliteral(lua_State* L, const HostString& s)
{
  lua_pushstring(L, s);
}


void lua_pushglobaltable(lua_State* L)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
}


std::optional<HostString> lua_tostring(lua_State* L, int idx)
{
  auto s = lua_tolstring(L, idx);
  if (!s)
    return std::nullopt;
  return HostString(*s);
}


void lua_insert(lua_State* L, int idx)
{
  lua_rotate(L, idx, 1);
}


void lua_remove(lua_State* L, int idx)
{
  lua_rotate(L, idx, -1);
  lua_pop(L, 1);
}


void lua_replace(lua_State* L, int idx)
{
  lua_copy(L, -1, idx);
  lua_pop(L, 1);
}


void* lua_newuserdata(lua_State* L, size_t sz)
{
  return lua_newuserdatauv(L, sz, 1);
}


int lua_resetthread(lua_State* L)
{
  return lua_closethread(L, nullptr);
}

namespace lua::api
{

/* }====================================================================== */

}
