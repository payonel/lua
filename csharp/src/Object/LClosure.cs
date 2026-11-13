using System;
using Payonel.LungoLua.Compat;
using global_State = Payonel.LungoLua.Runtime.global_State;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

public sealed class LClosure : Closure
{
  public Proto p = null;
  public UpVal[] upvals = new UpVal[0];

  public static LClosure luaF_newLclosure(lua_State L, int nupvals)
  {
    global_State g = L.l_G;
    LClosure c = luaM_malloc_LClosure(L);
    luaC_link(L, c, LUA_VLCL);
    bool oldgcstop = g.gcstopem;
    g.gcstopem = true;
    UpVal[] upvals = luaM_alloc_UpVal_ptr_span(L.l_G, L, new UpVal[0], nupvals, false);
    g.gcstopem = oldgcstop;
    if (upvals == null && nupvals > 0)
      luaD_throw(L, (sbyte) LUA_ERRMEM);
    c.upvals = upvals;
    while (nupvals-- != 0)
      c.upvals[nupvals] = null;
    return c;
  }

  /*
  ** fill a closure with new closed upvalues
  */
  public void luaF_initupvals(lua_State L)
  {
    for (int i = 0; i < upvals.Length; i++)
    {
      UpVal uv = luaM_malloc_UpVal(L);
      luaC_link(L, uv, LUA_VUPVAL);
      upvals[i] = uv;
      luaC_objbarrier(L, this, uv);
    }
  }
}
