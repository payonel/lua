using System;
using Payonel.LungoLua.Compat;
using lua_Alloc = Payonel.LungoLua.Api.lua_Alloc;

namespace Payonel.LungoLua.Runtime;

/* the debug build's checks and limits (LUA_DEBUG: the system property 'lua.debug') */
public sealed class Lhack
{
  private Lhack()
  {
  }

  public static readonly bool LUA_DEBUG = JBoolean.getBoolean("lua.debug");

  private sealed class Tests
  {
    internal static readonly TestHooks hooks = testhooks();

    private static TestHooks testhooks()
    {
      try
      {
        Type t = Type.GetType("Payonel.LungoLua.Tests.Ltests, lungolua-tests", false);
        if (t == null) /* not on the application's dependencies (the interpreter's): the assembly beside the engine's */
          t = System.Runtime.Loader.AssemblyLoadContext.Default.LoadFromAssemblyPath(System.IO.Path.Combine(AppContext.BaseDirectory, "lungolua-tests.dll")).GetType("Payonel.LungoLua.Tests.Ltests", true);
        return (TestHooks) t.GetField("hooks").GetValue(null);
      }
      catch (Exception e)
      {
        throw new InvalidOperationException("lua.debug needs the tests' Ltests (lungolua-tests.dll)", e);
      }
    }
  }

  public static void luai_userstateopen(lua_State l)
  {
    if (LUA_DEBUG)
      l.l_G.apilock = 0;
  }

  public static void luai_userstateclose(lua_State l)
  {
    if (LUA_DEBUG)
      { if (JAssert.on && !( l.l_G.apilock == 1 && l == l.l_G.mainth)) throw new AssertionError(); }
  }

  public static void luai_userstatethread(lua_State l, lua_State l1)
  {
    if (LUA_DEBUG)
      { if (JAssert.on && !( l1.l_G == l.l_G)) throw new AssertionError(); }
  }

  public static void luai_userstatefree(lua_State l, lua_State l1)
  {
    if (LUA_DEBUG)
      { if (JAssert.on && !( l.l_G == l1.l_G)) throw new AssertionError(); }
  }

  public static void lua_lock(lua_State l)
  {
    if (LUA_DEBUG)
      { if (JAssert.on && !( (l.l_G.apilock++) == 0)) throw new AssertionError(); }
  }

  public static void lua_unlock(lua_State l)
  {
    if (LUA_DEBUG)
      { if (JAssert.on && !( --(l.l_G.apilock) == 0)) throw new AssertionError(); }
  }

  public static void luai_tracegc(lua_State l, bool f)
  {
    if (LUA_DEBUG)
      Tests.hooks.luai_tracegctest(l, f);
  }

  public static lua_State _lua_newstate(lua_Alloc f, int seed)
  {
    if (LUA_DEBUG)
      return Tests.hooks.luai_newstate();
    return Lstate.lua_newstate(f, seed);
  }

  public static void luai_openlibs(lua_State L)
  {
    if (LUA_DEBUG)
      Tests.hooks.luai_openlibstest(L);
    else
      Payonel.LungoLua.Lib.Linit.luaL_openlibs(L);
  }

  public static readonly int LUAL_BUFFERSIZE = LUA_DEBUG ? 23 : 1024;

  public static readonly int MINSTRTABSIZE = LUA_DEBUG ? 2 : 128;

  public static readonly int MAXIWTHABS = LUA_DEBUG ? 3 : 128;

  public static readonly int MAXINDEXRK = LUA_DEBUG ? 1 : (1 << 8) - 1; /* MAXARG_B */

  public static readonly int LUAI_MAXSTACK = LUA_DEBUG ? 68000 : 1000000;

  public static readonly int LUAI_MAXCCALLS = LUA_DEBUG ? 180 : 200;
}
