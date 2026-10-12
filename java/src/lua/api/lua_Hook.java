package lua.api;

import lua.runtime.lua_State;

/*
** Functions to be called by the debugger in specific events
*/
@FunctionalInterface
public interface lua_Hook
{
  void call(lua_State L, lua_Debug ar);
}
