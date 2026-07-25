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
inline constexpr std::string_view LUA_VERSUFFIX = "_5_5";

constexpr int LUA_GLIBK = 1;
LUAMOD_API int(luaopen_base)(lua_State* L);

constexpr char LUA_LOADLIBNAME[] = "package";
constexpr int LUA_LOADLIBK = (LUA_GLIBK << 1);
LUAMOD_API int(luaopen_package)(lua_State* L);

constexpr char LUA_COLIBNAME[] = "coroutine";
constexpr int LUA_COLIBK = (LUA_LOADLIBK << 1);
LUAMOD_API int(luaopen_coroutine)(lua_State* L);

constexpr char LUA_DBLIBNAME[] = "debug";
constexpr int LUA_DBLIBK = (LUA_COLIBK << 1);
LUAMOD_API int(luaopen_debug)(lua_State* L);

constexpr char LUA_IOLIBNAME[] = "io";
constexpr int LUA_IOLIBK = (LUA_DBLIBK << 1);
LUAMOD_API int(luaopen_io)(lua_State* L);

constexpr char LUA_MATHLIBNAME[] = "math";
constexpr int LUA_MATHLIBK = (LUA_IOLIBK << 1);
LUAMOD_API int(luaopen_math)(lua_State* L);

constexpr char LUA_OSLIBNAME[] = "os";
constexpr int LUA_OSLIBK = (LUA_MATHLIBK << 1);
LUAMOD_API int(luaopen_os)(lua_State* L);

constexpr char LUA_STRLIBNAME[] = "string";
constexpr int LUA_STRLIBK = (LUA_OSLIBK << 1);
LUAMOD_API int(luaopen_string)(lua_State* L);

constexpr char LUA_TABLIBNAME[] = "table";
constexpr int LUA_TABLIBK = (LUA_STRLIBK << 1);
LUAMOD_API int(luaopen_table)(lua_State* L);

constexpr char LUA_UTF8LIBNAME[] = "utf8";
constexpr int LUA_UTF8LIBK = (LUA_TABLIBK << 1);
LUAMOD_API int(luaopen_utf8)(lua_State* L);

/* open selected libraries */
LUALIB_API void(luaL_openselectedlibs)(lua_State* L, int load, int preload);

/* open all libraries */
#define luaL_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
