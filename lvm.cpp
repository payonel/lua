/*
** $Id: lvm.c $
** Lua virtual machine
** See Copyright Notice in lua.h
*/


#include <algorithm>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"
#include "os_api.h"
#include "literals.h"

namespace lua::vm
{

/* limit for table tag-method chains (to avoid infinite loops) */
constexpr int MAXTAGLOOP = 2000;

/*
** 'l_intfitsf' checks whether a given integer is in the range that
** can be converted to a float without rounding. Used in comparisons.
*/

/* number of bits in the mantissa of a float */
constexpr int NBM = DBL_MANT_DIG;


/* limit for integers that fit in a float */
constexpr lua_Unsigned MAXINTFITSF = ((lua_Unsigned) 1 << NBM);

/* check whether 'i' is in the interval [-MAXINTFITSF, MAXINTFITSF] */
static inline int l_intfitsf(lua_Integer i)
{
  return -(lua_Integer)MAXINTFITSF <= i && i <= (lua_Integer)MAXINTFITSF;
}

/*
** Try to convert a value from string to a number value.
** If the value is not a string or is a string not representing
** a valid numeral (or if coercions from strings to numbers
** are disabled via macro 'cvt2num'), do not modify 'result'
** and return 0.
*/
static int l_strton(const TValue* obj, TValue* result)
{
  assert(obj != result);
  if (!cvt2num(obj)) /* is object not a string? */
    return 0;
  else
  {
    assert(ttisstring(obj));
    TString* st = tsvalue(obj);
    return (luaO_str2num(st->data.view(), result) == st->data.size() + 1);
  }
}

/*
** Try to convert a value to a float. The float case is already handled
** by the macro 'tonumber'.
*/
int luaV_tonumber_(const TValue* obj, NumResult* n)
{
  TValue v;
  if (ttisinteger(obj))
  {
    n->v = ((lua_Number)(ivalue(obj)));
    return 1;
  }
  else if (l_strton(obj, &v))
  {                  /* string coercible to number? */
    /* convert result of 'luaO_str2num' to a float */
    assert(ttisnumber(&v));
    n->v = ttisinteger(&v) ? (lua_Number)ivalue(&v) : fltvalue(&v);
    return 1;
  }
  else
    return 0; /* conversion failed */
}

/*
** try to convert a float to an integer, rounding according to 'mode'.
*/
int luaV_flttointeger(lua_Number n, IntResult* p, F2Imod mode)
{
  lua_Number f = floor(n);
  if (n != f)
  { /* not an integral value? */
    if (mode == F2Ieq)
      return 0;               /* fails if mode demands integral value */
    else if (mode == F2Iceil) /* needs ceiling? */
      f += 1;                 /* convert floor to ceiling (remember: n != f) */
  }
  return lua_numbertointeger(f, p);
}

/*
** try to convert a value to an integer, rounding according to 'mode',
** without string coercion.
** ("Fast track" handled by macro 'tointegerns'.)
*/
int luaV_tointegerns(const TValue* obj, IntResult* p, F2Imod mode)
{
  if (ttisfloat(obj))
    return luaV_flttointeger(fltvalue(obj), p, mode);
  else if (ttisinteger(obj))
  {
    p->v = ivalue(obj);
    return 1;
  }
  else
    return 0;
}

/*
** try to convert a value to an integer.
*/
int luaV_tointeger(const TValue* obj, IntResult* p, F2Imod mode)
{
  TValue v;
  if (l_strton(obj, &v)) /* does 'obj' point to a numerical string? */
    obj = &v;            /* change it to point to its corresponding number */
  return luaV_tointegerns(obj, p, mode);
}

/*
** Try to convert a 'for' limit to an integer, preserving the semantics
** of the loop. Return true if the loop must not run; otherwise, '*p'
** gets the integer limit.
** (The following explanation assumes a positive step; it is valid for
** negative steps mutatis mutandis.)
** If the limit is an integer or can be converted to an integer,
** rounding down, that is the limit.
** Otherwise, check whether the limit can be converted to a float. If
** the float is too large, clip it to LUA_MAXINTEGER.  If the float
** is too negative, the loop should not run, because any initial
** integer value is greater than such limit; so, the function returns
** true to signal that. (For this latter case, no integer limit would be
** correct; even a limit of LUA_MININTEGER would run the loop once for
** an initial value equal to LUA_MININTEGER.)
*/
static int forlimit(lua_State* L, lua_Integer init, StkId limslot,
                    IntResult* p, lua_Integer step)
{
  TValue limv = getslot(L, limslot);
  const TValue* lim = &limv;
  if (!luaV_tointeger(lim, p, (step < 0 ? F2Iceil : F2Ifloor)))
  {
    /* not coercible to in integer */
    NumResult flim;           /* try to convert to float */
    if (!tonumber(lim, &flim)) /* cannot convert to float? */
      luaG_forerror(L, lim, lt::limit);
    /* else 'flim' is a float out of integer bounds */
    if (((0) < (flim.v)))
    { /* if it is positive, it is too large */
      if (step < 0)
        return 1;          /* initial value must be less than it */
      p->v = LUA_MAXINTEGER; /* truncate */
    }
    else
    { /* it is less than min integer */
      if (step > 0)
        return 1;          /* initial value must be greater than it */
      p->v = LUA_MININTEGER; /* truncate */
    }
  }
  return (step > 0 ? init > p->v : init < p->v); /* not to run? */
}

/*
** Prepare a numerical for loop (opcode OP_FORPREP).
** Before execution, stack is as follows:
**   ra     : initial value
**   ra + 1 : limit
**   ra + 2 : step
** Return true to skip the loop. Otherwise,
** after preparation, stack will be as follows:
**   ra     : loop counter (integer loops) or limit (float loops)
**   ra + 1 : step
**   ra + 2 : control variable
*/
static int forprep(lua_State* L, StkId ra)
{
  StkId pinit = ra;
  StkId plimit = ra + 1;
  StkId pstep = ra + 2;
  if ((slottag(L, pinit) == LUA_VNUMINT) && (slottag(L, pstep) == LUA_VNUMINT))
  { /* integer loop? */
    lua_Integer init = slotival(L, pinit);
    lua_Integer step = slotival(L, pstep);
    IntResult limit;
    if (step == 0)
      luaG_runerror(L, lt::for_step_is_zero);
    if (forlimit(L, init, plimit, &limit, step))
      return 1; /* skip the loop */
    else
    { /* prepare loop counter */
      lua_Unsigned count;
      if (step > 0)
      { /* ascending loop? */
        count = ((lua_Unsigned)(limit.v)) - ((lua_Unsigned)(init));
        if (step != 1) /* avoid division in the too common case */
          count /= ((lua_Unsigned)(step));
      }
      else
      { /* step < 0; descending loop */
        count = ((lua_Unsigned)(init)) - ((lua_Unsigned)(limit.v));
        /* 'step+1' avoids negating 'mininteger' */
        count /= ((lua_Unsigned)(-(step + 1))) + 1u;
      }
      /* use 'chgivalue' for places that for sure had integers */
      chgsloti(L, ra, (count)); /* change init to count */
      setsloti(L, ra + 1, step);         /* change limit to step */
      chgsloti(L, ra + 2, init);         /* change step to init */
    }
  }
  else
  { /* try making all values floats */
    NumResult init;
    NumResult limit;
    NumResult step;
    if (l_unlikely(!tonumber(L, plimit, &limit)))
      luaG_forerror(L, plimit, lt::limit);
    if (l_unlikely(!tonumber(L, pstep, &step)))
      luaG_forerror(L, pstep, lt::step);
    if (l_unlikely(!tonumber(L, pinit, &init)))
      luaG_forerror(L, pinit, lt::initial_value);
    if (step.v == 0)
      luaG_runerror(L, lt::for_step_is_zero);
    if (((0) < (step.v)) ? ((limit.v) < (init.v))
                            : ((init.v) < (limit.v)))
      return 1; /* skip the loop */
    else
    {
      /* make sure all values are floats */
      setslotflt(L, ra, limit.v);
      setslotflt(L, ra + 1, step.v);
      setslotflt(L, ra + 2, init.v); /* control variable */
    }
  }
  return 0;
}

/*
** Execute a step of a float numerical for loop, returning
** true iff the loop must continue. (The integer case is
** written inline with opcode OP_FORLOOP, for performance.)
*/
static int floatforloop(lua_State* L, StkId ra)
{
  lua_Number step = slotfltval(L, ra + 1);
  assert(slottag(L, ra) == LUA_VNUMFLT);
  lua_Number limit = slotfltval(L, ra);
  assert(slottag(L, ra + 2) == LUA_VNUMFLT);
  lua_Number idx = slotfltval(L, ra + 2); /* control variable */
  idx = ((void)(L), (idx) + (step));        /* increment index */
  if (((0) < (step)) ? ((idx) <= (limit))
                          : ((limit) <= (idx)))
  {
    chgslotflt(L, ra + 2, idx); /* update control variable */
    return 1;                      /* jump back */
  }
  else
    return 0; /* finish the loop */
}

/*
** Finish the table access 'val = t[key]' and return the tag of the result.
*/
lu_byte luaV_finishget(lua_State* L, const TValue* t, TValue* key,
                       StkId val, lu_byte tag, StkId tsrc)
{
  int loop;         /* counter to avoid infinite loops */
  const TValue* tm; /* metamethod */
  TValue res;
  for (loop = 0; loop < MAXTAGLOOP; loop++)
  {
    if (tag == LUA_VNOTABLE)
    { /* 't' is not a table? */
      assert(!ttistable(t));
      tm = luaT_gettmbyobj(L, t, TM_INDEX);
      if (l_unlikely(notm(tm)))
        luaG_typeerror(L, t, lt::index_kw, tsrc); /* no metamethod */
      /* else will try the metamethod */
    }
    else
    {                                                 /* 't' is a table */
      Table* h = hvalue(t);
      tm = gfasttm(L->l_G, h->metatable, TM_INDEX); /* table's metamethod */
      if (tm == nullptr)
      {                     /* no metamethod? */
        setslottag(L, val, LUA_VNIL); /* result is nil */
        return LUA_VNIL;
      }
      /* else will try the metamethod */
    }
    if (ttisfunction(tm))
    {                                           /* is metamethod a function? */
      tag = luaT_callTMres(L, tm, t, key, val); /* call it */
      return tag;                               /* return tag of the result */
    }
    t = tm; /* else try to access 'tm[key]' */
    tsrc = NOSLOT;
    if (!ttistable(t))
      tag = LUA_VNOTABLE;
    else
    {
      tag = hvalue(t)->luaH_get(key, &res);
    }
    if (!tagisempty(tag))
    {
      setslot(L, val, &res);
      return tag; /* done */
    }
    /* else repeat (tail call 'luaV_finishget') */
  }
  luaG_runerror(L, lt::index_chain_too_long_possible_loop);
  return 0; /* to avoid warnings */
}

/*
** Finish a table assignment 't[key] = val'.
** About anchoring the table before the call to 'luaH_finishset':
** This call may trigger an emergency collection. When loop>0,
** the table being accessed is a field in some metatable. If this
** metatable is weak and the table is not anchored, this collection
** could collect that table while it is being updated.
*/
void luaV_finishset(lua_State* L, const TValue* t, TValue* key,
                    TValue* val, int hres, StkId tsrc)
{
  int loop; /* counter to avoid infinite loops */
  for (loop = 0; loop < MAXTAGLOOP; loop++)
  {
    const TValue* tm; /* '__newindex' metamethod */
    if (hres != HNOTATABLE)
    {                                            /* is 't' a table? */
      Table* h = hvalue(t);                      /* save 't' table */
      tm = gfasttm(L->l_G, h->metatable, TM_NEWINDEX); /* get metamethod */
      if (tm == nullptr)
      {                                       /* no metamethod? */
        setsloth(L, L->top, h);          /* anchor 't' */
        L->top++;                           /* assume EXTRA_STACK */
        h->luaH_finishset(L, key, val, hres); /* set new value */
        L->top--;
        invalidateTMcache(h);
        assert(novariant(h->tt) >= LUA_TSTRING);
        luaC_barrierback(L, h, val);
        return;
      }
      /* else will try the metamethod */
    }
    else
    { /* not a table; check metamethod */
      tm = luaT_gettmbyobj(L, t, TM_NEWINDEX);
      if (l_unlikely(notm(tm)))
        luaG_typeerror(L, t, lt::index_kw, tsrc);
    }
    /* try the metamethod */
    if (ttisfunction(tm))
    {
      luaT_callTM(L, tm, t, key, val);
      return;
    }
    t = tm; /* else repeat assignment over 'tm' */
    tsrc = NOSLOT;
    if (!ttistable(t))
      hres = HNOTATABLE;
    else
    {
      hres = hvalue(t)->luaH_pset(key, val);
    }
    if (hres == HOK)
    {
      luaV_finishfastset(L, t, val);
      return; /* done */
    }
    /* else 'return luaV_finishset(L, t, key, val, slot)' (loop) */
  }
  luaG_runerror(L, lt::newindex_chain_too_long_possible_loop);
}

static int l_strcmp(const TString* ts1, const TString* ts2)
{
  byte_view s1(ts1->data.view());
  byte_view s2(ts2->data.view());
  for (;;)
  {
    size_t zl1 = s1.find('\0');
    size_t zl2 = s2.find('\0');
    if (zl1 == byte_view::npos) zl1 = s1.size();
    if (zl2 == byte_view::npos) zl2 = s2.size();
    int temp = os_strcoll(s1.substr(0, zl1), s2.substr(0, zl2));
    if (temp != 0)
      return temp;
    if (zl2 == s2.size())
      return (zl1 == s1.size()) ? 0 : 1;
    if (zl1 == s1.size())
      return -1;
    s1 = s1.substr(zl1 + 1);
    s2 = s2.substr(zl2 + 1);
  }
}

/*
** Check whether integer 'i' is less than float 'f'. If 'i' has an
** exact representation as a float ('l_intfitsf'), compare numbers as
** floats. Otherwise, use the equivalence 'i < f <=> i < ceil(f)'.
** If 'ceil(f)' is out of integer range, either 'f' is greater than
** all integers or less than all integers.
** (The test with 'l_intfitsf' is only for performance; the else
** case is correct for all values, but it is slow due to the conversion
** from float to int.)
** When 'f' is NaN, comparisons must result in false.
*/
static inline int LTintfloat(lua_Integer i, lua_Number f)
{
  if (l_intfitsf(i))
    return ((((lua_Number)(i))) < (f)); /* compare them as floats */
  else
  { /* i < f <=> i < ceil(f) */
    IntResult fi;
    if (luaV_flttointeger(f, &fi, F2Iceil)) /* fi = ceil(f) */
      return i < fi.v;                        /* compare them as integers */
    else                                    /* 'f' is either greater or less than all integers */
      return f > 0;                         /* greater? */
  }
}

/*
** Check whether integer 'i' is less than or equal to float 'f'.
** See comments on previous function.
*/
static inline int LEintfloat(lua_Integer i, lua_Number f)
{
  if (l_intfitsf(i))
    return ((((lua_Number)(i))) <= (f)); /* compare them as floats */
  else
  { /* i <= f <=> i <= floor(f) */
    IntResult fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor)) /* fi = floor(f) */
      return i <= fi.v;                        /* compare them as integers */
    else                                     /* 'f' is either greater or less than all integers */
      return f > 0;                          /* greater? */
  }
}

/*
** Check whether float 'f' is less than integer 'i'.
** See comments on previous function.
*/
static inline int LTfloatint(lua_Number f, lua_Integer i)
{
  if (l_intfitsf(i))
    return ((f) < (((lua_Number)(i)))); /* compare them as floats */
  else
  { /* f < i <=> floor(f) < i */
    IntResult fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor)) /* fi = floor(f) */
      return fi.v < i;                         /* compare them as integers */
    else                                     /* 'f' is either greater or less than all integers */
      return f < 0;                          /* less? */
  }
}

/*
** Check whether float 'f' is less than or equal to integer 'i'.
** See comments on previous function.
*/
static inline int LEfloatint(lua_Number f, lua_Integer i)
{
  if (l_intfitsf(i))
    return ((f) <= (((lua_Number)(i)))); /* compare them as floats */
  else
  { /* f <= i <=> ceil(f) <= i */
    IntResult fi;
    if (luaV_flttointeger(f, &fi, F2Iceil)) /* fi = ceil(f) */
      return fi.v <= i;                       /* compare them as integers */
    else                                    /* 'f' is either greater or less than all integers */
      return f < 0;                         /* less? */
  }
}

/*
** Return 'l < r', for numbers.
*/
static inline int LTnum(const TValue* l, const TValue* r)
{
  assert(ttisnumber(l) && ttisnumber(r));
  if (ttisinteger(l))
  {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li < ivalue(r); /* both are integers */
    else
    { /* 'l' is int and 'r' is float */
      assert(ttisfloat(r));
      return LTintfloat(li, fltvalue(r)); /* l < r ? */
    }
  }
  else
  {
    assert(ttisfloat(l));
    lua_Number lf = fltvalue(l); /* 'l' must be float */
    if (ttisfloat(r))
      return lf < fltvalue(r); /* both are float */
    else
    { /* 'l' is float and 'r' is int */
      assert(ttisinteger(r));
      return LTfloatint(lf, ivalue(r));
    }
  }
}

/*
** Return 'l <= r', for numbers.
*/
static inline int LEnum(const TValue* l, const TValue* r)
{
  assert(ttisnumber(l) && ttisnumber(r));
  if (ttisinteger(l))
  {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li <= ivalue(r); /* both are integers */
    else
    { /* 'l' is int and 'r' is float */
      assert(ttisfloat(r));
      return LEintfloat(li, fltvalue(r)); /* l <= r ? */
    }
  }
  else
  {
    assert(ttisfloat(l));
    lua_Number lf = fltvalue(l); /* 'l' must be float */
    if (ttisfloat(r))
      return lf <= fltvalue(r); /* both are float */
    else
    { /* 'l' is float and 'r' is int */
      assert(ttisinteger(r));
      return LEfloatint(lf, ivalue(r));
    }
  }
}

/*
** return 'l < r' for non-numbers.
*/
static int lessthanothers(lua_State* L, const TValue* l, const TValue* r)
{
  assert(!ttisnumber(l) || !ttisnumber(r));
  if (ttisstring(l) && ttisstring(r)) /* both are strings? */
  {
    return l_strcmp(tsvalue(l), tsvalue(r)) < 0;
  }
  else
    return luaT_callorderTM(L, l, r, TM_LT);
}

/*
** Main operation less than; return 'l < r'.
*/
int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r)
{
  if (ttisnumber(l) && ttisnumber(r)) /* both operands are numbers? */
    return LTnum(l, r);
  else
    return lessthanothers(L, l, r);
}

/*
** return 'l <= r' for non-numbers.
*/
static int lessequalothers(lua_State* L, const TValue* l, const TValue* r)
{
  assert(!ttisnumber(l) || !ttisnumber(r));
  if (ttisstring(l) && ttisstring(r)) /* both are strings? */
  {
    return l_strcmp(tsvalue(l), tsvalue(r)) <= 0;
  }
  else
    return luaT_callorderTM(L, l, r, TM_LE);
}

static inline int LTnum(lua_State* L, StkId l, StkId r)
{
  TValue lv = getslot(L, l);
  TValue rv = getslot(L, r);
  return LTnum(&lv, &rv);
}

static inline int LEnum(lua_State* L, StkId l, StkId r)
{
  TValue lv = getslot(L, l);
  TValue rv = getslot(L, r);
  return LEnum(&lv, &rv);
}

static int lessthanothers(lua_State* L, StkId l, StkId r)
{
  TValue lv = getslot(L, l);
  TValue rv = getslot(L, r);
  return lessthanothers(L, &lv, &rv);
}

static int lessequalothers(lua_State* L, StkId l, StkId r)
{
  TValue lv = getslot(L, l);
  TValue rv = getslot(L, r);
  return lessequalothers(L, &lv, &rv);
}

/*
** Main operation less than or equal to; return 'l <= r'.
*/
int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r)
{
  if (ttisnumber(l) && ttisnumber(r)) /* both operands are numbers? */
    return LEnum(l, r);
  else
    return lessequalothers(L, l, r);
}

int luaV_equalobj(lua_State* L, const TValue* t1, const TValue* t2)
{
  const TValue* tm;
  if (ttype(t1) != ttype(t2)) /* not the same type? */
    return 0;
  else if (ttypetag(t1) != ttypetag(t2))
  {
    switch (ttypetag(t1))
    {
    case LUA_VNUMINT:
    { /* integer == float? */
      /* integer and float can only be equal if float has an integer
         value equal to the integer */
      IntResult i2;
      assert(ttisfloat(t2));
      return (luaV_flttointeger(fltvalue(t2), &i2, F2Ieq) && ivalue(t1) == i2.v);
    }
    case LUA_VNUMFLT:
    {                 /* float == integer? */
      IntResult i1; /* see comment in previous case */
      assert(ttisfloat(t1));
      assert(ttisinteger(t2));
      return (luaV_flttointeger(fltvalue(t1), &i1, F2Ieq) && i1.v == ivalue(t2));
    }
    case LUA_VSHRSTR:
    {
      return luaS_eqstr(tsvalue(t1), tsvalue(t2));
    }
    default:
      /* only numbers (integer/float) and strings (long/short) can have
         equal values with different variants */
      return 0;
    }
  }
  else
  { /* equal variants */
    switch (ttypetag(t1))
    {
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
      return 1;
    case LUA_VNUMINT:
      assert(ttisinteger(t2));
      return (ivalue(t1) == ivalue(t2));
    case LUA_VNUMFLT:
      assert(ttisfloat(t2));
      return (fltvalue(t1) == fltvalue(t2));
    case LUA_VLIGHTUSERDATA:
      assert(ttislightuserdata(t2));
      return pvalue(t1) == pvalue(t2);
    case LUA_VSHRSTR:
    {
      return luaS_eqstr(tsvalue(t1), tsvalue(t2));
    }
    case LUA_VUSERDATA:
    {
      Udata* u1 = uvalue(t1);
      Udata* u2 = uvalue(t2);
      if (u1 == u2)
        return 1;
      else if (L == nullptr)
        return 0;
      tm = gfasttm(L->l_G, u1->metatable, TM_EQ);
      if (tm == nullptr)
        tm = gfasttm(L->l_G, u2->metatable, TM_EQ);
      break; /* will try TM */
    }
    case LUA_VTABLE:
    {
      Table* h1 = hvalue(t1);
      Table* h2 = hvalue(t2);
      if (h1 == h2)
        return 1;
      else if (L == nullptr)
        return 0;
      tm = gfasttm(L->l_G, h1->metatable, TM_EQ);
      if (tm == nullptr)
        tm = gfasttm(L->l_G, h2->metatable, TM_EQ);
      break; /* will try TM */
    }
    default: /* functions, threads, and light C functions */
      assert(iscollectable(t1));
      assert(iscollectable(t2));
      return (gcvalue(t1) == gcvalue(t2));
    }
    if (tm == nullptr) /* no TM? */
      return 0;     /* objects are different */
    else
    {
      int tag = luaT_callTMres(L, tm, t1, t2, L->top); /* call TM */
      return !((tag) == LUA_VFALSE || tagisempty(tag));
    }
  }
}

int luaV_equalobj(lua_State* L, StkId t1, StkId t2)
{
  TValue v1 = getslot(L, t1);
  TValue v2 = getslot(L, t2);
  return luaV_equalobj(L, &v1, &v2);
}

/* macro used by 'luaV_concat' to ensure that element at 'o' is a string */

/*
** Check whether object is a short empty string to optimize concatenation.
** (External strings can be empty too; they will be concatenated like
** non-empty ones.)
*/
static int isemptystr(const TValue* o)
{
  return ttisstring(o) &&
         tsslen(tsvalue(o)) == 0;
}

static int isemptystr(lua_State* L, StkId idx)
{
  TValue v = getslot(L, idx);
  return isemptystr(&v);
}

static int tostring(lua_State* L, StkId idx)
{
  if (novariant(slottag(L, idx)) == LUA_TSTRING)
    return 1;
  TValue v = getslot(L, idx);
  if (!cvt2str(&v))
    return 0;
  luaO_tostring(L, &v);
  setslot(L, idx, &v);
  return 1;
}

/* copy strings in stack from top - n up to top - 1 to buffer */
static void copy2buff(lua_State* L, StkId top, int n, byte_span buff)
{
  size_t tl = 0; /* size already copied */
  do
  {
    StkId v = top - n;
    assert(novariant(slottag(L, v)) == LUA_TSTRING);
    TString* st = slottsval(L, v);
    byte_view s = st->data.view();
    buff.copy_from(s, tl);
    tl += s.size();
  } while (--n > 0);
}

void luaV_concat(lua_State* L, int total)
{
  if (total == 1)
    return; /* "all" values already concatenated */
  do
  {
    StkId top = L->top;
    int n = 2; /* number of elements handled in this pass (at least 2) */
    StkId v1 = top - 2;
    StkId v2 = top - 1;
    if (!((novariant(slottag(L, v1)) == LUA_TSTRING) || cvt2str(L, v1)) || !tostring(L, v2))
      luaT_tryconcatTM(L);                  /* may invalidate 'top' */
    else if (isemptystr(L, v2))                /* second operand is empty? */
      (void)tostring(L, v1);                /* result is first operand */
    else if (isemptystr(L, v1))
    {                                 /* first operand is empty string? */
      copyslot(L, v1, v2); /* result is second op. */
    }
    else
    {
      /* at least two string values; get as many as possible */
      size_t tl = tsslen(slottsval(L, v2)); /* total length */
      TString* ts;
      /* collect total length and number of strings */
      for (n = 1; n < total && tostring(L, top - n - 1); n++)
      {
        StkId o = top - n - 1;
        assert(novariant(slottag(L, o)) == LUA_TSTRING);
        size_t l = tsslen(slottsval(L, o));
        if (l_unlikely(l >= MAX_SIZE - cost::TString - tl))
        {
          L->top = top - total; /* pop strings to avoid wasting stack */
          luaG_runerror(L, lt::string_length_overflow);
        }
        tl += l;
      }
      if (tl <= 40)
      {
        byte_owner buff(tl);
        copy2buff(L, top, n, buff);
        ts = TString::luaS_createstr(L, buff.view());
      }
      else
      {
        byte_owner buff = luaM_alloc_char_span(L->l_G, L, {}, tl, true);
        copy2buff(L, top, n, buff);
        ts = TString::luaS_createstr(L, buff.view());
        luaM_alloc_char_span(L->l_G, L, buff, 0, false);
      }
      setslots(L, top - n, ts); /* create result */
    }
    total -= n - 1;    /* got 'n' strings to create one new */
    L->top -= n - 1; /* popped 'n' strings and pushed one */
  } while (total > 1); /* repeat until only 1 result left */
}

/*
** Main operation 'ra = #rb'.
*/
void luaV_objlen(lua_State* L, StkId ra, const TValue* rb, StkId src)
{
  const TValue* tm;
  switch (ttypetag(rb))
  {
  case LUA_VTABLE:
  {
    Table* h = hvalue(rb);
    tm = gfasttm(L->l_G, h->metatable, TM_LEN);
    if (tm)
      break;                                        /* metamethod? break switch to call it */
    setsloti(L, ra, h->luaH_getn(L)); /* else primitive len */
    return;
  }
  case LUA_VSHRSTR:
  {
    TString* ts = tsvalue(rb);
    setsloti(L, ra, tsslen(ts));
    return;
  }
  default:
  { /* try metamethod */
    tm = luaT_gettmbyobj(L, rb, TM_LEN);
    if (l_unlikely(notm(tm))) /* no metamethod? */
      luaG_typeerror(L, rb, lt::get_length_of, src);
    break;
  }
  }
  luaT_callTMres(L, tm, rb, rb, ra);
}

void luaV_objlen(lua_State* L, StkId ra, StkId rb)
{
  TValue v = getslot(L, rb);
  luaV_objlen(L, ra, &v, rb);
}

/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
lua_Integer luaV_idiv(lua_State* L, lua_Integer m, lua_Integer n)
{
  if (l_unlikely(n == 0 || n == -1))
  { /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, lt::attempt_to_divide_by_zero);
    return l_intunm(m); /* n==-1; avoid overflow with 0x80000...//-1 */
  }
  else
  {
    lua_Integer q = m / n;         /* perform C division */
    if ((m ^ n) < 0 && m % n != 0) /* 'm/n' would be negative non-integer? */
      q -= 1;                      /* correct result for different rounding */
    return q;
  }
}

/*
** Integer modulus; return 'm % n'. (Assume that C '%' with
** negative operands follows C99 behavior. See previous comment
** about luaV_idiv.)
*/
lua_Integer luaV_mod(lua_State* L, lua_Integer m, lua_Integer n)
{
  if (l_unlikely(n == 0 || n == -1))
  { /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, lt::attempt_to_perform_n_0);
    return 0; /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
  }
  else
  {
    lua_Integer r = m % n;
    if (r != 0 && (r ^ n) < 0) /* 'm/n' would be non-integer negative? */
      r += n;                  /* correct result for different rounding */
    return r;
  }
}

/*
** Float modulus
*/
lua_Number luaV_modf(lua_State* L, lua_Number m, lua_Number n)
{
  lua_Number r;
  r = luai_nummod(L, m, n);
  return r;
}

/* number of bits in an integer */
constexpr int NBITS = 64;
static_assert(NBITS == sizeof(lua_Integer) * CHAR_BIT);

/*
** Shift left operation. (Shift right just negates 'y'.)
*/
lua_Integer luaV_shiftl(lua_Integer x, lua_Integer y)
{
  if (y < 0)
  { /* shift right? */
    if (y <= -NBITS)
      return 0;
    else
      return (((lua_Unsigned)(x)) >> (-y));
  }
  else
  { /* shift left */
    if (y >= NBITS)
      return 0;
    else
      return (((lua_Unsigned)(x)) << ((lua_Unsigned)(y)));
  }
}

/*
** create a new Lua closure, push it in the stack, and initialize
** its upvalues.
*/
static void pushclosure(lua_State* L, Proto* p, std::span<UpVal*> encup, StkId base,
                        StkId ra)
{
  int nup = (int)p->upvalues.size();
  Upvaldesc* uv = p->upvalues.data();
  int i;
  LClosure* ncl = LClosure::luaF_newLclosure(L, nup);
  ncl->p = p;
  setslotclL(L, ra, ncl); /* anchor new closure in stack */
  for (i = 0; i < nup; i++)
  {                    /* fill in its upvalues */
    if (uv[i].instack) /* upvalue refers to local variable? */
      ncl->upvals[i] = UpVal::luaF_findupval(L, base + uv[i].idx);
    else /* get upvalue from enclosing function */
      ncl->upvals[i] = encup[uv[i].idx];
    luaC_objbarrier(L, ncl, ncl->upvals[i]);
  }
}

/*
** finish execution of an opcode interrupted by a yield
*/
void luaV_finishOp(lua_State* L)
{
  CallInfo* ci = L->ci;
  StkId base = ci->func + 1;
  const Instruction* code = ci_func(L, ci)->p->code.data();
  Instruction inst = code[ci->savedpc - 1]; /* interrupted instruction */
  OpCode op = GET_OPCODE(inst);
  switch (op)
  { /* finish its execution */
  case OP_MMBIN:
  case OP_MMBINI:
  case OP_MMBINK:
  {
    copyslot(L, base + GETARG_A(code[ci->savedpc - 2]), --L->top);
    break;
  }
  case OP_UNM:
  case OP_BNOT:
  case OP_LEN:
  case OP_GETTABUP:
  case OP_GETTABLE:
  case OP_GETI:
  case OP_GETFIELD:
  case OP_SELF:
  {
    copyslot(L, base + GETARG_A(inst), --L->top);
    break;
  }
  case OP_LT:
  case OP_LE:
  case OP_LTI:
  case OP_LEI:
  case OP_GTI:
  case OP_GEI:
  case OP_EQ:
  { /* note that 'OP_EQI'/'OP_EQK' cannot yield */
    int res = !((slottag(L, L->top - 1) == LUA_VFALSE) || tagisempty(slottag(L, L->top - 1)));
    L->top--;
    assert(GET_OPCODE(code[ci->savedpc]) == OP_JMP);
    if (res != GETARG_k(inst)) /* condition failed? */
      ci->savedpc++;       /* skip jump instruction */
    break;
  }
  case OP_CONCAT:
  {
    StkId top = L->top - 1;                   /* top when 'luaT_tryconcatTM' was called */
    int a = GETARG_A(inst);                     /* first element to concatenate */
    int total = ((int)(top - 1 - (base + a))); /* yet to concatenate */
    copyslot(L, top - 2, top);                 /* put TM result in proper position */
    L->top = top - 1;                         /* top is one after last element (at top-2) */
    luaV_concat(L, total);                      /* concat them (may yield again) */
    break;
  }
  case OP_CLOSE:
  {                    /* yielded closing variables */
    ci->savedpc--; /* repeat instruction to close other vars. */
    break;
  }
  case OP_RETURN:
  { /* yielded closing variables */
    StkId ra = base + GETARG_A(inst);
    /* adjust top to signal correct number of returns, in case the
       return is "up to top" ('isIT') */
    L->top = ra + ci->nres;
    /* repeat instruction to close other vars. and complete the return */
    ci->savedpc--;
    break;
  }
  default:
  {
    /* only these other opcodes can yield */
    assert(op == OP_TFORCALL || op == OP_CALL ||
               op == OP_TAILCALL || op == OP_SETTABUP || op == OP_SETTABLE ||
               op == OP_SETI || op == OP_SETFIELD);
    break;
  }
  }
}

/*
** {==================================================================
** Macros for arithmetic/bitwise/comparison opcodes in 'luaV_execute'
**
** All these macros are to be used exclusively inside the main
** iterpreter loop (function luaV_execute) and may access directly
** the local variables of that function (L, i, pc, ci, etc.).
** ===================================================================
*/

static l_forceinline lua_Integer l_addi(lua_State* L, lua_Integer a, lua_Integer b)
{
  (void) L;
  return l_intadd(a, b);
}
static l_forceinline lua_Integer l_subi(lua_State* L, lua_Integer a, lua_Integer b)
{
  (void) L;
  return l_intsub(a, b);
}
static l_forceinline lua_Integer l_muli(lua_State* L, lua_Integer a, lua_Integer b)
{
  (void) L;
  return l_intmul(a, b);
}
static l_forceinline lua_Integer l_band(lua_Integer a, lua_Integer b)
{
  return l_intband(a, b);
}
static l_forceinline lua_Integer l_bor(lua_Integer a, lua_Integer b)
{
  return l_intbor(a, b);
}
static l_forceinline lua_Integer l_bxor(lua_Integer a, lua_Integer b)
{
  return l_intbxor(a, b);
}

static l_forceinline int l_lti(lua_Integer a, lua_Integer b)
{
  return a < b;
}
static l_forceinline int l_lei(lua_Integer a, lua_Integer b)
{
  return a <= b;
}
static l_forceinline int l_gti(lua_Integer a, lua_Integer b)
{
  return a > b;
}
static l_forceinline int l_gei(lua_Integer a, lua_Integer b)
{
  return a >= b;
}

/*
** Arithmetic operations with immediate operands. 'iop' is the integer
** operation, 'fop' is the float operation.
*/

/*
** Auxiliary function for arithmetic operations over floats and others
** with two operands.
*/

/*
** Arithmetic operations over floats and others with register operands.
*/

/*
** Arithmetic operations with K operands for floats.
*/

/*
** Arithmetic operations over integers and floats.
*/

/*
** Arithmetic operations with register operands.
*/

/*
** Arithmetic operations with K operands.
*/

/*
** Bitwise operations with constant operand.
*/

/*
** Bitwise operations with register operands.
*/

/*
** Order operations with register operands. 'opn' actually works
** for all numbers, but the fast track improves performance for
** integers.
*/

/*
** Order operations with immediate operand. (Immediate operand is
** always small enough to have an exact representation as a float.)
*/

/* }================================================================== */

/*
** {==================================================================
** Function 'luaV_execute': main interpreter loop
** ===================================================================
*/

/*
** some macros for common tasks in 'luaV_execute'
*/

/*
** Execute a jump instruction. The 'updatetrap' allows signals to stop
** tight loops. (Without it, the local copy of 'trap' could never change.)
*/

/* for test instructions, execute the jump instruction that follows it */

/*
** do a conditional jump: skip next instruction if 'cond' is not what
** was expected (parameter 'k'), else do next instruction, which must
** be a jump.
*/

/*
** Correct global 'pc'.
*/

/*
** Whenever code can raise errors, the global 'pc' and the global
** 'top' must be correct to report occasional errors.
*/

/*
** Protect code that, in general, can raise errors, reallocate the
** stack, and change the hooks.
*/

/* special version that does not change the top */

/*
** Protect code that can only raise errors. (That is, it cannot change
** the stack or hooks.)
*/

static inline void luai_threadyield(lua_State* L)
{
  (void) L;
  lua_unlock(L);
  lua_lock(L);
}

/* 'c' is the limit of live values in the stack */

void luaV_execute(lua_State* L, CallInfo* ci)
{
  LClosure* cl;
  TValue* k;
  StkId base;
  const Instruction* code;
  int pc;
  int trap;
  trap = L->hookmask;
  cl = ci_func(L, ci);
  k = cl->p->k.data();
  code = cl->p->code.data();
  pc = ci->savedpc;
  if (l_unlikely(trap))
    trap = luaG_tracecall(L);
  base = ci->func + 1;
  /* main loop of interpreter */
  for (;;)
  {
    Instruction i; /* instruction being executed */
    if (l_unlikely(trap)) /* stack reallocation or hooks? */
    {
      trap = luaG_traceexec(L, pc); /* handle hooks */
      base = ci->func + 1;          /* correct stack */
    }
    i = code[pc++];
    assert(base == ci->func + 1);
    assert(base <= L->top && L->top <= L->stack_last);
    /* for tests, invalidate top for instructions not expecting it */
    assert(luaP_isIT(i) || (((void)(L->top = base)), 1));
    switch (GET_OPCODE(i))
    {
      case OP_MOVE:
      {
        StkId ra = (base + GETARG_A(i));
        copyslot(L, ra, (base + GETARG_B(i)));
        break;
      }
      case OP_LOADI:
      {
        StkId ra = (base + GETARG_A(i));
        lua_Integer b = GETARG_sBx(i);
        setsloti(L, ra, b);
        break;
      }
      case OP_LOADF:
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_sBx(i);
        setslotflt(L, ra, (b));
        break;
      }
      case OP_LOADK:
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = k + GETARG_Bx(i);
        setslot(L, ra, rb);
        break;
      }
      case OP_LOADKX:
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb;
        rb = k + GETARG_Ax(code[pc]);
        pc++;
        setslot(L, ra, rb);
        break;
      }
      case OP_LOADFALSE:
      {
        StkId ra = (base + GETARG_A(i));
        setslottag(L, ra, LUA_VFALSE);
        break;
      }
      case OP_LFALSESKIP:
      {
        StkId ra = (base + GETARG_A(i));
        setslottag(L, ra, LUA_VFALSE);
        pc++; /* skip next instruction */
        break;
      }
      case OP_LOADTRUE:
      {
        StkId ra = (base + GETARG_A(i));
        setslottag(L, ra, LUA_VTRUE);
        break;
      }
      case OP_LOADNIL:
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_B(i);
        do
        {
          setslottag(L, ra++, LUA_VNIL);
        } while (b--);
        break;
      }
      case OP_GETUPVAL:
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_B(i);
        TValue v = upvalgetval(cl->upvals[b]);
        setslot(L, ra, &v);
        break;
      }
      case OP_SETUPVAL:
      {
        StkId ra = (base + GETARG_A(i));
        UpVal* uv = cl->upvals[GETARG_B(i)];
        TValue rav = getslot(L, ra);
        upvalsetval(L, uv, &rav);
        luaC_barrier(L, uv, &rav);
        break;
      }
      case OP_GETTABUP:
      {
        StkId ra = (base + GETARG_A(i));
        TValue upval = upvalgetval(cl->upvals[GETARG_B(i)]);
        TValue* rc = (k + GETARG_C(i));
        assert(ttisstring(rc));
        TString* key = tsvalue(rc); /* key must be a short string */
        lu_byte tag;
        TValue res;
        if (!ttistable(&upval))
          tag = LUA_VNOTABLE;
        else
        {
          tag = hvalue(&upval)->luaH_getshortstr(key, &res);
        }
        if (tagisempty(tag))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, &upval, rc, ra, tag, upvalorigin(GETARG_B(i)));
          trap = ci->trap;
        }
        else
          setslot(L, ra, &res);
        break;
      }
      case OP_GETTABLE:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        StkId rc = (base + GETARG_C(i));
        lu_byte tag;
        TValue res;
        if ((slottag(L, rc) == LUA_VNUMINT))
        { /* fast track for integers? */
          if (slottag(L, rb) != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            Table* h = slothval(L, rb);
            lua_Integer u = l_intsub(slotival(L, rc), 1);
            if (l_ult(u, h->asize()))
            {
              tag = getArrTag(h, u);
              if (!tagisempty(tag))
                farr2val(h, u, tag, &res);
            }
            else
              tag = h->luaH_getint(slotival(L, rc), &res);
          }
        }
        else if (slottag(L, rb) != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          TValue rcv = getslot(L, rc);
          tag = slothval(L, rb)->luaH_get(&rcv, &res);
        }
        if (tagisempty(tag))
        {
          TValue rbv = getslot(L, rb);
          TValue rcv = getslot(L, rc);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, &rbv, &rcv, ra, tag, rb);
          trap = ci->trap;
        }
        else
          setslot(L, ra, &res);
        break;
      }
      case OP_GETI:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        int c = GETARG_C(i);
        lu_byte tag;
        TValue res;
        if (slottag(L, rb) != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          Table* h = slothval(L, rb);
          lua_Integer u = l_intsub(c, 1);
          if (l_ult(u, h->asize()))
          {
            tag = getArrTag(h, u);
            if (!tagisempty(tag))
              farr2val(h, u, tag, &res);
          }
          else
            tag = h->luaH_getint(c, &res);
        }
        if (tagisempty(tag))
        {
          TValue key;
          setivalue(&key, c);
          TValue rbv = getslot(L, rb);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, &rbv, &key, ra, tag, rb);
          trap = ci->trap;
        }
        else
          setslot(L, ra, &res);
        break;
      }
      case OP_GETFIELD:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        TValue* rc = (k + GETARG_C(i));
        assert(ttisstring(rc));
        TString* key = tsvalue(rc); /* key must be a short string */
        lu_byte tag;
        TValue res;
        if (slottag(L, rb) != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          tag = slothval(L, rb)->luaH_getshortstr(key, &res);
        }
        if (tagisempty(tag))
        {
          TValue rbv = getslot(L, rb);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, &rbv, rc, ra, tag, rb);
          trap = ci->trap;
        }
        else
          setslot(L, ra, &res);
        break;
      }
      case OP_SETTABUP:
      {
        int hres;
        TValue upval = upvalgetval(cl->upvals[GETARG_A(i)]);
        TValue* rb = (k + GETARG_B(i));
        TValue rc = TESTARG_k(i) ? getobj(k + GETARG_C(i)) : getslot(L, base + GETARG_C(i));
        assert(ttisstring(rb));
        TString* key = tsvalue(rb); /* key must be a short string */
        if (!ttistable(&upval))
          hres = HNOTATABLE;
        else
        {
          hres = hvalue(&upval)->luaH_psetshortstr(key, &rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, &upval, &rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishset(L, &upval, rb, &rc, hres, upvalorigin(GETARG_A(i)));
          trap = ci->trap;
        }
        break;
      }
      case OP_SETTABLE:
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        StkId rb = (base + GETARG_B(i)); /* key (table is in 'ra') */
        TValue rc = TESTARG_k(i) ? getobj(k + GETARG_C(i)) : getslot(L, base + GETARG_C(i)); /* value */
        if ((slottag(L, rb) == LUA_VNUMINT))
        { /* fast track for integers? */
          StkId t = ra;
          if (slottag(L, t) != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            Table* h = slothval(L, t);
            lua_Integer u = l_intsub(slotival(L, rb), 1);
            if (l_ult(u, h->asize()))
            {
              lu_byte tag = getArrTag(h, u);
              if (checknoTM(h->metatable, TM_NEWINDEX) || !tagisempty(tag))
              {
                fval2arr(h, u, &rc);
                hres = HOK;
              }
              else
                hres = ~(int)u;
            }
            else
              hres = h->luaH_psetint(slotival(L, rb), &rc);
          }
        }
        else
        {
          StkId t = ra;
          if (slottag(L, t) != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            TValue rbv = getslot(L, rb);
            hres = slothval(L, t)->luaH_pset(&rbv, &rc);
          }
        }
        if (hres == HOK)
          luaV_finishfastset(L, ra, &rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          TValue tv = getslot(L, ra);
          TValue rbv = getslot(L, rb);
          luaV_finishset(L, &tv, &rbv, &rc, hres, ra);
          trap = ci->trap;
        }
        break;
      }
      case OP_SETI:
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        int b = GETARG_B(i);
        TValue rc = TESTARG_k(i) ? getobj(k + GETARG_C(i)) : getslot(L, base + GETARG_C(i));
        StkId t = ra;
        if (slottag(L, t) != ctb(LUA_VTABLE))
          hres = HNOTATABLE;
        else
        {
          Table* h = slothval(L, t);
          lua_Integer u = l_intsub(b, 1);
          if (l_ult(u, h->asize()))
          {
            lu_byte tag = getArrTag(h, u);
            if (checknoTM(h->metatable, TM_NEWINDEX) || !tagisempty(tag))
            {
              fval2arr(h, u, &rc);
              hres = HOK;
            }
            else
              hres = ~(int)u;
          }
          else
            hres = h->luaH_psetint(b, &rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, ra, &rc);
        else
        {
          TValue key;
          setivalue(&key, b);
          ci->savedpc = pc;
          L->top = ci->top;
          TValue tv = getslot(L, ra);
          luaV_finishset(L, &tv, &key, &rc, hres, ra);
          trap = ci->trap;
        }
        break;
      }
      case OP_SETFIELD:
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        TValue* rb = (k + GETARG_B(i));
        TValue rc = TESTARG_k(i) ? getobj(k + GETARG_C(i)) : getslot(L, base + GETARG_C(i));
        assert(ttisstring(rb));
        TString* key = tsvalue(rb); /* key must be a short string */
        StkId t = ra;
        if (slottag(L, t) != ctb(LUA_VTABLE))
          hres = HNOTATABLE;
        else
        {
          hres = slothval(L, t)->luaH_psetshortstr(key, &rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, ra, &rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          TValue tv = getslot(L, ra);
          luaV_finishset(L, &tv, rb, &rc, hres, ra);
          trap = ci->trap;
        }
        break;
      }
      case OP_NEWTABLE:
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_vB(i); /* log2(hash size) + 1 */
        int c = GETARG_vC(i); /* array size */
        Table* t;
        if (b > 0)
          b = 1 << (b - 1); /* hash size is 2^(b - 1) */
        if (TESTARG_k(i))
        { /* non-zero extra argument? */
          assert(GETARG_Ax(code[pc]) != 0);
          /* add it to array size */
          c += GETARG_Ax(code[pc]) * (MAXARG_vC + 1);
        }
        pc++;              /* skip extra argument */
        L->top = ra + 1; /* correct top in case of emergency GC */
        t = Table::luaH_new(L);   /* memory allocation */
        setsloth(L, ra, t);
        if (b != 0 || c != 0)
          t->luaH_resize(L, c, b); /* idem */
        if (L->l_G->GCdebt <= 0)
        {
          ci->savedpc = pc;
          L->top = ra + 1;
          luaC_step(L);
          trap = ci->trap;
        }
        luai_threadyield(L);
        break;
      }
      case OP_SELF:
      {
        StkId ra = (base + GETARG_A(i));
        lu_byte tag;
        StkId rb = (base + GETARG_B(i));
        TValue* rc = (k + GETARG_C(i));
        assert(ttisstring(rc));
        TString* key = tsvalue(rc); /* key must be a short string */
        copyslot(L, ra + 1, rb);
        TValue res;
        if (slottag(L, rb) != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          tag = slothval(L, rb)->luaH_getshortstr(key, &res);
        }
        if (tagisempty(tag))
        {
          TValue rbv = getslot(L, rb);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, &rbv, rc, ra, tag, rb);
          trap = ci->trap;
        }
        else
          setslot(L, ra, &res);
        break;
      }
      case OP_ADDI:
      {
        StkId ra = base + GETARG_A(i);
        StkId v1 = base + GETARG_B(i);
        int imm = GETARG_sC(i);
        if (slottag(L, v1) == LUA_VNUMINT)
        {
          lua_Integer iv1 = slotival(L, v1);
          pc++;
          setsloti(L, ra, l_addi(L, iv1, imm));
        }
        else if (slottag(L, v1) == LUA_VNUMFLT)
        {
          lua_Number nb = slotfltval(L, v1);
          lua_Number fimm = imm;
          pc++;
          setslotflt(L, ra, luai_numadd(L, nb, fimm));
        }
        break;
      }
      case OP_ADDK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = ivalue(v2);
          pc++;
          setsloti(L, ra, l_addi(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numadd(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_SUBK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = ivalue(v2);
          pc++;
          setsloti(L, ra, l_subi(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numsub(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_MULK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = ivalue(v2);
          pc++;
          setsloti(L, ra, l_muli(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_nummul(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_MODK:
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = ivalue(v2);
          pc++;
          setsloti(L, ra, luaV_mod(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luaV_modf(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_POWK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        NumResult n1;
        NumResult n2;
        if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setslotflt(L, ra, luai_numpow(L, n1.v, n2.v));
        }
        break;
      }
      case OP_DIVK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        NumResult n1;
        NumResult n2;
        if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setslotflt(L, ra, luai_numdiv(L, n1.v, n2.v));
        }
        break;
      }
      case OP_IDIVK:
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        assert(ttisnumber(v2));
        if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = ivalue(v2);
          pc++;
          setsloti(L, ra, luaV_idiv(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numidiv(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_BANDK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        IntResult i1;
        lua_Integer i2 = ivalue(v2);
        assert(ttisinteger(v2));
        if (tointegerns(L, v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_band(i1.v, i2));
        }
        break;
      }
      case OP_BORK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        IntResult i1;
        lua_Integer i2 = ivalue(v2);
        assert(ttisinteger(v2));
        if (tointegerns(L, v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_bor(i1.v, i2));
        }
        break;
      }
      case OP_BXORK:
      {
        StkId v1 = base + GETARG_B(i);
        TValue* v2 = k + GETARG_C(i);
        IntResult i1;
        lua_Integer i2 = ivalue(v2);
        assert(ttisinteger(v2));
        if (tointegerns(L, v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_bxor(i1.v, i2));
        }
        break;
      }
      case OP_SHLI:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        int ic = GETARG_sC(i);
        IntResult ib;
        if (tointegerns(L, rb, &ib))
        {
          pc++;
          setsloti(L, ra, luaV_shiftl(ic, ib.v));
        }
        break;
      }
      case OP_SHRI:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        int ic = GETARG_sC(i);
        IntResult ib;
        if (tointegerns(L, rb, &ib))
        {
          pc++;
          setsloti(L, ra, luaV_shiftl(ib.v, -ic));
        }
        break;
      }
      case OP_ADD:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = slotival(L, v2);
          pc++;
          setsloti(L, ra, l_addi(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numadd(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_SUB:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = slotival(L, v2);
          pc++;
          setsloti(L, ra, l_subi(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numsub(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_MUL:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = slotival(L, v2);
          pc++;
          setsloti(L, ra, l_muli(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_nummul(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_MOD:
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = slotival(L, v2);
          pc++;
          setsloti(L, ra, luaV_mod(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luaV_modf(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_POW:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        NumResult n1;
        NumResult n2;
        if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setslotflt(L, ra, luai_numpow(L, n1.v, n2.v));
        }
        break;
      }
      case OP_DIV:
      { /* float division (always with floats) */
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        NumResult n1;
        NumResult n2;
        if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setslotflt(L, ra, luai_numdiv(L, n1.v, n2.v));
        }
        break;
      }
      case OP_IDIV:
      {                   /* floor division */
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = slotival(L, v1);
          lua_Integer i2 = slotival(L, v2);
          pc++;
          setsloti(L, ra, luaV_idiv(L, i1, i2));
        }
        else
        {
          NumResult n1;
          NumResult n2;
          if (tonumberns(L, v1, &n1) && tonumberns(L, v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numidiv(L, n1.v, n2.v));
          }
        }
        break;
      }
      case OP_BAND:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        IntResult i1;
        IntResult i2;
        if (tointegerns(L, v1, &i1) && tointegerns(L, v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_band(i1.v, i2.v));
        }
        break;
      }
      case OP_BOR:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        IntResult i1;
        IntResult i2;
        if (tointegerns(L, v1, &i1) && tointegerns(L, v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_bor(i1.v, i2.v));
        }
        break;
      }
      case OP_BXOR:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        IntResult i1;
        IntResult i2;
        if (tointegerns(L, v1, &i1) && tointegerns(L, v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, l_bxor(i1.v, i2.v));
        }
        break;
      }
      case OP_SHL:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        IntResult i1;
        IntResult i2;
        if (tointegerns(L, v1, &i1) && tointegerns(L, v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, luaV_shiftl(i1.v, i2.v));
        }
        break;
      }
      case OP_SHR:
      {
        StkId v1 = base + GETARG_B(i);
        StkId v2 = base + GETARG_C(i);
        IntResult i1;
        IntResult i2;
        if (tointegerns(L, v1, &i1) && tointegerns(L, v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setsloti(L, ra, luaV_shiftr(i1.v, i2.v));
        }
        break;
      }
      case OP_MMBIN:
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = code[pc - 2]; /* original arith. expression */
        StkId rb = (base + GETARG_B(i));
        TMS tm = (TMS) GETARG_C(i);
        StkId result = (base + GETARG_A(pi));
        assert(OP_ADD <= GET_OPCODE(pi) && GET_OPCODE(pi) <= OP_SHR);
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybinTM(L, ra, rb, result, tm);
        trap = ci->trap;
        break;
      }
      case OP_MMBINI:
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = code[pc - 2]; /* original arith. expression */
        int imm = GETARG_sB(i);
        TMS tm = (TMS) GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = (base + GETARG_A(pi));
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybiniTM(L, ra, imm, flip, result, tm);
        trap = ci->trap;
        break;
      }
      case OP_MMBINK:
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = code[pc - 2]; /* original arith. expression */
        TValue* imm = (k + GETARG_B(i));
        TMS tm = (TMS) GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = (base + GETARG_A(pi));
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybinassocTM(L, ra, imm, flip, result, tm);
        trap = ci->trap;
        break;
      }
      case OP_UNM:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        NumResult nb;
        if ((slottag(L, rb) == LUA_VNUMINT))
        {
          lua_Integer ib = slotival(L, rb);
          setsloti(L, ra, l_intunm(ib));
        }
        else if (tonumberns(L, rb, &nb))
        {
          setslotflt(L, ra, ((void)(L), -(nb.v)));
        }
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaT_trybinTM(L, rb, rb, ra, TM_UNM);
          trap = ci->trap;
        }
        break;
      }
      case OP_BNOT:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        IntResult ib;
        if (tointegerns(L, rb, &ib))
        {
          setsloti(L, ra, l_intbnot(ib.v));
        }
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaT_trybinTM(L, rb, rb, ra, TM_BNOT);
          trap = ci->trap;
        }
        break;
      }
      case OP_NOT:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        if (((slottag(L, rb) == LUA_VFALSE) || tagisempty(slottag(L, rb))))
          setslottag(L, ra, LUA_VTRUE);
        else
          setslottag(L, ra, LUA_VFALSE);
        break;
      }
      case OP_LEN:
      {
        StkId ra = (base + GETARG_A(i));
        ci->savedpc = pc;
        L->top = ci->top;
        luaV_objlen(L, ra, base + GETARG_B(i));
        trap = ci->trap;
        break;
      }
      case OP_CONCAT:
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_B(i); /* number of elements to concatenate */
        L->top = ra + n;   /* mark the end of concat operands */
        ci->savedpc = pc;
        luaV_concat(L, n);
        trap = ci->trap;
        if (L->l_G->GCdebt <= 0)
        { /* 'luaV_concat' ensures correct top */
          ci->savedpc = pc;
          luaC_step(L);
          trap = ci->trap;
        }
        luai_threadyield(L);
        break;
      }
      case OP_CLOSE:
      {
        StkId ra = (base + GETARG_A(i));
        assert(!GETARG_B(i)); /* 'close must be alive */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_close(L, ra, LUA_OK, 1);
        trap = ci->trap;
        break;
      }
      case OP_TBC:
      {
        StkId ra = (base + GETARG_A(i));
        /* create new to-be-closed upvalue */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_newtbcupval(L, ra);
        break;
      }
      case OP_JMP:
      {
        pc += GETARG_sJ(i);
        trap = ci->trap;
        break;
      }
      case OP_EQ:
      {
        StkId ra = (base + GETARG_A(i));
        int cond;
        StkId rb = (base + GETARG_B(i));
        ci->savedpc = pc;
        L->top = ci->top;
        cond = luaV_equalobj(L, ra, rb);
        trap = ci->trap;
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        break;
      }
      case OP_LT:
      {
        StkId ra = base + GETARG_A(i);
        StkId rb = base + GETARG_B(i);
        int cond;
        if (slottag(L, ra) == LUA_VNUMINT && slottag(L, rb) == LUA_VNUMINT)
        {
          lua_Integer ia = slotival(L, ra);
          lua_Integer ib = slotival(L, rb);
          cond = l_lti(ia, ib);
        }
        else if (novariant(slottag(L, ra)) == LUA_TNUMBER && novariant(slottag(L, rb)) == LUA_TNUMBER)
          cond = LTnum(L, ra, rb);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          cond = lessthanothers(L, ra, rb);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_LE:
      {
        StkId ra = base + GETARG_A(i);
        StkId rb = base + GETARG_B(i);
        int cond;
        if (slottag(L, ra) == LUA_VNUMINT && slottag(L, rb) == LUA_VNUMINT)
        {
          lua_Integer ia = slotival(L, ra);
          lua_Integer ib = slotival(L, rb);
          cond = l_lei(ia, ib);
        }
        else if (novariant(slottag(L, ra)) == LUA_TNUMBER && novariant(slottag(L, rb)) == LUA_TNUMBER)
          cond = LEnum(L, ra, rb);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          cond = lessequalothers(L, ra, rb);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_EQK:
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (k + GETARG_B(i));
        /* basic types do not use '__eq'; we can use raw equality */
        int cond = luaV_rawequalobj(L, ra, rb);
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        break;
      }
      case OP_EQI:
      {
        StkId ra = (base + GETARG_A(i));
        int cond;
        int im = GETARG_sB(i);
        if (slottag(L, ra) == LUA_VNUMINT)
          cond = (slotival(L, ra) == im);
        else if (slottag(L, ra) == LUA_VNUMFLT)
          cond = (slotfltval(L, ra) == (im));
        else
          cond = 0; /* other types cannot be equal to a number */
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        break;
      }
      case OP_LTI:
      {
        StkId ra = base + GETARG_A(i);
        int cond;
        int im = GETARG_sB(i);
        if (slottag(L, ra) == LUA_VNUMINT)
          cond = l_lti(slotival(L, ra), im);
        else if (slottag(L, ra) == LUA_VNUMFLT)
        {
          lua_Number fa = slotfltval(L, ra);
          lua_Number fim = im;
          cond = luai_numlt(fa, fim);
        }
        else
        {
          int isf = GETARG_C(i);
          ci->savedpc = pc;
          L->top = ci->top;
          cond = luaT_callorderiTM(L, ra, im, 0, isf, TM_LT);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_LEI:
      {
        StkId ra = base + GETARG_A(i);
        int cond;
        int im = GETARG_sB(i);
        if (slottag(L, ra) == LUA_VNUMINT)
          cond = l_lei(slotival(L, ra), im);
        else if (slottag(L, ra) == LUA_VNUMFLT)
        {
          lua_Number fa = slotfltval(L, ra);
          lua_Number fim = im;
          cond = luai_numle(fa, fim);
        }
        else
        {
          int isf = GETARG_C(i);
          ci->savedpc = pc;
          L->top = ci->top;
          cond = luaT_callorderiTM(L, ra, im, 0, isf, TM_LE);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_GTI:
      {
        StkId ra = base + GETARG_A(i);
        int cond;
        int im = GETARG_sB(i);
        if (slottag(L, ra) == LUA_VNUMINT)
          cond = l_gti(slotival(L, ra), im);
        else if (slottag(L, ra) == LUA_VNUMFLT)
        {
          lua_Number fa = slotfltval(L, ra);
          lua_Number fim = im;
          cond = luai_numgt(fa, fim);
        }
        else
        {
          int isf = GETARG_C(i);
          ci->savedpc = pc;
          L->top = ci->top;
          cond = luaT_callorderiTM(L, ra, im, 1, isf, TM_LT);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_GEI:
      {
        StkId ra = base + GETARG_A(i);
        int cond;
        int im = GETARG_sB(i);
        if (slottag(L, ra) == LUA_VNUMINT)
          cond = l_gei(slotival(L, ra), im);
        else if (slottag(L, ra) == LUA_VNUMFLT)
        {
          lua_Number fa = slotfltval(L, ra);
          lua_Number fim = im;
          cond = luai_numge(fa, fim);
        }
        else
        {
          int isf = GETARG_C(i);
          ci->savedpc = pc;
          L->top = ci->top;
          cond = luaT_callorderiTM(L, ra, im, 1, isf, TM_LE);
          trap = ci->trap;
        }
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        break;
      }
      case OP_TEST:
      {
        StkId ra = (base + GETARG_A(i));
        int cond = !((slottag(L, ra) == LUA_VFALSE) || tagisempty(slottag(L, ra)));
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        break;
      }
      case OP_TESTSET:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rb = (base + GETARG_B(i));
        if (((slottag(L, rb) == LUA_VFALSE) || tagisempty(slottag(L, rb))) == GETARG_k(i))
          pc++;
        else
        {
          copyslot(L, ra, rb);
          Instruction ni = code[pc];
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        break;
      }
      case OP_CALL:
      {
        StkId ra = (base + GETARG_A(i));
        CallInfo* newci;
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b != 0)          /* fixed number of arguments? */
          L->top = ra + b; /* top signals number of arguments */
        /* else previous instruction set top */
        ci->savedpc = pc; /* in case of errors */
        if ((newci = luaD_precall(L, ra, nresults)) == nullptr)
          trap = ci->trap; /* C call; nothing else to be done */
        else
        { /* Lua call: run function in this same C frame */
          ci = newci;
          trap = L->hookmask;
          cl = ci_func(L, ci);
          k = cl->p->k.data();
          code = cl->p->code.data();
          pc = ci->savedpc;
          if (l_unlikely(trap))
            trap = luaG_tracecall(L);
          base = ci->func + 1;
        }
        break;
      }
      case OP_TAILCALL:
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_B(i); /* number of arguments + 1 (function) */
        int n;               /* number of results when calling a C function */
        int nparams1 = GETARG_C(i);
        /* delta is virtual 'func' - real 'func' (vararg functions) */
        int delta = (nparams1) ? ci->nextraargs + nparams1 : 0;
        if (b != 0)
          L->top = ra + b;
        else /* previous instruction set top */
          b = ((int)(L->top - ra));
        ci->savedpc = pc; /* several calls here can raise errors */
        if (TESTARG_k(i))
        {
          luaF_closeupval(L, base);        /* close upvalues from current call */
          assert(L->tbclist < base); /* no pending tbc variables */
          assert(base == ci->func + 1);
        }
        if ((n = luaD_pretailcall(L, ci, ra, b, delta)) < 0) /* Lua function? */
        {
          trap = L->hookmask;
          cl = ci_func(L, ci);
          k = cl->p->k.data();
          code = cl->p->code.data();
          pc = ci->savedpc;
          if (l_unlikely(trap))
            trap = luaG_tracecall(L);
          base = ci->func + 1;
        }
        else
        {                         /* C function? */
          ci->func -= delta;    /* restore 'func' (if vararg) */
          luaD_poscall(L, ci, n); /* finish caller */
          trap = ci->trap; /* 'luaD_poscall' can change hooks */
          if (ci->callstatus & CIST_FRESH)
            return; /* end this frame */
          ci = ci->previous; /* continue running caller in this frame */
          cl = ci_func(L, ci);
          k = cl->p->k.data();
          code = cl->p->code.data();
          pc = ci->savedpc;
          if (l_unlikely(trap))
            trap = luaG_tracecall(L);
          base = ci->func + 1;
        }
        break;
      }
      case OP_RETURN:
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_B(i) - 1; /* number of results */
        int nparams1 = GETARG_C(i);
        if (n < 0)                     /* not fixed? */
          n = ((int)(L->top - ra)); /* get what is available */
        ci->savedpc = pc;
        if (TESTARG_k(i))
        {                  /* may there be open upvalues? */
          ci->nres = n; /* save number of returns */
          if (L->top < ci->top)
            L->top = ci->top;
          luaF_close(L, base, CLOSEKTOP, 1);
          trap = ci->trap;
          { if (l_unlikely(trap)) { (base = (ci)->func + 1); ra = (base + GETARG_A(i)); } };
        }
        if (nparams1) /* vararg function? */
          ci->func -= ci->nextraargs + nparams1;
        L->top = ra + n; /* set call for 'luaD_poscall' */
        luaD_poscall(L, ci, n);
        trap = ci->trap; /* 'luaD_poscall' can change hooks */
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k.data();
        code = cl->p->code.data();
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        break;
      }
      case OP_RETURN0:
      {
        if (l_unlikely(L->hookmask))
        {
          StkId ra = (base + GETARG_A(i));
          L->top = ra;
          ci->savedpc = pc;
          luaD_poscall(L, ci, 0); /* no hurry... */
          trap = 1;
        }
        else
        { /* do the 'poscall' here */
          int nres = get_nresults(ci->callstatus);
          L->ci = ci->previous; /* back to caller */
          L->top = base - 1;
          for (; l_unlikely(nres > 0); nres--)
            setslottag(L, L->top++, LUA_VNIL); /* all results are nil */
        }
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k.data();
        code = cl->p->code.data();
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        break;
      }
      case OP_RETURN1:
      {
        if (l_unlikely(L->hookmask))
        {
          StkId ra = (base + GETARG_A(i));
          L->top = ra + 1;
          ci->savedpc = pc;
          luaD_poscall(L, ci, 1); /* no hurry... */
          trap = 1;
        }
        else
        { /* do the 'poscall' here */
          int nres = get_nresults(ci->callstatus);
          L->ci = ci->previous; /* back to caller */
          if (nres == 0)
            L->top = base - 1; /* asked for no results */
          else
          {
            StkId ra = (base + GETARG_A(i));
            copyslot(L, base - 1, ra); /* at least this result */
            L->top = base;
            for (; l_unlikely(nres > 1); nres--)
              setslottag(L, L->top++, LUA_VNIL); /* complete missing results */
          }
        }
        /* return from a Lua function */
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k.data();
        code = cl->p->code.data();
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        break;
      }
      case OP_FORLOOP:
      {
        StkId ra = (base + GETARG_A(i));
        if (slottag(L, ra + 1) == LUA_VNUMINT)
        { /* integer loop? */
          assert(slottag(L, ra) == LUA_VNUMINT);
          lua_Integer count = slotival(L, ra);
          if (count != 0)
          { /* still more iterations? */
            assert(slottag(L, ra + 1) == LUA_VNUMINT);
            lua_Integer step = slotival(L, ra + 1);
            assert(slottag(L, ra + 2) == LUA_VNUMINT);
            lua_Integer idx = slotival(L, ra + 2);    /* control variable */
            chgsloti(L, ra, l_intsub(count, 1)); /* update counter */
            idx = l_intadd(idx, step);                /* add step to index */
            chgsloti(L, ra + 2, idx);              /* update control variable */
            pc -= GETARG_Bx(i);                       /* jump back */
          }
        }
        else if (floatforloop(L, ra)) /* float loop */
          pc -= GETARG_Bx(i);         /* jump back */
        trap = ci->trap; /* allows a signal to break the loop */
        break;
      }
      case OP_FORPREP:
      {
        StkId ra = (base + GETARG_A(i));
        ci->savedpc = pc; /* in case of errors */
        L->top = ci->top;
        if (forprep(L, ra))
          pc += GETARG_Bx(i) + 1; /* skip the loop */
        break;
      }
      case OP_TFORPREP:
      {
        /* before: 'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the initial value for the control variable, and
           'ra + 3' has the closing variable. This opcode then swaps the
           control and the closing variables and marks the closing variable
           as to-be-closed.
        */
        StkId ra = (base + GETARG_A(i));
        TValue temp = getslot(L, ra + 3); /* to swap control and closing variables */
        copyslot(L, ra + 3, ra + 2);
        setslot(L, ra + 2, &temp);
        /* create to-be-closed upvalue (if closing var. is not nil) */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_newtbcupval(L, ra + 2);
        pc += GETARG_Bx(i); /* go to end of the loop */
        i = code[pc++];        /* fetch next instruction */
        assert(GET_OPCODE(i) == OP_TFORCALL && ra == (base + GETARG_A(i)));
        ra = (base + GETARG_A(i));
        copyslot(L, ra + 5, ra + 3); /* copy the control variable */
        copyslot(L, ra + 4, ra + 1); /* copy state */
        copyslot(L, ra + 3, ra);     /* copy function */
        L->top = ra + 3 + 3;
        ci->savedpc = pc;
        luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
        trap = ci->trap;
        { if (l_unlikely(trap)) { (base = (ci)->func + 1); ra = (base + GETARG_A(i)); } };                              /* stack may have changed */
        i = code[pc++];                                  /* go to next instruction */
        assert(GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i)));
        ra = (base + GETARG_A(i));
        if (!tagisempty(slottag(L, ra + 3))) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        break;
      }
      case OP_TFORCALL:
      {
        /* 'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the closing variable, and 'ra + 3' has the control
           variable. The call will use the stack starting at 'ra + 3',
           so that it preserves the first three values, and the first
           return will be the new value for the control variable.
        */
        StkId ra = (base + GETARG_A(i));
        copyslot(L, ra + 5, ra + 3); /* copy the control variable */
        copyslot(L, ra + 4, ra + 1); /* copy state */
        copyslot(L, ra + 3, ra);     /* copy function */
        L->top = ra + 3 + 3;
        ci->savedpc = pc;
        luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
        trap = ci->trap;
        { if (l_unlikely(trap)) { (base = (ci)->func + 1); ra = (base + GETARG_A(i)); } };                              /* stack may have changed */
        i = code[pc++];                                  /* go to next instruction */
        assert(GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i)));
        ra = (base + GETARG_A(i));
        if (!tagisempty(slottag(L, ra + 3))) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        break;
      }
      case OP_TFORLOOP:
      {
        StkId ra = (base + GETARG_A(i));
        if (!tagisempty(slottag(L, ra + 3))) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        break;
      }
      case OP_SETLIST:
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_vB(i);
        int last = GETARG_vC(i);
        Table* h = slothval(L, ra);
        if (n == 0)
          n = ((int)(L->top - ra)) - 1; /* get up to the top */
        else
          L->top = ci->top; /* correct top in case of emergency GC */
        last += n;
        if (TESTARG_k(i))
        {
          last += GETARG_Ax(code[pc]) * (MAXARG_vC + 1);
          pc++;
        }
        /* when 'n' is known, table should have proper size */
        if (last > h->asize())
        { /* needs more space? */
          /* fixed-size sets should have space preallocated */
          assert(GETARG_vB(i) == 0);
          h->luaH_resizearray(L, last); /* preallocate it at once */
        }
        for (; n > 0; n--)
        {
          StkId val = ra + n;
          TValue vv = getslot(L, val);
          obj2arr(h, last - 1, &vv);
          last--;
          assert(novariant(h->tt) >= LUA_TSTRING);
          luaC_barrierback(L, h, &vv);
        }
        break;
      }
      case OP_CLOSURE:
      {
        StkId ra = (base + GETARG_A(i));
        Proto* p = cl->p->p[GETARG_Bx(i)];
        ci->savedpc = pc;
        L->top = ci->top;
        pushclosure(L, p, cl->upvals, base, ra);
        if (L->l_G->GCdebt <= 0)
        {
          ci->savedpc = pc;
          L->top = ra + 1;
          luaC_step(L);
          trap = ci->trap;
        }
        luai_threadyield(L);
        break;
      }
      case OP_VARARG:
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_C(i) - 1; /* required results (-1 means all) */
        int vatab = GETARG_k(i) ? GETARG_B(i) : -1;
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_getvarargs(L, ci, ra, n, vatab);
        trap = ci->trap;
        break;
      }
      case OP_GETVARG:
      {
        StkId ra = (base + GETARG_A(i));
        StkId rc = (base + GETARG_C(i));
        luaT_getvararg(L, ci, ra, rc);
        break;
      }
      case OP_ERRNNIL:
      {
        StkId ra = (base + GETARG_A(i));
        if (!tagisempty(slottag(L, ra)))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaG_errnnil(L, cl, GETARG_Bx(i));
        }
        break;
      }
      case OP_VARARGPREP:
      {
        ci->savedpc = pc;
        luaT_adjustvarargs(L, ci, cl->p);
        trap = ci->trap;
        if (l_unlikely(trap))
        { /* previous "Protect" updated trap */
          luaD_hookcall(L, ci);
          L->oldpc = 1; /* next opcode will be seen as a "new" line */
        }
        (base = (ci)->func + 1); /* function has new base after adjustment */
        break;
      }
      case OP_EXTRAARG:
      {
        assert(0);
        break;
      }
    }
  }
}

/* }================================================================== */

}
