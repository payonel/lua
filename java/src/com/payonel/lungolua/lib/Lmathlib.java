package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.api.LuaConf.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.platform.Libm.*;

import java.util.OptionalLong;

import com.payonel.lungolua.api.KNamer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.luaL_Regi;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_UserObject;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.runtime.lua_State;

public final class Lmathlib
{
  private Lmathlib()
  {
  }

  private static final double PI = 3.141592653589793238462643383279502884;

  private static int math_abs(lua_State L)
  {
    if (lua_isinteger(L, 1) != 0)
    {
      long n = lua_tointeger(L, 1);
      if (n < 0)
        n = l_intunm(n);
      lua_pushinteger(L, n);
    }
    else
      lua_pushnumber(L, fabs(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_abs = Lmathlib::math_abs;

  private static int math_sin(lua_State L)
  {
    lua_pushnumber(L, sin(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_sin = Lmathlib::math_sin;

  private static int math_cos(lua_State L)
  {
    lua_pushnumber(L, cos(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_cos = Lmathlib::math_cos;

  private static int math_tan(lua_State L)
  {
    lua_pushnumber(L, tan(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_tan = Lmathlib::math_tan;

  private static int math_asin(lua_State L)
  {
    lua_pushnumber(L, asin(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_asin = Lmathlib::math_asin;

  private static int math_acos(lua_State L)
  {
    lua_pushnumber(L, acos(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_acos = Lmathlib::math_acos;

  private static int math_atan(lua_State L)
  {
    double y = luaL_checknumber(L, 1);
    double x = luaL_optnumber(L, 2, 1);
    lua_pushnumber(L, atan2(y, x));
    return 1;
  }

  private static final lua_CFunction math_atan = Lmathlib::math_atan;

  private static int math_toint(lua_State L)
  {
    OptionalLong n = lua_tointegerx(L, 1);
    if (n.isPresent())
      lua_pushinteger(L, n.getAsLong());
    else
    {
      luaL_checkany(L, 1);
      luaL_pushfail(L); /* value is not convertible to integer */
    }
    return 1;
  }

  private static final lua_CFunction math_toint = Lmathlib::math_toint;

  private static void pushnumint(lua_State L, double d)
  {
    IntResult n = new IntResult();
    if (lua_numbertointeger(d, n) != 0) /* does 'd' fit in an integer? */
      lua_pushinteger(L, n.v);        /* result is integer */
    else
      lua_pushnumber(L, d); /* result is float */
  }

  private static int math_floor(lua_State L)
  {
    if (lua_isinteger(L, 1) != 0)
      lua_settop(L, 1); /* integer is its own floor */
    else
    {
      double d = floor(luaL_checknumber(L, 1));
      pushnumint(L, d);
    }
    return 1;
  }

  private static final lua_CFunction math_floor = Lmathlib::math_floor;

  private static int math_ceil(lua_State L)
  {
    if (lua_isinteger(L, 1) != 0)
      lua_settop(L, 1); /* integer is its own ceiling */
    else
    {
      double d = ceil(luaL_checknumber(L, 1));
      pushnumint(L, d);
    }
    return 1;
  }

  private static final lua_CFunction math_ceil = Lmathlib::math_ceil;

  private static int math_fmod(lua_State L)
  {
    if (lua_isinteger(L, 1) != 0 && lua_isinteger(L, 2) != 0)
    {
      long d = lua_tointeger(L, 2);
      if (d == 0 || d == -1)
      { /* special cases: -1 or 0 */
        luaL_argchecki(L, d != 0, 2, Lt.zero);
        lua_pushinteger(L, 0); /* avoid overflow with 0x80000... / -1 */
      }
      else
        lua_pushinteger(L, lua_tointeger(L, 1) % d);
    }
    else
      lua_pushnumber(L, fmod(luaL_checknumber(L, 1),
                                       luaL_checknumber(L, 2)));
    return 1;
  }

  private static final lua_CFunction math_fmod = Lmathlib::math_fmod;

  /*
  ** next function does not use 'modf', avoiding problems with 'double*'
  ** (which is not compatible with 'float*') when lua_Number is not
  ** 'double'.
  */
  private static int math_modf(lua_State L)
  {
    if (lua_isinteger(L, 1) != 0)
    {
      lua_settop(L, 1);     /* number is its own integer part */
      lua_pushnumber(L, 0); /* no fractional part */
    }
    else
    {
      double n = luaL_checknumber(L, 1);
      /* integer part (rounds toward zero) */
      double ip = (n < 0) ? ceil(n) : floor(n);
      pushnumint(L, ip);
      /* fractional part (test needed for inf/-inf) */
      lua_pushnumber(L, (n == ip) ? 0.0 : (n - ip));
    }
    return 2;
  }

  private static final lua_CFunction math_modf = Lmathlib::math_modf;

  private static int math_sqrt(lua_State L)
  {
    lua_pushnumber(L, sqrt(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_sqrt = Lmathlib::math_sqrt;

  private static int math_ult(lua_State L)
  {
    long a = luaL_checkinteger(L, 1);
    long b = luaL_checkinteger(L, 2);
    lua_pushboolean(L, l_ult(a, b) ? 1 : 0);
    return 1;
  }

  private static final lua_CFunction math_ult = Lmathlib::math_ult;

  private static int math_log(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    double res;
    if (lua_isnoneornil(L, 2) != 0)
      res = log(x);
    else
    {
      double base = luaL_checknumber(L, 2);
      if (base == 2.0)
        res = log2(x);
      else if (base == 10.0)
        res = log10(x);
      else
        res = log(x) / log(base);
    }
    lua_pushnumber(L, res);
    return 1;
  }

  private static final lua_CFunction math_log = Lmathlib::math_log;

  private static int math_exp(lua_State L)
  {
    lua_pushnumber(L, exp(luaL_checknumber(L, 1)));
    return 1;
  }

  private static final lua_CFunction math_exp = Lmathlib::math_exp;

  private static int math_deg(lua_State L)
  {
    lua_pushnumber(L, luaL_checknumber(L, 1) * (180.0 / PI));
    return 1;
  }

  private static final lua_CFunction math_deg = Lmathlib::math_deg;

  private static int math_rad(lua_State L)
  {
    lua_pushnumber(L, luaL_checknumber(L, 1) * (PI / 180.0));
    return 1;
  }

  private static final lua_CFunction math_rad = Lmathlib::math_rad;

  private static int math_frexp(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    Frexp ep = new Frexp();
    lua_pushnumber(L, frexp(x, ep));
    lua_pushinteger(L, ep.e);
    return 2;
  }

  private static final lua_CFunction math_frexp = Lmathlib::math_frexp;

  private static int math_ldexp(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    int ep = (int) luaL_checkinteger(L, 2);
    lua_pushnumber(L, ldexp(x, ep));
    return 1;
  }

  private static final lua_CFunction math_ldexp = Lmathlib::math_ldexp;

  /*
  ** 'math.min' and 'math.max' compare with '__lt' when they are given
  ** tables and the like, which can freeze: the loop state (the number of
  ** arguments, the one being compared, the index of the best so far) is in the
  ** continuation's context, 21 bits each.
  */
  private static long minmax_pack(int n, int i, int ibest)
  {
    return (long) n | ((long) i << 21) | ((long) ibest << 42);
  }

  /* compare the arguments from 'i' on with the best so far, 'ibest' */
  private static int minmax_loop(lua_State L, boolean ismax, int n, int i, int ibest)
  {
    for (; i <= n; i++)
    {
      long ctx = minmax_pack(n, i, ibest);
      if (ismax)
        lua_comparek(L, ibest, i, LUA_OPLT, ctx, max_cont);
      else
        lua_comparek(L, i, ibest, LUA_OPLT, ctx, min_cont);
      if (lua_toboolean(L, -1) != 0)
        ibest = i;
      lua_pop(L, 1);
    }
    lua_pushvalue(L, ibest);
    return 1;
  }

  /* the result of comparing argument 'i' is on the top */
  private static int minmax_cont(lua_State L, boolean ismax, long ctx)
  {
    int n = (int) (ctx & 0x1FFFFF);
    int i = (int) ((ctx >> 21) & 0x1FFFFF);
    int ibest = (int) ((ctx >> 42) & 0x1FFFFF);
    if (lua_toboolean(L, -1) != 0)
      ibest = i;
    lua_pop(L, 1);
    return minmax_loop(L, ismax, n, i + 1, ibest);
  }


  private static int min_cont(lua_State L, int status, long ctx)
  {
    return minmax_cont(L, false, ctx);
  }

  private static final lua_KFunction min_cont = Lmathlib::min_cont;

  private static final KNamer knamer_min_cont = new KNamer("math.min_cont", min_cont);

  private static int max_cont(lua_State L, int status, long ctx)
  {
    return minmax_cont(L, true, ctx);
  }

  private static final lua_KFunction max_cont = Lmathlib::max_cont;

  private static final KNamer knamer_max_cont = new KNamer("math.max_cont", max_cont);

  private static int math_min(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    luaL_argchecki(L, n >= 1, 1, Lt.value_expected);
    lua_freezeonly(L); /* ('__lt' cannot yield, as in stock) */
    return minmax_loop(L, false, n, 2, 1);
  }

  private static final lua_CFunction math_min = Lmathlib::math_min;

  private static int math_max(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    luaL_argchecki(L, n >= 1, 1, Lt.value_expected);
    lua_freezeonly(L); /* ('__lt' cannot yield, as in stock) */
    return minmax_loop(L, true, n, 2, 1);
  }

  private static final lua_CFunction math_max = Lmathlib::math_max;

  private static int math_type(lua_State L)
  {
    if (lua_type(L, 1) == LUA_TNUMBER)
      lua_pushistringref(L, lua_isinteger(L, 1) != 0 ? Lt.integer_2 : Lt.float_kw);
    else
    {
      luaL_checkany(L, 1);
      luaL_pushfail(L);
    }
    return 1;
  }

  private static final lua_CFunction math_type = Lmathlib::math_type;

  /*
  ** {==================================================================
  ** Pseudo-Random Number Generator based on 'xoshiro256**'.
  ** ===================================================================
  */

  private static final int FIGS = Double.PRECISION;

  static
  {
    assert FIGS <= 64 : "there are only 64 random bits";
  }

  /* try to find an integer type with at least 64 bits */

  /* avoid using extra bits when needed */
  private static long trim64(long x)
  {
    return x & 0xffffffffffffffffL;
  }

  /* rotate left 'x' by 'n' bits */
  private static long rotl(long x, int n)
  {
    return (x << n) | (trim64(x) >>> (64 - n));
  }

  private static long nextrand(long[] state)
  {
    long state0 = state[0];
    long state1 = state[1];
    long state2 = state[2] ^ state0;
    long state3 = state[3] ^ state1;
    long res = rotl(state1 * 5, 7) * 9;
    state[0] = state0 ^ state3;
    state[1] = state1 ^ state2;
    state[2] = state2 ^ (state1 << 17);
    state[3] = rotl(state3, 45);
    return res;
  }

  /*
  ** Convert bits from a random integer into a float in the
  ** interval [0,1), getting the higher FIG bits from the
  ** random unsigned integer and converting that to a float.
  ** Some old Microsoft compilers cannot cast an unsigned long
  ** to a floating-point number, so we use a signed long as an
  ** intermediary. When lua_Number is float or double, the shift ensures
  ** that 'sx' is non negative; in that case, a good compiler will remove
  ** the correction.
  */

  /* must throw out the extra (64 - FIGS) bits */
  private static final int shift64_FIG = 64 - FIGS;

  /* 2^(-FIGS) == 2^-1 / 2^(FIGS-1) */
  private static final double scaleFIG = 0.5 / (1L << (FIGS - 1));

  private static double I2d(long x)
  {
    long sx = trim64(x) >>> shift64_FIG;
    double res = (double) (sx) * scaleFIG;
    if (sx < 0)
      res += 1.0; /* correct the two's complement if negative */
    assert 0 <= res && res < 1;
    return res;
  }

  private static long I2UInt(long x)
  {
    return trim64(x);
  }

  private static long Int2I(long x)
  {
    return x;
  }

  private static final class RanState implements lua_UserObject
  {
    long[] s = new long[4];
  }

  private static final int RANSTATE_SIZE = 32;

  /*
  ** Project the random integer 'ran' into the interval [0, n].
  ** Because 'ran' has 2^B possible values, the projection can only be
  ** uniform when the size of the interval is a power of 2 (exact
  ** division). So, to get a uniform projection into [0, n], we
  ** first compute 'lim', the smallest Mersenne number not smaller than
  ** 'n'. We then project 'ran' into the interval [0, lim].  If the result
  ** is inside [0, n], we are done. Otherwise, we try with another 'ran',
  ** until we have a result inside the interval.
  */
  private static long project(long ran, long n,
                              RanState state)
  {
    long lim = n; /* to compute the Mersenne number */
    int sh;               /* how much to spread bits to the right in 'lim' */
    /* spread '1' bits in 'lim' until it becomes a Mersenne number */
    for (sh = 1; (lim & (lim + 1)) != 0; sh *= 2)
      lim |= lim >>> sh;               /* spread '1's to the right */
    while (Long.compareUnsigned(ran &= lim, n) > 0)            /* project 'ran' into [0..lim] and test */
      ran = I2UInt(nextrand(state.s)); /* not inside [0..n]? try again */
    return ran;
  }

  private static int math_random(lua_State L)
  {
    long low, up;
    long p;
    RanState state = (RanState) lua_touserobject(L, lua_upvalueindex(1));
    long rv = nextrand(state.s); /* next pseudo-random value */
    switch (lua_gettop(L))
    { /* check number of arguments */
    case 0:
    {                             /* no arguments */
      lua_pushnumber(L, I2d(rv)); /* float between 0 and 1 */
      return 1;
    }
    case 1:
    { /* only upper limit */
      low = 1;
      up = luaL_checkinteger(L, 1);
      if (up == 0)
      {                                            /* single 0 as argument? */
        lua_pushinteger(L, I2UInt(rv)); /* full random integer */
        return 1;
      }
      break;
    }
    case 2:
    { /* lower and upper limits */
      low = luaL_checkinteger(L, 1);
      up = luaL_checkinteger(L, 2);
      break;
    }
    default:
      return luaL_errori(L, Lt.wrong_number_of_arguments);
    }
    /* random integer in the interval [low, up] */
    luaL_argchecki(L, low <= up, 1, Lt.interval_is_empty);
    /* project random integer into the interval [0, up - low] */
    p = project(I2UInt(rv), (long) up - (long) low, state);
    lua_pushinteger(L, p + (long) low);
    return 1;
  }

  private static final lua_CFunction math_random = Lmathlib::math_random;

  private static void setseed(lua_State L, long[] state,
                              long n1, long n2)
  {
    int i;
    state[0] = Int2I(n1);
    state[1] = Int2I(0xff); /* avoid a zero state */
    state[2] = Int2I(n2);
    state[3] = Int2I(0);
    for (i = 0; i < 16; i++)
      nextrand(state); /* discard initial values to "spread" seed */
    lua_pushinteger(L, n1);
    lua_pushinteger(L, n2);
  }

  private static int math_randomseed(lua_State L)
  {
    RanState state = (RanState) lua_touserobject(L, lua_upvalueindex(1));
    long n1, n2;
    if (lua_isnone(L, 1) != 0)
    {
      n1 = Integer.toUnsignedLong(luaL_makeseed(L));           /* "random" seed */
      n2 = I2UInt(nextrand(state.s)); /* in case seed is not that random... */
    }
    else
    {
      n1 = luaL_checkinteger(L, 1);
      n2 = luaL_optinteger(L, 2, 0);
    }
    setseed(L, state.s, n1, n2);
    return 2; /* return seeds */
  }

  private static final lua_CFunction math_randomseed = Lmathlib::math_randomseed;

  private static final luaL_Regi[] randfuncs = {
    new luaL_Regi(Lt.random, math_random),
    new luaL_Regi(Lt.randomseed, math_randomseed),
  };

  /*
  ** Register the random functions and initialize their state.
  */
  private static void setrandfunc(lua_State L)
  {
    RanState o = new RanState();
    RanState state = o;
    lua_newuserobject(L, o, RANSTATE_SIZE, 0);
    setseed(L, state.s, Integer.toUnsignedLong(luaL_makeseed(L)), 0); /* initialize with random seed */
    lua_pop(L, 2);                             /* remove pushed seeds */
    luaL_setfuncsi(L, randfuncs, 1);
  }

  /* }================================================================== */

  /*
  ** {==================================================================
  ** Deprecated functions (for compatibility only)
  ** ===================================================================
  */
  /* }================================================================== */

  private static final CFuncInfo cfi_math_abs = new CFuncInfo(math_abs, "math.math_abs");
  private static final CFuncInfo cfi_math_acos = new CFuncInfo(math_acos, "math.math_acos");
  private static final CFuncInfo cfi_math_asin = new CFuncInfo(math_asin, "math.math_asin");
  private static final CFuncInfo cfi_math_atan = new CFuncInfo(math_atan, "math.math_atan");
  private static final CFuncInfo cfi_math_ceil = new CFuncInfo(math_ceil, "math.math_ceil");
  private static final CFuncInfo cfi_math_cos = new CFuncInfo(math_cos, "math.math_cos");
  private static final CFuncInfo cfi_math_deg = new CFuncInfo(math_deg, "math.math_deg");
  private static final CFuncInfo cfi_math_exp = new CFuncInfo(math_exp, "math.math_exp");
  private static final CFuncInfo cfi_math_toint = new CFuncInfo(math_toint, "math.math_toint");
  private static final CFuncInfo cfi_math_floor = new CFuncInfo(math_floor, "math.math_floor");
  private static final CFuncInfo cfi_math_fmod = new CFuncInfo(math_fmod, "math.math_fmod");
  private static final CFuncInfo cfi_math_frexp = new CFuncInfo(math_frexp, "math.math_frexp");
  private static final CFuncInfo cfi_math_ult = new CFuncInfo(math_ult, "math.math_ult");
  private static final CFuncInfo cfi_math_ldexp = new CFuncInfo(math_ldexp, "math.math_ldexp");
  private static final CFuncInfo cfi_math_log = new CFuncInfo(math_log, "math.math_log");
  private static final CFuncInfo cfi_math_max = new CFuncInfo(math_max, "math.math_max");
  private static final CFuncInfo cfi_math_min = new CFuncInfo(math_min, "math.math_min");
  private static final CFuncInfo cfi_math_modf = new CFuncInfo(math_modf, "math.math_modf");
  private static final CFuncInfo cfi_math_rad = new CFuncInfo(math_rad, "math.math_rad");
  private static final CFuncInfo cfi_math_sin = new CFuncInfo(math_sin, "math.math_sin");
  private static final CFuncInfo cfi_math_sqrt = new CFuncInfo(math_sqrt, "math.math_sqrt");
  private static final CFuncInfo cfi_math_tan = new CFuncInfo(math_tan, "math.math_tan");
  private static final CFuncInfo cfi_math_type = new CFuncInfo(math_type, "math.math_type");

  private static final luaL_RegCFI[] mathlib = {
    new luaL_RegCFI(Lt.abs_kw, cfi_math_abs),
    new luaL_RegCFI(Lt.acos, cfi_math_acos),
    new luaL_RegCFI(Lt.asin, cfi_math_asin),
    new luaL_RegCFI(Lt.atan, cfi_math_atan),
    new luaL_RegCFI(Lt.ceil_kw, cfi_math_ceil),
    new luaL_RegCFI(Lt.cos_kw, cfi_math_cos),
    new luaL_RegCFI(Lt.deg, cfi_math_deg),
    new luaL_RegCFI(Lt.exp_kw, cfi_math_exp),
    new luaL_RegCFI(Lt.tointeger, cfi_math_toint),
    new luaL_RegCFI(Lt.floor_kw, cfi_math_floor),
    new luaL_RegCFI(Lt.fmod, cfi_math_fmod),
    new luaL_RegCFI(Lt.frexp, cfi_math_frexp),
    new luaL_RegCFI(Lt.ult, cfi_math_ult),
    new luaL_RegCFI(Lt.ldexp, cfi_math_ldexp),
    new luaL_RegCFI(Lt.log_kw, cfi_math_log),
    new luaL_RegCFI(Lt.max, cfi_math_max),
    new luaL_RegCFI(Lt.min, cfi_math_min),
    new luaL_RegCFI(Lt.modf, cfi_math_modf),
    new luaL_RegCFI(Lt.rad, cfi_math_rad),
    new luaL_RegCFI(Lt.sin_kw, cfi_math_sin),
    new luaL_RegCFI(Lt.sqrt_kw, cfi_math_sqrt),
    new luaL_RegCFI(Lt.tan_kw, cfi_math_tan),
    new luaL_RegCFI(Lt.type, cfi_math_type),
    new luaL_RegCFI(Lt.random, null),
    new luaL_RegCFI(Lt.randomseed, null),
    new luaL_RegCFI(Lt.pi, null),
    new luaL_RegCFI(Lt.huge, null),
    new luaL_RegCFI(Lt.maxinteger, null),
    new luaL_RegCFI(Lt.mininteger, null),
  };

  /*
  ** Open math library
  */

  public static int luaopen_math(lua_State L)
  {
    luaL_newlibcfi(L, mathlib);
    lua_pushnumber(L, PI);
    lua_setfieldi(L, -2, Lt.pi);
    lua_pushnumber(L, Double.POSITIVE_INFINITY);
    lua_setfieldi(L, -2, Lt.huge);
    lua_pushinteger(L, LUA_MAXINTEGER);
    lua_setfieldi(L, -2, Lt.maxinteger);
    lua_pushinteger(L, LUA_MININTEGER);
    lua_setfieldi(L, -2, Lt.mininteger);
    setrandfunc(L);
    return 1;
  }

  public static final lua_CFunction luaopen_math = Lmathlib::luaopen_math;
}
