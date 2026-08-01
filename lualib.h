/*
** $Id: lualib.h $
** Lua standard libraries
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

/* version suffix for environment variable names */
/* version suffix for environment variable names;
   keep in sync with LUA_VERSION_MAJOR/MINOR (lua.h) */
inline const std::string_view LUA_VERSUFFIX = lit<"_5_5">()->view();

constexpr int LUA_GLIBK = 1;
LUAMOD_API int(luaopen_base)(lua_State* L);

inline const InternalString* const LUA_LOADLIBNAME = lit<"package">();
constexpr int LUA_LOADLIBK = (LUA_GLIBK << 1);
LUAMOD_API int(luaopen_package)(lua_State* L);

inline const InternalString* const LUA_COLIBNAME = lit<"coroutine">();
constexpr int LUA_COLIBK = (LUA_LOADLIBK << 1);
LUAMOD_API int(luaopen_coroutine)(lua_State* L);

inline const InternalString* const LUA_DBLIBNAME = lit<"debug">();
constexpr int LUA_DBLIBK = (LUA_COLIBK << 1);
LUAMOD_API int(luaopen_debug)(lua_State* L);

inline const InternalString* const LUA_IOLIBNAME = lit<"io">();
constexpr int LUA_IOLIBK = (LUA_DBLIBK << 1);
LUAMOD_API int(luaopen_io)(lua_State* L);

inline const InternalString* const LUA_MATHLIBNAME = lit<"math">();
constexpr int LUA_MATHLIBK = (LUA_IOLIBK << 1);
LUAMOD_API int(luaopen_math)(lua_State* L);

inline const InternalString* const LUA_OSLIBNAME = lit<"os">();
constexpr int LUA_OSLIBK = (LUA_MATHLIBK << 1);
LUAMOD_API int(luaopen_os)(lua_State* L);

inline const InternalString* const LUA_STRLIBNAME = lit<"string">();
constexpr int LUA_STRLIBK = (LUA_OSLIBK << 1);
LUAMOD_API int(luaopen_string)(lua_State* L);

inline const InternalString* const LUA_TABLIBNAME = lit<"table">();
constexpr int LUA_TABLIBK = (LUA_STRLIBK << 1);
LUAMOD_API int(luaopen_table)(lua_State* L);

inline const InternalString* const LUA_UTF8LIBNAME = lit<"utf8">();
constexpr int LUA_UTF8LIBK = (LUA_TABLIBK << 1);
LUAMOD_API int(luaopen_utf8)(lua_State* L);

/* open selected libraries */
LUALIB_API void(luaL_openselectedlibs)(lua_State* L, int load, int preload);

/* open all libraries */
LUALIB_API void luaL_openlibs(lua_State* L);
