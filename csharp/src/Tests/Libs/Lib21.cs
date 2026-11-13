using System;
using Payonel.LungoLua.Compat;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Tests.Libs.LibDecls;

namespace Payonel.LungoLua.Tests.Libs;

public sealed class Lib21
{
  private Lib21()
  {
  }

  public static int luaopen_lib21(lua_State L)
  {
    return luaopen_lib2(L);
  }

  public static readonly lua_CFunction luaopen_lib21_fn = Lib21.luaopen_lib21;
}
