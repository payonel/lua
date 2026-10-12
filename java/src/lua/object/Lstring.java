package lua.object;

import static lua.api.LuaApi.*;
import static lua.gc.Lgc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.runtime.Lhack.*;

import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class Lstring
{
  private Lstring()
  {
  }

  public static int isreserved(TString s)
  {
    return ((s.extra & 0xFF) > 0) ? 1 : 0;
  }

  public static int eqshrstr(TString a, TString b)
  {
    return (a == b) ? 1 : 0;
  }

  public static int luaS_gethash(TString ts)
  {
    if (ts.hashed == 0)
      ts.luaS_forcehash();
    return ts.hash;
  }

  /*
  ** generic equality for strings
  */
  public static int luaS_eqstr(TString a, TString b)
  {
    return ((a == b) || (a.data.equals(b.data))) ? 1 : 0;
  }

  static int luaS_hash(ByteView str)
  {
    return InternalString.hashBytes(str.array(), str.offset(), str.size());
  }

  /*
  ** Find an interned string by content. Deliberately returns DEAD
  ** (unmarked, not-yet-swept) strings too: the caller resurrects them
  ** via changewhite. Skipping dead entries here would create a duplicate
  ** TString while the dead twin is still chained, transiently breaking
  ** the one-TString-per-short-content invariant that eqshrstr (pointer
  ** equality) relies on. Chain memory is always valid: freeobj unlinks
  ** a string (luaS_remove) before freeing it.
  */
  static TString internfind(global_State g, ByteView sv, int h)
  {
    TString[] buckets = g.strt.buckets;
    if (buckets.length == 0)
      return null;
    TString ts = buckets[h & (buckets.length - 1)];
    while (ts != null)
    {
      if (ts.hash == h && ts.data.view().equals(sv))
        return ts;
      ts = ts.hnext;
    }
    return null;
  }

  private static void internresize(global_State g, int newsize)
  {
    assert ispow2(newsize);
    TString[] nb = new TString[newsize];
    for (TString ts : g.strt.buckets)
    {
      while (ts != null)
      {
        TString next = ts.hnext;
        int i = ts.hash & (newsize - 1);
        ts.hnext = nb[i];
        nb[i] = ts;
        ts = next;
      }
    }
    g.strt.buckets = nb;
  }

  static void interninsert(global_State g, TString ts)
  {
    global_State.StringTable st = g.strt;
    if (st.count >= st.buckets.length)
      internresize(g, st.buckets.length == 0 ? MINSTRTABSIZE : st.buckets.length * 2);
    int i = ts.hash & (st.buckets.length - 1);
    if (LUA_DEBUG)
      for (TString c = st.buckets[i]; c != null; c = c.hnext)
        assert !(c.hash == ts.hash && c.data.equals(ts.data));
    ts.hnext = st.buckets[i];
    st.buckets[i] = ts;
    st.count++;
  }

  public static void luaS_init(lua_State L)
  {
    global_State g = L.l_G;
    /* pre-create memory-error message */
    g.memerrmsg = TString.luaS_createstr(L, Lt.not_enough_memory);
    assert novariant(g.memerrmsg.tt) >= LUA_TSTRING;
    luaC_fix(L, g.memerrmsg); /* it should never be collected */
    g.nofreezemsg = TString.luaS_createstr(L, Lt.cannot_freeze); /* (made now: destroying a chain allocates nothing) */
    luaC_fix(L, g.nofreezemsg);
  }

  static final int LUAI_MAXSHORTLEN = 40;
}
