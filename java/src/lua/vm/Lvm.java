package lua.vm;

import static lua.api.LuaApi.*;
import static lua.api.LuaConf.*;
import static lua.gc.Lgc.*;
import static lua.object.Lfunc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.object.Lstring.*;
import static lua.object.Ltable.*;
import static lua.platform.Libm.*;
import static lua.platform.OsApi.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lstate.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Ldebug.*;
import static lua.vm.Lopcodes.*;

import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.Lt;
import lua.object.LClosure;
import lua.object.Proto;
import lua.object.TString;
import lua.object.TValue;
import lua.object.Table;
import lua.object.Udata;
import lua.object.UpVal;
import lua.object.Upvaldesc;
import lua.runtime.CallInfo;
import lua.runtime.Cost;
import lua.runtime.lua_State;

public final class Lvm
{
  private Lvm()
  {
  }

  public static int cvt2str(TValue o)
  {
    return (ttype(o) == LUA_TNUMBER) ? 1 : 0;
  }

  public static int cvt2str(lua_State L, int idx)
  {
    TValue v = getslot(L, idx);
    return cvt2str(v);
  }

  public static int cvt2num(TValue o)
  {
    return (ttype(o) == LUA_TSTRING) ? 1 : 0;
  }

  /*
  ** Rounding modes for float->integer coercion
  */
  public static final int F2Ieq = 0;    /* no rounding; accepts only integral values */
  public static final int F2Ifloor = 1; /* takes the floor of the number */
  public static final int F2Iceil = 2;  /* takes the ceiling of the number */

  public static final int LUA_FLOORN2I = F2Ieq;

  /* convert an object to a float (including string coercion) */

  /* convert an object to a float (without string coercion) */

  /* convert an object to an integer (including string coercion) */

  /* convert an object to an integer (without string coercion) */

  /*
  ** fast track for 'gettable'
  */

  /*
  ** Special case of 'luaV_fastget' for integers, inlining the fast case
  ** of 'luaH_getint'.
  */

  /*
  ** Finish a fast set operation (when fast set succeeds).
  */

  /*
  ** Shift right is the same as shift left with a negative 'y'
  */

  public static int tonumber(TValue o, NumResult nout)
  {
    if (ttisfloat(o))
    {
      nout.v = fltvalue(o);
      return 1;
    }
    return luaV_tonumber_(o, nout);
  }

  public static int tonumberns(TValue o, NumResult nout)
  {
    if (ttisfloat(o))
    {
      nout.v = fltvalue(o);
      return 1;
    }
    if (ttisinteger(o))
    {
      nout.v = (double) ivalue(o);
      return 1;
    }
    return 0;
  }

  public static int tointeger(TValue o, IntResult iout)
  {
    if (ttisinteger(o))
    {
      iout.v = ivalue(o);
      return 1;
    }
    return luaV_tointeger(o, iout, LUA_FLOORN2I);
  }

  public static int tointegerns(TValue o, IntResult iout)
  {
    if (ttisinteger(o))
    {
      iout.v = ivalue(o);
      return 1;
    }
    return luaV_tointegerns(o, iout, LUA_FLOORN2I);
  }

  public static int tonumber(lua_State L, int idx, NumResult nout)
  {
    TValue v = getslot(L, idx);
    return tonumber(v, nout);
  }

  public static int tonumberns(lua_State L, int idx, NumResult nout)
  {
    TValue v = getslot(L, idx);
    return tonumberns(v, nout);
  }

  public static int tointegerns(lua_State L, int idx, IntResult iout)
  {
    TValue v = getslot(L, idx);
    return tointegerns(v, iout);
  }

  public static int luaV_rawequalobj(TValue t1, TValue t2)
  {
    return luaV_equalobj(null, t1, t2);
  }

  public static int luaV_rawequalobj(lua_State L, int t1, TValue t2)
  {
    TValue v1 = getslot(L, t1);
    return luaV_equalobj(null, v1, t2);
  }

  public static void luaV_finishfastset(lua_State L, TValue t, TValue v)
  {
    assert iscollectable(t);
    luaC_barrierback(L, gcvalue(t), v);
  }

  public static void luaV_finishfastset(lua_State L, int t, TValue v)
  {
    TValue tv = getslot(L, t);
    luaV_finishfastset(L, tv, v);
  }

  public static long luaV_shiftr(long x, long y)
  {
    return luaV_shiftl(x, l_intunm(y));
  }

  /* limit for table tag-method chains (to avoid infinite loops) */
  private static final int MAXTAGLOOP = 2000;

  /*
  ** 'l_intfitsf' checks whether a given integer is in the range that
  ** can be converted to a float without rounding. Used in comparisons.
  */

  /* number of bits in the mantissa of a float */
  private static final int NBM = Double.PRECISION;


  /* limit for integers that fit in a float */
  private static final long MAXINTFITSF = (long) 1 << NBM;

  /* check whether 'i' is in the interval [-MAXINTFITSF, MAXINTFITSF] */
  private static int l_intfitsf(long i)
  {
    return (-(long) MAXINTFITSF <= i && i <= (long) MAXINTFITSF) ? 1 : 0;
  }

  /*
  ** Try to convert a value from string to a number value.
  ** If the value is not a string or is a string not representing
  ** a valid numeral (or if coercions from strings to numbers
  ** are disabled via macro 'cvt2num'), do not modify 'result'
  ** and return 0.
  */
  private static int l_strton(TValue obj, TValue result)
  {
    assert obj != result;
    if (cvt2num(obj) == 0) /* is object not a string? */
      return 0;
    else
    {
      assert ttisstring(obj);
      TString st = tsvalue(obj);
      return (luaO_str2num(st.data.view(), result) == st.data.size() + 1) ? 1 : 0;
    }
  }

  /*
  ** Try to convert a value to a float. The float case is already handled
  ** by the macro 'tonumber'.
  */
  public static int luaV_tonumber_(TValue obj, NumResult n)
  {
    TValue v = new TValue();
    if (ttisinteger(obj))
    {
      n.v = (double) ivalue(obj);
      return 1;
    }
    else if (l_strton(obj, v) != 0)
    {                  /* string coercible to number? */
      /* convert result of 'luaO_str2num' to a float */
      assert ttisnumber(v);
      n.v = ttisinteger(v) ? (double) ivalue(v) : fltvalue(v);
      return 1;
    }
    else
      return 0; /* conversion failed */
  }

  /*
  ** try to convert a float to an integer, rounding according to 'mode'.
  */
  public static int luaV_flttointeger(double n, IntResult p, int mode)
  {
    double f = floor(n);
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
  public static int luaV_tointegerns(TValue obj, IntResult p, int mode)
  {
    if (ttisfloat(obj))
      return luaV_flttointeger(fltvalue(obj), p, mode);
    else if (ttisinteger(obj))
    {
      p.v = ivalue(obj);
      return 1;
    }
    else
      return 0;
  }

  /*
  ** try to convert a value to an integer.
  */
  public static int luaV_tointeger(TValue obj, IntResult p, int mode)
  {
    TValue v = new TValue();
    if (l_strton(obj, v) != 0) /* does 'obj' point to a numerical string? */
      obj = v;                  /* change it to point to its corresponding number */
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
  private static int forlimit(lua_State L, long init, int limslot,
                              IntResult p, long step)
  {
    TValue limv = getslot(L, limslot);
    TValue lim = limv;
    if (luaV_tointeger(lim, p, (step < 0 ? F2Iceil : F2Ifloor)) == 0)
    {
      /* not coercible to in integer */
      NumResult flim = new NumResult();           /* try to convert to float */
      if (tonumber(lim, flim) == 0) /* cannot convert to float? */
        luaG_forerror(L, lim, Lt.limit);
      /* else 'flim' is a float out of integer bounds */
      if (0 < flim.v)
      { /* if it is positive, it is too large */
        if (step < 0)
          return 1;          /* initial value must be less than it */
        p.v = LUA_MAXINTEGER; /* truncate */
      }
      else
      { /* it is less than min integer */
        if (step > 0)
          return 1;          /* initial value must be greater than it */
        p.v = LUA_MININTEGER; /* truncate */
      }
    }
    return (step > 0 ? init > p.v : init < p.v) ? 1 : 0; /* not to run? */
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
  private static int forprep(lua_State L, int ra)
  {
    int pinit = ra;
    int plimit = ra + 1;
    int pstep = ra + 2;
    if ((slottag(L, pinit) == LUA_VNUMINT) && (slottag(L, pstep) == LUA_VNUMINT))
    { /* integer loop? */
      long init = slotival(L, pinit);
      long step = slotival(L, pstep);
      IntResult limit = new IntResult();
      if (step == 0)
        luaG_runerror(L, Lt.for_step_is_zero);
      if (forlimit(L, init, plimit, limit, step) != 0)
        return 1; /* skip the loop */
      else
      { /* prepare loop counter */
        long count;
        if (step > 0)
        { /* ascending loop? */
          count = limit.v - init;
          if (step != 1) /* avoid division in the too common case */
            count = Long.divideUnsigned(count, step);
        }
        else
        { /* step < 0; descending loop */
          count = init - limit.v;
          /* 'step+1' avoids negating 'mininteger' */
          count = Long.divideUnsigned(count, (-(step + 1)) + 1L);
        }
        /* use 'chgivalue' for places that for sure had integers */
        chgsloti(L, ra, count); /* change init to count */
        setsloti(L, ra + 1, step);         /* change limit to step */
        chgsloti(L, ra + 2, init);         /* change step to init */
      }
    }
    else
    { /* try making all values floats */
      NumResult init = new NumResult();
      NumResult limit = new NumResult();
      NumResult step = new NumResult();
      if (tonumber(L, plimit, limit) == 0)
        luaG_forerror(L, plimit, Lt.limit);
      if (tonumber(L, pstep, step) == 0)
        luaG_forerror(L, pstep, Lt.step);
      if (tonumber(L, pinit, init) == 0)
        luaG_forerror(L, pinit, Lt.initial_value);
      if (step.v == 0)
        luaG_runerror(L, Lt.for_step_is_zero);
      if ((0 < step.v) ? (limit.v < init.v)
                              : (init.v < limit.v))
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
  private static int floatforloop(lua_State L, int ra)
  {
    double step = slotfltval(L, ra + 1);
    assert slottag(L, ra) == LUA_VNUMFLT;
    double limit = slotfltval(L, ra);
    assert slottag(L, ra + 2) == LUA_VNUMFLT;
    double idx = slotfltval(L, ra + 2); /* control variable */
    idx = (idx) + step;        /* increment index */
    if ((0 < step) ? (idx <= limit)
                            : (limit <= idx))
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
  public static byte luaV_finishget(lua_State L, TValue t, TValue key,
                                    int val, byte tag, int tsrc)
  {
    int loop;         /* counter to avoid infinite loops */
    TValue tm; /* metamethod */
    TValue res = new TValue();
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
      if (tag == LUA_VNOTABLE)
      { /* 't' is not a table? */
        assert !ttistable(t);
        tm = luaT_gettmbyobj(L, t, TM_INDEX);
        if (notm(tm) != 0)
          luaG_typeerror(L, t, Lt.index_kw, tsrc); /* no metamethod */
        /* else will try the metamethod */
      }
      else
      {                                                 /* 't' is a table */
        Table h = hvalue(t);
        tm = gfasttm(L.l_G, h.metatable, TM_INDEX); /* table's metamethod */
        if (tm == null)
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
        tag = hvalue(t).luaH_get(key, res);
      }
      if (!tagisempty(tag))
      {
        setslot(L, val, res);
        return tag; /* done */
      }
      /* else repeat (tail call 'luaV_finishget') */
    }
    luaG_runerror(L, Lt.index_chain_too_long_possible_loop);
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
  public static void luaV_finishset(lua_State L, TValue t, TValue key,
                                    TValue val, int hres, int tsrc)
  {
    int loop; /* counter to avoid infinite loops */
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
      TValue tm; /* '__newindex' metamethod */
      if (hres != HNOTATABLE)
      {                                            /* is 't' a table? */
        Table h = hvalue(t);                      /* save 't' table */
        tm = gfasttm(L.l_G, h.metatable, TM_NEWINDEX); /* get metamethod */
        if (tm == null)
        {                                       /* no metamethod? */
          setsloth(L, L.top, h);          /* anchor 't' */
          L.top++;                           /* assume EXTRA_STACK */
          h.luaH_finishset(L, key, val, hres); /* set new value */
          L.top--;
          invalidateTMcache(h);
          assert novariant(h.tt) >= LUA_TSTRING;
          luaC_barrierback(L, h, val);
          return;
        }
        /* else will try the metamethod */
      }
      else
      { /* not a table; check metamethod */
        tm = luaT_gettmbyobj(L, t, TM_NEWINDEX);
        if (notm(tm) != 0)
          luaG_typeerror(L, t, Lt.index_kw, tsrc);
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
        hres = hvalue(t).luaH_pset(key, val);
      }
      if (hres == HOK)
      {
        luaV_finishfastset(L, t, val);
        return; /* done */
      }
      /* else 'return luaV_finishset(L, t, key, val, slot)' (loop) */
    }
    luaG_runerror(L, Lt.newindex_chain_too_long_possible_loop);
  }

  private static int l_strcmp(TString ts1, TString ts2)
  {
    ByteView s1 = ts1.data.view();
    ByteView s2 = ts2.data.view();
    for (;;)
    {
      int zl1 = s1.find('\0');
      int zl2 = s2.find('\0');
      if (zl1 == ByteView.npos) zl1 = s1.size();
      if (zl2 == ByteView.npos) zl2 = s2.size();
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
  private static int LTintfloat(long i, double f)
  {
    if (l_intfitsf(i) != 0)
      return ((double) i < f) ? 1 : 0; /* compare them as floats */
    else
    { /* i < f <=> i < ceil(f) */
      IntResult fi = new IntResult();
      if (luaV_flttointeger(f, fi, F2Iceil) != 0) /* fi = ceil(f) */
        return (i < fi.v) ? 1 : 0;                /* compare them as integers */
      else                                        /* 'f' is either greater or less than all integers */
        return (f > 0) ? 1 : 0;                   /* greater? */
    }
  }

  /*
  ** Check whether integer 'i' is less than or equal to float 'f'.
  ** See comments on previous function.
  */
  private static int LEintfloat(long i, double f)
  {
    if (l_intfitsf(i) != 0)
      return ((double) i <= f) ? 1 : 0; /* compare them as floats */
    else
    { /* i <= f <=> i <= floor(f) */
      IntResult fi = new IntResult();
      if (luaV_flttointeger(f, fi, F2Ifloor) != 0) /* fi = floor(f) */
        return (i <= fi.v) ? 1 : 0;                /* compare them as integers */
      else                                         /* 'f' is either greater or less than all integers */
        return (f > 0) ? 1 : 0;                    /* greater? */
    }
  }

  /*
  ** Check whether float 'f' is less than integer 'i'.
  ** See comments on previous function.
  */
  private static int LTfloatint(double f, long i)
  {
    if (l_intfitsf(i) != 0)
      return (f < (double) i) ? 1 : 0; /* compare them as floats */
    else
    { /* f < i <=> floor(f) < i */
      IntResult fi = new IntResult();
      if (luaV_flttointeger(f, fi, F2Ifloor) != 0) /* fi = floor(f) */
        return (fi.v < i) ? 1 : 0;                 /* compare them as integers */
      else                                         /* 'f' is either greater or less than all integers */
        return (f < 0) ? 1 : 0;                    /* less? */
    }
  }

  /*
  ** Check whether float 'f' is less than or equal to integer 'i'.
  ** See comments on previous function.
  */
  private static int LEfloatint(double f, long i)
  {
    if (l_intfitsf(i) != 0)
      return (f <= (double) i) ? 1 : 0; /* compare them as floats */
    else
    { /* f <= i <=> ceil(f) <= i */
      IntResult fi = new IntResult();
      if (luaV_flttointeger(f, fi, F2Iceil) != 0) /* fi = ceil(f) */
        return (fi.v <= i) ? 1 : 0;               /* compare them as integers */
      else                                        /* 'f' is either greater or less than all integers */
        return (f < 0) ? 1 : 0;                   /* less? */
    }
  }

  /*
  ** Return 'l < r', for numbers.
  */
  private static int LTnum(TValue l, TValue r)
  {
    assert ttisnumber(l) && ttisnumber(r);
    if (ttisinteger(l))
    {
      long li = ivalue(l);
      if (ttisinteger(r))
        return (li < ivalue(r)) ? 1 : 0; /* both are integers */
      else
      { /* 'l' is int and 'r' is float */
        assert ttisfloat(r);
        return LTintfloat(li, fltvalue(r)); /* l < r ? */
      }
    }
    else
    {
      assert ttisfloat(l);
      double lf = fltvalue(l); /* 'l' must be float */
      if (ttisfloat(r))
        return (lf < fltvalue(r)) ? 1 : 0; /* both are float */
      else
      { /* 'l' is float and 'r' is int */
        assert ttisinteger(r);
        return LTfloatint(lf, ivalue(r));
      }
    }
  }

  /*
  ** Return 'l <= r', for numbers.
  */
  private static int LEnum(TValue l, TValue r)
  {
    assert ttisnumber(l) && ttisnumber(r);
    if (ttisinteger(l))
    {
      long li = ivalue(l);
      if (ttisinteger(r))
        return (li <= ivalue(r)) ? 1 : 0; /* both are integers */
      else
      { /* 'l' is int and 'r' is float */
        assert ttisfloat(r);
        return LEintfloat(li, fltvalue(r)); /* l <= r ? */
      }
    }
    else
    {
      assert ttisfloat(l);
      double lf = fltvalue(l); /* 'l' must be float */
      if (ttisfloat(r))
        return (lf <= fltvalue(r)) ? 1 : 0; /* both are float */
      else
      { /* 'l' is float and 'r' is int */
        assert ttisinteger(r);
        return LEfloatint(lf, ivalue(r));
      }
    }
  }

  /*
  ** return 'l < r' for non-numbers.
  */
  private static int lessthanothers(lua_State L, TValue l, TValue r)
  {
    assert !ttisnumber(l) || !ttisnumber(r);
    if (ttisstring(l) && ttisstring(r)) /* both are strings? */
    {
      return (l_strcmp(tsvalue(l), tsvalue(r)) < 0) ? 1 : 0;
    }
    else
      return luaT_callorderTM(L, l, r, TM_LT);
  }

  /*
  ** Main operation less than; return 'l < r'.
  */
  public static int luaV_lessthan(lua_State L, TValue l, TValue r)
  {
    if (ttisnumber(l) && ttisnumber(r)) /* both operands are numbers? */
      return LTnum(l, r);
    else
      return lessthanothers(L, l, r);
  }

  /*
  ** return 'l <= r' for non-numbers.
  */
  private static int lessequalothers(lua_State L, TValue l, TValue r)
  {
    assert !ttisnumber(l) || !ttisnumber(r);
    if (ttisstring(l) && ttisstring(r)) /* both are strings? */
    {
      return (l_strcmp(tsvalue(l), tsvalue(r)) <= 0) ? 1 : 0;
    }
    else
      return luaT_callorderTM(L, l, r, TM_LE);
  }

  private static int LTnum(lua_State L, int l, int r)
  {
    TValue lv = getslot(L, l);
    TValue rv = getslot(L, r);
    return LTnum(lv, rv);
  }

  private static int LEnum(lua_State L, int l, int r)
  {
    TValue lv = getslot(L, l);
    TValue rv = getslot(L, r);
    return LEnum(lv, rv);
  }

  private static int lessthanothers(lua_State L, int l, int r)
  {
    TValue lv = getslot(L, l);
    TValue rv = getslot(L, r);
    return lessthanothers(L, lv, rv);
  }

  private static int lessequalothers(lua_State L, int l, int r)
  {
    TValue lv = getslot(L, l);
    TValue rv = getslot(L, r);
    return lessequalothers(L, lv, rv);
  }

  /*
  ** Main operation less than or equal to; return 'l <= r'.
  */
  public static int luaV_lessequal(lua_State L, TValue l, TValue r)
  {
    if (ttisnumber(l) && ttisnumber(r)) /* both operands are numbers? */
      return LEnum(l, r);
    else
      return lessequalothers(L, l, r);
  }

  public static int luaV_equalobj(lua_State L, TValue t1, TValue t2)
  {
    TValue tm;
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
        IntResult i2 = new IntResult();
        assert ttisfloat(t2);
        return (luaV_flttointeger(fltvalue(t2), i2, F2Ieq) != 0 && ivalue(t1) == i2.v) ? 1 : 0;
      }
      case LUA_VNUMFLT:
      {                 /* float == integer? */
        IntResult i1 = new IntResult(); /* see comment in previous case */
        assert ttisfloat(t1);
        assert ttisinteger(t2);
        return (luaV_flttointeger(fltvalue(t1), i1, F2Ieq) != 0 && i1.v == ivalue(t2)) ? 1 : 0;
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
        assert ttisinteger(t2);
        return (ivalue(t1) == ivalue(t2)) ? 1 : 0;
      case LUA_VNUMFLT:
        assert ttisfloat(t2);
        return (fltvalue(t1) == fltvalue(t2)) ? 1 : 0;
      case LUA_VLIGHTUSERDATA:
        assert ttislightuserdata(t2);
        return (pvalue(t1) == pvalue(t2)) ? 1 : 0;
      case LUA_VSHRSTR:
      {
        return luaS_eqstr(tsvalue(t1), tsvalue(t2));
      }
      case LUA_VUSERDATA:
      {
        Udata u1 = uvalue(t1);
        Udata u2 = uvalue(t2);
        if (u1 == u2)
          return 1;
        else if (L == null)
          return 0;
        tm = gfasttm(L.l_G, u1.metatable, TM_EQ);
        if (tm == null)
          tm = gfasttm(L.l_G, u2.metatable, TM_EQ);
        break; /* will try TM */
      }
      case LUA_VTABLE:
      {
        Table h1 = hvalue(t1);
        Table h2 = hvalue(t2);
        if (h1 == h2)
          return 1;
        else if (L == null)
          return 0;
        tm = gfasttm(L.l_G, h1.metatable, TM_EQ);
        if (tm == null)
          tm = gfasttm(L.l_G, h2.metatable, TM_EQ);
        break; /* will try TM */
      }
      default: /* functions, threads, and light C functions */
        assert iscollectable(t1);
        assert iscollectable(t2);
        return (gcvalue(t1) == gcvalue(t2)) ? 1 : 0;
      }
      if (tm == null) /* no TM? */
        return 0;     /* objects are different */
      else
      {
        int tag = luaT_callTMres(L, tm, t1, t2, L.top); /* call TM */
        return !(tag == LUA_VFALSE || tagisempty(tag)) ? 1 : 0;
      }
    }
  }

  public static int luaV_equalobj(lua_State L, int t1, int t2)
  {
    TValue v1 = getslot(L, t1);
    TValue v2 = getslot(L, t2);
    return luaV_equalobj(L, v1, v2);
  }

  /* macro used by 'luaV_concat' to ensure that element at 'o' is a string */

  /*
  ** Check whether object is a short empty string to optimize concatenation.
  ** (External strings can be empty too; they will be concatenated like
  ** non-empty ones.)
  */
  private static int isemptystr(TValue o)
  {
    return (ttisstring(o) &&
           tsslen(tsvalue(o)) == 0) ? 1 : 0;
  }

  private static int isemptystr(lua_State L, int idx)
  {
    TValue v = getslot(L, idx);
    return isemptystr(v);
  }

  private static int tostring(lua_State L, int idx)
  {
    if (novariant(slottag(L, idx)) == LUA_TSTRING)
      return 1;
    TValue v = getslot(L, idx);
    if (cvt2str(v) == 0)
      return 0;
    luaO_tostring(L, v);
    setslot(L, idx, v);
    return 1;
  }

  /* copy strings in stack from top - n up to top - 1 to buffer */
  private static void copy2buff(lua_State L, int top, int n, ByteSpan buff)
  {
    int tl = 0; /* size already copied */
    do
    {
      int v = top - n;
      assert novariant(slottag(L, v)) == LUA_TSTRING;
      TString st = slottsval(L, v);
      ByteView s = st.data.view();
      buff.copy_from(s, tl);
      tl += s.size();
    } while (--n > 0);
  }

  public static void luaV_concat(lua_State L, int total)
  {
    if (total == 1)
      return; /* "all" values already concatenated */
    do
    {
      int top = L.top;
      int n = 2; /* number of elements handled in this pass (at least 2) */
      int v1 = top - 2;
      int v2 = top - 1;
      if (!((novariant(slottag(L, v1)) == LUA_TSTRING) || cvt2str(L, v1) != 0) || tostring(L, v2) == 0)
        luaT_tryconcatTM(L);                  /* may invalidate 'top' */
      else if (isemptystr(L, v2) != 0)           /* second operand is empty? */
        tostring(L, v1);                      /* result is first operand */
      else if (isemptystr(L, v1) != 0)
      {                                 /* first operand is empty string? */
        copyslot(L, v1, v2); /* result is second op. */
      }
      else
      {
        /* at least two string values; get as many as possible */
        int tl = tsslen(slottsval(L, v2)); /* total length */
        TString ts;
        /* collect total length and number of strings */
        for (n = 1; n < total && tostring(L, top - n - 1) != 0; n++)
        {
          int o = top - n - 1;
          assert novariant(slottag(L, o)) == LUA_TSTRING;
          int l = tsslen(slottsval(L, o));
          if (l >= MAX_SIZE - Cost.TString - tl)
          {
            L.top = top - total; /* pop strings to avoid wasting stack */
            luaG_runerror(L, Lt.string_length_overflow);
          }
          tl += l;
        }
        if (tl <= 40)
        {
          ByteOwner buff = new ByteOwner(tl);
          copy2buff(L, top, n, buff.span());
          ts = TString.luaS_createstr(L, buff.view());
        }
        else
        { /* long string; copy strings directly to final result */
          ts = TString.luaS_createlngstrobj(L, tl);
          copy2buff(L, top, n, ts.data.area());
        }
        setslots(L, top - n, ts); /* create result */
      }
      total -= n - 1;    /* got 'n' strings to create one new */
      L.top -= n - 1; /* popped 'n' strings and pushed one */
    } while (total > 1); /* repeat until only 1 result left */
  }

  /*
  ** Main operation 'ra = #rb'.
  */
  public static void luaV_objlen(lua_State L, int ra, TValue rb, int src)
  {
    TValue tm;
    switch (ttypetag(rb))
    {
    case LUA_VTABLE:
    {
      Table h = hvalue(rb);
      tm = gfasttm(L.l_G, h.metatable, TM_LEN);
      if (tm != null)
        break;                                        /* metamethod? break switch to call it */
      setsloti(L, ra, h.luaH_getn(L)); /* else primitive len */
      return;
    }
    case LUA_VSHRSTR:
    {
      TString ts = tsvalue(rb);
      setsloti(L, ra, tsslen(ts));
      return;
    }
    default:
    { /* try metamethod */
      tm = luaT_gettmbyobj(L, rb, TM_LEN);
      if (notm(tm) != 0) /* no metamethod? */
        luaG_typeerror(L, rb, Lt.get_length_of, src);
      break;
    }
    }
    luaT_callTMres(L, tm, rb, rb, ra);
  }

  public static void luaV_objlen(lua_State L, int ra, int rb)
  {
    TValue v = getslot(L, rb);
    luaV_objlen(L, ra, v, rb);
  }

  /*
  ** Integer division; return 'm // n', that is, floor(m/n).
  ** C division truncates its result (rounds towards zero).
  ** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
  ** otherwise 'floor(q) == trunc(q) - 1'.
  */
  public static long luaV_idiv(lua_State L, long m, long n)
  {
    if (n == 0 || n == -1)
    { /* special cases: -1 or 0 */
      if (n == 0)
        luaG_runerror(L, Lt.attempt_to_divide_by_zero);
      return l_intunm(m); /* n==-1; avoid overflow with 0x80000...//-1 */
    }
    else
    {
      long q = m / n;         /* perform C division */
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
  public static long luaV_mod(lua_State L, long m, long n)
  {
    if (n == 0 || n == -1)
    { /* special cases: -1 or 0 */
      if (n == 0)
        luaG_runerror(L, Lt.attempt_to_perform_n_0);
      return 0; /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
    }
    else
    {
      long r = m % n;
      if (r != 0 && (r ^ n) < 0) /* 'm/n' would be non-integer negative? */
        r += n;                  /* correct result for different rounding */
      return r;
    }
  }

  /*
  ** Float modulus
  */
  public static double luaV_modf(lua_State L, double m, double n)
  {
    double r;
    r = luai_nummod(L, m, n);
    return r;
  }

  /* number of bits in an integer */
  private static final int NBITS = 64;
  static
  {
    assert NBITS == Long.SIZE;
  }

  /*
  ** Shift left operation. (Shift right just negates 'y'.)
  */
  public static long luaV_shiftl(long x, long y)
  {
    if (y < 0)
    { /* shift right? */
      if (y <= -NBITS)
        return 0;
      else
        return x >>> (-y);
    }
    else
    { /* shift left */
      if (y >= NBITS)
        return 0;
      else
        return x << y;
    }
  }

  /*
  ** create a new Lua closure, push it in the stack, and initialize
  ** its upvalues.
  */
  private static void pushclosure(lua_State L, Proto p, UpVal[] encup, int base,
                                  int ra)
  {
    int nup = (int) p.upvalues.length;
    Upvaldesc[] uv = p.upvalues;
    int i;
    LClosure ncl = LClosure.luaF_newLclosure(L, nup);
    ncl.p = p;
    setslotclL(L, ra, ncl); /* anchor new closure in stack */
    for (i = 0; i < nup; i++)
    {                    /* fill in its upvalues */
      if (uv[i].instack != 0) /* upvalue refers to local variable? */
        ncl.upvals[i] = UpVal.luaF_findupval(L, base + uv[i].idx);
      else /* get upvalue from enclosing function */
        ncl.upvals[i] = encup[uv[i].idx];
      luaC_objbarrier(L, ncl, ncl.upvals[i]);
    }
  }

  /*
  ** finish execution of an opcode interrupted by a yield
  */
  public static void luaV_finishOp(lua_State L)
  {
    CallInfo ci = L.ci;
    int base = ci.func + 1;
    int[] code = ci_func(L, ci).p.code;
    int inst = code[ci.savedpc - 1]; /* interrupted instruction */
    int op = GET_OPCODE(inst);
    switch (op)
    { /* finish its execution */
    case OP_MMBIN:
    case OP_MMBINI:
    case OP_MMBINK:
    {
      copyslot(L, base + GETARG_A(code[ci.savedpc - 2]), --L.top);
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
      copyslot(L, base + GETARG_A(inst), --L.top);
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
      int res = !tagisfalse(slottag(L, L.top - 1)) ? 1 : 0;
      L.top--;
      assert GET_OPCODE(code[ci.savedpc]) == OP_JMP;
      if (res != GETARG_k(inst)) /* condition failed? */
        ci.savedpc++;       /* skip jump instruction */
      break;
    }
    case OP_CONCAT:
    {
      int top = L.top - 1;                   /* top when 'luaT_tryconcatTM' was called */
      int a = GETARG_A(inst);                     /* first element to concatenate */
      int total = (int) (top - 1 - (base + a)); /* yet to concatenate */
      copyslot(L, top - 2, top);                 /* put TM result in proper position */
      L.top = top - 1;                         /* top is one after last element (at top-2) */
      luaV_concat(L, total);                      /* concat them (may yield again) */
      break;
    }
    case OP_CLOSE:
    {                    /* yielded closing variables */
      ci.savedpc--; /* repeat instruction to close other vars. */
      break;
    }
    case OP_RETURN:
    { /* yielded closing variables */
      int ra = base + GETARG_A(inst);
      /* adjust top to signal correct number of returns, in case the
         return is "up to top" ('isIT') */
      L.top = ra + ci.nres;
      /* repeat instruction to close other vars. and complete the return */
      ci.savedpc--;
      break;
    }
    default:
    {
      /* only these other opcodes can yield */
      assert op == OP_TFORCALL || op == OP_CALL ||
             op == OP_TAILCALL || op == OP_SETTABUP || op == OP_SETTABLE ||
             op == OP_SETI || op == OP_SETFIELD ||
             op == OP_VARARGPREP; /* (its call hook froze: see 'finishhooked') */
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

  private static long l_addi(lua_State L, long a, long b)
  {
    return l_intadd(a, b);
  }
  private static long l_subi(lua_State L, long a, long b)
  {
    return l_intsub(a, b);
  }
  private static long l_muli(lua_State L, long a, long b)
  {
    return l_intmul(a, b);
  }
  private static long l_band(long a, long b)
  {
    return l_intband(a, b);
  }
  private static long l_bor(long a, long b)
  {
    return l_intbor(a, b);
  }
  private static long l_bxor(long a, long b)
  {
    return l_intbxor(a, b);
  }

  private static int l_lti(long a, long b)
  {
    return (a < b) ? 1 : 0;
  }
  private static int l_lei(long a, long b)
  {
    return (a <= b) ? 1 : 0;
  }
  private static int l_gti(long a, long b)
  {
    return (a > b) ? 1 : 0;
  }
  private static int l_gei(long a, long b)
  {
    return (a >= b) ? 1 : 0;
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
  ** Trace a function as it starts or goes on. With only the budget, there
  ** is nothing to trace: just keep the trap on, to count steps.
  */
  private static int tracecall(lua_State L, CallInfo ci)
  {
    if (L.hookmask == LUA_MASKBUDGET)
    {
      ci.trap = 1;
      return 1;
    }
    return luaG_tracecall(L);
  }

  private static void luai_threadyield(lua_State L)
  {
    lua_unlock(L);
    lua_lock(L);
  }

  /* 'c' is the limit of live values in the stack */

  public static void luaV_execute(lua_State L, CallInfo ci)
  {
    LClosure cl;
    TValue[] k;
    int base;
    int[] code;
    int pc;
    int trap;
    trap = L.hookmask;
    cl = ci_func(L, ci);
    k = cl.p.k;
    code = cl.p.code;
    pc = ci.savedpc;
    if (trap != 0)
      trap = tracecall(L, ci);
    ci.callstatus &= ~CIST_FROZE; /* (a frozen function goes on: see 'unroll') */
    base = ci.func + 1;
    /* main loop of interpreter */
    for (;;)
    {
      int i; /* instruction being executed */
      if (trap != 0) /* stack reallocation, hooks or budget? */
      {
        if (L.hookmask == LUA_MASKBUDGET && L.beff.left > 0 && L.beff.fnz.next == null)
          L.beff.left--; /* a step of the budget (no hooks, no '__gc' waiting) */
        else
        {
          trap = luaG_traceexec(L, pc); /* handle hooks */
          base = ci.func + 1;          /* correct stack */
        }
      }
      i = code[pc++];
      assert base == ci.func + 1;
      assert base <= L.top && L.top <= L.stack_last;
      /* for tests, invalidate top for instructions not expecting it */
      assert luaP_isIT(i) != 0 || (L.top = base) == base;
      switch (GET_OPCODE(i))
      {
        case OP_MOVE:
        {
          int ra = base + GETARG_A(i);
          copyslot(L, ra, base + GETARG_B(i));
          break;
        }
        case OP_LOADI:
        {
          int ra = base + GETARG_A(i);
          long b = GETARG_sBx(i);
          setsloti(L, ra, b);
          break;
        }
        case OP_LOADF:
        {
          int ra = base + GETARG_A(i);
          int b = GETARG_sBx(i);
          setslotflt(L, ra, b);
          break;
        }
        case OP_LOADK:
        {
          int ra = base + GETARG_A(i);
          TValue rb = k[GETARG_Bx(i)];
          setslot(L, ra, rb);
          break;
        }
        case OP_LOADKX:
        {
          int ra = base + GETARG_A(i);
          TValue rb;
          rb = k[GETARG_Ax(code[pc])];
          pc++;
          setslot(L, ra, rb);
          break;
        }
        case OP_LOADFALSE:
        {
          int ra = base + GETARG_A(i);
          setslottag(L, ra, LUA_VFALSE);
          break;
        }
        case OP_LFALSESKIP:
        {
          int ra = base + GETARG_A(i);
          setslottag(L, ra, LUA_VFALSE);
          pc++; /* skip next instruction */
          break;
        }
        case OP_LOADTRUE:
        {
          int ra = base + GETARG_A(i);
          setslottag(L, ra, LUA_VTRUE);
          break;
        }
        case OP_LOADNIL:
        {
          int ra = base + GETARG_A(i);
          int b = GETARG_B(i);
          do
          {
            setslottag(L, ra++, LUA_VNIL);
          } while (b-- != 0);
          break;
        }
        case OP_GETUPVAL:
        {
          int ra = base + GETARG_A(i);
          int b = GETARG_B(i);
          TValue v = upvalgetval(cl.upvals[b]);
          setslot(L, ra, v);
          break;
        }
        case OP_SETUPVAL:
        {
          int ra = base + GETARG_A(i);
          UpVal uv = cl.upvals[GETARG_B(i)];
          TValue rav = getslot(L, ra);
          upvalsetval(L, uv, rav);
          luaC_barrier(L, uv, rav);
          break;
        }
        case OP_GETTABUP:
        {
          int ra = base + GETARG_A(i);
          TValue upval = upvalgetval(cl.upvals[GETARG_B(i)]);
          TValue rc = k[GETARG_C(i)];
          assert ttisstring(rc);
          TString key = tsvalue(rc); /* key must be a short string */
          byte tag;
          TValue res = new TValue();
          if (!ttistable(upval))
            tag = LUA_VNOTABLE;
          else
          {
            tag = hvalue(upval).luaH_getshortstr(key, res);
          }
          if (tagisempty(tag))
          {
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishget(L, upval, rc, ra, tag, upvalorigin(GETARG_B(i)));
            trap = ci.trap;
          }
          else
            setslot(L, ra, res);
          break;
        }
        case OP_GETTABLE:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int rc = base + GETARG_C(i);
          byte tag;
          TValue res = new TValue();
          if (slottag(L, rc) == LUA_VNUMINT)
          { /* fast track for integers? */
            if (slottag(L, rb) != ctb(LUA_VTABLE))
              tag = LUA_VNOTABLE;
            else
            {
              Table h = slothval(L, rb);
              long u = l_intsub(slotival(L, rc), 1);
              if (l_ult(u, h.asize()))
              {
                tag = getArrTag(h, u);
                if (!tagisempty(tag))
                  farr2val(h, u, tag, res);
              }
              else
                tag = h.luaH_getint(slotival(L, rc), res);
            }
          }
          else if (slottag(L, rb) != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            TValue rcv = getslot(L, rc);
            tag = slothval(L, rb).luaH_get(rcv, res);
          }
          if (tagisempty(tag))
          {
            TValue rbv = getslot(L, rb);
            TValue rcv = getslot(L, rc);
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishget(L, rbv, rcv, ra, tag, rb);
            trap = ci.trap;
          }
          else
            setslot(L, ra, res);
          break;
        }
        case OP_GETI:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int c = GETARG_C(i);
          byte tag;
          TValue res = new TValue();
          if (slottag(L, rb) != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            Table h = slothval(L, rb);
            long u = l_intsub(c, 1);
            if (l_ult(u, h.asize()))
            {
              tag = getArrTag(h, u);
              if (!tagisempty(tag))
                farr2val(h, u, tag, res);
            }
            else
              tag = h.luaH_getint(c, res);
          }
          if (tagisempty(tag))
          {
            TValue key = new TValue();
            setivalue(key, c);
            TValue rbv = getslot(L, rb);
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishget(L, rbv, key, ra, tag, rb);
            trap = ci.trap;
          }
          else
            setslot(L, ra, res);
          break;
        }
        case OP_GETFIELD:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          TValue rc = k[GETARG_C(i)];
          assert ttisstring(rc);
          TString key = tsvalue(rc); /* key must be a short string */
          byte tag;
          TValue res = new TValue();
          if (slottag(L, rb) != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            tag = slothval(L, rb).luaH_getshortstr(key, res);
          }
          if (tagisempty(tag))
          {
            TValue rbv = getslot(L, rb);
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishget(L, rbv, rc, ra, tag, rb);
            trap = ci.trap;
          }
          else
            setslot(L, ra, res);
          break;
        }
        case OP_SETTABUP:
        {
          int hres;
          TValue upval = upvalgetval(cl.upvals[GETARG_A(i)]);
          TValue rb = k[GETARG_B(i)];
          TValue rc = TESTARG_k(i) != 0 ? getobj(k[GETARG_C(i)]) : getslot(L, base + GETARG_C(i));
          assert ttisstring(rb);
          TString key = tsvalue(rb); /* key must be a short string */
          if (!ttistable(upval))
            hres = HNOTATABLE;
          else
          {
            hres = hvalue(upval).luaH_psetshortstr(key, rc);
          }
          if (hres == HOK)
            luaV_finishfastset(L, upval, rc);
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishset(L, upval, rb, rc, hres, upvalorigin(GETARG_A(i)));
            trap = ci.trap;
          }
          break;
        }
        case OP_SETTABLE:
        {
          int ra = base + GETARG_A(i);
          int hres;
          int rb = base + GETARG_B(i); /* key (table is in 'ra') */
          TValue rc = TESTARG_k(i) != 0 ? getobj(k[GETARG_C(i)]) : getslot(L, base + GETARG_C(i)); /* value */
          if (slottag(L, rb) == LUA_VNUMINT)
          { /* fast track for integers? */
            int t = ra;
            if (slottag(L, t) != ctb(LUA_VTABLE))
              hres = HNOTATABLE;
            else
            {
              Table h = slothval(L, t);
              long u = l_intsub(slotival(L, rb), 1);
              if (l_ult(u, h.asize()))
              {
                byte tag = getArrTag(h, u);
                if (checknoTM(h.metatable, TM_NEWINDEX) != 0 || !tagisempty(tag))
                {
                  fval2arr(h, u, rc);
                  hres = HOK;
                }
                else
                  hres = ~(int) u;
              }
              else
                hres = h.luaH_psetint(slotival(L, rb), rc);
            }
          }
          else
          {
            int t = ra;
            if (slottag(L, t) != ctb(LUA_VTABLE))
              hres = HNOTATABLE;
            else
            {
              TValue rbv = getslot(L, rb);
              hres = slothval(L, t).luaH_pset(rbv, rc);
            }
          }
          if (hres == HOK)
            luaV_finishfastset(L, ra, rc);
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            TValue tv = getslot(L, ra);
            TValue rbv = getslot(L, rb);
            luaV_finishset(L, tv, rbv, rc, hres, ra);
            trap = ci.trap;
          }
          break;
        }
        case OP_SETI:
        {
          int ra = base + GETARG_A(i);
          int hres;
          int b = GETARG_B(i);
          TValue rc = TESTARG_k(i) != 0 ? getobj(k[GETARG_C(i)]) : getslot(L, base + GETARG_C(i));
          int t = ra;
          if (slottag(L, t) != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            Table h = slothval(L, t);
            long u = l_intsub(b, 1);
            if (l_ult(u, h.asize()))
            {
              byte tag = getArrTag(h, u);
              if (checknoTM(h.metatable, TM_NEWINDEX) != 0 || !tagisempty(tag))
              {
                fval2arr(h, u, rc);
                hres = HOK;
              }
              else
                hres = ~(int) u;
            }
            else
              hres = h.luaH_psetint(b, rc);
          }
          if (hres == HOK)
            luaV_finishfastset(L, ra, rc);
          else
          {
            TValue key = new TValue();
            setivalue(key, b);
            ci.savedpc = pc;
            L.top = ci.top;
            TValue tv = getslot(L, ra);
            luaV_finishset(L, tv, key, rc, hres, ra);
            trap = ci.trap;
          }
          break;
        }
        case OP_SETFIELD:
        {
          int ra = base + GETARG_A(i);
          int hres;
          TValue rb = k[GETARG_B(i)];
          TValue rc = TESTARG_k(i) != 0 ? getobj(k[GETARG_C(i)]) : getslot(L, base + GETARG_C(i));
          assert ttisstring(rb);
          TString key = tsvalue(rb); /* key must be a short string */
          int t = ra;
          if (slottag(L, t) != ctb(LUA_VTABLE))
            hres = HNOTATABLE;
          else
          {
            hres = slothval(L, t).luaH_psetshortstr(key, rc);
          }
          if (hres == HOK)
            luaV_finishfastset(L, ra, rc);
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            TValue tv = getslot(L, ra);
            luaV_finishset(L, tv, rb, rc, hres, ra);
            trap = ci.trap;
          }
          break;
        }
        case OP_NEWTABLE:
        {
          int ra = base + GETARG_A(i);
          int b = GETARG_vB(i); /* log2(hash size) + 1 */
          int c = GETARG_vC(i); /* array size */
          Table t;
          if (b > 0)
            b = 1 << (b - 1); /* hash size is 2^(b - 1) */
          if (TESTARG_k(i) != 0)
          { /* non-zero extra argument? */
            assert GETARG_Ax(code[pc]) != 0;
            /* add it to array size */
            c += GETARG_Ax(code[pc]) * (MAXARG_vC + 1);
          }
          pc++;              /* skip extra argument */
          L.top = ra + 1; /* correct top in case of emergency GC */
          t = Table.luaH_new(L);   /* memory allocation */
          setsloth(L, ra, t);
          if (b != 0 || c != 0)
            t.luaH_resize(L, c, b); /* idem */
          if (L.l_G.GCdebt <= 0)
          {
            ci.savedpc = pc;
            L.top = ra + 1;
            luaC_step(L);
            trap = ci.trap;
          }
          luai_threadyield(L);
          break;
        }
        case OP_SELF:
        {
          int ra = base + GETARG_A(i);
          byte tag;
          int rb = base + GETARG_B(i);
          TValue rc = k[GETARG_C(i)];
          assert ttisstring(rc);
          TString key = tsvalue(rc); /* key must be a short string */
          copyslot(L, ra + 1, rb);
          TValue res = new TValue();
          if (slottag(L, rb) != ctb(LUA_VTABLE))
            tag = LUA_VNOTABLE;
          else
          {
            tag = slothval(L, rb).luaH_getshortstr(key, res);
          }
          if (tagisempty(tag))
          {
            TValue rbv = getslot(L, rb);
            ci.savedpc = pc;
            L.top = ci.top;
            luaV_finishget(L, rbv, rc, ra, tag, rb);
            trap = ci.trap;
          }
          else
            setslot(L, ra, res);
          break;
        }
        case OP_ADDI:
        {
          int ra = base + GETARG_A(i);
          int v1 = base + GETARG_B(i);
          int imm = GETARG_sC(i);
          if (slottag(L, v1) == LUA_VNUMINT)
          {
            long iv1 = slotival(L, v1);
            pc++;
            setsloti(L, ra, l_addi(L, iv1, imm));
          }
          else if (slottag(L, v1) == LUA_VNUMFLT)
          {
            double nb = slotfltval(L, v1);
            double fimm = imm;
            pc++;
            setslotflt(L, ra, luai_numadd(L, nb, fimm));
          }
          break;
        }
        case OP_ADDK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = ivalue(v2);
            pc++;
            setsloti(L, ra, l_addi(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numadd(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_SUBK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = ivalue(v2);
            pc++;
            setsloti(L, ra, l_subi(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numsub(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_MULK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = ivalue(v2);
            pc++;
            setsloti(L, ra, l_muli(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_nummul(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_MODK:
        {
          ci.savedpc = pc; /* in case of division by 0 */
          L.top = ci.top;
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = ivalue(v2);
            pc++;
            setsloti(L, ra, luaV_mod(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luaV_modf(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_POWK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          NumResult n1 = new NumResult();
          NumResult n2 = new NumResult();
          if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numpow(L, n1.v, n2.v));
          }
          break;
        }
        case OP_DIVK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          NumResult n1 = new NumResult();
          NumResult n2 = new NumResult();
          if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numdiv(L, n1.v, n2.v));
          }
          break;
        }
        case OP_IDIVK:
        {
          ci.savedpc = pc; /* in case of division by 0 */
          L.top = ci.top;
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          assert ttisnumber(v2);
          if (slottag(L, v1) == LUA_VNUMINT && ttisinteger(v2))
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = ivalue(v2);
            pc++;
            setsloti(L, ra, luaV_idiv(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numidiv(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_BANDK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          IntResult i1 = new IntResult();
          long i2 = ivalue(v2);
          assert ttisinteger(v2);
          if (tointegerns(L, v1, i1) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_band(i1.v, i2));
          }
          break;
        }
        case OP_BORK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          IntResult i1 = new IntResult();
          long i2 = ivalue(v2);
          assert ttisinteger(v2);
          if (tointegerns(L, v1, i1) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_bor(i1.v, i2));
          }
          break;
        }
        case OP_BXORK:
        {
          int v1 = base + GETARG_B(i);
          TValue v2 = k[GETARG_C(i)];
          IntResult i1 = new IntResult();
          long i2 = ivalue(v2);
          assert ttisinteger(v2);
          if (tointegerns(L, v1, i1) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_bxor(i1.v, i2));
          }
          break;
        }
        case OP_SHLI:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int ic = GETARG_sC(i);
          IntResult ib = new IntResult();
          if (tointegerns(L, rb, ib) != 0)
          {
            pc++;
            setsloti(L, ra, luaV_shiftl(ic, ib.v));
          }
          break;
        }
        case OP_SHRI:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int ic = GETARG_sC(i);
          IntResult ib = new IntResult();
          if (tointegerns(L, rb, ib) != 0)
          {
            pc++;
            setsloti(L, ra, luaV_shiftl(ib.v, -ic));
          }
          break;
        }
        case OP_ADD:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = slotival(L, v2);
            pc++;
            setsloti(L, ra, l_addi(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numadd(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_SUB:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = slotival(L, v2);
            pc++;
            setsloti(L, ra, l_subi(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numsub(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_MUL:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = slotival(L, v2);
            pc++;
            setsloti(L, ra, l_muli(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_nummul(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_MOD:
        {
          ci.savedpc = pc; /* in case of division by 0 */
          L.top = ci.top;
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = slotival(L, v2);
            pc++;
            setsloti(L, ra, luaV_mod(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luaV_modf(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_POW:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          NumResult n1 = new NumResult();
          NumResult n2 = new NumResult();
          if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numpow(L, n1.v, n2.v));
          }
          break;
        }
        case OP_DIV:
        { /* float division (always with floats) */
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          NumResult n1 = new NumResult();
          NumResult n2 = new NumResult();
          if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setslotflt(L, ra, luai_numdiv(L, n1.v, n2.v));
          }
          break;
        }
        case OP_IDIV:
        {                   /* floor division */
          ci.savedpc = pc; /* in case of division by 0 */
          L.top = ci.top;
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          if (slottag(L, v1) == LUA_VNUMINT && slottag(L, v2) == LUA_VNUMINT)
          {
            int ra = base + GETARG_A(i);
            long i1 = slotival(L, v1);
            long i2 = slotival(L, v2);
            pc++;
            setsloti(L, ra, luaV_idiv(L, i1, i2));
          }
          else
          {
            NumResult n1 = new NumResult();
            NumResult n2 = new NumResult();
            if (tonumberns(L, v1, n1) != 0 && tonumberns(L, v2, n2) != 0)
            {
              int ra = base + GETARG_A(i);
              pc++;
              setslotflt(L, ra, luai_numidiv(L, n1.v, n2.v));
            }
          }
          break;
        }
        case OP_BAND:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          IntResult i1 = new IntResult();
          IntResult i2 = new IntResult();
          if (tointegerns(L, v1, i1) != 0 && tointegerns(L, v2, i2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_band(i1.v, i2.v));
          }
          break;
        }
        case OP_BOR:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          IntResult i1 = new IntResult();
          IntResult i2 = new IntResult();
          if (tointegerns(L, v1, i1) != 0 && tointegerns(L, v2, i2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_bor(i1.v, i2.v));
          }
          break;
        }
        case OP_BXOR:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          IntResult i1 = new IntResult();
          IntResult i2 = new IntResult();
          if (tointegerns(L, v1, i1) != 0 && tointegerns(L, v2, i2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, l_bxor(i1.v, i2.v));
          }
          break;
        }
        case OP_SHL:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          IntResult i1 = new IntResult();
          IntResult i2 = new IntResult();
          if (tointegerns(L, v1, i1) != 0 && tointegerns(L, v2, i2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, luaV_shiftl(i1.v, i2.v));
          }
          break;
        }
        case OP_SHR:
        {
          int v1 = base + GETARG_B(i);
          int v2 = base + GETARG_C(i);
          IntResult i1 = new IntResult();
          IntResult i2 = new IntResult();
          if (tointegerns(L, v1, i1) != 0 && tointegerns(L, v2, i2) != 0)
          {
            int ra = base + GETARG_A(i);
            pc++;
            setsloti(L, ra, luaV_shiftr(i1.v, i2.v));
          }
          break;
        }
        case OP_MMBIN:
        {
          int ra = base + GETARG_A(i);
          int pi = code[pc - 2]; /* original arith. expression */
          int rb = base + GETARG_B(i);
          int tm = GETARG_C(i);
          int result = base + GETARG_A(pi);
          assert OP_ADD <= GET_OPCODE(pi) && GET_OPCODE(pi) <= OP_SHR;
          ci.savedpc = pc;
          L.top = ci.top;
          luaT_trybinTM(L, ra, rb, result, tm);
          trap = ci.trap;
          break;
        }
        case OP_MMBINI:
        {
          int ra = base + GETARG_A(i);
          int pi = code[pc - 2]; /* original arith. expression */
          int imm = GETARG_sB(i);
          int tm = GETARG_C(i);
          int flip = GETARG_k(i);
          int result = base + GETARG_A(pi);
          ci.savedpc = pc;
          L.top = ci.top;
          luaT_trybiniTM(L, ra, imm, flip, result, tm);
          trap = ci.trap;
          break;
        }
        case OP_MMBINK:
        {
          int ra = base + GETARG_A(i);
          int pi = code[pc - 2]; /* original arith. expression */
          TValue imm = k[GETARG_B(i)];
          int tm = GETARG_C(i);
          int flip = GETARG_k(i);
          int result = base + GETARG_A(pi);
          ci.savedpc = pc;
          L.top = ci.top;
          luaT_trybinassocTM(L, ra, imm, flip, result, tm);
          trap = ci.trap;
          break;
        }
        case OP_UNM:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          NumResult nb = new NumResult();
          if (slottag(L, rb) == LUA_VNUMINT)
          {
            long ib = slotival(L, rb);
            setsloti(L, ra, l_intunm(ib));
          }
          else if (tonumberns(L, rb, nb) != 0)
          {
            setslotflt(L, ra, -nb.v);
          }
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            luaT_trybinTM(L, rb, rb, ra, TM_UNM);
            trap = ci.trap;
          }
          break;
        }
        case OP_BNOT:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          IntResult ib = new IntResult();
          if (tointegerns(L, rb, ib) != 0)
          {
            setsloti(L, ra, l_intbnot(ib.v));
          }
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            luaT_trybinTM(L, rb, rb, ra, TM_BNOT);
            trap = ci.trap;
          }
          break;
        }
        case OP_NOT:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          if (tagisfalse(slottag(L, rb)))
            setslottag(L, ra, LUA_VTRUE);
          else
            setslottag(L, ra, LUA_VFALSE);
          break;
        }
        case OP_LEN:
        {
          int ra = base + GETARG_A(i);
          ci.savedpc = pc;
          L.top = ci.top;
          luaV_objlen(L, ra, base + GETARG_B(i));
          trap = ci.trap;
          break;
        }
        case OP_CONCAT:
        {
          int ra = base + GETARG_A(i);
          int n = GETARG_B(i); /* number of elements to concatenate */
          L.top = ra + n;   /* mark the end of concat operands */
          ci.savedpc = pc;
          luaV_concat(L, n);
          trap = ci.trap;
          if (L.l_G.GCdebt <= 0)
          { /* 'luaV_concat' ensures correct top */
            ci.savedpc = pc;
            luaC_step(L);
            trap = ci.trap;
          }
          luai_threadyield(L);
          break;
        }
        case OP_CLOSE:
        {
          int ra = base + GETARG_A(i);
          assert GETARG_B(i) == 0; /* 'close must be alive */
          ci.savedpc = pc;
          L.top = ci.top;
          luaF_close(L, ra, (byte) LUA_OK, 1);
          trap = ci.trap;
          break;
        }
        case OP_TBC:
        {
          int ra = base + GETARG_A(i);
          /* create new to-be-closed upvalue */
          ci.savedpc = pc;
          L.top = ci.top;
          luaF_newtbcupval(L, ra);
          break;
        }
        case OP_JMP:
        {
          pc += GETARG_sJ(i);
          trap = ci.trap;
          break;
        }
        case OP_EQ:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int rb = base + GETARG_B(i);
          ci.savedpc = pc;
          L.top = ci.top;
          cond = luaV_equalobj(L, ra, rb);
          trap = ci.trap;
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_LT:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int cond;
          if (slottag(L, ra) == LUA_VNUMINT && slottag(L, rb) == LUA_VNUMINT)
          {
            long ia = slotival(L, ra);
            long ib = slotival(L, rb);
            cond = l_lti(ia, ib);
          }
          else if (novariant(slottag(L, ra)) == LUA_TNUMBER && novariant(slottag(L, rb)) == LUA_TNUMBER)
            cond = LTnum(L, ra, rb);
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            cond = lessthanothers(L, ra, rb);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_LE:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          int cond;
          if (slottag(L, ra) == LUA_VNUMINT && slottag(L, rb) == LUA_VNUMINT)
          {
            long ia = slotival(L, ra);
            long ib = slotival(L, rb);
            cond = l_lei(ia, ib);
          }
          else if (novariant(slottag(L, ra)) == LUA_TNUMBER && novariant(slottag(L, rb)) == LUA_TNUMBER)
            cond = LEnum(L, ra, rb);
          else
          {
            ci.savedpc = pc;
            L.top = ci.top;
            cond = lessequalothers(L, ra, rb);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_EQK:
        {
          int ra = base + GETARG_A(i);
          TValue rb = k[GETARG_B(i)];
          /* basic types do not use '__eq'; we can use raw equality */
          int cond = luaV_rawequalobj(L, ra, rb);
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_EQI:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int im = GETARG_sB(i);
          if (slottag(L, ra) == LUA_VNUMINT)
            cond = (slotival(L, ra) == im) ? 1 : 0;
          else if (slottag(L, ra) == LUA_VNUMFLT)
            cond = (slotfltval(L, ra) == im) ? 1 : 0;
          else
            cond = 0; /* other types cannot be equal to a number */
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_LTI:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int im = GETARG_sB(i);
          if (slottag(L, ra) == LUA_VNUMINT)
            cond = l_lti(slotival(L, ra), im);
          else if (slottag(L, ra) == LUA_VNUMFLT)
          {
            double fa = slotfltval(L, ra);
            double fim = im;
            cond = luai_numlt(fa, fim) ? 1 : 0;
          }
          else
          {
            int isf = GETARG_C(i);
            ci.savedpc = pc;
            L.top = ci.top;
            cond = luaT_callorderiTM(L, ra, im, 0, isf, TM_LT);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_LEI:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int im = GETARG_sB(i);
          if (slottag(L, ra) == LUA_VNUMINT)
            cond = l_lei(slotival(L, ra), im);
          else if (slottag(L, ra) == LUA_VNUMFLT)
          {
            double fa = slotfltval(L, ra);
            double fim = im;
            cond = luai_numle(fa, fim) ? 1 : 0;
          }
          else
          {
            int isf = GETARG_C(i);
            ci.savedpc = pc;
            L.top = ci.top;
            cond = luaT_callorderiTM(L, ra, im, 0, isf, TM_LE);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_GTI:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int im = GETARG_sB(i);
          if (slottag(L, ra) == LUA_VNUMINT)
            cond = l_gti(slotival(L, ra), im);
          else if (slottag(L, ra) == LUA_VNUMFLT)
          {
            double fa = slotfltval(L, ra);
            double fim = im;
            cond = luai_numgt(fa, fim) ? 1 : 0;
          }
          else
          {
            int isf = GETARG_C(i);
            ci.savedpc = pc;
            L.top = ci.top;
            cond = luaT_callorderiTM(L, ra, im, 1, isf, TM_LT);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_GEI:
        {
          int ra = base + GETARG_A(i);
          int cond;
          int im = GETARG_sB(i);
          if (slottag(L, ra) == LUA_VNUMINT)
            cond = l_gei(slotival(L, ra), im);
          else if (slottag(L, ra) == LUA_VNUMFLT)
          {
            double fa = slotfltval(L, ra);
            double fim = im;
            cond = luai_numge(fa, fim) ? 1 : 0;
          }
          else
          {
            int isf = GETARG_C(i);
            ci.savedpc = pc;
            L.top = ci.top;
            cond = luaT_callorderiTM(L, ra, im, 1, isf, TM_LE);
            trap = ci.trap;
          }
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_TEST:
        {
          int ra = base + GETARG_A(i);
          int cond = !tagisfalse(slottag(L, ra)) ? 1 : 0;
          if (cond != GETARG_k(i))
            pc++;
          else
          {
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_TESTSET:
        {
          int ra = base + GETARG_A(i);
          int rb = base + GETARG_B(i);
          if ((tagisfalse(slottag(L, rb)) ? 1 : 0) == GETARG_k(i))
            pc++;
          else
          {
            copyslot(L, ra, rb);
            int ni = code[pc];
            pc += GETARG_sJ(ni) + 1;
            trap = ci.trap;
          }
          break;
        }
        case OP_CALL:
        {
          int ra = base + GETARG_A(i);
          CallInfo newci;
          int b = GETARG_B(i);
          int nresults = GETARG_C(i) - 1;
          if (b != 0)          /* fixed number of arguments? */
            L.top = ra + b; /* top signals number of arguments */
          /* else previous instruction set top */
          ci.savedpc = pc; /* in case of errors */
          if ((newci = luaD_precall(L, ra, nresults)) == null)
            trap = ci.trap; /* C call; nothing else to be done */
          else
          { /* Lua call: run function in this same C frame */
            ci = newci;
            trap = L.hookmask;
            cl = ci_func(L, ci);
            k = cl.p.k;
            code = cl.p.code;
            pc = ci.savedpc;
            if (trap != 0)
              trap = tracecall(L, ci);
            base = ci.func + 1;
          }
          break;
        }
        case OP_TAILCALL:
        {
          int ra = base + GETARG_A(i);
          int b = GETARG_B(i); /* number of arguments + 1 (function) */
          int n;               /* number of results when calling a C function */
          int nparams1 = GETARG_C(i);
          /* delta is virtual 'func' - real 'func' (vararg functions) */
          int delta = nparams1 != 0 ? ci.nextraargs + nparams1 : 0;
          if (b != 0)
            L.top = ra + b;
          else /* previous instruction set top */
            b = (int) (L.top - ra);
          ci.savedpc = pc; /* several calls here can raise errors */
          if (TESTARG_k(i) != 0)
          {
            luaF_closeupval(L, base);        /* close upvalues from current call */
            assert L.tbclist < base; /* no pending tbc variables */
            assert base == ci.func + 1;
          }
          if ((n = luaD_pretailcall(L, ci, ra, b, delta)) < 0) /* Lua function? */
          {
            trap = L.hookmask;
            cl = ci_func(L, ci);
            k = cl.p.k;
            code = cl.p.code;
            pc = ci.savedpc;
            if (trap != 0)
              trap = tracecall(L, ci);
            base = ci.func + 1;
          }
          else
          {                         /* C function? */
            ci.func -= delta;    /* restore 'func' (if vararg) */
            luaD_poscall(L, ci, n); /* finish caller */
            trap = ci.trap; /* 'luaD_poscall' can change hooks */
            if ((ci.callstatus & CIST_FRESH) != 0)
              return; /* end this frame */
            ci = ci.previous; /* continue running caller in this frame */
            cl = ci_func(L, ci);
            k = cl.p.k;
            code = cl.p.code;
            pc = ci.savedpc;
            if (trap != 0)
              trap = tracecall(L, ci);
            base = ci.func + 1;
          }
          break;
        }
        case OP_RETURN:
        {
          int ra = base + GETARG_A(i);
          int n = GETARG_B(i) - 1; /* number of results */
          int nparams1 = GETARG_C(i);
          if (n < 0)                     /* not fixed? */
            n = (int) (L.top - ra); /* get what is available */
          ci.savedpc = pc;
          if (TESTARG_k(i) != 0)
          {                  /* may there be open upvalues? */
            ci.nres = n; /* save number of returns */
            if (L.top < ci.top)
              L.top = ci.top;
            luaF_close(L, base, (byte) CLOSEKTOP, 1);
            trap = ci.trap;
            { if (trap != 0) { base = ci.func + 1; ra = base + GETARG_A(i); } };
          }
          if (nparams1 != 0) /* vararg function? */
            ci.func -= ci.nextraargs + nparams1;
          L.top = ra + n; /* set call for 'luaD_poscall' */
          luaD_poscall(L, ci, n);
          trap = ci.trap; /* 'luaD_poscall' can change hooks */
          if ((ci.callstatus & CIST_FRESH) != 0)
            return; /* end this frame */
          ci = ci.previous; /* continue running caller in this frame */
          cl = ci_func(L, ci);
          k = cl.p.k;
          code = cl.p.code;
          pc = ci.savedpc;
          if (trap != 0)
            trap = tracecall(L, ci);
          base = ci.func + 1;
          break;
        }
        case OP_RETURN0:
        {
          if ((L.hookmask & ~LUA_MASKBUDGET) != 0)
          {
            int ra = base + GETARG_A(i);
            L.top = ra;
            ci.savedpc = pc;
            luaD_poscall(L, ci, 0); /* no hurry... */
            trap = 1;
          }
          else
          { /* do the 'poscall' here */
            int nres = get_nresults(ci.callstatus);
            L.ci = ci.previous; /* back to caller */
            L.top = base - 1;
            for (; nres > 0; nres--)
              setslottag(L, L.top++, LUA_VNIL); /* all results are nil */
          }
          if ((ci.callstatus & CIST_FRESH) != 0)
            return; /* end this frame */
          ci = ci.previous; /* continue running caller in this frame */
          cl = ci_func(L, ci);
          k = cl.p.k;
          code = cl.p.code;
          pc = ci.savedpc;
          if (trap != 0)
            trap = tracecall(L, ci);
          base = ci.func + 1;
          break;
        }
        case OP_RETURN1:
        {
          if ((L.hookmask & ~LUA_MASKBUDGET) != 0)
          {
            int ra = base + GETARG_A(i);
            L.top = ra + 1;
            ci.savedpc = pc;
            luaD_poscall(L, ci, 1); /* no hurry... */
            trap = 1;
          }
          else
          { /* do the 'poscall' here */
            int nres = get_nresults(ci.callstatus);
            L.ci = ci.previous; /* back to caller */
            if (nres == 0)
              L.top = base - 1; /* asked for no results */
            else
            {
              int ra = base + GETARG_A(i);
              copyslot(L, base - 1, ra); /* at least this result */
              L.top = base;
              for (; nres > 1; nres--)
                setslottag(L, L.top++, LUA_VNIL); /* complete missing results */
            }
          }
          /* return from a Lua function */
          if ((ci.callstatus & CIST_FRESH) != 0)
            return; /* end this frame */
          ci = ci.previous; /* continue running caller in this frame */
          cl = ci_func(L, ci);
          k = cl.p.k;
          code = cl.p.code;
          pc = ci.savedpc;
          if (trap != 0)
            trap = tracecall(L, ci);
          base = ci.func + 1;
          break;
        }
        case OP_FORLOOP:
        {
          int ra = base + GETARG_A(i);
          if (slottag(L, ra + 1) == LUA_VNUMINT)
          { /* integer loop? */
            assert slottag(L, ra) == LUA_VNUMINT;
            long count = slotival(L, ra);
            if (count != 0)
            { /* still more iterations? */
              assert slottag(L, ra + 1) == LUA_VNUMINT;
              long step = slotival(L, ra + 1);
              assert slottag(L, ra + 2) == LUA_VNUMINT;
              long idx = slotival(L, ra + 2);    /* control variable */
              chgsloti(L, ra, l_intsub(count, 1)); /* update counter */
              idx = l_intadd(idx, step);                /* add step to index */
              chgsloti(L, ra + 2, idx);              /* update control variable */
              pc -= GETARG_Bx(i);                       /* jump back */
            }
          }
          else if (floatforloop(L, ra) != 0) /* float loop */
            pc -= GETARG_Bx(i);         /* jump back */
          trap = ci.trap; /* allows a signal to break the loop */
          break;
        }
        case OP_FORPREP:
        {
          int ra = base + GETARG_A(i);
          ci.savedpc = pc; /* in case of errors */
          L.top = ci.top;
          if (forprep(L, ra) != 0)
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
          int ra = base + GETARG_A(i);
          TValue temp = getslot(L, ra + 3); /* to swap control and closing variables */
          copyslot(L, ra + 3, ra + 2);
          setslot(L, ra + 2, temp);
          /* create to-be-closed upvalue (if closing var. is not nil) */
          ci.savedpc = pc;
          L.top = ci.top;
          luaF_newtbcupval(L, ra + 2);
          pc += GETARG_Bx(i); /* go to end of the loop */
          i = code[pc++];        /* fetch next instruction */
          assert GET_OPCODE(i) == OP_TFORCALL && ra == (base + GETARG_A(i));
          ra = base + GETARG_A(i);
          copyslot(L, ra + 5, ra + 3); /* copy the control variable */
          copyslot(L, ra + 4, ra + 1); /* copy state */
          copyslot(L, ra + 3, ra);     /* copy function */
          L.top = ra + 3 + 3;
          ci.savedpc = pc;
          luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
          trap = ci.trap;
          { if (trap != 0) { base = ci.func + 1; ra = base + GETARG_A(i); } };                              /* stack may have changed */
          i = code[pc++];                                  /* go to next instruction */
          assert GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i));
          ra = base + GETARG_A(i);
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
          int ra = base + GETARG_A(i);
          copyslot(L, ra + 5, ra + 3); /* copy the control variable */
          copyslot(L, ra + 4, ra + 1); /* copy state */
          copyslot(L, ra + 3, ra);     /* copy function */
          L.top = ra + 3 + 3;
          ci.savedpc = pc;
          luaD_call(L, ra + 3, GETARG_C(i)); /* do the call */
          trap = ci.trap;
          { if (trap != 0) { base = ci.func + 1; ra = base + GETARG_A(i); } };                              /* stack may have changed */
          i = code[pc++];                                  /* go to next instruction */
          assert GET_OPCODE(i) == OP_TFORLOOP && ra == (base + GETARG_A(i));
          ra = base + GETARG_A(i);
          if (!tagisempty(slottag(L, ra + 3))) /* continue loop? */
            pc -= GETARG_Bx(i);      /* jump back */
          break;
        }
        case OP_TFORLOOP:
        {
          int ra = base + GETARG_A(i);
          if (!tagisempty(slottag(L, ra + 3))) /* continue loop? */
            pc -= GETARG_Bx(i);      /* jump back */
          break;
        }
        case OP_SETLIST:
        {
          int ra = base + GETARG_A(i);
          int n = GETARG_vB(i);
          int last = GETARG_vC(i);
          Table h = slothval(L, ra);
          if (n == 0)
            n = (int) (L.top - ra) - 1; /* get up to the top */
          else
            L.top = ci.top; /* correct top in case of emergency GC */
          last += n;
          if (TESTARG_k(i) != 0)
          {
            last += GETARG_Ax(code[pc]) * (MAXARG_vC + 1);
            pc++;
          }
          /* when 'n' is known, table should have proper size */
          if (last > h.asize())
          { /* needs more space? */
            /* fixed-size sets should have space preallocated */
            assert GETARG_vB(i) == 0;
            h.luaH_resizearray(L, last); /* preallocate it at once */
          }
          for (; n > 0; n--)
          {
            int val = ra + n;
            TValue vv = getslot(L, val);
            obj2arr(h, last - 1, vv);
            last--;
            assert novariant(h.tt) >= LUA_TSTRING;
            luaC_barrierback(L, h, vv);
          }
          break;
        }
        case OP_CLOSURE:
        {
          int ra = base + GETARG_A(i);
          Proto p = cl.p.p[GETARG_Bx(i)];
          ci.savedpc = pc;
          L.top = ci.top;
          pushclosure(L, p, cl.upvals, base, ra);
          if (L.l_G.GCdebt <= 0)
          {
            ci.savedpc = pc;
            L.top = ra + 1;
            luaC_step(L);
            trap = ci.trap;
          }
          luai_threadyield(L);
          break;
        }
        case OP_VARARG:
        {
          int ra = base + GETARG_A(i);
          int n = GETARG_C(i) - 1; /* required results (-1 means all) */
          int vatab = GETARG_k(i) != 0 ? GETARG_B(i) : -1;
          ci.savedpc = pc;
          L.top = ci.top;
          luaT_getvarargs(L, ci, ra, n, vatab);
          trap = ci.trap;
          break;
        }
        case OP_GETVARG:
        {
          int ra = base + GETARG_A(i);
          int rc = base + GETARG_C(i);
          luaT_getvararg(L, ci, ra, rc);
          break;
        }
        case OP_ERRNNIL:
        {
          int ra = base + GETARG_A(i);
          if (!tagisempty(slottag(L, ra)))
          {
            ci.savedpc = pc;
            L.top = ci.top;
            luaG_errnnil(L, cl, GETARG_Bx(i));
          }
          break;
        }
        case OP_VARARGPREP:
        {
          ci.savedpc = pc;
          luaT_adjustvarargs(L, ci, cl.p);
          trap = ci.trap;
          if (trap != 0)
          { /* previous "Protect" updated trap */
            luaD_hookcall(L, ci);
            L.oldpc = 1; /* next opcode will be seen as a "new" line */
          }
          base = ci.func + 1; /* function has new base after adjustment */
          break;
        }
        case OP_EXTRAARG:
        {
          assert false;
          break;
        }
      }
    }
  }

  /* }================================================================== */
}
