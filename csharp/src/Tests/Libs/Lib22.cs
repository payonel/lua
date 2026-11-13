using System;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using luaL_Reg = Payonel.LungoLua.Api.luaL_Reg;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;

namespace Payonel.LungoLua.Tests.Libs;

/* implementation for lib2-v2 */
public sealed class Lib22
{
  private Lib22()
  {
  }

  private static int id(lua_State L)
  {
    lua_pushboolean(L, true);
    lua_insert(L, 1);
    return lua_gettop(L);
  }

  private static readonly lua_CFunction id_fn = Lib22.id;

  private static int newstr(lua_State L)
  {
    int len;
    ByteView sv = luaL_checklstring(L, 1);
    ByteView str = sv;
    len = sv.size();
    sbyte[] buf = new sbyte[len + 1];
    if (buf == null)
    {
      lua_pushliteral(L, new HostString("not enough memory"));
      lua_error(L);
    }
    JSystem.arraycopy(str.array(), str.offset(), buf, 0, len);
    lua_pushlstring(L, new ByteView(buf, 0, len), 0, len);
    return 1;
  }

  private static readonly lua_CFunction newstr_fn = Lib22.newstr;

  private static void initstr(lua_State L)
  {
    lua_pushcfunction(L, newstr_fn);
    lua_pushstring(L,
       new HostString("012345678901234567890123456789012345678901234567890123456789"));
    lua_call(L, 1, 1); /* call newstr("0123...") */
    luaL_ref(L, LUA_REGISTRYINDEX); /* keep string in the registry */
  }

  private static readonly luaL_Reg[] funcs = {
    new luaL_Reg(new HostString("id"), id_fn),
    new luaL_Reg(new HostString("newstr"), newstr_fn)
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

  public static readonly lua_CFunction luaopen_lib2_fn = Lib22.luaopen_lib2;
}
