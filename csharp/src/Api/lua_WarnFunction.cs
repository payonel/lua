using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

public interface lua_WarnFunction
{
  void warn(lua_State L, HostString msg, int tocont);
}
