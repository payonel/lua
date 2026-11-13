using System;
using Payonel.LungoLua.Compat;
using static Payonel.LungoLua.Object.Lnames;

namespace Payonel.LungoLua.Api;

public sealed class CClosureNamer
{
  public CClosureNamer(lua_CClosureKind kind)
  {
    lua_registercclosure(kind);
  }
}
