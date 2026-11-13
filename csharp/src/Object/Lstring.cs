using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using Lt = Payonel.LungoLua.Bytes.Lt;
using global_State = Payonel.LungoLua.Runtime.global_State;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Llimits;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Lhack;

namespace Payonel.LungoLua.Object;

public sealed class Lstring
{
  private Lstring()
  {
  }

  public static bool isreserved(TString s)
  {
    return (s.extra & 0xFF) > 0;
  }

  public static bool eqshrstr(TString a, TString b)
  {
    return a == b;
  }

  public static int luaS_gethash(TString ts)
  {
    if (!ts.hashed)
      ts.luaS_forcehash();
    return ts.hash;
  }

  /*
  ** generic equality for strings
  */
  public static bool luaS_eqstr(TString a, TString b)
  {
    return (a == b) || (a.data.equals(b.data));
  }

  internal static int luaS_hash(ByteView str)
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
  internal static TString internfind(global_State g, ByteView sv, int h)
  {
    TString[] buckets = g.strt.buckets;
    if (buckets.Length == 0)
      return null;
    TString ts = buckets[h & (buckets.Length - 1)];
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
    { if (JAssert.on && !( ispow2(newsize))) throw new AssertionError(); }
    TString[] nb = new TString[newsize];
    foreach (TString ts0 in g.strt.buckets)
    {
      TString ts = ts0;
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

  internal static void interninsert(global_State g, TString ts)
  {
    global_State.StringTable st = g.strt;
    if (st.count >= st.buckets.Length)
      internresize(g, st.buckets.Length == 0 ? MINSTRTABSIZE : st.buckets.Length * 2);
    int i = ts.hash & (st.buckets.Length - 1);
    if (LUA_DEBUG)
      for (TString c = st.buckets[i]; c != null; c = c.hnext)
        { if (JAssert.on && !( !(c.hash == ts.hash && c.data.equals(ts.data)))) throw new AssertionError(); }
    ts.hnext = st.buckets[i];
    st.buckets[i] = ts;
    st.count++;
  }

  public static void luaS_init(lua_State L)
  {
    global_State g = L.l_G;
    /* pre-create memory-error message */
    g.memerrmsg = TString.luaS_createstr(L, Lt.not_enough_memory);
    { if (JAssert.on && !( novariant(g.memerrmsg.tt) >= LUA_TSTRING)) throw new AssertionError(); }
    luaC_fix(L, g.memerrmsg); /* it should never be collected */
    g.nofreezemsg = TString.luaS_createstr(L, Lt.cannot_freeze); /* (made now: destroying a chain allocates nothing) */
    luaC_fix(L, g.nofreezemsg);
  }

  internal const int LUAI_MAXSHORTLEN = 40;
}
