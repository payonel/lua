package lua.api;

import lua.runtime.lua_State;

@FunctionalInterface
public interface lua_CFunction
{
  int call(lua_State L);
}
