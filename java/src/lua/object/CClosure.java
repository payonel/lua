package lua.object;

import static lua.api.LuaApi.*;
import static lua.gc.Lgc.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;

import lua.api.lua_CFunction;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class CClosure extends Closure
{
  public lua_CFunction f = null;
  public TValue[] upvalue = new TValue[0];

  public static CClosure luaF_newCclosure(lua_State L, int nupvals)
  {
    global_State g = L.l_G;
    CClosure c = luaM_malloc_CClosure(L);
    luaC_link(L, c, LUA_VCCL);
    byte oldgcstop = g.gcstopem;
    g.gcstopem = 1;
    TValue[] upvalue = luaM_alloc_TValue_span(L.l_G, L, new TValue[0], nupvals, false);
    g.gcstopem = oldgcstop;
    if (upvalue == null && nupvals > 0)
      luaD_throw(L, (byte) LUA_ERRMEM);
    c.upvalue = upvalue;
    return c;
  }
}
