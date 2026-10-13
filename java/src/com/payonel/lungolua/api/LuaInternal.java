package com.payonel.lungolua.api;

public final class LuaInternal
{
  private LuaInternal()
  {
  }

  /* 'readeridx' of a load whose chunk is in its box (see 'lua_loadkread') */
  public static final int LUA_LOADBOXED = -1;

  public static final int LUAL_SLICE = 256 * 1024;
  public static final int LUAL_SLICESTEP = 64;
}
