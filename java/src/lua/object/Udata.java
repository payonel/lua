package lua.object;

import static lua.api.LuaApi.*;
import static lua.gc.Lgc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;

import lua.api.lua_UserObject;
import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.runtime.Cost;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class Udata extends GCListed
{
  public Table metatable = null;
  public TValue[] uv = new TValue[0];
  public ByteOwner data = new ByteOwner();
  public boolean zb = false;
  public lua_UserObject obj = null;
  public long objsize = 0;

  public static Udata luaS_newudata(lua_State L, int s, int nuvalue)
  {
    global_State g = L.l_G;
    Udata u;
    int i;
    if (s >= MAX_SIZE - Cost.Udata)
      luaM_toobig(L);
    u = luaM_malloc_Udata(L);
    luaC_link(L, u, LUA_VUSERDATA);
    u.zb = s == 0;
    byte oldgcstop = g.gcstopem;
    g.gcstopem = 1;
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
