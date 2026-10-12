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

/*
** Styx: the kernel's persistence. The host persists the whole state while
** it is at rest (no Lua code runs: the kernel has frozen or suspended its
** programs and yielded back to the host) and restores it into a new
** state; what it needs back it anchors in the registry (the main thread's
** stack is not part of a state). Scripts cannot persist.
**
** luaL_persiststate writes the state to 'w'; 'perms' is the stack index of
** the permanents table (object -> key), or 0 for none. Nothing is
** allocated in the state and no Lua code runs. 'L' is the main thread, or
** a thread the state holds (for a frozen main thread, the host's other
** thread, anchored before): luaL_restorestate gives it back. Gives LUA_OK,
** or an error status with a message.
*/
struct luaL_StyxResult
{
  int status;
  HostString msg;
};
luaL_StyxResult(luaL_persiststate)(lua_State* L, int perms, lua_Writer* w);

/*
** The kernel's restore, in one call: a new state with allocator 'f', which
** hashes as the persisting state did (its tables keep their order), set
** up by 'setup' (if given), which opens the same libraries and makes the
** host's permanents, in the same order as for the persisting state, and
** pushes the inverse permanents table (key -> object), or nil; then the
** whole state in 'data' (luaL_persiststate) is restored into it (a frozen
** main thread into its main thread). Gives the restored thread the state
** was persisted from (its main thread, or the host's other thread), or
** NULL with a message (nothing is left open).
*/
struct luaL_RestoreSetup
{
  virtual ~luaL_RestoreSetup() = default;
  virtual void setup(lua_State* L) = 0;
};
struct luaL_Restored
{
  lua_State* L;
  HostString msg;
};
luaL_Restored(luaL_restorestate)(lua_Alloc* f, byte_view data, luaL_RestoreSetup* setup);

/* open selected libraries */
void(luaL_openselectedlibs)(lua_State* L, int load, int preload);

/* open all libraries */
void luaL_openlibs(lua_State* L);
