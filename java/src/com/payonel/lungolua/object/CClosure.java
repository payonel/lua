package com.payonel.lungolua.object;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lmem.*;

import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

public final class CClosure extends Closure
{
  public lua_CFunction f = null;
  public TValue[] upvalue = new TValue[0];

  public static CClosure luaF_newCclosure(lua_State L, int nupvals)
  {
    global_State g = L.l_G;
    CClosure c = luaM_malloc_CClosure(L);
    luaC_link(L, c, LUA_VCCL);
    boolean oldgcstop = g.gcstopem;
    g.gcstopem = true;
    TValue[] upvalue = luaM_alloc_TValue_span(L.l_G, L, new TValue[0], nupvals, false);
    g.gcstopem = oldgcstop;
    if (upvalue == null && nupvals > 0)
      luaD_throw(L, (byte) LUA_ERRMEM);
    c.upvalue = upvalue;
    return c;
  }
}
