/*
** $Id: luaconf.h $
** Configuration file for Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>
#include <stddef.h>

/*
** ===================================================================
** General Configuration File for Lua
**
** Some definitions here can be changed externally, through the compiler
** (e.g., with '-D' options): They are commented out or protected
** by '#if !defined' guards. However, several other definitions
** should be changed directly here, either because they affect the
** Lua ABI (by making the changes here, you ensure that all software
** connected to Lua, such as C libraries, will be compiled with the same
** configuration); or because they are seldom changed.
**
** Search for "@@" to find all configurable definitions.
** ===================================================================
*/

/*
** {====================================================================
** System Configuration: macros to adapt (if needed) Lua to some
** particular platform, for instance restricting it to C89.
** =====================================================================
*/

/*
@@ LUA_USE_C89 controls the use of non-ISO-C89 features.
** Define it if you want Lua to avoid the use of a few C99 features
** or Windows-specific features on Windows.
*/
/* #define LUA_USE_C89 */

/*
** By default, Lua on Windows use (some) specific Windows features
*/

/*
** When POSIX DLL ('LUA_USE_DLOPEN') is enabled, the Lua stand-alone
** application will try to dynamically link a 'readline' facility
** for its REPL.  In that case, LUA_READLINELIB is the name of the
** library it will look for those facilities.  If lua.c cannot open
** the specified library, it will generate a warning and then run
** without 'readline'.  If that macro is not defined, lua.c will not
** use 'readline'.
*/
#define LUA_USE_POSIX
#define LUA_USE_DLOPEN /* needs an extra library: -ldl */
#if !defined(LUA_READLINELIB)
#define LUA_READLINELIB "libreadline.so"
#endif

/*
@@ LUAI_IS32INT is true iff 'int' has (at least) 32 bits.
*/
#define LUAI_IS32INT ((UINT_MAX >> 30) >= 3)

/* }================================================================== */

/*
** {==================================================================
** Configuration for Number types. These options should not be
** set externally, because any other code connected to Lua must
** use the same configuration.
** ===================================================================
*/

/*
@@ LUA_INT_TYPE defines the type for Lua integers.
@@ LUA_FLOAT_TYPE defines the type for Lua floats.
** Lua should work fine with any mix of these options supported
** by your C compiler. The usual configurations are 64-bit integers
** and 'double' (the default), 32-bit integers and 'float' (for
** restricted platforms), and 'long'/'double' (for C compilers not
** compliant with C99, which may not have support for 'long long').
*/

/* predefined options for LUA_INT_TYPE */
#define LUA_INT_INT      1
#define LUA_INT_LONG     2
#define LUA_INT_LONGLONG 3

/* predefined options for LUA_FLOAT_TYPE */
#define LUA_FLOAT_FLOAT      1
#define LUA_FLOAT_DOUBLE     2
#define LUA_FLOAT_LONGDOUBLE 3

/* Default configuration ('long long' and 'double', for 64-bit Lua) */
#define LUA_INT_DEFAULT   LUA_INT_LONGLONG
#define LUA_FLOAT_DEFAULT LUA_FLOAT_DOUBLE

/*
@@ LUA_32BITS enables Lua with 32-bit integers and 32-bit floats.
*/
/* #define LUA_32BITS */

/*
@@ LUA_C89_NUMBERS ensures that Lua uses the largest types available for
** C89 ('long' and 'double'); Windows always has '__int64', so it does
** not need to use this case.
*/
#define LUA_C89_NUMBERS 0

#if defined(LUA_32BITS) /* { */
/*
** 32-bit integers and 'float'
*/
#define LUA_INT_TYPE   LUA_INT_INT
#define LUA_FLOAT_TYPE LUA_FLOAT_FLOAT

#elif LUA_C89_NUMBERS /* }{ */
/*
** largest types available for C89 ('long' and 'double')
*/
#define LUA_INT_TYPE   LUA_INT_LONG
#define LUA_FLOAT_TYPE LUA_FLOAT_DOUBLE

#else /* }{ */
/* use defaults */

#define LUA_INT_TYPE   LUA_INT_DEFAULT
#define LUA_FLOAT_TYPE LUA_FLOAT_DEFAULT

#endif /* } */

/* }================================================================== */

/*
** {==================================================================
** Configuration for Paths.
** ===================================================================
*/

/*
** LUA_PATH_SEP is the character that separates templates in a path.
** LUA_PATH_MARK is the string that marks the substitution points in a
** template.
** LUA_EXEC_DIR in a Windows path is replaced by the executable's
** directory.
*/
#define LUA_PATH_SEP  ";"
#define LUA_PATH_MARK "?"
#define LUA_EXEC_DIR  "!"

/*
@@ LUA_PATH_DEFAULT is the default path that Lua uses to look for
** Lua libraries.
@@ LUA_CPATH_DEFAULT is the default path that Lua uses to look for
** C libraries.
** CHANGE them if your machine has a non-conventional directory
** hierarchy or if you want to install your libraries in
** non-conventional directories.
*/

/* keep in sync with LUA_VERSION_MAJOR/MINOR (lua.h) */
#define LUA_VDIR "5.5"

#define LUA_ROOT "/usr/local/"
#define LUA_LDIR LUA_ROOT "share/lua/" LUA_VDIR "/"
#define LUA_CDIR LUA_ROOT "lib/lua/" LUA_VDIR "/"

#if !defined(LUA_PATH_DEFAULT)
#define LUA_PATH_DEFAULT LUA_LDIR "?.lua;" LUA_LDIR "?/init.lua;" LUA_CDIR "?.lua;" LUA_CDIR "?/init.lua;" \
                                  "./?.lua;"                                                               \
                                  "./?/init.lua"
#endif

#if !defined(LUA_CPATH_DEFAULT)
#define LUA_CPATH_DEFAULT LUA_CDIR "?.so;" LUA_CDIR "loadall.so;" \
                                   "./?.so"
#endif

/*
@@ LUA_DIRSEP is the directory separator (for submodules).
** CHANGE it if your machine does not use "/" as the directory separator
** and is not Windows. (On Windows Lua automatically uses "\".)
*/
#if !defined(LUA_DIRSEP)

#define LUA_DIRSEP "/"

#endif

/*
** LUA_IGMARK is a mark to ignore all after it when building the
** module name (e.g., used to build the luaopen_ function name).
** Typically, the suffix after the mark is the module version,
** as in "mod-v1.2.so".
*/
#define LUA_IGMARK "-"

/* }================================================================== */

/*
** {==================================================================
** Marks for exported symbols in the C code
** ===================================================================
*/

/*
@@ LUA_API is a mark for all core API functions.
@@ LUALIB_API is a mark for all auxiliary library functions.
@@ LUAMOD_API is a mark for all standard library opening functions.
** CHANGE them if you need to define those functions in some special way.
** For instance, if you want to create one Windows DLL with the core and
** the libraries, you may want to use the following definition (define
** LUA_BUILD_AS_DLL to get it).
*/

#define LUA_API extern "C"

/*
** More often than not the libs go together with the core.
*/
#define LUALIB_API LUA_API

/* Lua uses the "C name" when calling open functions */
#define LUAMOD_API extern "C"

/* }================================================================== */

/*
** {==================================================================
** Compatibility with previous versions
** ===================================================================
*/

/*
@@ LUA_COMPAT_LOOPVAR makes for-loop control variables not read-only,
** as they were in previous versions.
*/
#if !defined(LUA_COMPAT_LOOPVAR)
#define LUA_COMPAT_LOOPVAR 0
#endif

/*
@@ The following macros supply trivial compatibility for some
** changes in the API. The macros themselves document how to
** change your code to avoid using them.
** (Once more, these macros were officially removed in 5.3, but they are
** still available here.)
*/
#define lua_strlen(L, i) lua_rawlen(L, (i))

#define lua_objlen(L, i) lua_rawlen(L, (i))

#define lua_equal(L, idx1, idx2)    lua_compare(L, (idx1), (idx2), LUA_OPEQ)
#define lua_lessthan(L, idx1, idx2) lua_compare(L, (idx1), (idx2), LUA_OPLT)

/* }================================================================== */

/*
** {==================================================================
** Configuration for Numbers (low-level part).
** Change these definitions if no predefined LUA_FLOAT_* / LUA_INT_*
** satisfy your needs.
** ===================================================================
*/

/*
@@ LUAI_UACNUMBER is the result of a 'default argument promotion'
@@ over a floating number.
@@ l_floatatt(x) corrects float attribute 'x' to the proper float type
** by prefixing it with one of FLT/DBL/LDBL.
@@ LUA_NUMBER_FRMLEN is the length modifier for writing floats.
@@ LUA_NUMBER_FMT is the format for writing floats with the maximum
** number of digits that respects tostring(tonumber(numeral)) == numeral.
** (That would be floor(log10(2^n)), where n is the number of bits in
** the float mantissa.)
@@ LUA_NUMBER_FMT_N is the format for writing floats with the minimum
** number of digits that ensures tonumber(tostring(number)) == number.
** (That would be LUA_NUMBER_FMT+2.)
@@ l_mathop allows the addition of an 'l' or 'f' to all math operations.
@@ l_floor takes the floor of a float.
@@ lua_str2number converts a decimal numeral to a number.
*/

/* The following definition is good for most cases here */

#define l_floor(x) (l_mathop(floor)(x))

/* now the variable definitions */

#if LUA_FLOAT_TYPE == LUA_FLOAT_FLOAT /* { single float */

#define LUA_NUMBER float

#define l_floatatt(n) (FLT_##n)

#define LUAI_UACNUMBER double

#define LUA_NUMBER_FRMLEN ""
#define LUA_NUMBER_FMT    "%.7g"
#define LUA_NUMBER_FMT_N  "%.9g"

#define l_mathop(op) op##f

#define lua_str2number(s, p) strtof((s), (p))

#elif LUA_FLOAT_TYPE == LUA_FLOAT_LONGDOUBLE /* }{ long double */

#define LUA_NUMBER long double

#define l_floatatt(n) (LDBL_##n)

#define LUAI_UACNUMBER long double

#define LUA_NUMBER_FRMLEN "L"
#define LUA_NUMBER_FMT    "%.19Lg"
#define LUA_NUMBER_FMT_N  "%.21Lg"

#define l_mathop(op) op##l

#define lua_str2number(s, p) strtold((s), (p))

#elif LUA_FLOAT_TYPE == LUA_FLOAT_DOUBLE /* }{ double */

#define LUA_NUMBER double

#define l_floatatt(n) (DBL_##n)

#define LUAI_UACNUMBER double

#define LUA_NUMBER_FRMLEN ""
#define LUA_NUMBER_FMT    "%.15g"
#define LUA_NUMBER_FMT_N  "%.17g"

#define l_mathop(op) op

#define lua_str2number(s, p) strtod((s), (p))

#else /* }{ */

#error "numeric float type not defined"

#endif /* } */

/*
@@ LUA_UNSIGNED is the unsigned version of LUA_INTEGER.
@@ LUAI_UACINT is the result of a 'default argument promotion'
@@ over a LUA_INTEGER.
@@ LUA_INTEGER_FRMLEN is the length modifier for reading/writing integers.
@@ LUA_INTEGER_FMT is the format for writing integers.
@@ LUA_MAXINTEGER is the maximum value for a LUA_INTEGER.
@@ LUA_MININTEGER is the minimum value for a LUA_INTEGER.
@@ LUA_MAXUNSIGNED is the maximum value for a LUA_UNSIGNED.
@@ lua_integer2str converts an integer to a string.
*/

/* The following definitions are good for most cases here */

#define LUA_INTEGER_FMT "%" LUA_INTEGER_FRMLEN "d"

#define LUAI_UACINT LUA_INTEGER

#define lua_integer2str(s, sz, n) l_sprintf((s), sz, LUA_INTEGER_FMT, (LUAI_UACINT) (n))

/*
** use LUAI_UACINT here to avoid problems with promotions (which
** can turn a comparison between unsigneds into a signed comparison)
*/
#define LUA_UNSIGNED unsigned LUAI_UACINT

/* now the variable definitions */

#if LUA_INT_TYPE == LUA_INT_INT /* { int */

#define LUA_INTEGER        int
#define LUA_INTEGER_FRMLEN ""

#define LUA_MAXINTEGER INT_MAX
#define LUA_MININTEGER INT_MIN

#define LUA_MAXUNSIGNED UINT_MAX

#elif LUA_INT_TYPE == LUA_INT_LONG /* }{ long */

#define LUA_INTEGER        long
#define LUA_INTEGER_FRMLEN "l"

#define LUA_MAXINTEGER LONG_MAX
#define LUA_MININTEGER LONG_MIN

#define LUA_MAXUNSIGNED ULONG_MAX

#elif LUA_INT_TYPE == LUA_INT_LONGLONG /* }{ long long */

/* use presence of macro LLONG_MAX as proxy for C99 compliance */
/* use ISO C99 stuff */

#define LUA_INTEGER        long long
#define LUA_INTEGER_FRMLEN "ll"

#define LUA_MAXINTEGER LLONG_MAX
#define LUA_MININTEGER LLONG_MIN

#define LUA_MAXUNSIGNED ULLONG_MAX

#else /* }{ */

#error "numeric integer type not defined"

#endif /* } */

/* }================================================================== */

/*
** {==================================================================
** Dependencies with C99 and other C details
** ===================================================================
*/

/*
@@ lua_pointer2str converts a pointer to a readable string in a
** non-specified way.
*/
#define lua_pointer2str(buff, sz, p) l_sprintf(buff, sz, "%p", p)

/*
** 'strtof' and 'opf' variants for math functions are not valid in
** C89. Otherwise, the macro 'HUGE_VALF' is a good proxy for testing the
** availability of these variants. ('math.h' is already included in
** all files that use these macros.)
*/

/*
@@ LUA_KCONTEXT is the type of the context ('ctx') for continuation
** functions.  It must be a numerical type; Lua will use 'intptr_t' if
** available, otherwise it will use 'ptrdiff_t' (the nearest thing to
** 'intptr_t' in C89)
*/
#define LUA_KCONTEXT ptrdiff_t

#if !defined(LUA_USE_C89) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
#endif

/*
@@ lua_getlocaledecpoint gets the locale "radix character" (decimal point).
** Change that if you do not want to use C locales. (Code using this
** macro must include the header 'locale.h'.)
*/
#if !defined(lua_getlocaledecpoint)
#define lua_getlocaledecpoint() (localeconv()->decimal_point[0])
#endif

/*
** macros to improve jump prediction, used mostly for error handling
** and debug facilities. (Some macros in the Lua API use these macros.
** Define LUA_NOBUILTIN if you do not want '__builtin_expect' in your
** code.)
*/
#if !defined(luai_likely)

#define luai_likely(x)   (__builtin_expect(((x) != 0), 1))
#define luai_unlikely(x) (__builtin_expect(((x) != 0), 0))

#endif

/* }================================================================== */

/*
** {==================================================================
** Language Variations
** =====================================================================
*/

/*
@@ LUA_NOCVTN2S/LUA_NOCVTS2N control how Lua performs some
** coercions. Define LUA_NOCVTN2S to turn off automatic coercion from
** numbers to strings. Define LUA_NOCVTS2N to turn off automatic
** coercion from strings to numbers.
*/
/* #define LUA_NOCVTN2S */
/* #define LUA_NOCVTS2N */

/*
@@ LUA_USE_APICHECK turns on several consistency checks on the C API.
** Define it as a help when debugging C code.
*/
/* #define LUA_USE_APICHECK */

/* }================================================================== */

/*
** {==================================================================
** Macros that affect the API and must be stable (that is, must be the
** same when you compile Lua and when you compile code that links to
** Lua).
** =====================================================================
*/

/*
@@ LUA_IDSIZE gives the maximum size for the description of the source
** of a function in debug information.
** CHANGE it if you want a different size.
*/
#define LUA_IDSIZE 60

/*
@@ LUAI_MAXALIGN defines fields that, when used in a union, ensure
** maximum alignment for the other items in that union.
*/
/* use ISO C99 stuff */
#define LUAI_MAXALIGN \
  long double u;      \
  void* s;            \
  long long l

/* }================================================================== */

/* =================================================================== */

/*
** Local configuration. You can use this space to add your redefinitions
** without modifying the main part of the file.
*/
