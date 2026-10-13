package com.payonel.lungolua.tests.libs;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.luaL_Reg;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.runtime.lua_State;

public final class Lib2
{
  private Lib2()
  {
  }

  private static int id(lua_State L)
  {
    return lua_gettop(L);
  }

  private static final lua_CFunction id = Lib2::id;

  private static final luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id)
  };

  public static int luaopen_lib2(lua_State L)
  {
    lua_settop(L, 2);
    lua_setglobal(L, new HostString("y")); /* y gets 2nd parameter */
    lua_setglobal(L, new HostString("x")); /* x gets 1st parameter */
    luaL_newlib(L, funcs);
    return 1;
  }

  public static final lua_CFunction luaopen_lib2 = Lib2::luaopen_lib2;
}
