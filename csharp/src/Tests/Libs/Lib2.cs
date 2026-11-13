using System;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using luaL_Reg = Payonel.LungoLua.Api.luaL_Reg;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;

namespace Payonel.LungoLua.Tests.Libs;

public sealed class Lib2
{
  private Lib2()
  {
  }

  private static int id(lua_State L)
  {
    return lua_gettop(L);
  }

  private static readonly lua_CFunction id_fn = Lib2.id;

  private static readonly luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id_fn)
  };

  public static int luaopen_lib2(lua_State L)
  {
    lua_settop(L, 2);
    lua_setglobal(L, new HostString("y")); /* y gets 2nd parameter */
    lua_setglobal(L, new HostString("x")); /* x gets 1st parameter */
    luaL_newlib(L, funcs);
    return 1;
  }

  public static readonly lua_CFunction luaopen_lib2_fn = Lib2.luaopen_lib2;
}
