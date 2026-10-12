package lua.api;

import lua.bytes.ByteView;

public final class luaL_Regi
{
  public ByteView name;
  public lua_CFunction func;

  public luaL_Regi(ByteView name, lua_CFunction func)
  {
    this.name = name;
    this.func = func;
  }
}
