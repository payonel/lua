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
static const luaL_Reg stdlibs[] = {
    {LUA_GNAME->view(), luaopen_base},
    {LUA_LOADLIBNAME->view(), luaopen_package},
    {LUA_COLIBNAME->view(), luaopen_coroutine},
    {LUA_DBLIBNAME->view(), luaopen_debug},
    {LUA_IOLIBNAME->view(), luaopen_io},
    {LUA_MATHLIBNAME->view(), luaopen_math},
    {LUA_OSLIBNAME->view(), luaopen_os},
    {LUA_STRLIBNAME->view(), luaopen_string},
    {LUA_TABLIBNAME->view(), luaopen_table},
    {LUA_UTF8LIBNAME->view(), luaopen_utf8},
    {{}, nullptr}};

/*
** require and preload selected standard libraries
*/
LUALIB_API void luaL_openselectedlibs(lua_State* L, int load, int preload)
{
  int mask;
  const luaL_Reg* lib;
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE->view());
  for (lib = stdlibs, mask = 1; lib->name.data() != nullptr; lib++, mask <<= 1)
  {
    if (load & mask)
    {                                            /* selected? */
      luaL_requiref(L, HostString{lib->name}, lib->func, 1); /* require library */
      lua_pop(L, 1);                             /* remove result from the stack */
    }
    else if (preload & mask)
    { /* selected? */
      lua_pushcfunction(L, lib->func);
      lua_setfield(L, -2, HostString{lib->name}); /* add library to PRELOAD table */
    }
  }
  assert((mask >> 1) == LUA_UTF8LIBK);
  lua_pop(L, 1); /* remove PRELOAD table */
}

LUALIB_API void luaL_openlibs(lua_State* L)
{
  luaL_openselectedlibs(L, ~0, 0);
}
