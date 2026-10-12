package lua.api;

public final class luaL_Reg
{
  public HostString name;
  public lua_CFunction func;

  public luaL_Reg(HostString name, lua_CFunction func)
  {
    this.name = name;
    this.func = func;
  }
}
