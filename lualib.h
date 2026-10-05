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

inline constexpr byte_view LUA_STYXLIBNAME = "styx";
constexpr int LUA_STYXLIBK = LUA_UTF8LIBK << 1;
LUAMOD_API int(luaopen_styx)(lua_State* L);

/*
** Styx on the host side: persist a value, or the whole state, of a state
** that is not running Lua code. Nothing is allocated in the state and no
** Lua code runs; 'perms' is the stack index of the permanents table
** (object -> key), or 0 for none. The stream goes to 'w'. Returns LUA_OK,
** or an error status with a message in 'msg'.
*/
int(luaL_persistvalue)(lua_State* L, int perms, int idx, lua_Writer* w, HostString* msg);
int(luaL_persiststate)(lua_State* L, int perms, lua_Writer* w, HostString* msg);

/*
** The host side restore. 'perms' is the stack index of the inverse
** permanents (key -> object), or 0. luaL_unpersistvalue pushes the value;
** luaL_unpersiststate makes the stream's registry and type metatables the
** state's (create the state with luaL_persistedseed's seed). The state
** must not be running; what is restored is charged through its allocator.
** On error nothing is pushed or changed and 'msg' says why.
** luaL_persistedseed returns 1 and the string hash seed of a state stream,
** or 0 for any other data.
*/
int(luaL_unpersistvalue)(lua_State* L, int perms, byte_view data, HostString* msg);
int(luaL_unpersiststate)(lua_State* L, int perms, byte_view data, HostString* msg);
int(luaL_persistedseed)(byte_view data, unsigned int* seed);

/* open selected libraries */
void(luaL_openselectedlibs)(lua_State* L, int load, int preload);

/* open all libraries */
void luaL_openlibs(lua_State* L);
