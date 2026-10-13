package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.lib.Lbaselib.luaopen_base;
import static com.payonel.lungolua.lib.Lcorolib.luaopen_coroutine;
import static com.payonel.lungolua.lib.Ldblib.luaopen_debug;
import static com.payonel.lungolua.lib.Liolib.luaopen_io;
import static com.payonel.lungolua.lib.Lmathlib.luaopen_math;
import static com.payonel.lungolua.lib.Loadlib.luaopen_package;
import static com.payonel.lungolua.lib.Loslib.luaopen_os;
import static com.payonel.lungolua.lib.Lstrlib.luaopen_string;
import static com.payonel.lungolua.lib.Ltablib.luaopen_table;
import static com.payonel.lungolua.lib.Lualib.*;
import static com.payonel.lungolua.lib.Lutf8lib.luaopen_utf8;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.luaL_Regi;
import com.payonel.lungolua.runtime.lua_State;

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
