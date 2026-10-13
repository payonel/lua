/*
** $Id: llimits.h $
** Limits, basic types, and some other 'installation-dependent' definitions
** See Copyright Notice in lua.h
*/

#pragma once

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <bit>
#include <limits>
#include <cstddef>

#include "lua.h"

#if defined(LUA_DEBUG)
#include "ltests.h"
#endif
#include "lhack.h"

namespace lua::object
{

static_assert(std::endian::native == std::endian::little,
              "this port supports little-endian hosts only");

static_assert(std::numeric_limits<lua_Number>::is_iec559 &&
                  sizeof(lua_Number) == 8,
              "this port requires lua_Number to be IEEE-754 binary64");
static_assert(sizeof(lua_Integer) == 8,
              "this port requires a 64-bit lua_Integer");

/*
** 'l_mem' is a signed integer big enough to count the total memory
** used by Lua.  (It is signed due to the use of debt in several
** computations.) 'lu_mem' is a corresponding unsigned type.  Usually,
** 'ptrdiff_t' should work, but we use 'long' for 16-bit machines.
*/
typedef ptrdiff_t l_mem;
typedef size_t lu_mem;

constexpr l_mem MAX_LMEM = PTRDIFF_MAX;

/* chars used as small naturals (so that 'char' is reserved for characters) */
typedef unsigned char lu_byte;
typedef signed char ls_byte;

/* Type for thread status/error codes */
typedef lu_byte TStatus;

/* The C API still uses 'int' for status/error codes */

/* maximum value for size_t */
constexpr size_t MAX_SIZET = ~(size_t)0;

/*
** Maximum size for strings and userdata visible for Lua; should be
** representable as a lua_Integer and as a size_t.
*/
constexpr size_t MAX_SIZE = LUA_MAXINTEGER;
static_assert(sizeof(size_t) >= sizeof(lua_Integer));

/*
** test whether an unsigned value is a power of 2 (or zero)
*/
constexpr bool ispow2(lua_Unsigned x)
{
  return (x & (x - 1)) == 0;
}

/* number of chars of a literal string without the ending \0 */


static inline unsigned int point2uint(const void* p)
{
  return (size_t)(p) & UINT_MAX;
}

static inline unsigned int point2uint(lua_CFunction p)
{
  return (size_t)(p) & UINT_MAX;
}

/* types of 'usual argument conversions' for lua_Number and lua_Integer */


/*
** cast a size_t to lua_Integer: These casts are always valid for
** sizes of Lua objects (see MAX_SIZE)
*/

/* Cast a ptrdiff_t to size_t, when it is known that the minuend
** comes from the subtrahend (the base)
*/

/* ptrdiff_t to lua_Integer */

/*
** non-return type
*/

/*
** An unsigned with (at least) 4 bytes
*/
typedef unsigned int l_uint32;

/*
** modulo: defined as 'a - floor(a/b)*b'; the direct computation
** using this definition has several problems with rounding errors,
** so it is better to use 'fmod'. 'fmod' gives the result of
** 'a - trunc(a/b)*b', and therefore must be corrected when
** 'trunc(a/b) ~= floor(a/b)'. That happens when the division has a
** non-integer negative result: non-integer result is equivalent to
** a non-zero remainder 'm'; negative result is equivalent to 'a' and
** 'b' with different signs, or 'm' and 'b' with different signs
** (as the result 'm' of 'fmod' has the same sign of 'a').
*/

#define l_forceinline inline __attribute__((always_inline))

static l_forceinline lua_Number luai_numadd(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a + b; }
static l_forceinline lua_Number luai_numsub(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a - b; }
static l_forceinline lua_Number luai_nummul(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a * b; }
static l_forceinline lua_Number luai_numdiv(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a / b; }
static l_forceinline lua_Number luai_numpow(lua_State* L, lua_Number a, lua_Number b) { (void) L; return (b == 2) ? a * a : pow(a, b); }
static l_forceinline lua_Number luai_numidiv(lua_State* L, lua_Number a, lua_Number b) { return floor(luai_numdiv(L, a, b)); }
static l_forceinline lua_Number luai_numunm(lua_State* L, lua_Number a) { (void) L; return -a; }

static l_forceinline bool luai_numeq(lua_Number a, lua_Number b) { return a == b; }
static l_forceinline bool luai_numlt(lua_Number a, lua_Number b) { return a < b; }
static l_forceinline bool luai_numle(lua_Number a, lua_Number b) { return a <= b; }
static l_forceinline bool luai_numgt(lua_Number a, lua_Number b) { return a > b; }
static l_forceinline bool luai_numge(lua_Number a, lua_Number b) { return a >= b; }
static l_forceinline bool luai_numisnan(lua_Number a) { return !(a == a); }

static l_forceinline lua_Number luai_nummod(lua_State* L, lua_Number a, lua_Number b)
{
  lua_Number m = fmod(a, b);
  (void) L;
  if ((m > 0) ? b < 0 : (m < 0 && b > 0))
    m += b;
  return m;
}

/*
** lua_numbertointeger converts a float number with an integral value
** to an integer, or returns 0 if the float is not within the range of
** a lua_Integer.  (The range comparisons are tricky because of
** rounding. The tests here assume a two-complement representation,
** where MININTEGER always has an exact representation as a float;
** MAXINTEGER may not have one, and therefore its conversion to float
** may have an ill-defined value.)
*/
static inline lua_Unsigned l_umod(lua_Unsigned a, lua_Unsigned b)
{
  return a % b;
}

static inline bool l_ult(lua_Integer a, lua_Integer b)
{
  return (lua_Unsigned)a < (lua_Unsigned)b;
}

static l_forceinline lua_Integer l_intadd(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a + (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intsub(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a - (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intmul(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a * (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intband(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a & (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intbor(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a | (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intbxor(lua_Integer a, lua_Integer b)
{
  return (lua_Integer)((lua_Unsigned)a ^ (lua_Unsigned)b);
}

static l_forceinline lua_Integer l_intunm(lua_Integer a)
{
  return (lua_Integer)(0u - (lua_Unsigned)a);
}

static l_forceinline lua_Integer l_intbnot(lua_Integer a)
{
  return (lua_Integer)(~(lua_Unsigned)a);
}

struct IntResult
{
  lua_Integer v;
};

struct NumResult
{
  lua_Number v;
};

static inline bool lua_numbertointeger(lua_Number n, IntResult* p)
{
  if (n >= (lua_Number)LUA_MININTEGER && n < -(lua_Number)LUA_MININTEGER)
  {
    p->v = (lua_Integer)n;
    return true;
  }
  return false;
}

/* Give these macros simpler names for internal use */
#define l_likely(x)   luai_likely(x)
#define l_unlikely(x) luai_unlikely(x)

}
