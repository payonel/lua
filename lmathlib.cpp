/*
** $Id: lmathlib.c $
** Standard mathematical library
** See Copyright Notice in lua.h
*/



#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

#include <stdint.h>
#include "lua_internal.h"
#include "lobject.h"
#include "literals.h"

namespace lua::lib
{

constexpr lua_Number PI = (3.141592653589793238462643383279502884);

static int math_abs(lua_State* L)
{
  if (lua_isinteger(L, 1))
  {
    lua_Integer n = lua_tointeger(L, 1);
    if (n < 0)
      n = l_intunm(n);
    lua_pushinteger(L, n);
  }
  else
    lua_pushnumber(L, fabs(luaL_checknumber(L, 1)));
  return 1;
}

static int math_sin(lua_State* L)
{
  lua_pushnumber(L, sin(luaL_checknumber(L, 1)));
  return 1;
}

static int math_cos(lua_State* L)
{
  lua_pushnumber(L, cos(luaL_checknumber(L, 1)));
  return 1;
}

static int math_tan(lua_State* L)
{
  lua_pushnumber(L, tan(luaL_checknumber(L, 1)));
  return 1;
}

static int math_asin(lua_State* L)
{
  lua_pushnumber(L, asin(luaL_checknumber(L, 1)));
  return 1;
}

static int math_acos(lua_State* L)
{
  lua_pushnumber(L, acos(luaL_checknumber(L, 1)));
  return 1;
}

static int math_atan(lua_State* L)
{
  lua_Number y = luaL_checknumber(L, 1);
  lua_Number x = luaL_optnumber(L, 2, 1);
  lua_pushnumber(L, atan2(y, x));
  return 1;
}

static int math_toint(lua_State* L)
{
  std::optional<lua_Integer> n = lua_tointegerx(L, 1);
  if (l_likely(n.has_value()))
    lua_pushinteger(L, *n);
  else
  {
    luaL_checkany(L, 1);
    luaL_pushfail(L); /* value is not convertible to integer */
  }
  return 1;
}

static void pushnumint(lua_State* L, lua_Number d)
{
  IntResult n;
  if (lua_numbertointeger(d, &n)) /* does 'd' fit in an integer? */
    lua_pushinteger(L, n.v);        /* result is integer */
  else
    lua_pushnumber(L, d); /* result is float */
}

static int math_floor(lua_State* L)
{
  if (lua_isinteger(L, 1))
    lua_settop(L, 1); /* integer is its own floor */
  else
  {
    lua_Number d = floor(luaL_checknumber(L, 1));
    pushnumint(L, d);
  }
  return 1;
}

static int math_ceil(lua_State* L)
{
  if (lua_isinteger(L, 1))
    lua_settop(L, 1); /* integer is its own ceiling */
  else
  {
    lua_Number d = ceil(luaL_checknumber(L, 1));
    pushnumint(L, d);
  }
  return 1;
}

static int math_fmod(lua_State* L)
{
  if (lua_isinteger(L, 1) && lua_isinteger(L, 2))
  {
    lua_Integer d = lua_tointeger(L, 2);
    if (d == 0 || d == -1)
    { /* special cases: -1 or 0 */
      luaL_argchecki(L, d != 0, 2, lt::zero);
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

/*
** next function does not use 'modf', avoiding problems with 'double*'
** (which is not compatible with 'float*') when lua_Number is not
** 'double'.
*/
static int math_modf(lua_State* L)
{
  if (lua_isinteger(L, 1))
  {
    lua_settop(L, 1);     /* number is its own integer part */
    lua_pushnumber(L, 0); /* no fractional part */
  }
  else
  {
    lua_Number n = luaL_checknumber(L, 1);
    /* integer part (rounds toward zero) */
    lua_Number ip = (n < 0) ? ceil(n) : floor(n);
    pushnumint(L, ip);
    /* fractional part (test needed for inf/-inf) */
    lua_pushnumber(L, (n == ip) ? 0.0 : (n - ip));
  }
  return 2;
}

static int math_sqrt(lua_State* L)
{
  lua_pushnumber(L, sqrt(luaL_checknumber(L, 1)));
  return 1;
}

static int math_ult(lua_State* L)
{
  lua_Integer a = luaL_checkinteger(L, 1);
  lua_Integer b = luaL_checkinteger(L, 2);
  lua_pushboolean(L, l_ult(a, b));
  return 1;
}

static int math_log(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  lua_Number res;
  if (lua_isnoneornil(L, 2))
    res = log(x);
  else
  {
    lua_Number base = luaL_checknumber(L, 2);
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

static int math_exp(lua_State* L)
{
  lua_pushnumber(L, exp(luaL_checknumber(L, 1)));
  return 1;
}

static int math_deg(lua_State* L)
{
  lua_pushnumber(L, luaL_checknumber(L, 1) * (180.0 / PI));
  return 1;
}

static int math_rad(lua_State* L)
{
  lua_pushnumber(L, luaL_checknumber(L, 1) * (PI / 180.0));
  return 1;
}

static int math_frexp(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  int ep;
  lua_pushnumber(L, frexp(x, &ep));
  lua_pushinteger(L, ep);
  return 2;
}

static int math_ldexp(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  int ep = (int) luaL_checkinteger(L, 2);
  lua_pushnumber(L, ldexp(x, ep));
  return 1;
}

static int math_min(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int imin = 1;          /* index of current minimum value */
  int i;
  luaL_argchecki(L, n >= 1, 1, lt::value_expected);
  for (i = 2; i <= n; i++)
  {
    if (lua_compare(L, i, imin, LUA_OPLT))
      imin = i;
  }
  lua_pushvalue(L, imin);
  return 1;
}

static int math_max(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int imax = 1;          /* index of current maximum value */
  int i;
  luaL_argchecki(L, n >= 1, 1, lt::value_expected);
  for (i = 2; i <= n; i++)
  {
    if (lua_compare(L, imax, i, LUA_OPLT))
      imax = i;
  }
  lua_pushvalue(L, imax);
  return 1;
}

static int math_type(lua_State* L)
{
  if (lua_type(L, 1) == LUA_TNUMBER)
    lua_pushistringref(L, lua_isinteger(L, 1) ? lt::integer_2 : lt::float_kw);
  else
  {
    luaL_checkany(L, 1);
    luaL_pushfail(L);
  }
  return 1;
}

/*
** {==================================================================
** Pseudo-Random Number Generator based on 'xoshiro256**'.
** ===================================================================
*/

constexpr int FIGS = DBL_MANT_DIG;
static_assert(FIGS <= 64, "there are only 64 random bits");

/* try to find an integer type with at least 64 bits */

/* avoid using extra bits when needed */
constexpr lua_Unsigned trim64(lua_Unsigned x)
{
  return x & 0xffffffffffffffffu;
}

/* rotate left 'x' by 'n' bits */
static uint64_t rotl(uint64_t x, int n)
{
  return (x << n) | (trim64(x) >> (64 - n));
}

static uint64_t nextrand(uint64_t* state)
{
  uint64_t state0 = state[0];
  uint64_t state1 = state[1];
  uint64_t state2 = state[2] ^ state0;
  uint64_t state3 = state[3] ^ state1;
  uint64_t res = rotl(state1 * 5, 7) * 9;
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
constexpr int shift64_FIG = (64 - FIGS);

/* 2^(-FIGS) == 2^-1 / 2^(FIGS-1) */
constexpr lua_Number scaleFIG = (0.5 / ((uint64_t) 1 << (FIGS - 1)));

static lua_Number I2d(uint64_t x)
{
  int64_t sx = (trim64(x) >> shift64_FIG);
  lua_Number res = (lua_Number) (sx) *scaleFIG;
  if (sx < 0)
    res += 1.0; /* correct the two's complement if negative */
  assert(0 <= res && res < 1);
  return res;
}

constexpr lua_Unsigned I2UInt(lua_Integer x)
{
  return trim64(x);
}

constexpr uint64_t Int2I(lua_Integer x)
{
  return x;
}

struct RanState : lua_UserObject
{
  uint64_t s[4];
};

constexpr size_t RANSTATE_SIZE = 32;

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
static lua_Unsigned project(lua_Unsigned ran, lua_Unsigned n,
                            RanState* state)
{
  lua_Unsigned lim = n; /* to compute the Mersenne number */
  int sh;               /* how much to spread bits to the right in 'lim' */
  /* spread '1' bits in 'lim' until it becomes a Mersenne number */
  for (sh = 1; (lim & (lim + 1)) != 0; sh *= 2)
    lim |= (lim >> sh);               /* spread '1's to the right */
  while ((ran &= lim) > n)            /* project 'ran' into [0..lim] and test */
    ran = I2UInt(nextrand(state->s)); /* not inside [0..n]? try again */
  return ran;
}

static int math_random(lua_State* L)
{
  lua_Integer low, up;
  lua_Unsigned p;
  RanState* state = static_cast<RanState*>(lua_touserobject(L, lua_upvalueindex(1)));
  uint64_t rv = nextrand(state->s); /* next pseudo-random value */
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
    return luaL_errori(L, lt::wrong_number_of_arguments);
  }
  /* random integer in the interval [low, up] */
  luaL_argchecki(L, low <= up, 1, lt::interval_is_empty);
  /* project random integer into the interval [0, up - low] */
  p = project(I2UInt(rv), ((lua_Unsigned)(up)) - ((lua_Unsigned)(low)), state);
  lua_pushinteger(L, (p + ((lua_Unsigned)(low))));
  return 1;
}

static void setseed(lua_State* L, uint64_t* state,
                    lua_Unsigned n1, lua_Unsigned n2)
{
  int i;
  state[0] = Int2I(n1);
  state[1] = Int2I(0xff); /* avoid a zero state */
  state[2] = Int2I(n2);
  state[3] = Int2I(0);
  for (i = 0; i < 16; i++)
    nextrand(state); /* discard initial values to "spread" seed */
  lua_pushinteger(L, (n1));
  lua_pushinteger(L, (n2));
}

static int math_randomseed(lua_State* L)
{
  RanState* state = static_cast<RanState*>(lua_touserobject(L, lua_upvalueindex(1)));
  lua_Unsigned n1, n2;
  if (lua_isnone(L, 1))
  {
    n1 = luaL_makeseed(L);           /* "random" seed */
    n2 = I2UInt(nextrand(state->s)); /* in case seed is not that random... */
  }
  else
  {
    n1 = luaL_checkinteger(L, 1);
    n2 = luaL_optinteger(L, 2, 0);
  }
  setseed(L, state->s, n1, n2);
  return 2; /* return seeds */
}

static const luaL_Regi randfuncs[] = {
  {lt::random, math_random},
  {lt::randomseed, math_randomseed},
};

/*
** Register the random functions and initialize their state.
*/
static void setrandfunc(lua_State* L)
{
  std::unique_ptr<RanState> o = std::make_unique<RanState>();
  RanState* state = o.get();
  lua_newuserobject(L, std::move(o), RANSTATE_SIZE, 0);
  setseed(L, state->s, luaL_makeseed(L), 0); /* initialize with random seed */
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

static CFuncInfo cfi_math_abs("abs"_hs, math_abs);
static CFuncInfo cfi_math_acos("acos"_hs, math_acos);
static CFuncInfo cfi_math_asin("asin"_hs, math_asin);
static CFuncInfo cfi_math_atan("atan"_hs, math_atan);
static CFuncInfo cfi_math_ceil("ceil"_hs, math_ceil);
static CFuncInfo cfi_math_cos("cos"_hs, math_cos);
static CFuncInfo cfi_math_deg("deg"_hs, math_deg);
static CFuncInfo cfi_math_exp("exp"_hs, math_exp);
static CFuncInfo cfi_math_toint("tointeger"_hs, math_toint);
static CFuncInfo cfi_math_floor("floor"_hs, math_floor);
static CFuncInfo cfi_math_fmod("fmod"_hs, math_fmod);
static CFuncInfo cfi_math_frexp("frexp"_hs, math_frexp);
static CFuncInfo cfi_math_ult("ult"_hs, math_ult);
static CFuncInfo cfi_math_ldexp("ldexp"_hs, math_ldexp);
static CFuncInfo cfi_math_log("log"_hs, math_log);
static CFuncInfo cfi_math_max("max"_hs, math_max);
static CFuncInfo cfi_math_min("min"_hs, math_min);
static CFuncInfo cfi_math_modf("modf"_hs, math_modf);
static CFuncInfo cfi_math_rad("rad"_hs, math_rad);
static CFuncInfo cfi_math_sin("sin"_hs, math_sin);
static CFuncInfo cfi_math_sqrt("sqrt"_hs, math_sqrt);
static CFuncInfo cfi_math_tan("tan"_hs, math_tan);
static CFuncInfo cfi_math_type("type"_hs, math_type);

static const luaL_RegCFI mathlib[] = {
  {lt::abs_kw, &cfi_math_abs},
  {lt::acos, &cfi_math_acos},
  {lt::asin, &cfi_math_asin},
  {lt::atan, &cfi_math_atan},
  {lt::ceil_kw, &cfi_math_ceil},
  {lt::cos_kw, &cfi_math_cos},
  {lt::deg, &cfi_math_deg},
  {lt::exp_kw, &cfi_math_exp},
  {lt::tointeger, &cfi_math_toint},
  {lt::floor_kw, &cfi_math_floor},
  {lt::fmod, &cfi_math_fmod},
  {lt::frexp, &cfi_math_frexp},
  {lt::ult, &cfi_math_ult},
  {lt::ldexp, &cfi_math_ldexp},
  {lt::log_kw, &cfi_math_log},
  {lt::max, &cfi_math_max},
  {lt::min, &cfi_math_min},
  {lt::modf, &cfi_math_modf},
  {lt::rad, &cfi_math_rad},
  {lt::sin_kw, &cfi_math_sin},
  {lt::sqrt_kw, &cfi_math_sqrt},
  {lt::tan_kw, &cfi_math_tan},
  {lt::type, &cfi_math_type},
  {lt::random, nullptr},
  {lt::randomseed, nullptr},
  {lt::pi, nullptr},
  {lt::huge, nullptr},
  {lt::maxinteger, nullptr},
  {lt::mininteger, nullptr},
};

/*
** Open math library
*/
}

LUAMOD_API int luaopen_math(lua_State* L)
{
  luaL_newlibcfi(L, mathlib);
  lua_pushnumber(L, PI);
  lua_setfieldi(L, -2, lt::pi);
  lua_pushnumber(L, HUGE_VAL);
  lua_setfieldi(L, -2, lt::huge);
  lua_pushinteger(L, LUA_MAXINTEGER);
  lua_setfieldi(L, -2, lt::maxinteger);
  lua_pushinteger(L, LUA_MININTEGER);
  lua_setfieldi(L, -2, lt::mininteger);
  setrandfunc(L);
  return 1;
}

