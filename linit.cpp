/*
** $Id: linit.c $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/



#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "lua_internal.h"

/*
** Standard Libraries. (Must be listed in the same ORDER of their
** respective constants LUA_<libname>K.)
*/
static const luaL_Regi stdlibs[] = {
  {LUA_GNAME, luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_DBLIBNAME, luaopen_debug},
  {LUA_IOLIBNAME, luaopen_io},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_UTF8LIBNAME, luaopen_utf8}
};

/*
** require and preload selected standard libraries
*/
void luaL_openselectedlibs(lua_State* L, int load, int preload)
{
  int mask = 1;
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  for (const auto& lib : stdlibs)
  {
    if (load & mask)
    {                                            /* selected? */
      luaL_requiref(L, HostString{lib.name}, lib.func, 1); /* require library */
      lua_pop(L, 1);                             /* remove result from the stack */
    }
    else if (preload & mask)
    { /* selected? */
      lua_pushcfunction(L, lib.func);
      lua_setfield(L, -2, HostString{lib.name}); /* add library to PRELOAD table */
    }
    mask <<= 1;
  }
  assert((mask >> 1) == LUA_UTF8LIBK);
  lua_pop(L, 1); /* remove PRELOAD table */
}

void luaL_openlibs(lua_State* L)
{
  luaL_openselectedlibs(L, ~0, 0);
}
