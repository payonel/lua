using System;
using Payonel.LungoLua.Compat;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaAuxLib;

namespace Payonel.LungoLua.Api;

public sealed class luaL_Buffer
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
