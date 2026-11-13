using System;
using Payonel.LungoLua.Compat;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using global_State = Payonel.LungoLua.Runtime.global_State;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

public sealed class CClosure : Closure
{
  public lua_CFunction f = null;
  public TValue[] upvalue = new TValue[0];

  public static CClosure luaF_newCclosure(lua_State L, int nupvals)
  {
    global_State g = L.l_G;
    CClosure c = luaM_malloc_CClosure(L);
    luaC_link(L, c, LUA_VCCL);
    bool oldgcstop = g.gcstopem;
    g.gcstopem = true;
    TValue[] upvalue = luaM_alloc_TValue_span(L.l_G, L, new TValue[0], nupvals, false);
    g.gcstopem = oldgcstop;
    if (upvalue == null && nupvals > 0)
      luaD_throw(L, (sbyte) LUA_ERRMEM);
    c.upvalue = upvalue;
    return c;
  }
}
