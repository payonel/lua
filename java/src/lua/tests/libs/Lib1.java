package lua.tests.libs;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;

import lua.api.HostString;
import lua.api.luaL_Reg;
import lua.api.lua_CFunction;
import lua.runtime.lua_State;

public final class Lib1
{
  private Lib1()
  {
  }

  private static int id(lua_State L)
  {
    return lua_gettop(L);
  }

  private static final lua_CFunction id = Lib1::id;

  private static final luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id)
  };

  /* function used by lib11.c */
  public static int lib1_export(lua_State L)
  {
    lua_pushstring(L, new HostString("exported"));
    return 1;
  }

  public static final lua_CFunction lib1_export = Lib1::lib1_export;

  public static int onefunction(lua_State L)
  {
    luaL_checkversion(L);
    lua_settop(L, 2);
    lua_pushvalue(L, 1);
    return 2;
  }

  public static final lua_CFunction onefunction = Lib1::onefunction;

  public static int anotherfunc(lua_State L)
  {
    luaL_checkversion(L);
    lua_pushinteger(L, lua_tointeger(L, 1));
    lua_pushinteger(L, lua_tointeger(L, 2));
    lua_pushfstring(L, new HostString("%d%%%d\n"));
    return 1;
  }

  public static final lua_CFunction anotherfunc = Lib1::anotherfunc;

  public static int luaopen_lib1_sub(lua_State L)
  {
    lua_setglobal(L, new HostString("y")); /* 2nd arg: extra value (file name) */
    lua_setglobal(L, new HostString("x")); /* 1st arg: module name */
    luaL_newlib(L, funcs);
    return 1;
  }

  public static final lua_CFunction luaopen_lib1_sub = Lib1::luaopen_lib1_sub;
}
