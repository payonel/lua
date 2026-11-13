using System;
using Payonel.LungoLua.Compat;
using OsFile = Payonel.LungoLua.Platform.OsFile;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Lib.Liolib;

namespace Payonel.LungoLua.Api;

public sealed class luaL_Stream : lua_UserObject
{
  public OsFile f;
  public lua_CFunction closef;

  public void freed(lua_State L) /* a handle freed without its '__gc' does not leak its file */
  {
    closeleft(this);
    f = null;
  }
}
