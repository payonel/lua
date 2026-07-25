/*
** $Id: llimits.h $
** Limits, basic types, and some other 'installation-dependent' definitions
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <bit>
#include <cstddef>

#include "lua.h"

/*
** This port supports only little-endian hosts (ryanbean decision,
** 2026-07-25). Byte order is used naively wherever it leaks into
** observable behavior — those places carry a "little-endian" comment
** and would have behaved differently on a big-endian host (which
** instead fails here at compile time).
*/
static_assert(std::endian::native == std::endian::little,
              "this port supports little-endian hosts only");

/*
** 'l_mem' is a signed integer big enough to count the total memory
** used by Lua.  (It is signed due to the use of debt in several
** computations.) 'lu_mem' is a corresponding unsigned type.  Usually,
** 'ptrdiff_t' should work, but we use 'long' for 16-bit machines.
*/
typedef ptrdiff_t l_mem;
typedef size_t lu_mem;

constexpr l_mem MAX_LMEM = (l_mem)((((lu_mem)(1)) << (((int)(sizeof(l_mem) * CHAR_BIT)) - 1)) - 1);

/* chars used as small naturals (so that 'char' is reserved for characters) */
typedef unsigned char lu_byte;
typedef signed char ls_byte;

/* Type for thread status/error codes */
typedef lu_byte TStatus;

/* The C API still uses 'int' for status/error codes */

/* maximum value for size_t */
constexpr size_t MAX_SIZET = (size_t)(~(size_t)0);

/*
** Maximum size for strings and userdata visible for Lua; should be
** representable as a lua_Integer and as a size_t.
*/
constexpr size_t MAX_SIZE = sizeof(size_t) < sizeof(lua_Integer) ? MAX_SIZET : ((size_t)(LUA_MAXINTEGER));

/*
** test whether an unsigned value is a power of 2 (or zero)
*/

/* number of chars of a literal string without the ending \0 */

/*
** conversion of pointer to unsigned integer: this is for hashing only;
** there is no problem if the integer cannot hold the whole pointer
** value. (In strict ISO C this may cause undefined behavior, but no
** actual machine seems to bother.)
*/
#if !defined(LUA_USE_C89) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
#if defined(UINTPTR_MAX) /* even in C99 this type is optional */
#define L_P2I uintptr_t
#else                   /* no 'intptr'? */
#define L_P2I uintmax_t /* use the largest available integer */
#endif
#else /* C89 option */
#define L_P2I size_t
#endif

static inline unsigned int point2uint(const void* p)
{
  return (unsigned int)((L_P2I)(p) & UINT_MAX);
}

/* overload for light C function pointers (not convertible to void*) */
static inline unsigned int point2uint(lua_CFunction p)
{
  return (unsigned int)((L_P2I)(p) & UINT_MAX);
}

/* types of 'usual argument conversions' for lua_Number and lua_Integer */
typedef LUAI_UACNUMBER l_uacNumber;
typedef LUAI_UACINT l_uacInt;

/*
** Internal assertions for in-house debugging.
** 'assert' is used literally throughout the core; no build mode
** defines NDEBUG, so assertions are active in both build modes
** (matching the managed-target model, where 'assert' is a runtime
** switch rather than a build flavor). Checks with side-effect-free
** setup are written as 'assert(f(...))' where 'f' returns 1.
*/
#include <assert.h>

#if !defined(l_castS2U)
#endif

/*
** cast a lua_Unsigned to a signed lua_Integer; this cast is
** not strict ISO C, but two-complement architectures should
** work fine.
*/
#if !defined(l_castU2S)
#endif

/*
** cast a size_t to lua_Integer: These casts are always valid for
** sizes of Lua objects (see MAX_SIZE)
*/

/* Cast a ptrdiff_t to size_t, when it is known that the minuend
** comes from the subtrahend (the base)
*/

/* ptrdiff_t to lua_Integer */

/*
** Special type equivalent to '(void*)' for functions (to suppress some
** warnings when converting function pointers)
*/
typedef void (*voidf)(void);

/*
** Macro to convert pointer-to-void* to pointer-to-function. This cast
** is undefined according to ISO C, but POSIX assumes that it works.
** (The '__extension__' in gnu compilers is only to avoid warnings.)
*/
#define cast_func(p) (__extension__(voidf)(p))

/*
** non-return type
*/
#if !defined(l_noret)

#define l_noret void __attribute__((noreturn))

#endif

/*
** An unsigned with (at least) 4 bytes
*/
typedef unsigned int l_uint32;

/*
** The luai_num* macros define the primitive operations over numbers.
*/

#if !defined(luai_numidiv)
#endif

/* float division */
#if !defined(luai_numdiv)
#endif

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
#if !defined(luai_nummod)
/*
** Force inlining for tiny hot helpers even at -Og (debug builds).
** Build-config attribute, in the same category as LUAI_FUNC/l_noret.
*/
#define l_forceinline inline __attribute__((always_inline))

/*
** Primitive operations over floats. Real functions (not macros) so the
** interpreter's op_arith* dispatch can pass them by name.
*/
static l_forceinline lua_Number luai_numadd(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a + b; }
static l_forceinline lua_Number luai_numsub(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a - b; }
static l_forceinline lua_Number luai_nummul(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a * b; }
static l_forceinline lua_Number luai_numdiv(lua_State* L, lua_Number a, lua_Number b) { (void) L; return a / b; }
static l_forceinline lua_Number luai_numpow(lua_State* L, lua_Number a, lua_Number b) { (void) L; return (b == 2) ? a * a : l_mathop(pow)(a, b); }
static l_forceinline lua_Number luai_numidiv(lua_State* L, lua_Number a, lua_Number b) { return l_floor(luai_numdiv(L, a, b)); }
static l_forceinline lua_Number luai_numunm(lua_State* L, lua_Number a) { (void) L; return -a; }

static l_forceinline int luai_numeq(lua_Number a, lua_Number b) { return a == b; }
static l_forceinline int luai_numlt(lua_Number a, lua_Number b) { return a < b; }
static l_forceinline int luai_numle(lua_Number a, lua_Number b) { return a <= b; }
static l_forceinline int luai_numgt(lua_Number a, lua_Number b) { return a > b; }
static l_forceinline int luai_numge(lua_Number a, lua_Number b) { return a >= b; }
static l_forceinline int luai_numisnan(lua_Number a) { return !(a == a); }

static l_forceinline lua_Number luai_nummod(lua_State* L, lua_Number a, lua_Number b)
{
  lua_Number m = l_mathop(fmod)(a, b);
  (void) L;
  if ((m > 0) ? b < 0 : (m < 0 && b > 0))
    m += b;
  return m;
}
#endif

/* exponentiation */
#if !defined(luai_numpow)
#endif

/* the others are quite standard operations */
#if !defined(luai_numadd)
#endif

/*
** lua_numbertointeger converts a float number with an integral value
** to an integer, or returns 0 if the float is not within the range of
** a lua_Integer.  (The range comparisons are tricky because of
** rounding. The tests here assume a two-complement representation,
** where MININTEGER always has an exact representation as a float;
** MAXINTEGER may not have one, and therefore its conversion to float
** may have an ill-defined value.)
*/
#define lua_numbertointeger(n, p) ((n) >= (LUA_NUMBER) (LUA_MININTEGER) && (n) < -(LUA_NUMBER) (LUA_MININTEGER) && (*(p) = (LUA_INTEGER) (n), 1))

/*
** LUAI_FUNC is a mark for all extern functions that are not to be
** exported to outside modules.
** LUAI_DDEF and LUAI_DDEC are marks for all extern (const) variables,
** none of which to be exported to outside modules (LUAI_DDEF for
** definitions and LUAI_DDEC for declarations).
** Elf and MACH/gcc (versions 3.2 and later) mark them as "hidden" to
** optimize access when Lua is compiled as a shared library. Not all elf
** targets support this attribute. Unfortunately, gcc does not offer
** a way to check whether the target offers that support, and those
** without support give a warning about it. To avoid these warnings,
** change to the default definition.
*/
#if !defined(LUAI_FUNC)

#define LUAI_FUNC __attribute__((visibility("internal"))) extern

#define LUAI_DDEC(dec) LUAI_FUNC dec
#define LUAI_DDEF      /* empty */

#endif

/* Give these macros simpler names for internal use */
#define l_likely(x)   luai_likely(x)
#define l_unlikely(x) luai_unlikely(x)

/*
** {==================================================================
** "Abstraction Layer" for basic report of messages and errors
** ===================================================================
*/

/* print a string */
#if !defined(lua_writestring)
#define lua_writestring(s, l) fwrite((s), sizeof(char), (l), stdout)
#endif

/* print a newline and flush the output */
#if !defined(lua_writeline)
#define lua_writeline() (lua_writestring("\n", 1), fflush(stdout))
#endif

/* print an error message */
#if !defined(lua_writestringerror)
#define lua_writestringerror(s, p) (fprintf(stderr, (s), (p)), fflush(stderr))
#endif

/* }================================================================== */
