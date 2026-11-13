using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using luaL_Regi = Payonel.LungoLua.Api.luaL_Regi;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;
using static Payonel.LungoLua.Lib.Lbaselib;
using static Payonel.LungoLua.Lib.Lcorolib;
using static Payonel.LungoLua.Lib.Ldblib;
using static Payonel.LungoLua.Lib.Liolib;
using static Payonel.LungoLua.Lib.Lmathlib;
using static Payonel.LungoLua.Lib.Loadlib;
using static Payonel.LungoLua.Lib.Loslib;
using static Payonel.LungoLua.Lib.Lstrlib;
using static Payonel.LungoLua.Lib.Ltablib;
using static Payonel.LungoLua.Lib.Lualib;
using static Payonel.LungoLua.Lib.Lutf8lib;

namespace Payonel.LungoLua.Lib;

public sealed class Linit
{
  private Linit()
  {
  }

  /*
  ** Standard Libraries. (Must be listed in the same ORDER of their
  ** respective constants LUA_<libname>K.)
  */
  private static readonly luaL_Regi[] stdlibs = {
    new luaL_Regi(LUA_GNAME, luaopen_base_fn),
    new luaL_Regi(LUA_LOADLIBNAME, luaopen_package_fn),
    new luaL_Regi(LUA_COLIBNAME, luaopen_coroutine_fn),
    new luaL_Regi(LUA_DBLIBNAME, luaopen_debug_fn),
    new luaL_Regi(LUA_IOLIBNAME, luaopen_io_fn),
    new luaL_Regi(LUA_MATHLIBNAME, luaopen_math_fn),
    new luaL_Regi(LUA_OSLIBNAME, luaopen_os_fn),
    new luaL_Regi(LUA_STRLIBNAME, luaopen_string_fn),
    new luaL_Regi(LUA_TABLIBNAME, luaopen_table_fn),
    new luaL_Regi(LUA_UTF8LIBNAME, luaopen_utf8_fn)
  };

  /*
  ** require and preload selected standard libraries
  */

  /* open selected libraries */
  public static void luaL_openselectedlibs(lua_State L, int load, int preload)
  {
    int mask = 1;
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    foreach (luaL_Regi lib in stdlibs)
    {
      if ((load & mask) != 0)
      {                                            /* selected? */
        luaL_requiref(L, new HostString(lib.name), lib.func, true); /* require library */
        lua_pop(L, 1);                             /* remove result from the stack */
      }
      else if ((preload & mask) != 0)
      { /* selected? */
        lua_pushcfunction(L, lib.func);
        lua_setfield(L, -2, new HostString(lib.name)); /* add library to PRELOAD table */
      }
      mask <<= 1;
    }
    { if (JAssert.on && !( (mask >> 1) == LUA_UTF8LIBK)) throw new AssertionError(); }
    lua_pop(L, 1); /* remove PRELOAD table */
  }

  /* open all libraries */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void luaL_openlibs(lua_State L)
  {
    luaL_openselectedlibs(L, ~0, 0);
  }
}
