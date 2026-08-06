/*
** $Id: luaconf.h $
** Configuration file for Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>
#include <stddef.h>


/*
** {====================================================================
** System Configuration
** =====================================================================
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
inline constexpr std::string_view LUA_PATH_SEP = ";";
inline constexpr std::string_view LUA_PATH_MARK = "?";
inline constexpr std::string_view LUA_EXEC_DIR = "!";

/*
@@ LUA_PATH_DEFAULT is the default path that Lua uses to look for
** Lua libraries.
@@ LUA_CPATH_DEFAULT is the default path that Lua uses to look for
** C libraries.
** CHANGE them if your machine has a non-conventional directory
** hierarchy or if you want to install your libraries in
** non-conventional directories.
*/

inline constexpr std::string_view LUA_PATH_DEFAULT = "/usr/local/share/lua/5.5/?.lua;/usr/local/share/lua/5.5/?/init.lua;/usr/local/lib/lua/5.5/?.lua;/usr/local/lib/lua/5.5/?/init.lua;./?.lua;./?/init.lua";
inline constexpr std::string_view LUA_CPATH_DEFAULT = "/usr/local/lib/lua/5.5/?.so;/usr/local/lib/lua/5.5/loadall.so;./?.so";

/*
** LUA_DIRSEP is the directory separator (for submodules).
*/
inline constexpr std::string_view LUA_DIRSEP = "/";

/*
** LUA_IGMARK is a mark to ignore all after it when building the
** module name (e.g., used to build the luaopen_ function name).
** Typically, the suffix after the mark is the module version,
** as in "mod-v1.2.so".
*/
inline constexpr std::string_view LUA_IGMARK = "-";
inline constexpr std::string_view CONFIG = "/\n;\n?\n!\n-\n";

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



constexpr long long LUA_MAXINTEGER = LLONG_MAX;
constexpr long long LUA_MININTEGER = LLONG_MIN;

constexpr unsigned long long LUA_MAXUNSIGNED = ULLONG_MAX;

/* }================================================================== */

/*
** {==================================================================
** Other C details
** ===================================================================
*/

#include <locale.h>
static inline char lua_getlocaledecpoint(void)
{
  return localeconv()->decimal_point[0];
}


#define luai_likely(x)   (__builtin_expect(((x) != 0), 1))
#define luai_unlikely(x) (__builtin_expect(((x) != 0), 0))

/* }================================================================== */

constexpr int LUA_IDSIZE = 60;
