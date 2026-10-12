package lua.lib;

import lua.api.HostString;
import lua.runtime.lua_State;

public final class luaL_Restored
{
  public lua_State L;
  public HostString msg = new HostString();

  public luaL_Restored()
  {
  }

  public luaL_Restored(lua_State L, HostString msg)
  {
    this.L = L;
    this.msg = msg;
  }
}
