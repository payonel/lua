using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using KNamer = Payonel.LungoLua.Api.KNamer;
using luaL_RegCFI = Payonel.LungoLua.Api.luaL_RegCFI;
using luaL_Regi = Payonel.LungoLua.Api.luaL_Regi;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_KFunction = Payonel.LungoLua.Api.lua_KFunction;
using lua_UserObject = Payonel.LungoLua.Api.lua_UserObject;
using Lt = Payonel.LungoLua.Bytes.Lt;
using CFuncInfo = Payonel.LungoLua.Object.CFuncInfo;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;
using static Payonel.LungoLua.Api.LuaConf;
using static Payonel.LungoLua.Object.Llimits;
using static Payonel.LungoLua.Platform.Libm;

namespace Payonel.LungoLua.Lib;

public sealed class Lmathlib
{
  private Lmathlib()
  {
  }

  private const double PI = 3.141592653589793238462643383279502884;

  private static int math_abs(lua_State L)
  {
    if (lua_isinteger(L, 1))
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

  private static readonly lua_CFunction math_abs_fn = Lmathlib.math_abs;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_sin(lua_State L)
  {
    lua_pushnumber(L, sin(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_sin_fn = Lmathlib.math_sin;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_cos(lua_State L)
  {
    lua_pushnumber(L, cos(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_cos_fn = Lmathlib.math_cos;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_tan(lua_State L)
  {
    lua_pushnumber(L, tan(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_tan_fn = Lmathlib.math_tan;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_asin(lua_State L)
  {
    lua_pushnumber(L, asin(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_asin_fn = Lmathlib.math_asin;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_acos(lua_State L)
  {
    lua_pushnumber(L, acos(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_acos_fn = Lmathlib.math_acos;

  private static int math_atan(lua_State L)
  {
    double y = luaL_checknumber(L, 1);
    double x = luaL_optnumber(L, 2, 1);
    lua_pushnumber(L, atan2(y, x));
    return 1;
  }

  private static readonly lua_CFunction math_atan_fn = Lmathlib.math_atan;

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

  private static readonly lua_CFunction math_toint_fn = Lmathlib.math_toint;

  private static void pushnumint(lua_State L, double d)
  {
    IntResult n = default;
    if (lua_numbertointeger(d, ref n)) /* does 'd' fit in an integer? */
      lua_pushinteger(L, n.v);        /* result is integer */
    else
      lua_pushnumber(L, d); /* result is float */
  }

  private static int math_floor(lua_State L)
  {
    if (lua_isinteger(L, 1))
      lua_settop(L, 1); /* integer is its own floor */
    else
    {
      double d = floor(luaL_checknumber(L, 1));
      pushnumint(L, d);
    }
    return 1;
  }

  private static readonly lua_CFunction math_floor_fn = Lmathlib.math_floor;

  private static int math_ceil(lua_State L)
  {
    if (lua_isinteger(L, 1))
      lua_settop(L, 1); /* integer is its own ceiling */
    else
    {
      double d = ceil(luaL_checknumber(L, 1));
      pushnumint(L, d);
    }
    return 1;
  }

  private static readonly lua_CFunction math_ceil_fn = Lmathlib.math_ceil;

  private static int math_fmod(lua_State L)
  {
    if (lua_isinteger(L, 1) && lua_isinteger(L, 2))
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

  private static readonly lua_CFunction math_fmod_fn = Lmathlib.math_fmod;

  /*
  ** next function does not use 'modf', avoiding problems with 'double*'
  ** (which is not compatible with 'float*') when lua_Number is not
  ** 'double'.
  */
  private static int math_modf(lua_State L)
  {
    if (lua_isinteger(L, 1))
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

  private static readonly lua_CFunction math_modf_fn = Lmathlib.math_modf;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_sqrt(lua_State L)
  {
    lua_pushnumber(L, sqrt(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_sqrt_fn = Lmathlib.math_sqrt;

  private static int math_ult(lua_State L)
  {
    long a = luaL_checkinteger(L, 1);
    long b = luaL_checkinteger(L, 2);
    lua_pushboolean(L, l_ult(a, b));
    return 1;
  }

  private static readonly lua_CFunction math_ult_fn = Lmathlib.math_ult;

  private static int math_log(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    double res;
    if (lua_isnoneornil(L, 2))
      res = log(x);
    else
    {
      double @base = luaL_checknumber(L, 2);
      if (@base == 2.0)
        res = log2(x);
      else if (@base == 10.0)
        res = log10(x);
      else
        res = log(x) / log(@base);
    }
    lua_pushnumber(L, res);
    return 1;
  }

  private static readonly lua_CFunction math_log_fn = Lmathlib.math_log;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_exp(lua_State L)
  {
    lua_pushnumber(L, exp(luaL_checknumber(L, 1)));
    return 1;
  }

  private static readonly lua_CFunction math_exp_fn = Lmathlib.math_exp;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_deg(lua_State L)
  {
    lua_pushnumber(L, luaL_checknumber(L, 1) * (180.0 / PI));
    return 1;
  }

  private static readonly lua_CFunction math_deg_fn = Lmathlib.math_deg;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int math_rad(lua_State L)
  {
    lua_pushnumber(L, luaL_checknumber(L, 1) * (PI / 180.0));
    return 1;
  }

  private static readonly lua_CFunction math_rad_fn = Lmathlib.math_rad;

  private static int math_frexp(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    Frexp ep = new Frexp();
    lua_pushnumber(L, frexp(x, ep));
    lua_pushinteger(L, ep.e);
    return 2;
  }

  private static readonly lua_CFunction math_frexp_fn = Lmathlib.math_frexp;

  private static int math_ldexp(lua_State L)
  {
    double x = luaL_checknumber(L, 1);
    int ep = (int) luaL_checkinteger(L, 2);
    lua_pushnumber(L, ldexp(x, ep));
    return 1;
  }

  private static readonly lua_CFunction math_ldexp_fn = Lmathlib.math_ldexp;

  /*
  ** 'math.min' and 'math.max' compare with '__lt' when they are given
  ** tables and the like, which can freeze: the loop state (the number of
  ** arguments, the one being compared, the index of the best so far) is in the
  ** continuation's context, 21 bits each.
  */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static long minmax_pack(int n, int i, int ibest)
  {
    return (long) n | ((long) i << 21) | ((long) ibest << 42);
  }

  /* compare the arguments from 'i' on with the best so far, 'ibest' */
  private static int minmax_loop(lua_State L, bool ismax, int n, int i, int ibest)
  {
    for (; i <= n; i++)
    {
      long ctx = minmax_pack(n, i, ibest);
      if (ismax)
        lua_comparek(L, ibest, i, LUA_OPLT, ctx, max_cont_fn);
      else
        lua_comparek(L, i, ibest, LUA_OPLT, ctx, min_cont_fn);
      if (lua_toboolean(L, -1))
        ibest = i;
      lua_pop(L, 1);
    }
    lua_pushvalue(L, ibest);
    return 1;
  }

  /* the result of comparing argument 'i' is on the top */
  private static int minmax_cont(lua_State L, bool ismax, long ctx)
  {
    int n = (int) (ctx & 0x1FFFFF);
    int i = (int) ((ctx >> 21) & 0x1FFFFF);
    int ibest = (int) ((ctx >> 42) & 0x1FFFFF);
    if (lua_toboolean(L, -1))
      ibest = i;
    lua_pop(L, 1);
    return minmax_loop(L, ismax, n, i + 1, ibest);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int min_cont(lua_State L, int status, long ctx)
  {
    return minmax_cont(L, false, ctx);
  }

  private static readonly lua_KFunction min_cont_fn = Lmathlib.min_cont;

  private static readonly KNamer knamer_min_cont = new KNamer("math.min_cont", min_cont_fn);

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int max_cont(lua_State L, int status, long ctx)
  {
    return minmax_cont(L, true, ctx);
  }

  private static readonly lua_KFunction max_cont_fn = Lmathlib.max_cont;

  private static readonly KNamer knamer_max_cont = new KNamer("math.max_cont", max_cont_fn);

  private static int math_min(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    luaL_argchecki(L, n >= 1, 1, Lt.value_expected);
    lua_freezeonly(L); /* ('__lt' cannot yield, as in stock) */
    return minmax_loop(L, false, n, 2, 1);
  }

  private static readonly lua_CFunction math_min_fn = Lmathlib.math_min;

  private static int math_max(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    luaL_argchecki(L, n >= 1, 1, Lt.value_expected);
    lua_freezeonly(L); /* ('__lt' cannot yield, as in stock) */
    return minmax_loop(L, true, n, 2, 1);
  }

  private static readonly lua_CFunction math_max_fn = Lmathlib.math_max;

  private static int math_type(lua_State L)
  {
    if (lua_type(L, 1) == LUA_TNUMBER)
      lua_pushistringref(L, lua_isinteger(L, 1) ? Lt.integer_2 : Lt.float_kw);
    else
    {
      luaL_checkany(L, 1);
      luaL_pushfail(L);
    }
    return 1;
  }

  private static readonly lua_CFunction math_type_fn = Lmathlib.math_type;

  /*
  ** {==================================================================
  ** Pseudo-Random Number Generator based on 'xoshiro256**'.
  ** ===================================================================
  */

  private const int FIGS = JDouble.PRECISION;

  private static readonly bool static_init_1 = static_init_1_run();
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static bool static_init_1_run()
  {
    { if (JAssert.on && !( FIGS <= 64 )) throw new AssertionError(JString.valueOf( "there are only 64 random bits")); }
  return true; }

  /* try to find an integer type with at least 64 bits */

  /* avoid using extra bits when needed */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static long trim64(long x)
  {
    return x & unchecked((long)0xFFFFFFFFFFFFFFFFUL);
  }

  /* rotate left 'x' by 'n' bits */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
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
  private const int shift64_FIG = 64 - FIGS;

  /* 2^(-FIGS) == 2^-1 / 2^(FIGS-1) */
  private const double scaleFIG = 0.5 / (1L << (FIGS - 1));

  private static double I2d(long x)
  {
    long sx = trim64(x) >>> shift64_FIG;
    double res = (double) (sx) * scaleFIG;
    if (sx < 0)
      res += 1.0; /* correct the two's complement if negative */
    { if (JAssert.on && !( 0 <= res && res < 1)) throw new AssertionError(); }
    return res;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static long I2UInt(long x)
  {
    return trim64(x);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static long Int2I(long x)
  {
    return x;
  }

  private sealed class RanState : lua_UserObject
  {
    internal long[] s = new long[4];
  }

  private const int RANSTATE_SIZE = 32;

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
    while (JLong.compareUnsigned(ran &= lim, n) > 0)            /* project 'ran' into [0..lim] and test */
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

  private static readonly lua_CFunction math_random_fn = Lmathlib.math_random;

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
    if (lua_isnone(L, 1))
    {
      n1 = JInteger.toUnsignedLong(luaL_makeseed(L));           /* "random" seed */
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

  private static readonly lua_CFunction math_randomseed_fn = Lmathlib.math_randomseed;

  private static readonly luaL_Regi[] randfuncs = {
    new luaL_Regi(Lt.random, math_random_fn),
    new luaL_Regi(Lt.randomseed, math_randomseed_fn),
  };

  /*
  ** Register the random functions and initialize their state.
  */
  private static void setrandfunc(lua_State L)
  {
    RanState o = new RanState();
    RanState state = o;
    lua_newuserobject(L, o, RANSTATE_SIZE, 0);
    setseed(L, state.s, JInteger.toUnsignedLong(luaL_makeseed(L)), 0); /* initialize with random seed */
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

  private static readonly CFuncInfo cfi_math_abs = new CFuncInfo(math_abs_fn, "math.math_abs");
  private static readonly CFuncInfo cfi_math_acos = new CFuncInfo(math_acos_fn, "math.math_acos");
  private static readonly CFuncInfo cfi_math_asin = new CFuncInfo(math_asin_fn, "math.math_asin");
  private static readonly CFuncInfo cfi_math_atan = new CFuncInfo(math_atan_fn, "math.math_atan");
  private static readonly CFuncInfo cfi_math_ceil = new CFuncInfo(math_ceil_fn, "math.math_ceil");
  private static readonly CFuncInfo cfi_math_cos = new CFuncInfo(math_cos_fn, "math.math_cos");
  private static readonly CFuncInfo cfi_math_deg = new CFuncInfo(math_deg_fn, "math.math_deg");
  private static readonly CFuncInfo cfi_math_exp = new CFuncInfo(math_exp_fn, "math.math_exp");
  private static readonly CFuncInfo cfi_math_toint = new CFuncInfo(math_toint_fn, "math.math_toint");
  private static readonly CFuncInfo cfi_math_floor = new CFuncInfo(math_floor_fn, "math.math_floor");
  private static readonly CFuncInfo cfi_math_fmod = new CFuncInfo(math_fmod_fn, "math.math_fmod");
  private static readonly CFuncInfo cfi_math_frexp = new CFuncInfo(math_frexp_fn, "math.math_frexp");
  private static readonly CFuncInfo cfi_math_ult = new CFuncInfo(math_ult_fn, "math.math_ult");
  private static readonly CFuncInfo cfi_math_ldexp = new CFuncInfo(math_ldexp_fn, "math.math_ldexp");
  private static readonly CFuncInfo cfi_math_log = new CFuncInfo(math_log_fn, "math.math_log");
  private static readonly CFuncInfo cfi_math_max = new CFuncInfo(math_max_fn, "math.math_max");
  private static readonly CFuncInfo cfi_math_min = new CFuncInfo(math_min_fn, "math.math_min");
  private static readonly CFuncInfo cfi_math_modf = new CFuncInfo(math_modf_fn, "math.math_modf");
  private static readonly CFuncInfo cfi_math_rad = new CFuncInfo(math_rad_fn, "math.math_rad");
  private static readonly CFuncInfo cfi_math_sin = new CFuncInfo(math_sin_fn, "math.math_sin");
  private static readonly CFuncInfo cfi_math_sqrt = new CFuncInfo(math_sqrt_fn, "math.math_sqrt");
  private static readonly CFuncInfo cfi_math_tan = new CFuncInfo(math_tan_fn, "math.math_tan");
  private static readonly CFuncInfo cfi_math_type = new CFuncInfo(math_type_fn, "math.math_type");

  private static readonly luaL_RegCFI[] mathlib = {
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
    lua_pushnumber(L, JDouble.POSITIVE_INFINITY);
    lua_setfieldi(L, -2, Lt.huge);
    lua_pushinteger(L, LUA_MAXINTEGER);
    lua_setfieldi(L, -2, Lt.maxinteger);
    lua_pushinteger(L, LUA_MININTEGER);
    lua_setfieldi(L, -2, Lt.mininteger);
    setrandfunc(L);
    return 1;
  }

  public static readonly lua_CFunction luaopen_math_fn = Lmathlib.luaopen_math;
}
