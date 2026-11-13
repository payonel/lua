using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lfunc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

/*
** Upvalues for Lua closures
*/
public sealed class UpVal : GCObject
{
  public UpVal opennext = null;
  public UpVal prevopen = null;
  new public lua_State owner = null;
  public int stkid = 0;
  public TValue value = new TValue(0, null, Lobject.LUA_VNIL);

  /*
  ** Create a new upvalue at the given level, and link it to the list of
  ** open upvalues of 'L' after entry 'prev'.
  **/
  private static UpVal newupval(lua_State L, int level, UpVal prev)
  {
    UpVal uv = luaM_malloc_UpVal(L);
    luaC_link(L, uv, LUA_VUPVAL);
    UpVal next = (prev != null) ? prev.opennext : L.openupval;
    uv.owner = L;
    uv.stkid = level;
    uv.opennext = next;        /* link it to list of open upvalues */
    uv.prevopen = prev;
    if (next != null)
      next.prevopen = uv;
    if (prev != null)
      prev.opennext = uv;
    else
      L.openupval = uv;
    if (!isintwups(L))
    {                         /* thread not in list of threads with upvalues? */
      L.twups = L.l_G.twups; /* link it to the list */
      L.l_G.twups = L;
    }
    return uv;
  }

  /*
  ** Find and reuse, or create if it does not exist, an upvalue
  ** at the given level.
  */
  public static UpVal luaF_findupval(lua_State L, int level)
  {
    UpVal pp = null;
    UpVal p = L.openupval;
    { if (JAssert.on && !( isintwups(L) || L.openupval == null)) throw new AssertionError(); }
    while (p != null && uplevel(p) >= level)
    { /* search for it */
      { if (JAssert.on && !( !isdead(L.l_G, p))) throw new AssertionError(); }
      if (uplevel(p) == level) /* corresponding upvalue? */
        return p;              /* return it */
      pp = p;
      p = p.opennext;
    }
    /* not found: create a new upvalue after 'pp' */
    return UpVal.newupval(L, level, pp);
  }

  public void luaF_unlinkupval()
  {
    { if (JAssert.on && !( upisopen(this))) throw new AssertionError(); }
    if (prevopen != null)
      prevopen.opennext = opennext;
    else
      owner.openupval = opennext;
    if (opennext != null)
      opennext.prevopen = prevopen;
  }
}
