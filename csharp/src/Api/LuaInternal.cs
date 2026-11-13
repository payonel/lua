using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Api;

public sealed class LuaInternal
{
  private LuaInternal()
  {
  }

  /* 'readeridx' of a load whose chunk is in its box (see 'lua_loadkread') */
  public const int LUA_LOADBOXED = -1;

  public const int LUAL_SLICE = 256 * 1024;
  public const int LUAL_SLICESTEP = 64;
}
