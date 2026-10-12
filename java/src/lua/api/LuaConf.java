package lua.api;

public final class LuaConf
{
  private LuaConf()
  {
  }

  public static final int LUA_MAXINPUT = 512;
  public static final long LUA_MAXINTEGER = Long.MAX_VALUE;
  public static final long LUA_MININTEGER = Long.MIN_VALUE;
  public static final long LUA_MAXUNSIGNED = -1L;
  public static final int LUA_IDSIZE = 60;
}
