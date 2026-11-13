using System;
using Payonel.LungoLua.Compat;
using lua_UserObject = Payonel.LungoLua.Api.lua_UserObject;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteSpan = Payonel.LungoLua.Bytes.ByteSpan;
using Cost = Payonel.LungoLua.Runtime.Cost;
using global_State = Payonel.LungoLua.Runtime.global_State;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Llimits;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

public sealed class Udata : GCListed
{
  public Table metatable = null;
  public TValue[] uv = new TValue[0];
  public ByteOwner data = new ByteOwner();
  public bool zb = false;
  public lua_UserObject obj = null;
  public long objsize = 0;

  public static Udata luaS_newudata(lua_State L, long s, int nuvalue)
  {
    global_State g = L.l_G;
    Udata u;
    int i;
    if (JLong.compareUnsigned(s, MAX_SIZE - Cost.Udata) >= 0)
      luaM_toobig(L);
    u = luaM_malloc_Udata(L);
    luaC_link(L, u, LUA_VUSERDATA);
    u.zb = s == 0;
    bool oldgcstop = g.gcstopem;
    g.gcstopem = true;
    TValue[] uv = luaM_alloc_TValue_span(L.l_G, L, new TValue[0], nuvalue, false);
    if (uv != null || nuvalue == 0)
    {
      u.uv = uv;
      u.data = luaM_alloc_char_span(L.l_G, L, new ByteSpan(), s == 0 ? 1 : s, false);

    }
    g.gcstopem = oldgcstop;
    if ((uv == null && nuvalue > 0) || !u.data.held())
      luaD_throw(L, (sbyte) LUA_ERRMEM);
    for (i = 0; i < nuvalue; i++)
      setnilvalue(u.uv[i]);
    return u;
  }
}
