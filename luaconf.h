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
** Definitions here affect the Lua ABI: by making changes here, you
** ensure that all software connected to Lua, such as C libraries,
** will be compiled with the same configuration.
** ===================================================================
*/

/*
** {====================================================================
** System Configuration
** =====================================================================
*/

/*
** The Lua stand-alone application dynamically links a 'readline'
** facility for its REPL.  LUA_READLINELIB is the name of the library
** it will look for those facilities.  If lua.cpp cannot open the
** specified library, it will generate a warning and then run without
** 'readline'.
*/
constexpr char LUA_READLINELIB[] = "libreadline.so";

/*
** LUAI_IS32INT is true iff 'int' has (at least) 32 bits.
*/
constexpr int LUAI_IS32INT = ((UINT_MAX >> 30) >= 3);

/* }================================================================== */

/*
** {==================================================================
** Configuration for Number types. These options should not be
** set externally, because any other code connected to Lua must
** use the same configuration.
** ===================================================================
*/

/*
** Numeric types are pinned for this port (managed-target contract,
** ryanbean decision 2026-07-28): lua_Number is IEEE-754 binary64
** ('double') and lua_Integer is 64-bit ('long long') -- the exact
** Java 'double'/'long' shapes. Enforced by static_asserts in
** llimits.h. The upstream LUA_INT_TYPE/LUA_FLOAT_TYPE selection
** machinery (LUA_32BITS, LUA_C89_NUMBERS) is deleted.
*/

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
static const InternalString* LUA_PATH_SEP = lit<";">();
static const InternalString* LUA_PATH_MARK = lit<"?">();
static const InternalString* LUA_EXEC_DIR = lit<"!">();

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
static const InternalString* LUA_PATH_DEFAULT = lit<"/usr/local/share/lua/5.5/?.lua;/usr/local/share/lua/5.5/?/init.lua;/usr/local/lib/lua/5.5/?.lua;/usr/local/lib/lua/5.5/?/init.lua;./?.lua;./?/init.lua">();
static const InternalString* LUA_CPATH_DEFAULT = lit<"/usr/local/lib/lua/5.5/?.so;/usr/local/lib/lua/5.5/loadall.so;./?.so">();

/*
** LUA_DIRSEP is the directory separator (for submodules).
*/
static const InternalString* LUA_DIRSEP = lit<"/">();

/*
** LUA_IGMARK is a mark to ignore all after it when building the
** module name (e.g., used to build the luaopen_ function name).
** Typically, the suffix after the mark is the module version,
** as in "mod-v1.2.so".
*/
static const InternalString* LUA_IGMARK = lit<"-">();
static const InternalString* CONFIG = lit<"/\n;\n?\n!\n-\n">();

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
** Configuration for Numbers (low-level part).
** ===================================================================
*/

/*
** Float-to-string conversions (in lobject.cpp) use 15 significant
** digits — the maximum that respects tostring(tonumber(numeral)) ==
** numeral (floor(log10(2^53)) = 15 for binary64) — upgrading to 17,
** the minimum that ensures tonumber(tostring(number)) == number, when
** the shorter form does not round-trip.
*/

/* lua_Number: IEEE-754 binary64 ('double');
** lua_Integer: 64-bit ('long long');
** lua_Unsigned: 'unsigned long long'.
** The typedefs live in lua.h. */

/*
** LUA_MAXINTEGER is the maximum value for a lua_Integer.
** LUA_MININTEGER is the minimum value for a lua_Integer.
** LUA_MAXUNSIGNED is the maximum value for a lua_Unsigned.
*/
constexpr long long LUA_MAXINTEGER = LLONG_MAX;
constexpr long long LUA_MININTEGER = LLONG_MIN;

constexpr unsigned long long LUA_MAXUNSIGNED = ULLONG_MAX;

/* }================================================================== */

/*
** {==================================================================
** Other C details
** ===================================================================
*/

/*
** lua_getlocaledecpoint gets the locale "radix character" (decimal point).
** The conversion core (to_chars/from_chars) is always '.'-canonical;
** this is used only by the explicit locale shims at the input/output
** boundaries.
*/
#include <locale.h>
static inline char lua_getlocaledecpoint(void)
{
  return localeconv()->decimal_point[0];
}

/* lua_KContext (the type of the context for continuation functions) is
** 'ptrdiff_t'; the typedef lives in lua.h. */

/*
** macros to improve jump prediction, used mostly for error handling
** and debug facilities. (Some macros in the Lua API use these macros.)
*/
#define luai_likely(x)   (__builtin_expect(((x) != 0), 1))
#define luai_unlikely(x) (__builtin_expect(((x) != 0), 0))

/* }================================================================== */

/*
** LUA_IDSIZE gives the maximum size for the description of the source
** of a function in debug information.
** (Part of the API: must be the same when you compile Lua and when you
** compile code that links to Lua.)
*/
constexpr int LUA_IDSIZE = 60;
