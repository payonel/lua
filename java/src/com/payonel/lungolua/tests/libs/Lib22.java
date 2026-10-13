package com.payonel.lungolua.tests.libs;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.luaL_Reg;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.runtime.lua_State;

/* implementation for lib2-v2 */
public final class Lib22
{
  private Lib22()
  {
  }

  private static int id(lua_State L)
  {
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    return lua_gettop(L);
  }

  private static final lua_CFunction id = Lib22::id;


  private static int newstr(lua_State L)
  {
    int len;
    ByteView sv = luaL_checklstring(L, 1);
    ByteView str = sv;
    len = sv.size();
    byte[] buf = new byte[len + 1];
    if (buf == null)
    {
      lua_pushliteral(L, new HostString("not enough memory"));
      lua_error(L);
    }
    System.arraycopy(str.array(), str.offset(), buf, 0, len);
    lua_pushlstring(L, new ByteView(buf, 0, len), 0, len);
    return 1;
  }

  private static final lua_CFunction newstr = Lib22::newstr;


  private static void initstr(lua_State L)
  {
    lua_pushcfunction(L, newstr);
    lua_pushstring(L,
       new HostString("012345678901234567890123456789012345678901234567890123456789"));
    lua_call(L, 1, 1); /* call newstr("0123...") */
    luaL_ref(L, LUA_REGISTRYINDEX); /* keep string in the registry */
  }


  private static final luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id),
    new luaL_Reg(new HostString("newstr"), newstr)
  };

  public static int luaopen_lib2(lua_State L)
  {
    lua_settop(L, 2);
    lua_setglobal(L, new HostString("y")); /* y gets 2nd parameter */
    lua_setglobal(L, new HostString("x")); /* x gets 1st parameter */
    initstr(L);
    luaL_newlib(L, funcs);
    return 1;
  }

  public static final lua_CFunction luaopen_lib2 = Lib22::luaopen_lib2;
}
