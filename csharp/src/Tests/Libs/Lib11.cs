using System;
using Payonel.LungoLua.Compat;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Tests.Libs.LibDecls;

namespace Payonel.LungoLua.Tests.Libs;

public sealed class Lib11
{
  private Lib11()
  {
  }

  public static int luaopen_lib11(lua_State L)
  {
    return lib1_export(L);
  }

  public static readonly lua_CFunction luaopen_lib11_fn = Lib11.luaopen_lib11;
}
