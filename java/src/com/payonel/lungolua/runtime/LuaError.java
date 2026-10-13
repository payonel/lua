package com.payonel.lungolua.runtime;

/* a Lua error (or freeze) propagating to its recover point ('luaD_throw') */
public final class LuaError extends RuntimeException
{
  public final byte status;
  public final int level;

  public LuaError(byte status, int level)
  {
    super(null, null, false, false);
    this.status = status;
    this.level = level;
  }
}
