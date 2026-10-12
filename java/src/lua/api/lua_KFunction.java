package lua.api;

import lua.runtime.lua_State;

@FunctionalInterface
public interface lua_KFunction
{
  int call(lua_State L, int status, long ctx);
}
