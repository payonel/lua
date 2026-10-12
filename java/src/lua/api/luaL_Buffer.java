package lua.api;

import static lua.api.LuaAuxLib.bb_dealloc;

import lua.bytes.ByteOwner;
import lua.runtime.lua_State;

public final class luaL_Buffer
{
  public ByteOwner bytes = new ByteOwner();
  public int used = 0;
  public lua_State L = null;

  public luaL_Buffer()
  {
  }

  public void release()
  {
    bb_dealloc(this);
  }
}
