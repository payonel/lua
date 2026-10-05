/*
** $Id: lualib.h $
** Lua standard libraries
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

/* version suffix for environment variable names */
inline constexpr byte_view LUA_VERSUFFIX = "_5_5";

constexpr int LUA_GLIBK = 1;
LUAMOD_API int(luaopen_base)(lua_State* L);

inline constexpr byte_view LUA_LOADLIBNAME = "package";
constexpr int LUA_LOADLIBK = LUA_GLIBK << 1;
LUAMOD_API int(luaopen_package)(lua_State* L);

inline constexpr byte_view LUA_COLIBNAME = "coroutine";
constexpr int LUA_COLIBK = LUA_LOADLIBK << 1;
LUAMOD_API int(luaopen_coroutine)(lua_State* L);

inline constexpr byte_view LUA_DBLIBNAME = "debug";
constexpr int LUA_DBLIBK = LUA_COLIBK << 1;
LUAMOD_API int(luaopen_debug)(lua_State* L);

/* the hook function 'debug.sethook' installs (with the Lua hook kept in
   the registry); lets a host or library recognize such a hook */
lua_Hook(luaL_debughook)(void);

inline constexpr byte_view LUA_IOLIBNAME = "io";
constexpr int LUA_IOLIBK = LUA_DBLIBK << 1;
LUAMOD_API int(luaopen_io)(lua_State* L);

inline constexpr byte_view LUA_MATHLIBNAME = "math";
constexpr int LUA_MATHLIBK = LUA_IOLIBK << 1;
LUAMOD_API int(luaopen_math)(lua_State* L);

inline constexpr byte_view LUA_OSLIBNAME = "os";
constexpr int LUA_OSLIBK = LUA_MATHLIBK << 1;
LUAMOD_API int(luaopen_os)(lua_State* L);

inline constexpr byte_view LUA_STRLIBNAME = "string";
constexpr int LUA_STRLIBK = LUA_OSLIBK << 1;
LUAMOD_API int(luaopen_string)(lua_State* L);

inline constexpr byte_view LUA_TABLIBNAME = "table";
constexpr int LUA_TABLIBK = LUA_STRLIBK << 1;
LUAMOD_API int(luaopen_table)(lua_State* L);

inline constexpr byte_view LUA_UTF8LIBNAME = "utf8";
constexpr int LUA_UTF8LIBK = LUA_TABLIBK << 1;
LUAMOD_API int(luaopen_utf8)(lua_State* L);

/* Styx persistence: not part of the standard set, hosts open it explicitly */
inline constexpr byte_view LUA_STYXLIBNAME = "styx";
LUAMOD_API int(luaopen_styx)(lua_State* L);

/* open selected libraries */
void(luaL_openselectedlibs)(lua_State* L, int load, int preload);

/* open all libraries */
void luaL_openlibs(lua_State* L);
