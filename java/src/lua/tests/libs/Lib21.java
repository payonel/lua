package lua.tests.libs;

import static lua.tests.libs.LibDecls.luaopen_lib2;

import lua.api.lua_CFunction;
import lua.runtime.lua_State;

public final class Lib21
{
  private Lib21()
  {
  }

  public static int luaopen_lib21(lua_State L)
  {
    return luaopen_lib2(L);
  }

  public static final lua_CFunction luaopen_lib21 = Lib21::luaopen_lib21;
}
