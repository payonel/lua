package com.payonel.lungolua.tests.libs;

import static com.payonel.lungolua.tests.libs.LibDecls.lib1_export;

import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.runtime.lua_State;

public final class Lib11
{
  private Lib11()
  {
  }

  public static int luaopen_lib11(lua_State L)
  {
    return lib1_export(L);
  }

  public static final lua_CFunction luaopen_lib11 = Lib11::luaopen_lib11;
}
