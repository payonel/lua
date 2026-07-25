/*
** $Id: lvm.c $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

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

/* limit for table tag-method chains (to avoid infinite loops) */
constexpr int MAXTAGLOOP = 2000;

/*
** 'l_intfitsf' checks whether a given integer is in the range that
** can be converted to a float without rounding. Used in comparisons.
*/

/* number of bits in the mantissa of a float */
#define NBM (l_floatatt(MANT_DIG))

/*
** Check whether some integers may not fit in a float, testing whether
** (maxinteger >> NBM) > 0. (That implies (1 << NBM) <= maxinteger.)
** (The shifts are done in parts, to avoid shifting by more than the size
** of an integer. In a worst case, NBM == 113 for long double and
** sizeof(long) == 32.)
*/
#if ((((LUA_MAXINTEGER >> (NBM / 4)) >> (NBM / 4)) >> (NBM / 4)) >> (NBM - (3 * (NBM / 4)))) > 0

/* limit for integers that fit in a float */
constexpr lua_Unsigned MAXINTFITSF = ((lua_Unsigned) 1 << NBM);

/* check whether 'i' is in the interval [-MAXINTFITSF, MAXINTFITSF] */
static inline int l_intfitsf(lua_Integer i)
{
  return (MAXINTFITSF + (lua_Unsigned)i) <= (2 * MAXINTFITSF);
}

#else /* all integers fit in a float precisely */

static inline int l_intfitsf(lua_Integer i)
{
  (void) i;
  return 1;
}

#endif

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
    assert((obj->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    assert((obj->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    TString* st = static_cast<TString*>(obj->value_.gc);
    size_t stlen;
    const char* s = ((void)((stlen) = (st)->content.size()), (st)->content.c_str());
    return (luaO_str2num(s, result) == stlen + 1);
  }
}

/*
** Try to convert a value to a float. The float case is already handled
** by the macro 'tonumber'.
*/
int luaV_tonumber_(const TValue* obj, lua_Number* n)
{
  TValue v;
  if (((obj)->tt_ == LUA_VNUMINT))
  {
    *n = ((lua_Number)(obj->value_.i));
    return 1;
  }
  else if (l_strton(obj, &v))
  {                  /* string coercible to number? */
    /* convert result of 'luaO_str2num' to a float */
    assert((v.tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
    *n = (v.tt_ == LUA_VNUMINT) ? (lua_Number)v.value_.i : v.value_.n;
    return 1;
  }
  else
    return 0; /* conversion failed */
}

/*
** try to convert a float to an integer, rounding according to 'mode'.
*/
int luaV_flttointeger(lua_Number n, lua_Integer* p, F2Imod mode)
{
  lua_Number f = l_floor(n);
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
int luaV_tointegerns(const TValue* obj, lua_Integer* p, F2Imod mode)
{
  if (((obj)->tt_ == LUA_VNUMFLT))
    return luaV_flttointeger(obj->value_.n, p, mode);
  else if (((obj)->tt_ == LUA_VNUMINT))
  {
    *p = obj->value_.i;
    return 1;
  }
  else
    return 0;
}

/*
** try to convert a value to an integer.
*/
int luaV_tointeger(const TValue* obj, lua_Integer* p, F2Imod mode)
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
static int forlimit(lua_State* L, lua_Integer init, const TValue* lim,
                    lua_Integer* p, lua_Integer step)
{
  if (!luaV_tointeger(lim, p, (step < 0 ? F2Iceil : F2Ifloor)))
  {
    /* not coercible to in integer */
    lua_Number flim;           /* try to convert to float */
    if (!tonumber(lim, &flim)) /* cannot convert to float? */
      luaG_forerror(L, lim, "limit");
    /* else 'flim' is a float out of integer bounds */
    if (((0) < (flim)))
    { /* if it is positive, it is too large */
      if (step < 0)
        return 1;          /* initial value must be less than it */
      *p = LUA_MAXINTEGER; /* truncate */
    }
    else
    { /* it is less than min integer */
      if (step > 0)
        return 1;          /* initial value must be greater than it */
      *p = LUA_MININTEGER; /* truncate */
    }
  }
  return (step > 0 ? init > *p : init < *p); /* not to run? */
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
  TValue* pinit = (&(L)->stack.p[ra]);
  TValue* plimit = (&(L)->stack.p[ra + 1]);
  TValue* pstep = (&(L)->stack.p[ra + 2]);
  if (((pinit)->tt_ == LUA_VNUMINT) && ((pstep)->tt_ == LUA_VNUMINT))
  { /* integer loop? */
    lua_Integer init = pinit->value_.i;
    lua_Integer step = pstep->value_.i;
    lua_Integer limit;
    if (step == 0)
      luaG_runerror(L, "'for' step is zero");
    if (forlimit(L, init, plimit, &limit, step))
      return 1; /* skip the loop */
    else
    { /* prepare loop counter */
      lua_Unsigned count;
      if (step > 0)
      { /* ascending loop? */
        count = ((lua_Unsigned)(limit)) - ((lua_Unsigned)(init));
        if (step != 1) /* avoid division in the too common case */
          count /= ((lua_Unsigned)(step));
      }
      else
      { /* step < 0; descending loop */
        count = ((lua_Unsigned)(init)) - ((lua_Unsigned)(limit));
        /* 'step+1' avoids negating 'mininteger' */
        count /= ((lua_Unsigned)(-(step + 1))) + 1u;
      }
      /* use 'chgivalue' for places that for sure had integers */
      chgivalue((&(L)->stack.p[ra]), ((lua_Integer)(count))); /* change init to count */
      setivalue((&(L)->stack.p[ra + 1]), step);         /* change limit to step */
      chgivalue((&(L)->stack.p[ra + 2]), init);         /* change step to init */
    }
  }
  else
  { /* try making all values floats */
    lua_Number init;
    lua_Number limit;
    lua_Number step;
    if (l_unlikely(!tonumber(plimit, &limit)))
      luaG_forerror(L, plimit, "limit");
    if (l_unlikely(!tonumber(pstep, &step)))
      luaG_forerror(L, pstep, "step");
    if (l_unlikely(!tonumber(pinit, &init)))
      luaG_forerror(L, pinit, "initial value");
    if (step == 0)
      luaG_runerror(L, "'for' step is zero");
    if (((0) < (step)) ? ((limit) < (init))
                            : ((init) < (limit)))
      return 1; /* skip the loop */
    else
    {
      /* make sure all values are floats */
      setfltvalue((&(L)->stack.p[ra]), limit);
      setfltvalue((&(L)->stack.p[ra + 1]), step);
      setfltvalue((&(L)->stack.p[ra + 2]), init); /* control variable */
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
  assert(((&L->stack.p[ra + 1]))->tt_ == LUA_VNUMFLT);
  lua_Number step = (((&L->stack.p[ra + 1]))->value_).n;
  assert(((&L->stack.p[ra]))->tt_ == LUA_VNUMFLT);
  lua_Number limit = (((&L->stack.p[ra]))->value_).n;
  assert(((&L->stack.p[ra + 2]))->tt_ == LUA_VNUMFLT);
  lua_Number idx = (((&L->stack.p[ra + 2]))->value_).n; /* control variable */
  idx = ((void)(L), (idx) + (step));        /* increment index */
  if (((0) < (step)) ? ((idx) <= (limit))
                          : ((limit) <= (idx)))
  {
    chgfltvalue((&(L)->stack.p[ra + 2]), idx); /* update control variable */
    return 1;                      /* jump back */
  }
  else
    return 0; /* finish the loop */
}

/*
** Finish the table access 'val = t[key]' and return the tag of the result.
*/
lu_byte luaV_finishget(lua_State* L, const TValue* t, TValue* key,
                       StkId val, lu_byte tag)
{
  int loop;         /* counter to avoid infinite loops */
  const TValue* tm; /* metamethod */
  for (loop = 0; loop < MAXTAGLOOP; loop++)
  {
    if (tag == LUA_VNOTABLE)
    { /* 't' is not a table? */
      assert(!((t)->tt_ == ctb(LUA_VTABLE)));
      tm = luaT_gettmbyobj(L, t, TM_INDEX);
      if (l_unlikely(notm(tm)))
        luaG_typeerror(L, t, "index"); /* no metamethod */
      /* else will try the metamethod */
    }
    else
    {                                                 /* 't' is a table */
      assert(t->tt_ == ctb(LUA_VTABLE));
      assert(t->value_.gc->tt == LUA_VTABLE);
      Table* h = static_cast<Table*>(t->value_.gc);
      tm = fasttm(L, h->metatable, TM_INDEX); /* table's metamethod */
      if (tm == NULL)
      {                     /* no metamethod? */
        (((&(L)->stack.p[val]))->tt_ = LUA_VNIL); /* result is nil */
        return LUA_VNIL;
      }
      /* else will try the metamethod */
    }
    if ((((tm)->tt_ & NOVARIANT_MASK) == LUA_TFUNCTION))
    {                                           /* is metamethod a function? */
      tag = luaT_callTMres(L, tm, t, key, val); /* call it */
      return tag;                               /* return tag of the result */
    }
    t = tm; /* else try to access 'tm[key]' */
    if (t->tt_ != ctb(LUA_VTABLE))
      tag = LUA_VNOTABLE;
    else
    {
      assert(t->value_.gc->tt == LUA_VTABLE);
      tag = luaH_get(static_cast<Table*>(t->value_.gc), key, &L->stack.p[val]);
    }
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
      return tag; /* done */
    /* else repeat (tail call 'luaV_finishget') */
  }
  luaG_runerror(L, "'__index' chain too long; possible loop");
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
                    TValue* val, int hres)
{
  int loop; /* counter to avoid infinite loops */
  for (loop = 0; loop < MAXTAGLOOP; loop++)
  {
    const TValue* tm; /* '__newindex' metamethod */
    if (hres != HNOTATABLE)
    {                                            /* is 't' a table? */
      assert(t->tt_ == ctb(LUA_VTABLE));
      assert(t->value_.gc->tt == LUA_VTABLE);
      Table* h = static_cast<Table*>(t->value_.gc);                      /* save 't' table */
      tm = fasttm(L, h->metatable, TM_NEWINDEX); /* get metamethod */
      if (tm == NULL)
      {                                       /* no metamethod? */
        sethvalue(L, (&(L)->stack.p[L->top]), h);          /* anchor 't' */
        L->top++;                           /* assume EXTRA_STACK */
        luaH_finishset(L, h, key, val, hres); /* set new value */
        L->top--;
        invalidateTMcache(h);
        assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
        luaC_barrierback(L, const_cast<GCObject*>(static_cast<const GCObject*>(h)), val);
        return;
      }
      /* else will try the metamethod */
    }
    else
    { /* not a table; check metamethod */
      tm = luaT_gettmbyobj(L, t, TM_NEWINDEX);
      if (l_unlikely(notm(tm)))
        luaG_typeerror(L, t, "index");
    }
    /* try the metamethod */
    if ((((tm)->tt_ & NOVARIANT_MASK) == LUA_TFUNCTION))
    {
      luaT_callTM(L, tm, t, key, val);
      return;
    }
    t = tm; /* else repeat assignment over 'tm' */
    if (t->tt_ != ctb(LUA_VTABLE))
      hres = HNOTATABLE;
    else
    {
      assert(t->value_.gc->tt == LUA_VTABLE);
      hres = luaH_pset(static_cast<Table*>(t->value_.gc), key, val);
    }
    if (hres == HOK)
    {
      luaV_finishfastset(L, t, val);
      return; /* done */
    }
    /* else 'return luaV_finishset(L, t, key, val, slot)' (loop) */
  }
  luaG_runerror(L, "'__newindex' chain too long; possible loop");
}

/*
** Function to be used for 0-terminated string order comparison
*/
#if !defined(l_strcoll)
#define l_strcoll strcoll
#endif

/*
** Compare two strings 'ts1' x 'ts2', returning an integer less-equal-
** -greater than zero if 'ts1' is less-equal-greater than 'ts2'.
** The code is a little tricky because it allows '\0' in the strings
** and it uses 'strcoll' (to respect locales) for each segment
** of the strings. Note that segments can compare equal but still
** have different lengths.
*/
static int l_strcmp(const TString* ts1, const TString* ts2)
{
  size_t rl1; /* real length */
  const char* s1 = ((void)((rl1) = (ts1)->content.size()), (ts1)->content.c_str());
  size_t rl2;
  const char* s2 = ((void)((rl2) = (ts2)->content.size()), (ts2)->content.c_str());
  for (;;)
  { /* for each segment */
    int temp = l_strcoll(s1, s2);
    if (temp != 0) /* not equal? */
      return temp; /* done */
    else
    {                                /* strings are equal up to a '\0' */
      size_t zl1 = strlen(s1);       /* index of first '\0' in 's1' */
      size_t zl2 = strlen(s2);       /* index of first '\0' in 's2' */
      if (zl2 == rl2)                /* 's2' is finished? */
        return (zl1 == rl1) ? 0 : 1; /* check 's1' */
      else if (zl1 == rl1)           /* 's1' is finished? */
        return -1;                   /* 's1' is less than 's2' ('s2' is not finished) */
      /* both strings longer than 'zl'; go on comparing after the '\0' */
      zl1++;
      zl2++;
      s1 += zl1;
      rl1 -= zl1;
      s2 += zl2;
      rl2 -= zl2;
    }
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
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Iceil)) /* fi = ceil(f) */
      return i < fi;                        /* compare them as integers */
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
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor)) /* fi = floor(f) */
      return i <= fi;                        /* compare them as integers */
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
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor)) /* fi = floor(f) */
      return fi < i;                         /* compare them as integers */
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
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Iceil)) /* fi = ceil(f) */
      return fi <= i;                       /* compare them as integers */
    else                                    /* 'f' is either greater or less than all integers */
      return f < 0;                         /* less? */
  }
}

/*
** Return 'l < r', for numbers.
*/
static inline int LTnum(const TValue* l, const TValue* r)
{
  assert((((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER));
  if (((l)->tt_ == LUA_VNUMINT))
  {
    lua_Integer li = l->value_.i;
    if (((r)->tt_ == LUA_VNUMINT))
      return li < r->value_.i; /* both are integers */
    else
    { /* 'l' is int and 'r' is float */
      assert(r->tt_ == LUA_VNUMFLT);
      return LTintfloat(li, r->value_.n); /* l < r ? */
    }
  }
  else
  {
    assert(l->tt_ == LUA_VNUMFLT);
    lua_Number lf = l->value_.n; /* 'l' must be float */
    if (((r)->tt_ == LUA_VNUMFLT))
      return lf < r->value_.n; /* both are float */
    else
    { /* 'l' is float and 'r' is int */
      assert(r->tt_ == LUA_VNUMINT);
      return LTfloatint(lf, r->value_.i);
    }
  }
}

/*
** Return 'l <= r', for numbers.
*/
static inline int LEnum(const TValue* l, const TValue* r)
{
  assert((((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER));
  if (((l)->tt_ == LUA_VNUMINT))
  {
    lua_Integer li = l->value_.i;
    if (((r)->tt_ == LUA_VNUMINT))
      return li <= r->value_.i; /* both are integers */
    else
    { /* 'l' is int and 'r' is float */
      assert(r->tt_ == LUA_VNUMFLT);
      return LEintfloat(li, r->value_.n); /* l <= r ? */
    }
  }
  else
  {
    assert(l->tt_ == LUA_VNUMFLT);
    lua_Number lf = l->value_.n; /* 'l' must be float */
    if (((r)->tt_ == LUA_VNUMFLT))
      return lf <= r->value_.n; /* both are float */
    else
    { /* 'l' is float and 'r' is int */
      assert(r->tt_ == LUA_VNUMINT);
      return LEfloatint(lf, r->value_.i);
    }
  }
}

/*
** return 'l < r' for non-numbers.
*/
static int lessthanothers(lua_State* L, const TValue* l, const TValue* r)
{
  assert(!(((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) || !(((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER));
  if ((((l)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TSTRING)) /* both are strings? */
  {
    assert((l->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    assert((r->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    return l_strcmp(static_cast<TString*>(l->value_.gc), static_cast<TString*>(r->value_.gc)) < 0;
  }
  else
    return luaT_callorderTM(L, l, r, TM_LT);
}

/*
** Main operation less than; return 'l < r'.
*/
int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r)
{
  if ((((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)) /* both operands are numbers? */
    return LTnum(l, r);
  else
    return lessthanothers(L, l, r);
}

/*
** return 'l <= r' for non-numbers.
*/
static int lessequalothers(lua_State* L, const TValue* l, const TValue* r)
{
  assert(!(((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) || !(((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER));
  if ((((l)->tt_ & NOVARIANT_MASK) == LUA_TSTRING) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TSTRING)) /* both are strings? */
  {
    assert((l->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    assert((r->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    return l_strcmp(static_cast<TString*>(l->value_.gc), static_cast<TString*>(r->value_.gc)) <= 0;
  }
  else
    return luaT_callorderTM(L, l, r, TM_LE);
}

/*
** Main operation less than or equal to; return 'l <= r'.
*/
int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r)
{
  if ((((l)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER) && (((r)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)) /* both operands are numbers? */
    return LEnum(l, r);
  else
    return lessequalothers(L, l, r);
}

/*
** Main operation for equality of Lua values; return 't1 == t2'.
** L == NULL means raw equality (no metamethods)
*/
int luaV_equalobj(lua_State* L, const TValue* t1, const TValue* t2)
{
  const TValue* tm;
  if ((t1->tt_ & NOVARIANT_MASK) != (t2->tt_ & NOVARIANT_MASK)) /* not the same type? */
    return 0;
  else if (((t1)->tt_ & WITHVARIANT_MASK) != ((t2)->tt_ & WITHVARIANT_MASK))
  {
    switch (((t1)->tt_ & WITHVARIANT_MASK))
    {
    case LUA_VNUMINT:
    { /* integer == float? */
      /* integer and float can only be equal if float has an integer
         value equal to the integer */
      lua_Integer i2;
      assert(t2->tt_ == LUA_VNUMFLT);
      return (luaV_flttointeger(t2->value_.n, &i2, F2Ieq) && t1->value_.i == i2);
    }
    case LUA_VNUMFLT:
    {                 /* float == integer? */
      lua_Integer i1; /* see comment in previous case */
      assert(t1->tt_ == LUA_VNUMFLT);
      assert(t2->tt_ == LUA_VNUMINT);
      return (luaV_flttointeger(t1->value_.n, &i1, F2Ieq) && i1 == t2->value_.i);
    }
    case LUA_VSHRSTR:
    {
      assert((t1->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      assert((t2->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      return luaS_eqstr(static_cast<TString*>(t1->value_.gc), static_cast<TString*>(t2->value_.gc));
    }
    default:
      /* only numbers (integer/float) and strings (long/short) can have
         equal values with different variants */
      return 0;
    }
  }
  else
  { /* equal variants */
    switch (((t1)->tt_ & WITHVARIANT_MASK))
    {
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
      return 1;
    case LUA_VNUMINT:
      assert(t2->tt_ == LUA_VNUMINT);
      return (t1->value_.i == t2->value_.i);
    case LUA_VNUMFLT:
      assert(t2->tt_ == LUA_VNUMFLT);
      return (t1->value_.n == t2->value_.n);
    case LUA_VLIGHTUSERDATA:
      assert(t2->tt_ == LUA_VLIGHTUSERDATA);
      return t1->value_.p == t2->value_.p;
    case LUA_VSHRSTR:
    {
      assert((t1->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      assert((t2->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      return luaS_eqstr(static_cast<TString*>(t1->value_.gc), static_cast<TString*>(t2->value_.gc));
    }
    case LUA_VUSERDATA:
    {
      assert(t1->value_.gc->tt == LUA_VUSERDATA);
      assert(t2->value_.gc->tt == LUA_VUSERDATA);
      Udata* u1 = static_cast<Udata*>(t1->value_.gc);
      Udata* u2 = static_cast<Udata*>(t2->value_.gc);
      if (u1 == u2)
        return 1;
      else if (L == NULL)
        return 0;
      tm = fasttm(L, u1->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, u2->metatable, TM_EQ);
      break; /* will try TM */
    }
    case LUA_VTABLE:
    {
      assert(t1->value_.gc->tt == LUA_VTABLE);
      assert(t2->value_.gc->tt == LUA_VTABLE);
      Table* h1 = static_cast<Table*>(t1->value_.gc);
      Table* h2 = static_cast<Table*>(t2->value_.gc);
      if (h1 == h2)
        return 1;
      else if (L == NULL)
        return 0;
      tm = fasttm(L, h1->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, h2->metatable, TM_EQ);
      break; /* will try TM */
    }
    case LUA_VLCF:
      assert(t2->tt_ == LUA_VLCF);
      return (t1->value_.f == t2->value_.f);
    default: /* functions and threads */
      assert(t1->tt_ & BIT_ISCOLLECTABLE);
      assert(t2->tt_ & BIT_ISCOLLECTABLE);
      return (t1->value_.gc == t2->value_.gc);
    }
    if (tm == NULL) /* no TM? */
      return 0;     /* objects are different */
    else
    {
      int tag = luaT_callTMres(L, tm, t1, t2, L->top); /* call TM */
      return !((tag) == LUA_VFALSE || ((tag) & NOVARIANT_MASK) == LUA_TNIL);
    }
  }
}

/* macro used by 'luaV_concat' to ensure that element at 'o' is a string */

/*
** Check whether object is a short empty string to optimize concatenation.
** (External strings can be empty too; they will be concatenated like
** non-empty ones.)
*/
static int isemptystr(const TValue* o)
{
  return ((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING) &&
         tsslen(static_cast<TString*>(o->value_.gc)) == 0;
}

/* convert 'o' to a string; return 1 if the value is (now) a string */
static int tostring(lua_State* L, TValue* o)
{
  if ((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING)
    return 1;
  if (!cvt2str(o))
    return 0;
  luaO_tostring(L, o);
  return 1;
}

/* copy strings in stack from top - n up to top - 1 to buffer */
static void copy2buff(lua_State* L, StkId top, int n, char* buff)
{
  size_t tl = 0; /* size already copied */
  do
  {
    TValue* v = &L->stack.p[top - n];
    assert((v->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    assert((v->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    TString* st = static_cast<TString*>(v->value_.gc);
    size_t l; /* length of string being copied */
    const char* s = ((void)((l) = (st)->content.size()), (st)->content.c_str());
    memcpy(buff + tl, s, l * sizeof(char));
    tl += l;
  } while (--n > 0);
}

/*
** Main operation for concatenation: concat 'total' values in the stack,
** from 'L->top - total' up to 'L->top - 1'.
*/
void luaV_concat(lua_State* L, int total)
{
  if (total == 1)
    return; /* "all" values already concatenated */
  do
  {
    StkId top = L->top;
    int n = 2; /* number of elements handled in this pass (at least 2) */
    TValue* v1 = &L->stack.p[top - 2];
    TValue* v2 = &L->stack.p[top - 1];
    if (!(((v1->tt_ & NOVARIANT_MASK) == LUA_TSTRING) || cvt2str(v1)) || !tostring(L, v2))
      luaT_tryconcatTM(L);                  /* may invalidate 'top' */
    else if (isemptystr(v2))                /* second operand is empty? */
      (void)tostring(L, v1);                /* result is first operand */
    else if (isemptystr(v1))
    {                                 /* first operand is empty string? */
      setobj(L, v1, v2); /* result is second op. */
    }
    else
    {
      /* at least two string values; get as many as possible */
      assert((v2->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      size_t tl = tsslen(static_cast<TString*>(v2->value_.gc)); /* total length */
      TString* ts;
      /* collect total length and number of strings */
      for (n = 1; n < total && tostring(L, &L->stack.p[top - n - 1]); n++)
      {
        TValue* o = &L->stack.p[top - n - 1];
        assert((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((o->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        size_t l = tsslen(static_cast<TString*>(o->value_.gc));
        if (l_unlikely(l >= MAX_SIZE - sizeof(TString) - tl))
        {
          L->top = top - total; /* pop strings to avoid wasting stack */
          luaG_runerror(L, "string length overflow");
        }
        tl += l;
      }
      if (tl <= 40)
      { /* small result? use stack buffer */
        char buff[40];
        copy2buff(L, top, n, buff);
        ts = luaS_newlstr(L, buff, tl);
      }
      else
      { /* large result; heap buffer */
        char* buff = ((char*)(luaM_malloc_(L, ((size_t)(tl)) * sizeof(char), 0)));
        copy2buff(L, top, n, buff);
        ts = luaS_newlstr(L, buff, tl);
        luaM_free_(L, (buff), (tl) * sizeof(*(buff)));
      }
      setsvalue(L, (&(L)->stack.p[top - n]), ts); /* create result */
    }
    total -= n - 1;    /* got 'n' strings to create one new */
    L->top -= n - 1; /* popped 'n' strings and pushed one */
  } while (total > 1); /* repeat until only 1 result left */
}

/*
** Main operation 'ra = #rb'.
*/
void luaV_objlen(lua_State* L, StkId ra, const TValue* rb)
{
  const TValue* tm;
  switch (((rb)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VTABLE:
  {
    assert(rb->tt_ == ctb(LUA_VTABLE));
    assert(rb->value_.gc->tt == LUA_VTABLE);
    Table* h = static_cast<Table*>(rb->value_.gc);
    tm = fasttm(L, h->metatable, TM_LEN);
    if (tm)
      break;                                        /* metamethod? break switch to call it */
    setivalue((&(L)->stack.p[ra]), ((lua_Integer)(luaH_getn(L, h)))); /* else primitive len */
    return;
  }
  case LUA_VSHRSTR:
  {
    assert((rb->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
    TString* ts = static_cast<TString*>(rb->value_.gc);
    setivalue((&(L)->stack.p[ra]), (lua_Integer)tsslen(ts));
    return;
  }
  default:
  { /* try metamethod */
    tm = luaT_gettmbyobj(L, rb, TM_LEN);
    if (l_unlikely(notm(tm))) /* no metamethod? */
      luaG_typeerror(L, rb, "get length of");
    break;
  }
  }
  luaT_callTMres(L, tm, rb, rb, ra);
}

/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
lua_Integer luaV_idiv(lua_State* L, lua_Integer m, lua_Integer n)
{
  if (l_unlikely(((lua_Unsigned)(n)) + 1u <= 1u))
  { /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to divide by zero");
    return ((lua_Integer)(((lua_Unsigned)(0)) - ((lua_Unsigned)(m)))); /* n==-1; avoid overflow with 0x80000...//-1 */
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
  if (l_unlikely(((lua_Unsigned)(n)) + 1u <= 1u))
  { /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to perform 'n%%0'");
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
constexpr int NBITS = ((int)(sizeof(lua_Integer) * CHAR_BIT));

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
      return ((lua_Integer)(((lua_Unsigned)(x)) >> ((lua_Unsigned)(-y))));
  }
  else
  { /* shift left */
    if (y >= NBITS)
      return 0;
    else
      return ((lua_Integer)(((lua_Unsigned)(x)) << ((lua_Unsigned)(y))));
  }
}

/*
** create a new Lua closure, push it in the stack, and initialize
** its upvalues.
*/
static void pushclosure(lua_State* L, Proto* p, UpVal** encup, StkId base,
                        StkId ra)
{
  int nup = p->sizeupvalues;
  Upvaldesc* uv = p->upvalues;
  int i;
  LClosure* ncl = luaF_newLclosure(L, nup);
  ncl->p = p;
  setclLvalue(L, (&(L)->stack.p[ra]), ncl); /* anchor new closure in stack */
  for (i = 0; i < nup; i++)
  {                    /* fill in its upvalues */
    if (uv[i].instack) /* upvalue refers to local variable? */
      ncl->upvals[i] = luaF_findupval(L, base + uv[i].idx);
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
  Instruction inst = *(ci->savedpc - 1); /* interrupted instruction */
  OpCode op = GET_OPCODE(inst);
  switch (op)
  { /* finish its execution */
  case OP_MMBIN:
  case OP_MMBINI:
  case OP_MMBINK:
  {
    setobj(L, (&(L)->stack.p[base + GETARG_A(*(ci->savedpc - 2))]), (&(L)->stack.p[--L->top]));
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
    setobj(L, (&(L)->stack.p[base + GETARG_A(inst)]), (&(L)->stack.p[--L->top]));
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
    int res = !((((&(L)->stack.p[L->top - 1]))->tt_ == LUA_VFALSE) || ((((&(L)->stack.p[L->top - 1]))->tt_ & NOVARIANT_MASK) == LUA_TNIL));
    L->top--;
    assert(GET_OPCODE(*ci->savedpc) == OP_JMP);
    if (res != GETARG_k(inst)) /* condition failed? */
      ci->savedpc++;       /* skip jump instruction */
    break;
  }
  case OP_CONCAT:
  {
    StkId top = L->top - 1;                   /* top when 'luaT_tryconcatTM' was called */
    int a = GETARG_A(inst);                     /* first element to concatenate */
    int total = ((int)(top - 1 - (base + a))); /* yet to concatenate */
    setobj(L, (&(L)->stack.p[top - 2]), (&(L)->stack.p[top]));                 /* put TM result in proper position */
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
  return (lua_Integer)((lua_Unsigned)a + (lua_Unsigned)b);
}
static l_forceinline lua_Integer l_subi(lua_State* L, lua_Integer a, lua_Integer b)
{
  (void) L;
  return (lua_Integer)((lua_Unsigned)a - (lua_Unsigned)b);
}
static l_forceinline lua_Integer l_muli(lua_State* L, lua_Integer a, lua_Integer b)
{
  (void) L;
  return (lua_Integer)((lua_Unsigned)a * (lua_Unsigned)b);
}
static l_forceinline lua_Integer l_band(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a & (lua_Unsigned)b);
}
static l_forceinline lua_Integer l_bor(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a | (lua_Unsigned)b);
}
static l_forceinline lua_Integer l_bxor(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a ^ (lua_Unsigned)b);
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

/*
** macro executed during Lua functions at points where the
** function can yield.
*/
#if !defined(luai_threadyield)
#define luai_threadyield(L) \
  {                         \
    lua_unlock(L);          \
    lua_lock(L);            \
  }
#endif

/* 'c' is the limit of live values in the stack */

/* fetch an instruction and prepare its execution */

#define vmdispatch(o) switch (o)
#define vmcase(l)     case l:
#define vmbreak       break

void luaV_execute(lua_State* L, CallInfo* ci)
{
  LClosure* cl;
  TValue* k;
  StkId base;
  const Instruction* pc;
  int trap;
#if LUA_USE_JUMPTABLE
#include "ljumptab.h"
#endif
  /* frame entry: this sequence is repeated inline at the call/return
     arms (OP_CALL, OP_TAILCALL, OP_RETURN*) that switch frames */
  trap = L->hookmask;
  cl = ci_func(L, ci);
  k = cl->p->k;
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
    i = *(pc++);
    assert(base == ci->func + 1);
    assert(base <= L->top && L->top <= L->stack_last);
    /* for tests, invalidate top for instructions not expecting it */
    assert(luaP_isIT(i) || (((void)(L->top = base)), 1));
    vmdispatch(GET_OPCODE(i))
    {
      vmcase(OP_MOVE)
      {
        StkId ra = (base + GETARG_A(i));
        setobj(L, (&(L)->stack.p[ra]), (&(L)->stack.p[(base + GETARG_B(i))]));
        vmbreak;
      }
      vmcase(OP_LOADI)
      {
        StkId ra = (base + GETARG_A(i));
        lua_Integer b = GETARG_sBx(i);
        setivalue((&(L)->stack.p[ra]), b);
        vmbreak;
      }
      vmcase(OP_LOADF)
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_sBx(i);
        setfltvalue((&(L)->stack.p[ra]), ((lua_Number)(b)));
        vmbreak;
      }
      vmcase(OP_LOADK)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = k + GETARG_Bx(i);
        setobj(L, (&(L)->stack.p[ra]), rb);
        vmbreak;
      }
      vmcase(OP_LOADKX)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb;
        rb = k + GETARG_Ax(*pc);
        pc++;
        setobj(L, (&(L)->stack.p[ra]), rb);
        vmbreak;
      }
      vmcase(OP_LOADFALSE)
      {
        StkId ra = (base + GETARG_A(i));
        (((&(L)->stack.p[ra]))->tt_ = LUA_VFALSE);
        vmbreak;
      }
      vmcase(OP_LFALSESKIP)
      {
        StkId ra = (base + GETARG_A(i));
        (((&(L)->stack.p[ra]))->tt_ = LUA_VFALSE);
        pc++; /* skip next instruction */
        vmbreak;
      }
      vmcase(OP_LOADTRUE)
      {
        StkId ra = (base + GETARG_A(i));
        (((&(L)->stack.p[ra]))->tt_ = LUA_VTRUE);
        vmbreak;
      }
      vmcase(OP_LOADNIL)
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_B(i);
        do
        {
          (((&(L)->stack.p[ra++]))->tt_ = LUA_VNIL);
        } while (b--);
        vmbreak;
      }
      vmcase(OP_GETUPVAL)
      {
        StkId ra = (base + GETARG_A(i));
        int b = GETARG_B(i);
        setobj(L, (&(L)->stack.p[ra]), upvalval(cl->upvals[b]));
        vmbreak;
      }
      vmcase(OP_SETUPVAL)
      {
        StkId ra = (base + GETARG_A(i));
        UpVal* uv = cl->upvals[GETARG_B(i)];
        setobj(L, upvalval(uv), (&(L)->stack.p[ra]));
        luaC_barrier(L, uv, (&(L)->stack.p[ra]));
        vmbreak;
      }
      vmcase(OP_GETTABUP)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* upval = upvalval(cl->upvals[GETARG_B(i)]);
        TValue* rc = (k + GETARG_C(i));
        assert((rc->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((rc->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        TString* key = static_cast<TString*>(rc->value_.gc); /* key must be a short string */
        lu_byte tag;
        if (upval->tt_ != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          assert(upval->value_.gc->tt == LUA_VTABLE);
          tag = luaH_getshortstr(static_cast<Table*>(upval->value_.gc), key, &L->stack.p[ra]);
        }
        if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, upval, rc, ra, tag);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_GETTABLE)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        TValue* rc = (&(L)->stack.p[(base + GETARG_C(i))]);
        lu_byte tag;
        if (((rc)->tt_ == LUA_VNUMINT))
        { /* fast track for integers? */
          if (rb->tt_ != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            assert(rb->value_.gc->tt == LUA_VTABLE);
            Table* h = static_cast<Table*>(rb->value_.gc);
            lua_Unsigned u = (lua_Unsigned)rc->value_.i - 1u;
            if (u < h->asize)
            {
              tag = *getArrTag(h, u);
              if ((tag & NOVARIANT_MASK) != LUA_TNIL)
                farr2val(h, u, tag, &L->stack.p[ra]);
            }
            else
              tag = luaH_getint(h, rc->value_.i, &L->stack.p[ra]);
          }
        }
        else if (rb->tt_ != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          assert(rb->value_.gc->tt == LUA_VTABLE);
          tag = luaH_get(static_cast<Table*>(rb->value_.gc), rc, &L->stack.p[ra]);
        }
        if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, rb, rc, ra, tag);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_GETI)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        int c = GETARG_C(i);
        lu_byte tag;
        if (rb->tt_ != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          assert(rb->value_.gc->tt == LUA_VTABLE);
          Table* h = static_cast<Table*>(rb->value_.gc);
          lua_Unsigned u = (lua_Unsigned)c - 1u;
          if (u < h->asize)
          {
            tag = *getArrTag(h, u);
            if ((tag & NOVARIANT_MASK) != LUA_TNIL)
              farr2val(h, u, tag, &L->stack.p[ra]);
          }
          else
            tag = luaH_getint(h, c, &L->stack.p[ra]);
        }
        if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        {
          TValue key;
          setivalue(&key, c);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, rb, &key, ra, tag);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_GETFIELD)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        TValue* rc = (k + GETARG_C(i));
        assert((rc->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((rc->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        TString* key = static_cast<TString*>(rc->value_.gc); /* key must be a short string */
        lu_byte tag;
        if (rb->tt_ != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          assert(rb->value_.gc->tt == LUA_VTABLE);
          tag = luaH_getshortstr(static_cast<Table*>(rb->value_.gc), key, &L->stack.p[ra]);
        }
        if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, rb, rc, ra, tag);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_SETTABUP)
      {
        int hres;
        TValue* upval = upvalval(cl->upvals[GETARG_A(i)]);
        TValue* rb = (k + GETARG_B(i));
        TValue* rc = ((TESTARG_k(i)) ? k + GETARG_C(i) : (&(L)->stack.p[base + GETARG_C(i)]));
        assert((rb->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((rb->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        TString* key = static_cast<TString*>(rb->value_.gc); /* key must be a short string */
        if (upval->tt_ != ctb(LUA_VTABLE))
          hres = HNOTATABLE;
        else
        {
          assert(upval->value_.gc->tt == LUA_VTABLE);
          hres = luaH_psetshortstr(static_cast<Table*>(upval->value_.gc), key, rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, upval, rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishset(L, upval, rb, rc, hres);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_SETTABLE)
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]); /* key (table is in 'ra') */
        TValue* rc = ((TESTARG_k(i)) ? k + GETARG_C(i) : (&(L)->stack.p[base + GETARG_C(i)])); /* value */
        if (((rb)->tt_ == LUA_VNUMINT))
        { /* fast track for integers? */
          TValue* t = (&(L)->stack.p[ra]);
          if (t->tt_ != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            assert(t->value_.gc->tt == LUA_VTABLE);
            Table* h = static_cast<Table*>(t->value_.gc);
            lua_Unsigned u = (lua_Unsigned)rb->value_.i - 1u;
            if (u < h->asize)
            {
              lu_byte* tag = getArrTag(h, u);
              if (checknoTM(h->metatable, TM_NEWINDEX) || (*tag & NOVARIANT_MASK) != LUA_TNIL)
              {
                fval2arr(h, u, tag, rc);
                hres = HOK;
              }
              else
                hres = ~(int)u;
            }
            else
              hres = luaH_psetint(h, rb->value_.i, rc);
          }
        }
        else
        {
          TValue* t = (&(L)->stack.p[ra]);
          if (t->tt_ != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            assert(t->value_.gc->tt == LUA_VTABLE);
            hres = luaH_pset(static_cast<Table*>(t->value_.gc), rb, rc);
          }
        }
        if (hres == HOK)
          luaV_finishfastset(L, (&(L)->stack.p[ra]), rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishset(L, (&(L)->stack.p[ra]), rb, rc, hres);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_SETI)
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        int b = GETARG_B(i);
        TValue* rc = ((TESTARG_k(i)) ? k + GETARG_C(i) : (&(L)->stack.p[base + GETARG_C(i)]));
        TValue* t = (&(L)->stack.p[ra]);
        if (t->tt_ != ctb(LUA_VTABLE))
          hres = HNOTATABLE;
        else
        {
          assert(t->value_.gc->tt == LUA_VTABLE);
          Table* h = static_cast<Table*>(t->value_.gc);
          lua_Unsigned u = (lua_Unsigned)b - 1u;
          if (u < h->asize)
          {
            lu_byte* tag = getArrTag(h, u);
            if (checknoTM(h->metatable, TM_NEWINDEX) || (*tag & NOVARIANT_MASK) != LUA_TNIL)
            {
              fval2arr(h, u, tag, rc);
              hres = HOK;
            }
            else
              hres = ~(int)u;
          }
          else
            hres = luaH_psetint(h, b, rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, (&(L)->stack.p[ra]), rc);
        else
        {
          TValue key;
          setivalue(&key, b);
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishset(L, (&(L)->stack.p[ra]), &key, rc, hres);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_SETFIELD)
      {
        StkId ra = (base + GETARG_A(i));
        int hres;
        TValue* rb = (k + GETARG_B(i));
        TValue* rc = ((TESTARG_k(i)) ? k + GETARG_C(i) : (&(L)->stack.p[base + GETARG_C(i)]));
        assert((rb->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((rb->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        TString* key = static_cast<TString*>(rb->value_.gc); /* key must be a short string */
        TValue* t = (&(L)->stack.p[ra]);
        if (t->tt_ != ctb(LUA_VTABLE))
          hres = HNOTATABLE;
        else
        {
          assert(t->value_.gc->tt == LUA_VTABLE);
          hres = luaH_psetshortstr(static_cast<Table*>(t->value_.gc), key, rc);
        }
        if (hres == HOK)
          luaV_finishfastset(L, (&(L)->stack.p[ra]), rc);
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishset(L, (&(L)->stack.p[ra]), rb, rc, hres);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_NEWTABLE)
      {
        StkId ra = (base + GETARG_A(i));
        unsigned b = ((unsigned int)(GETARG_vB(i))); /* log2(hash size) + 1 */
        unsigned c = ((unsigned int)(GETARG_vC(i))); /* array size */
        Table* t;
        if (b > 0)
          b = 1u << (b - 1); /* hash size is 2^(b - 1) */
        if (TESTARG_k(i))
        { /* non-zero extra argument? */
          assert(GETARG_Ax(*pc) != 0);
          /* add it to array size */
          c += ((unsigned int)(GETARG_Ax(*pc))) * (MAXARG_vC + 1);
        }
        pc++;              /* skip extra argument */
        L->top = ra + 1; /* correct top in case of emergency GC */
        t = luaH_new(L);   /* memory allocation */
        sethvalue(L, (&(L)->stack.p[ra]), t);
        if (b != 0 || c != 0)
          luaH_resize(L, t, c, b); /* idem */
        if (G(L)->GCdebt <= 0)
        {
          ci->savedpc = pc;
          L->top = ra + 1;
          luaC_step(L);
          trap = ci->trap;
        }
        condchangemem(L, (ci->savedpc = pc, L->top = ra + 1), (trap = ci->trap), 0);
        luai_threadyield(L);
        vmbreak;
      }
      vmcase(OP_SELF)
      {
        StkId ra = (base + GETARG_A(i));
        lu_byte tag;
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        TValue* rc = (k + GETARG_C(i));
        assert((rc->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
        assert((rc->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
        TString* key = static_cast<TString*>(rc->value_.gc); /* key must be a short string */
        setobj(L, (&(L)->stack.p[ra + 1]), rb);
        if (rb->tt_ != ctb(LUA_VTABLE))
          tag = LUA_VNOTABLE;
        else
        {
          assert(rb->value_.gc->tt == LUA_VTABLE);
          tag = luaH_getshortstr(static_cast<Table*>(rb->value_.gc), key, &L->stack.p[ra]);
        }
        if ((((tag) & NOVARIANT_MASK) == LUA_TNIL))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaV_finishget(L, rb, rc, ra, tag);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_ADDI)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        int imm = GETARG_sC(i);
        if (v1->tt_ == LUA_VNUMINT)
        {
          lua_Integer iv1 = v1->value_.i;
          pc++;
          setivalue(ra, l_addi(L, iv1, imm));
        }
        else if (v1->tt_ == LUA_VNUMFLT)
        {
          lua_Number nb = v1->value_.n;
          lua_Number fimm = (lua_Number)imm;
          pc++;
          setfltvalue(ra, luai_numadd(L, nb, fimm));
        }
        vmbreak;
      }
      vmcase(OP_ADDK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_addi(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numadd(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_SUBK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_subi(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numsub(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_MULK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_muli(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_nummul(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_MODK)
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], luaV_mod(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luaV_modf(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_POWK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        lua_Number n1;
        lua_Number n2;
        if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setfltvalue(&L->stack.p[ra], luai_numpow(L, n1, n2));
        }
        vmbreak;
      }
      vmcase(OP_DIVK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        lua_Number n1;
        lua_Number n2;
        if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setfltvalue(&L->stack.p[ra], luai_numdiv(L, n1, n2));
        }
        vmbreak;
      }
      vmcase(OP_IDIVK)
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], luaV_idiv(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numidiv(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_BANDK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        lua_Integer i1;
        lua_Integer i2 = v2->value_.i;
        assert(v2->tt_ == LUA_VNUMINT);
        if (tointegerns(v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_band(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_BORK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        lua_Integer i1;
        lua_Integer i2 = v2->value_.i;
        assert(v2->tt_ == LUA_VNUMINT);
        if (tointegerns(v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_bor(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_BXORK)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = k + GETARG_C(i);
        lua_Integer i1;
        lua_Integer i2 = v2->value_.i;
        assert(v2->tt_ == LUA_VNUMINT);
        if (tointegerns(v1, &i1))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_bxor(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_SHLI)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        int ic = GETARG_sC(i);
        lua_Integer ib;
        if (tointegerns(rb, &ib))
        {
          pc++;
          setivalue((&(L)->stack.p[ra]), luaV_shiftl(ic, ib));
        }
        vmbreak;
      }
      vmcase(OP_SHRI)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        int ic = GETARG_sC(i);
        lua_Integer ib;
        if (tointegerns(rb, &ib))
        {
          pc++;
          setivalue((&(L)->stack.p[ra]), luaV_shiftl(ib, -ic));
        }
        vmbreak;
      }
      vmcase(OP_ADD)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_addi(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numadd(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_SUB)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_subi(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numsub(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_MUL)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], l_muli(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_nummul(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_MOD)
      {
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], luaV_mod(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luaV_modf(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_POW)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Number n1;
        lua_Number n2;
        if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setfltvalue(&L->stack.p[ra], luai_numpow(L, n1, n2));
        }
        vmbreak;
      }
      vmcase(OP_DIV)
      { /* float division (always with floats) */
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Number n1;
        lua_Number n2;
        if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setfltvalue(&L->stack.p[ra], luai_numdiv(L, n1, n2));
        }
        vmbreak;
      }
      vmcase(OP_IDIV)
      {                   /* floor division */
        ci->savedpc = pc; /* in case of division by 0 */
        L->top = ci->top;
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        if (v1->tt_ == LUA_VNUMINT && v2->tt_ == LUA_VNUMINT)
        {
          StkId ra = base + GETARG_A(i);
          lua_Integer i1 = v1->value_.i;
          lua_Integer i2 = v2->value_.i;
          pc++;
          setivalue(&L->stack.p[ra], luaV_idiv(L, i1, i2));
        }
        else
        {
          lua_Number n1;
          lua_Number n2;
          if (tonumberns(v1, &n1) && tonumberns(v2, &n2))
          {
            StkId ra = base + GETARG_A(i);
            pc++;
            setfltvalue(&L->stack.p[ra], luai_numidiv(L, n1, n2));
          }
        }
        vmbreak;
      }
      vmcase(OP_BAND)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Integer i1;
        lua_Integer i2;
        if (tointegerns(v1, &i1) && tointegerns(v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_band(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_BOR)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Integer i1;
        lua_Integer i2;
        if (tointegerns(v1, &i1) && tointegerns(v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_bor(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_BXOR)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Integer i1;
        lua_Integer i2;
        if (tointegerns(v1, &i1) && tointegerns(v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], l_bxor(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_SHL)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Integer i1;
        lua_Integer i2;
        if (tointegerns(v1, &i1) && tointegerns(v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], luaV_shiftl(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_SHR)
      {
        TValue* v1 = &L->stack.p[base + GETARG_B(i)];
        TValue* v2 = &L->stack.p[base + GETARG_C(i)];
        lua_Integer i1;
        lua_Integer i2;
        if (tointegerns(v1, &i1) && tointegerns(v2, &i2))
        {
          StkId ra = base + GETARG_A(i);
          pc++;
          setivalue(&L->stack.p[ra], luaV_shiftr(i1, i2));
        }
        vmbreak;
      }
      vmcase(OP_MMBIN)
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = *(pc - 2); /* original arith. expression */
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        TMS tm = (TMS) GETARG_C(i);
        StkId result = (base + GETARG_A(pi));
        assert(OP_ADD <= GET_OPCODE(pi) && GET_OPCODE(pi) <= OP_SHR);
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybinTM(L, (&(L)->stack.p[ra]), rb, result, tm);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_MMBINI)
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = *(pc - 2); /* original arith. expression */
        int imm = GETARG_sB(i);
        TMS tm = (TMS) GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = (base + GETARG_A(pi));
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybiniTM(L, (&(L)->stack.p[ra]), imm, flip, result, tm);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_MMBINK)
      {
        StkId ra = (base + GETARG_A(i));
        Instruction pi = *(pc - 2); /* original arith. expression */
        TValue* imm = (k + GETARG_B(i));
        TMS tm = (TMS) GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = (base + GETARG_A(pi));
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_trybinassocTM(L, (&(L)->stack.p[ra]), imm, flip, result, tm);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_UNM)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        lua_Number nb;
        if (((rb)->tt_ == LUA_VNUMINT))
        {
          lua_Integer ib = rb->value_.i;
          setivalue((&(L)->stack.p[ra]), ((lua_Integer)(((lua_Unsigned)(0)) - ((lua_Unsigned)(ib)))));
        }
        else if ((((rb)->tt_ == LUA_VNUMFLT) ? ((nb) = (rb)->value_.n, 1) : (((rb)->tt_ == LUA_VNUMINT) ? ((nb) = ((lua_Number)((rb)->value_.i)), 1) : 0)))
        {
          setfltvalue((&(L)->stack.p[ra]), ((void)(L), -(nb)));
        }
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaT_trybinTM(L, rb, rb, ra, TM_UNM);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_BNOT)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        lua_Integer ib;
        if (tointegerns(rb, &ib))
        {
          setivalue((&(L)->stack.p[ra]), ((lua_Integer)(((lua_Unsigned)(~((lua_Unsigned)(0)))) ^ ((lua_Unsigned)(ib)))));
        }
        else
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaT_trybinTM(L, rb, rb, ra, TM_BNOT);
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_NOT)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        if ((((rb)->tt_ == LUA_VFALSE) || (((rb)->tt_ & NOVARIANT_MASK) == LUA_TNIL)))
          (((&(L)->stack.p[ra]))->tt_ = LUA_VTRUE);
        else
          (((&(L)->stack.p[ra]))->tt_ = LUA_VFALSE);
        vmbreak;
      }
      vmcase(OP_LEN)
      {
        StkId ra = (base + GETARG_A(i));
        ci->savedpc = pc;
        L->top = ci->top;
        luaV_objlen(L, ra, (&(L)->stack.p[(base + GETARG_B(i))]));
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_CONCAT)
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_B(i); /* number of elements to concatenate */
        L->top = ra + n;   /* mark the end of concat operands */
        ci->savedpc = pc;
        luaV_concat(L, n);
        trap = ci->trap;
        if (G(L)->GCdebt <= 0)
        { /* 'luaV_concat' ensures correct top */
          ci->savedpc = pc;
          luaC_step(L);
          trap = ci->trap;
        }
        condchangemem(L, (ci->savedpc = pc), (trap = ci->trap), 0);
        luai_threadyield(L);
        vmbreak;
      }
      vmcase(OP_CLOSE)
      {
        StkId ra = (base + GETARG_A(i));
        assert(!GETARG_B(i)); /* 'close must be alive */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_close(L, ra, LUA_OK, 1);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_TBC)
      {
        StkId ra = (base + GETARG_A(i));
        /* create new to-be-closed upvalue */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_newtbcupval(L, ra);
        vmbreak;
      }
      vmcase(OP_JMP)
      {
        pc += GETARG_sJ(i);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_EQ)
      {
        StkId ra = (base + GETARG_A(i));
        int cond;
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        ci->savedpc = pc;
        L->top = ci->top;
        cond = luaV_equalobj(L, (&(L)->stack.p[ra]), rb);
        trap = ci->trap;
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_LT)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        TValue* rb = &L->stack.p[base + GETARG_B(i)];
        int cond;
        if (ra->tt_ == LUA_VNUMINT && rb->tt_ == LUA_VNUMINT)
        {
          lua_Integer ia = ra->value_.i;
          lua_Integer ib = rb->value_.i;
          cond = l_lti(ia, ib);
        }
        else if ((ra->tt_ & NOVARIANT_MASK) == LUA_TNUMBER && (rb->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)
          cond = LTnum(ra, rb);
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_LE)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        TValue* rb = &L->stack.p[base + GETARG_B(i)];
        int cond;
        if (ra->tt_ == LUA_VNUMINT && rb->tt_ == LUA_VNUMINT)
        {
          lua_Integer ia = ra->value_.i;
          lua_Integer ib = rb->value_.i;
          cond = l_lei(ia, ib);
        }
        else if ((ra->tt_ & NOVARIANT_MASK) == LUA_TNUMBER && (rb->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)
          cond = LEnum(ra, rb);
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_EQK)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (k + GETARG_B(i));
        /* basic types do not use '__eq'; we can use raw equality */
        int cond = luaV_rawequalobj((&(L)->stack.p[ra]), rb);
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_EQI)
      {
        StkId ra = (base + GETARG_A(i));
        int cond;
        int im = GETARG_sB(i);
        if ((((&(L)->stack.p[ra]))->tt_ == LUA_VNUMINT))
          cond = ((((&(L)->stack.p[ra]))->value_).i == im);
        else if ((((&(L)->stack.p[ra]))->tt_ == LUA_VNUMFLT))
          cond = ((((&(L)->stack.p[ra]))->value_).n == (((lua_Number)(im))));
        else
          cond = 0; /* other types cannot be equal to a number */
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_LTI)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        int cond;
        int im = GETARG_sB(i);
        if (ra->tt_ == LUA_VNUMINT)
          cond = l_lti(ra->value_.i, im);
        else if (ra->tt_ == LUA_VNUMFLT)
        {
          lua_Number fa = ra->value_.n;
          lua_Number fim = (lua_Number)im;
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_LEI)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        int cond;
        int im = GETARG_sB(i);
        if (ra->tt_ == LUA_VNUMINT)
          cond = l_lei(ra->value_.i, im);
        else if (ra->tt_ == LUA_VNUMFLT)
        {
          lua_Number fa = ra->value_.n;
          lua_Number fim = (lua_Number)im;
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_GTI)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        int cond;
        int im = GETARG_sB(i);
        if (ra->tt_ == LUA_VNUMINT)
          cond = l_gti(ra->value_.i, im);
        else if (ra->tt_ == LUA_VNUMFLT)
        {
          lua_Number fa = ra->value_.n;
          lua_Number fim = (lua_Number)im;
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_GEI)
      {
        TValue* ra = &L->stack.p[base + GETARG_A(i)];
        int cond;
        int im = GETARG_sB(i);
        if (ra->tt_ == LUA_VNUMINT)
          cond = l_gei(ra->value_.i, im);
        else if (ra->tt_ == LUA_VNUMFLT)
        {
          lua_Number fa = ra->value_.n;
          lua_Number fim = (lua_Number)im;
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
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = (ci)->trap;
        }
        vmbreak;
      }
      vmcase(OP_TEST)
      {
        StkId ra = (base + GETARG_A(i));
        int cond = !((((&(L)->stack.p[ra]))->tt_ == LUA_VFALSE) || ((((&(L)->stack.p[ra]))->tt_ & NOVARIANT_MASK) == LUA_TNIL));
        if (cond != GETARG_k(i))
          pc++;
        else
        {
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_TESTSET)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rb = (&(L)->stack.p[(base + GETARG_B(i))]);
        if ((((rb)->tt_ == LUA_VFALSE) || (((rb)->tt_ & NOVARIANT_MASK) == LUA_TNIL)) == GETARG_k(i))
          pc++;
        else
        {
          setobj(L, (&(L)->stack.p[ra]), rb);
          Instruction ni = *pc;
          pc += GETARG_sJ(ni) + 1;
          trap = ci->trap;
        }
        vmbreak;
      }
      vmcase(OP_CALL)
      {
        StkId ra = (base + GETARG_A(i));
        CallInfo* newci;
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b != 0)          /* fixed number of arguments? */
          L->top = ra + b; /* top signals number of arguments */
        /* else previous instruction set top */
        ci->savedpc = pc; /* in case of errors */
        if ((newci = luaD_precall(L, ra, nresults)) == NULL)
          trap = ci->trap; /* C call; nothing else to be done */
        else
        { /* Lua call: run function in this same C frame */
          ci = newci;
          /* frame entry (was 'goto startfunc') */
          trap = L->hookmask;
          cl = ci_func(L, ci);
          k = cl->p->k;
          pc = ci->savedpc;
          if (l_unlikely(trap))
            trap = luaG_tracecall(L);
          base = ci->func + 1;
        }
        vmbreak;
      }
      vmcase(OP_TAILCALL)
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
        { /* execute the callee (was 'goto startfunc') */
          trap = L->hookmask;
          cl = ci_func(L, ci);
          k = cl->p->k;
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
          /* caller returns after the tail call (was 'goto ret') */
          if (ci->callstatus & CIST_FRESH)
            return; /* end this frame */
          ci = ci->previous; /* continue running caller in this frame */
          cl = ci_func(L, ci);
          k = cl->p->k;
          pc = ci->savedpc;
          if (l_unlikely(trap))
            trap = luaG_tracecall(L);
          base = ci->func + 1;
        }
        vmbreak;
      }
      vmcase(OP_RETURN)
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
        /* return from a Lua function (was 'goto ret') */
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k;
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        vmbreak;
      }
      vmcase(OP_RETURN0)
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
            (((&(L)->stack.p[L->top++]))->tt_ = LUA_VNIL); /* all results are nil */
        }
        /* return from a Lua function (was 'goto ret') */
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k;
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        vmbreak;
      }
      vmcase(OP_RETURN1)
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
            setobj(L, (&(L)->stack.p[base - 1]), (&(L)->stack.p[ra])); /* at least this result */
            L->top = base;
            for (; l_unlikely(nres > 1); nres--)
              (((&(L)->stack.p[L->top++]))->tt_ = LUA_VNIL); /* complete missing results */
          }
        }
        /* return from a Lua function */
        if (ci->callstatus & CIST_FRESH)
          return; /* end this frame */
        ci = ci->previous; /* continue running caller in this frame */
        cl = ci_func(L, ci);
        k = cl->p->k;
        pc = ci->savedpc;
        if (l_unlikely(trap))
          trap = luaG_tracecall(L);
        base = ci->func + 1;
        vmbreak;
      }
      vmcase(OP_FORLOOP)
      {
        StkId ra = (base + GETARG_A(i));
        if ((((&(L)->stack.p[ra + 1]))->tt_ == LUA_VNUMINT))
        { /* integer loop? */
          assert(((&L->stack.p[ra]))->tt_ == LUA_VNUMINT);
          lua_Unsigned count = ((lua_Unsigned)((((&L->stack.p[ra]))->value_).i));
          if (count > 0)
          { /* still more iterations? */
            assert(((&L->stack.p[ra + 1]))->tt_ == LUA_VNUMINT);
            lua_Integer step = (((&L->stack.p[ra + 1]))->value_).i;
            assert(((&L->stack.p[ra + 2]))->tt_ == LUA_VNUMINT);
            lua_Integer idx = (((&L->stack.p[ra + 2]))->value_).i;    /* control variable */
            chgivalue((&(L)->stack.p[ra]), ((lua_Integer)(count - 1))); /* update counter */
            idx = ((lua_Integer)(((lua_Unsigned)(idx)) + ((lua_Unsigned)(step))));                /* add step to index */
            chgivalue((&(L)->stack.p[ra + 2]), idx);              /* update control variable */
            pc -= GETARG_Bx(i);                       /* jump back */
          }
        }
        else if (floatforloop(L, ra)) /* float loop */
          pc -= GETARG_Bx(i);         /* jump back */
        trap = ci->trap; /* allows a signal to break the loop */
        vmbreak;
      }
      vmcase(OP_FORPREP)
      {
        StkId ra = (base + GETARG_A(i));
        ci->savedpc = pc; /* in case of errors */
        L->top = ci->top;
        if (forprep(L, ra))
          pc += GETARG_Bx(i) + 1; /* skip the loop */
        vmbreak;
      }
      vmcase(OP_TFORPREP)
      {
        /* before: 'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the initial value for the control variable, and
           'ra + 3' has the closing variable. This opcode then swaps the
           control and the closing variables and marks the closing variable
           as to-be-closed.
        */
        StkId ra = (base + GETARG_A(i));
        TValue temp; /* to swap control and closing variables */
        setobj(L, &temp, (&(L)->stack.p[ra + 3]));
        setobj(L, (&(L)->stack.p[ra + 3]), (&(L)->stack.p[ra + 2]));
        setobj(L, (&(L)->stack.p[ra + 2]), &temp);
        /* create to-be-closed upvalue (if closing var. is not nil) */
        ci->savedpc = pc;
        L->top = ci->top;
        luaF_newtbcupval(L, ra + 2);
        pc += GETARG_Bx(i); /* go to end of the loop */
        i = *(pc++);        /* fetch next instruction */
        assert(GET_OPCODE(i) == OP_TFORCALL && ra == (base + GETARG_A(i)));
        /* run the iterator call (was 'goto l_tforcall'):
           'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the closing variable, and 'ra + 3' has the control
           variable. The call will use the stack starting at 'ra + 3',
           so that it preserves the first three values, and the first
           return will be the new value for the control variable.
        */
        ra = (base + GETARG_A(i));
        setobj(L, (&(L)->stack.p[ra + 5]), (&(L)->stack.p[ra + 3])); /* copy the control variable */
        setobj(L, (&(L)->stack.p[ra + 4]), (&(L)->stack.p[ra + 1])); /* copy state */
        setobj(L, (&(L)->stack.p[ra + 3]), (&(L)->stack.p[ra]));     /* copy function */
        L->top = ra + 3 + 3;
        ci->savedpc = pc;
        luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
        trap = ci->trap;
        { if (l_unlikely(trap)) { (base = (ci)->func + 1); ra = (base + GETARG_A(i)); } };                              /* stack may have changed */
        i = *(pc++);                                  /* go to next instruction */
        assert(GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i)));
        /* close the loop iteration (was 'goto l_tforloop') */
        ra = (base + GETARG_A(i));
        if (!((((&(L)->stack.p[ra + 3]))->tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        vmbreak;
      }
      vmcase(OP_TFORCALL)
      {
        /* 'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the closing variable, and 'ra + 3' has the control
           variable. The call will use the stack starting at 'ra + 3',
           so that it preserves the first three values, and the first
           return will be the new value for the control variable.
        */
        StkId ra = (base + GETARG_A(i));
        setobj(L, (&(L)->stack.p[ra + 5]), (&(L)->stack.p[ra + 3])); /* copy the control variable */
        setobj(L, (&(L)->stack.p[ra + 4]), (&(L)->stack.p[ra + 1])); /* copy state */
        setobj(L, (&(L)->stack.p[ra + 3]), (&(L)->stack.p[ra]));     /* copy function */
        L->top = ra + 3 + 3;
        ci->savedpc = pc;
        luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
        trap = ci->trap;
        { if (l_unlikely(trap)) { (base = (ci)->func + 1); ra = (base + GETARG_A(i)); } };                              /* stack may have changed */
        i = *(pc++);                                  /* go to next instruction */
        assert(GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i)));
        /* close the loop iteration (was 'goto l_tforloop') */
        ra = (base + GETARG_A(i));
        if (!((((&(L)->stack.p[ra + 3]))->tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        vmbreak;
      }
      vmcase(OP_TFORLOOP)
      {
        StkId ra = (base + GETARG_A(i));
        if (!((((&(L)->stack.p[ra + 3]))->tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* continue loop? */
          pc -= GETARG_Bx(i);      /* jump back */
        vmbreak;
      }
      vmcase(OP_SETLIST)
      {
        StkId ra = (base + GETARG_A(i));
        unsigned n = ((unsigned int)(GETARG_vB(i)));
        unsigned last = ((unsigned int)(GETARG_vC(i)));
        assert((&L->stack.p[ra])->tt_ == ctb(LUA_VTABLE));
        assert((&L->stack.p[ra])->value_.gc->tt == LUA_VTABLE);
        Table* h = static_cast<Table*>((&L->stack.p[ra])->value_.gc);
        if (n == 0)
          n = ((unsigned int)(L->top - ra)) - 1; /* get up to the top */
        else
          L->top = ci->top; /* correct top in case of emergency GC */
        last += n;
        if (TESTARG_k(i))
        {
          last += ((unsigned int)(GETARG_Ax(*pc))) * (MAXARG_vC + 1);
          pc++;
        }
        /* when 'n' is known, table should have proper size */
        if (last > h->asize)
        { /* needs more space? */
          /* fixed-size sets should have space preallocated */
          assert(GETARG_vB(i) == 0);
          luaH_resizearray(L, h, last); /* preallocate it at once */
        }
        for (; n > 0; n--)
        {
          TValue* val = (&(L)->stack.p[ra + n]);
          obj2arr(h, last - 1, val);
          last--;
          assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
          luaC_barrierback(L, const_cast<GCObject*>(static_cast<const GCObject*>(h)), val);
        }
        vmbreak;
      }
      vmcase(OP_CLOSURE)
      {
        StkId ra = (base + GETARG_A(i));
        Proto* p = cl->p->p[GETARG_Bx(i)];
        ci->savedpc = pc;
        L->top = ci->top;
        pushclosure(L, p, cl->upvals, base, ra);
        if (G(L)->GCdebt <= 0)
        {
          ci->savedpc = pc;
          L->top = ra + 1;
          luaC_step(L);
          trap = ci->trap;
        }
        condchangemem(L, (ci->savedpc = pc, L->top = ra + 1), (trap = ci->trap), 0);
        luai_threadyield(L);
        vmbreak;
      }
      vmcase(OP_VARARG)
      {
        StkId ra = (base + GETARG_A(i));
        int n = GETARG_C(i) - 1; /* required results (-1 means all) */
        int vatab = GETARG_k(i) ? GETARG_B(i) : -1;
        ci->savedpc = pc;
        L->top = ci->top;
        luaT_getvarargs(L, ci, ra, n, vatab);
        trap = ci->trap;
        vmbreak;
      }
      vmcase(OP_GETVARG)
      {
        StkId ra = (base + GETARG_A(i));
        TValue* rc = (&(L)->stack.p[(base + GETARG_C(i))]);
        luaT_getvararg(L, ci, ra, rc);
        vmbreak;
      }
      vmcase(OP_ERRNNIL)
      {
        TValue* ra = (&(L)->stack.p[(base + GETARG_A(i))]);
        if (!(((ra)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
        {
          ci->savedpc = pc;
          L->top = ci->top;
          luaG_errnnil(L, cl, GETARG_Bx(i));
        }
        vmbreak;
      }
      vmcase(OP_VARARGPREP)
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
        vmbreak;
      }
      vmcase(OP_EXTRAARG)
      {
        assert(0);
        vmbreak;
      }
    }
  }
}

/* }================================================================== */
