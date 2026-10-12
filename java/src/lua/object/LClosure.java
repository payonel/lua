package lua.object;

import static lua.api.LuaApi.*;
import static lua.gc.Lgc.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;

import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class LClosure extends Closure
{
  public Proto p = null;
  public UpVal[] upvals = new UpVal[0];

  public static LClosure luaF_newLclosure(lua_State L, int nupvals)
  {
    global_State g = L.l_G;
    LClosure c = luaM_malloc_LClosure(L);
    luaC_link(L, c, LUA_VLCL);
    byte oldgcstop = g.gcstopem;
    g.gcstopem = 1;
    UpVal[] upvals = luaM_alloc_UpVal_ptr_span(L.l_G, L, new UpVal[0], nupvals, false);
    g.gcstopem = oldgcstop;
    if (upvals == null && nupvals > 0)
      luaD_throw(L, (byte) LUA_ERRMEM);
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
    for (int i = 0; i < upvals.length; i++)
    {
      UpVal uv = luaM_malloc_UpVal(L);
      luaC_link(L, uv, LUA_VUPVAL);
      upvals[i] = uv;
      luaC_objbarrier(L, this, uv);
    }
  }
}
