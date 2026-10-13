package com.payonel.lungolua.api;

import static com.payonel.lungolua.api.LuaAuxLib.bb_dealloc;

import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.runtime.lua_State;

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
