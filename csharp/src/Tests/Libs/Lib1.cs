using System;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using luaL_Reg = Payonel.LungoLua.Api.luaL_Reg;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;

namespace Payonel.LungoLua.Tests.Libs;

public sealed class Lib1
{
  private Lib1()
  {
  }

  private static int id(lua_State L)
  {
    return lua_gettop(L);
  }

  private static readonly lua_CFunction id_fn = Lib1.id;

  private static readonly luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id_fn)
  };

  /* function used by lib11.c */
  public static int lib1_export(lua_State L)
  {
    lua_pushstring(L, new HostString("exported"));
    return 1;
  }

  public static readonly lua_CFunction lib1_export_fn = Lib1.lib1_export;

  public static int onefunction(lua_State L)
  {
    luaL_checkversion(L);
    lua_settop(L, 2);
    lua_pushvalue(L, 1);
    return 2;
  }

  public static readonly lua_CFunction onefunction_fn = Lib1.onefunction;

  public static int anotherfunc(lua_State L)
  {
    luaL_checkversion(L);
    lua_pushinteger(L, lua_tointeger(L, 1));
    lua_pushinteger(L, lua_tointeger(L, 2));
    lua_pushfstring(L, new HostString("%d%%%d\n"));
    return 1;
  }

  public static readonly lua_CFunction anotherfunc_fn = Lib1.anotherfunc;

  public static int luaopen_lib1_sub(lua_State L)
  {
    lua_setglobal(L, new HostString("y")); /* 2nd arg: extra value (file name) */
    lua_setglobal(L, new HostString("x")); /* 1st arg: module name */
    luaL_newlib(L, funcs);
    return 1;
  }

  public static readonly lua_CFunction luaopen_lib1_sub_fn = Lib1.luaopen_lib1_sub;
}
