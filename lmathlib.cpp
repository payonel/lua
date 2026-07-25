/*
** $Id: lmathlib.c $
** Standard mathematical library
** See Copyright Notice in lua.h
*/

#define lmathlib_c
#define LUA_LIB

#include "lprefix.h"

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

#undef PI
constexpr lua_Number PI = (l_mathop(3.141592653589793238462643383279502884));

static int math_abs(lua_State* L)
{
  if (lua_isinteger(L, 1))
  {
    lua_Integer n = lua_tointeger(L, 1);
    if (n < 0)
      n = (lua_Integer) (0u - (lua_Unsigned) n);
    lua_pushinteger(L, n);
  }
  else
    lua_pushnumber(L, l_mathop(fabs)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_sin(lua_State* L)
{
  lua_pushnumber(L, l_mathop(sin)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_cos(lua_State* L)
{
  lua_pushnumber(L, l_mathop(cos)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_tan(lua_State* L)
{
  lua_pushnumber(L, l_mathop(tan)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_asin(lua_State* L)
{
  lua_pushnumber(L, l_mathop(asin)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_acos(lua_State* L)
{
  lua_pushnumber(L, l_mathop(acos)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_atan(lua_State* L)
{
  lua_Number y = luaL_checknumber(L, 1);
  lua_Number x = luaL_optnumber(L, 2, 1);
  lua_pushnumber(L, l_mathop(atan2)(y, x));
  return 1;
}

static int math_toint(lua_State* L)
{
  int valid;
  lua_Integer n = lua_tointegerx(L, 1, &valid);
  if (l_likely(valid))
    lua_pushinteger(L, n);
  else
  {
    luaL_checkany(L, 1);
    luaL_pushfail(L); /* value is not convertible to integer */
  }
  return 1;
}

static void pushnumint(lua_State* L, lua_Number d)
{
  lua_Integer n;
  if (lua_numbertointeger(d, &n)) /* does 'd' fit in an integer? */
    lua_pushinteger(L, n);        /* result is integer */
  else
    lua_pushnumber(L, d); /* result is float */
}

static int math_floor(lua_State* L)
{
  if (lua_isinteger(L, 1))
    lua_settop(L, 1); /* integer is its own floor */
  else
  {
    lua_Number d = l_mathop(floor)(luaL_checknumber(L, 1));
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
    lua_Number d = l_mathop(ceil)(luaL_checknumber(L, 1));
    pushnumint(L, d);
  }
  return 1;
}

static int math_fmod(lua_State* L)
{
  if (lua_isinteger(L, 1) && lua_isinteger(L, 2))
  {
    lua_Integer d = lua_tointeger(L, 2);
    if ((lua_Unsigned) d + 1u <= 1u)
    { /* special cases: -1 or 0 */
      luaL_argcheck(L, d != 0, 2, "zero");
      lua_pushinteger(L, 0); /* avoid overflow with 0x80000... / -1 */
    }
    else
      lua_pushinteger(L, lua_tointeger(L, 1) % d);
  }
  else
    lua_pushnumber(L, l_mathop(fmod)(luaL_checknumber(L, 1),
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
    lua_Number ip = (n < 0) ? l_mathop(ceil)(n) : l_mathop(floor)(n);
    pushnumint(L, ip);
    /* fractional part (test needed for inf/-inf) */
    lua_pushnumber(L, (n == ip) ? l_mathop(0.0) : (n - ip));
  }
  return 2;
}

static int math_sqrt(lua_State* L)
{
  lua_pushnumber(L, l_mathop(sqrt)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_ult(lua_State* L)
{
  lua_Integer a = luaL_checkinteger(L, 1);
  lua_Integer b = luaL_checkinteger(L, 2);
  lua_pushboolean(L, (lua_Unsigned) a < (lua_Unsigned) b);
  return 1;
}

static int math_log(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  lua_Number res;
  if (lua_isnoneornil(L, 2))
    res = l_mathop(log)(x);
  else
  {
    lua_Number base = luaL_checknumber(L, 2);
    if (base == l_mathop(2.0))
      res = l_mathop(log2)(x);
    else if (base == l_mathop(10.0))
      res = l_mathop(log10)(x);
    else
      res = l_mathop(log)(x) / l_mathop(log)(base);
  }
  lua_pushnumber(L, res);
  return 1;
}

static int math_exp(lua_State* L)
{
  lua_pushnumber(L, l_mathop(exp)(luaL_checknumber(L, 1)));
  return 1;
}

static int math_deg(lua_State* L)
{
  lua_pushnumber(L, luaL_checknumber(L, 1) * (l_mathop(180.0) / PI));
  return 1;
}

static int math_rad(lua_State* L)
{
  lua_pushnumber(L, luaL_checknumber(L, 1) * (PI / l_mathop(180.0)));
  return 1;
}

static int math_frexp(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  int ep;
  lua_pushnumber(L, l_mathop(frexp)(x, &ep));
  lua_pushinteger(L, ep);
  return 2;
}

static int math_ldexp(lua_State* L)
{
  lua_Number x = luaL_checknumber(L, 1);
  int ep = (int) luaL_checkinteger(L, 2);
  lua_pushnumber(L, l_mathop(ldexp)(x, ep));
  return 1;
}

static int math_min(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int imin = 1;          /* index of current minimum value */
  int i;
  luaL_argcheck(L, n >= 1, 1, "value expected");
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
  luaL_argcheck(L, n >= 1, 1, "value expected");
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
    lua_pushstring(L, (lua_isinteger(L, 1)) ? "integer" : "float");
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

/*
** This code uses lots of shifts. ISO C does not allow shifts greater
** than or equal to the width of the type being shifted, so some shifts
** are written in convoluted ways to match that restriction. For
** preprocessor tests, it assumes a width of 32 bits, so the maximum
** shift there is 31 bits.
*/

/* number of binary digits in the mantissa of a float */
#define FIGS l_floatatt(MANT_DIG)

#if FIGS > 64
/* there are only 64 random bits; use them all */
#undef FIGS
#define FIGS 64
#endif

/* try to find an integer type with at least 64 bits */

/*
** Standard implementation, using 64-bit integers.
** If 'uint64_t' has more than 64 bits, the extra bits do not interfere
** with the 64 initial bits, except in a right shift. Moreover, the
** final result has to discard the extra bits.
*/

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
constexpr lua_Number scaleFIG = (l_mathop(0.5) / ((uint64_t) 1 << (FIGS - 1)));

static lua_Number I2d(uint64_t x)
{
  int64_t sx = (int64_t) (trim64(x) >> shift64_FIG);
  lua_Number res = (lua_Number) (sx) *scaleFIG;
  if (sx < 0)
    res += l_mathop(1.0); /* correct the two's complement if negative */
  assert(0 <= res && res < 1);
  return res;
}

/* convert a 'uint64_t' to a 'lua_Unsigned' */
constexpr lua_Unsigned I2UInt(lua_Integer x)
{
  return trim64((lua_Unsigned)x);
}

/* convert a 'lua_Unsigned' to a 'uint64_t' */
constexpr uint64_t Int2I(lua_Integer x)
{
  return (uint64_t)x;
}

/*
** A state uses four 'uint64_t' values.
*/
typedef struct
{
  uint64_t s[4];
} RanState;

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
  RanState* state = (RanState*) lua_touserdata(L, lua_upvalueindex(1));
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
      lua_pushinteger(L, ((lua_Integer)(I2UInt(rv)))); /* full random integer */
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
    return luaL_error(L, "wrong number of arguments");
  }
  /* random integer in the interval [low, up] */
  luaL_argcheck(L, low <= up, 1, "interval is empty");
  /* project random integer into the interval [0, up - low] */
  p = project(I2UInt(rv), ((lua_Unsigned)(up)) - ((lua_Unsigned)(low)), state);
  lua_pushinteger(L, ((lua_Integer)(p + ((lua_Unsigned)(low)))));
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
  lua_pushinteger(L, ((lua_Integer)(n1)));
  lua_pushinteger(L, ((lua_Integer)(n2)));
}

static int math_randomseed(lua_State* L)
{
  RanState* state = (RanState*) lua_touserdata(L, lua_upvalueindex(1));
  lua_Unsigned n1, n2;
  if (lua_isnone(L, 1))
  {
    n1 = luaL_makeseed(L);           /* "random" seed */
    n2 = I2UInt(nextrand(state->s)); /* in case seed is not that random... */
  }
  else
  {
    n1 = ((lua_Unsigned)(luaL_checkinteger(L, 1)));
    n2 = ((lua_Unsigned)(luaL_optinteger(L, 2, 0)));
  }
  setseed(L, state->s, n1, n2);
  return 2; /* return seeds */
}

static const luaL_Reg randfuncs[] = {
    {"random", math_random},
    {"randomseed", math_randomseed},
    {NULL, NULL}};

/*
** Register the random functions and initialize their state.
*/
static void setrandfunc(lua_State* L)
{
  RanState* state = (RanState*) lua_newuserdatauv(L, sizeof(RanState), 0);
  setseed(L, state->s, luaL_makeseed(L), 0); /* initialize with random seed */
  lua_pop(L, 2);                             /* remove pushed seeds */
  luaL_setfuncs(L, randfuncs, 1);
}

/* }================================================================== */

/*
** {==================================================================
** Deprecated functions (for compatibility only)
** ===================================================================
*/
/* }================================================================== */

static const luaL_Reg mathlib[] = {
    {"abs", math_abs},
    {"acos", math_acos},
    {"asin", math_asin},
    {"atan", math_atan},
    {"ceil", math_ceil},
    {"cos", math_cos},
    {"deg", math_deg},
    {"exp", math_exp},
    {"tointeger", math_toint},
    {"floor", math_floor},
    {"fmod", math_fmod},
    {"frexp", math_frexp},
    {"ult", math_ult},
    {"ldexp", math_ldexp},
    {"log", math_log},
    {"max", math_max},
    {"min", math_min},
    {"modf", math_modf},
    {"rad", math_rad},
    {"sin", math_sin},
    {"sqrt", math_sqrt},
    {"tan", math_tan},
    {"type", math_type},
    /* placeholders */
    {"random", NULL},
    {"randomseed", NULL},
    {"pi", NULL},
    {"huge", NULL},
    {"maxinteger", NULL},
    {"mininteger", NULL},
    {NULL, NULL}};

/*
** Open math library
*/
LUAMOD_API int luaopen_math(lua_State* L)
{
  luaL_newlib(L, mathlib);
  lua_pushnumber(L, PI);
  lua_setfield(L, -2, "pi");
  lua_pushnumber(L, (lua_Number) HUGE_VAL);
  lua_setfield(L, -2, "huge");
  lua_pushinteger(L, LUA_MAXINTEGER);
  lua_setfield(L, -2, "maxinteger");
  lua_pushinteger(L, LUA_MININTEGER);
  lua_setfield(L, -2, "mininteger");
  setrandfunc(L);
  return 1;
}
