using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Api;

public sealed class luaL_Regi
{
  public ByteView name;
  public lua_CFunction func;

  public luaL_Regi(ByteView name, lua_CFunction func)
  {
    this.name = name;
    this.func = func;
  }
}
