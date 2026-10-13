package com.payonel.lungolua.tests.libs;

import static com.payonel.lungolua.tests.libs.LibDecls.luaopen_lib2;

import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.runtime.lua_State;

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
