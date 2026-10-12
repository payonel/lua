package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.lib.Lbaselib.luaopen_base;
import static lua.lib.Lcorolib.luaopen_coroutine;
import static lua.lib.Ldblib.luaopen_debug;
import static lua.lib.Liolib.luaopen_io;
import static lua.lib.Lmathlib.luaopen_math;
import static lua.lib.Loadlib.luaopen_package;
import static lua.lib.Loslib.luaopen_os;
import static lua.lib.Lstrlib.luaopen_string;
import static lua.lib.Ltablib.luaopen_table;
import static lua.lib.Lualib.*;
import static lua.lib.Lutf8lib.luaopen_utf8;

import lua.api.HostString;
import lua.api.luaL_Regi;
import lua.runtime.lua_State;

public final class Linit
{
  private Linit()
  {
  }

  /*
  ** Standard Libraries. (Must be listed in the same ORDER of their
  ** respective constants LUA_<libname>K.)
  */
  private static final luaL_Regi[] stdlibs = {
    new luaL_Regi(LUA_GNAME, luaopen_base),
    new luaL_Regi(LUA_LOADLIBNAME, luaopen_package),
    new luaL_Regi(LUA_COLIBNAME, luaopen_coroutine),
    new luaL_Regi(LUA_DBLIBNAME, luaopen_debug),
    new luaL_Regi(LUA_IOLIBNAME, luaopen_io),
    new luaL_Regi(LUA_MATHLIBNAME, luaopen_math),
    new luaL_Regi(LUA_OSLIBNAME, luaopen_os),
    new luaL_Regi(LUA_STRLIBNAME, luaopen_string),
    new luaL_Regi(LUA_TABLIBNAME, luaopen_table),
    new luaL_Regi(LUA_UTF8LIBNAME, luaopen_utf8)
  };

  /*
  ** require and preload selected standard libraries
  */

  /* open selected libraries */
  public static void luaL_openselectedlibs(lua_State L, int load, int preload)
  {
    int mask = 1;
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    for (luaL_Regi lib : stdlibs)
    {
      if ((load & mask) != 0)
      {                                            /* selected? */
        luaL_requiref(L, new HostString(lib.name), lib.func, 1); /* require library */
        lua_pop(L, 1);                             /* remove result from the stack */
      }
      else if ((preload & mask) != 0)
      { /* selected? */
        lua_pushcfunction(L, lib.func);
        lua_setfield(L, -2, new HostString(lib.name)); /* add library to PRELOAD table */
      }
      mask <<= 1;
    }
    assert (mask >> 1) == LUA_UTF8LIBK;
    lua_pop(L, 1); /* remove PRELOAD table */
  }


  /* open all libraries */
  public static void luaL_openlibs(lua_State L)
  {
    luaL_openselectedlibs(L, ~0, 0);
  }
}
