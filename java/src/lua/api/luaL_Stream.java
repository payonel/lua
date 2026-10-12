package lua.api;

import static lua.lib.Liolib.closeleft;

import lua.platform.OsFile;
import lua.runtime.lua_State;

public final class luaL_Stream implements lua_UserObject
{
  public OsFile f;
  public lua_CFunction closef;

  @Override
  public void freed(lua_State L) /* a handle freed without its '__gc' does not leak its file */
  {
    closeleft(this);
    f = null;
  }
}
