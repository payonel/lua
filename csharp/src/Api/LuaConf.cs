using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Api;

public sealed class LuaConf
{
  private LuaConf()
  {
  }

  public const int LUA_MAXINPUT = 512;
  public const long LUA_MAXINTEGER = JLong.MAX_VALUE;
  public const long LUA_MININTEGER = JLong.MIN_VALUE;
  public const long LUA_MAXUNSIGNED = -1L;
  public const int LUA_IDSIZE = 60;
}
