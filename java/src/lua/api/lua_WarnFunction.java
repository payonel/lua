package lua.api;

import lua.runtime.lua_State;

public interface lua_WarnFunction
{
  void warn(lua_State L, HostString msg, int tocont);
}
