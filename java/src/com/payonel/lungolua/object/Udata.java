package com.payonel.lungolua.object;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lmem.*;

import com.payonel.lungolua.api.lua_UserObject;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteSpan;
import com.payonel.lungolua.runtime.Cost;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

public final class Udata extends GCListed
{
  public Table metatable = null;
  public TValue[] uv = new TValue[0];
  public ByteOwner data = new ByteOwner();
  public boolean zb = false;
  public lua_UserObject obj = null;
  public long objsize = 0;

  public static Udata luaS_newudata(lua_State L, long s, int nuvalue)
  {
    global_State g = L.l_G;
    Udata u;
    int i;
    if (Long.compareUnsigned(s, MAX_SIZE - Cost.Udata) >= 0)
      luaM_toobig(L);
    u = luaM_malloc_Udata(L);
    luaC_link(L, u, LUA_VUSERDATA);
    u.zb = s == 0;
    boolean oldgcstop = g.gcstopem;
    g.gcstopem = true;
    TValue[] uv = luaM_alloc_TValue_span(L.l_G, L, new TValue[0], nuvalue, false);
    if (uv != null || nuvalue == 0)
    {
      u.uv = uv;
      u.data = luaM_alloc_char_span(L.l_G, L, new ByteSpan(), s == 0 ? 1 : s, false);

    }
    g.gcstopem = oldgcstop;
    if ((uv == null && nuvalue > 0) || !u.data.held())
      luaD_throw(L, (byte) LUA_ERRMEM);
    for (i = 0; i < nuvalue; i++)
      setnilvalue(u.uv[i]);
    return u;
  }
}
