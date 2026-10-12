package lua.runtime;

/* the debug build's checks and limits (LUA_DEBUG: the system property 'lua.debug') */
public final class Lhack
{
  private Lhack()
  {
  }

  public static final boolean LUA_DEBUG = Boolean.getBoolean("lua.debug");

  public static void luai_userstateopen(lua_State l)
  {
    if (LUA_DEBUG)
      l.l_G.apilock = 0;
  }

  public static void luai_userstateclose(lua_State l)
  {
    if (LUA_DEBUG)
      assert l.l_G.apilock == 1 && l == l.l_G.mainth;
  }

  public static void luai_userstatethread(lua_State l, lua_State l1)
  {
    if (LUA_DEBUG)
      assert l1.l_G == l.l_G;
  }

  public static void luai_userstatefree(lua_State l, lua_State l1)
  {
    if (LUA_DEBUG)
      assert l.l_G == l1.l_G;
  }

  public static void lua_lock(lua_State l)
  {
    if (LUA_DEBUG)
      assert (l.l_G.apilock++) == 0;
  }

  public static void lua_unlock(lua_State l)
  {
    if (LUA_DEBUG)
      assert --(l.l_G.apilock) == 0;
  }

  public static final int LUAL_BUFFERSIZE = LUA_DEBUG ? 23 : 1024;

  public static final int MINSTRTABSIZE = LUA_DEBUG ? 2 : 128;

  public static final int MAXIWTHABS = LUA_DEBUG ? 3 : 128;

  public static final int MAXINDEXRK = LUA_DEBUG ? 1 : (1 << 8) - 1; /* MAXARG_B */

  public static final int LUAI_MAXSTACK = LUA_DEBUG ? 68000 : 1000000;

  public static final int LUAI_MAXCCALLS = LUA_DEBUG ? 180 : 200;
}
