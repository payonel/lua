using System;
using Payonel.LungoLua.Compat;
using static Payonel.LungoLua.Object.Lnames;

namespace Payonel.LungoLua.Api;

public sealed class KNamer
{
  public KNamer(string name, lua_KFunction k)
  {
    lua_registerk(name, k);
  }
}
