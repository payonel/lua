using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Runtime;

/* a Lua error (or freeze) propagating to its recover point ('luaD_throw') */
public sealed class LuaError : RuntimeException
{
  public readonly sbyte status;
  public readonly int level;

  public LuaError(sbyte status, int level) : base(null, null, false, false)
  {

    this.status = status;
    this.level = level;
  }
}
