using System;
using Payonel.LungoLua.Compat;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using global_State = Payonel.LungoLua.Runtime.global_State;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Object.Lstring;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

/*
** Header for a string value.
*/
public sealed class TString : GCObject
{
  public sbyte extra;
  public bool hashed;
  new public int hash;
  public TString hnext = null;
  public InternalString data = new InternalString();

  public void luaS_forcehash()
  {
    TString ts = this;
    ByteView bytes = ts.data.view();
    ts.hash = InternalString.hashBytes(bytes.array(), bytes.offset(), bytes.size());
    ts.hashed = true;
  }

  private static TString createstrobj(lua_State L, long l, int h, bool hashed, bool external)
  {
    /* the string and its 'l' bytes are charged together, before either exists
       and before the string is known to the collector (a refusal may run an
       emergency collection); its maker gives it its bytes */
    TString ts = luaM_malloc_TString(L, external ? 0 : l);
    luaC_link(L, ts, LUA_VSTRING);
    ts.hash = h;
    ts.hashed = hashed;
    ts.extra = (sbyte) (external ? STREXTRA_EXTERNAL : 0);
    return ts;
  }

  public static TString luaS_createstr(lua_State L, ByteView str)
  {
    global_State g = L.l_G;
    int l = str.size();
    if (l <= LUAI_MAXSHORTLEN)
    {
      int h = luaS_hash(str);
      TString ts_ = internfind(g, str, h);
      if (ts_ != null)
      {
        if (isdead(g, ts_))
          changewhite(ts_); /* resurrect it */
        return ts_;
      }
      ts_ = TString.createstrobj(L, l, h, true, false);
      ts_.data = new InternalString(str);
      interninsert(g, ts_);
      return ts_;
    }
    TString ts = TString.createstrobj(L, l, 0, false, false);
    ts.data = new InternalString(str);
    return ts;
  }

  public static TString luaS_take(lua_State L, InternalString str)
  {
    global_State g = L.l_G;
    if (str.size() <= LUAI_MAXSHORTLEN)
    {
      ByteView bytes = str.view();
      int h = InternalString.hashBytes(bytes.array(), bytes.offset(), bytes.size());
      TString ts_ = internfind(g, bytes, h);
      if (ts_ != null)
      {
        if (isdead(g, ts_))
          changewhite(ts_); /* resurrect it */
        return ts_;
      }
      ts_ = TString.createstrobj(L, str.size(), h, true, false);
      ts_.data = InternalString.move(str);
      interninsert(g, ts_);
      return ts_;
    }
    TString ts = TString.createstrobj(L, str.size(), 0, false, false);
    ts.data = InternalString.move(str);
    return ts;
  }

  public static TString luaS_takeexternal(lua_State L, InternalString str)
  {
    if (str.size() <= LUAI_MAXSHORTLEN)
      return TString.luaS_take(L, InternalString.move(str));
    TString ts = TString.createstrobj(L, str.size(), 0, false, true);
    ts.data = InternalString.move(str);
    return ts;
  }

  /*
  ** A long string of 'l' zero bytes, made in its final place for its maker to
  ** write them before it is shared
  */
  public static TString luaS_createlngstrobj(lua_State L, long l)
  {
    { if (JAssert.on && !( l > LUAI_MAXSHORTLEN)) throw new AssertionError(); }
    TString ts = TString.createstrobj(L, l, 0, false, false);
    ts.data = new InternalString(new ByteOwner((int) l), (int) l);
    return ts;
  }

  /*
  ** A string of the first 'len' bytes of 'block', a buffer whose maker no
  ** longer counts it: the block becomes the string's bytes, kept with the
  ** tail after them when that is at most half the string (a grown buffer's);
  ** a longer tail is not kept, the bytes are copied
  */
  public static TString luaS_takeblock(lua_State L, ByteOwner block, int len)
  {
    if (len <= LUAI_MAXSHORTLEN)
      return TString.luaS_createstr(L, new ByteView(block.array(), 0, len));
    TString ts = TString.createstrobj(L, len, 0, false, false);
    if (block.size() - len > len / 2)
      ts.data = new InternalString(new ByteView(block.array(), 0, len));
    else
      ts.data = new InternalString(ByteOwner.move(block), len);
    return ts;
  }

  public void luaS_remove(lua_State L)
  {
    TString ts = this;
    if (ts.data.size() <= LUAI_MAXSHORTLEN)
    {
      global_State g = L.l_G;
      global_State.StringTable st = g.strt;
      if (st.buckets.Length == 0)
        return;
      int b = ts.hash & (st.buckets.Length - 1);
      TString prev = null;
      for (TString p = st.buckets[b]; p != null; p = p.hnext)
      {
        if (p == ts)
        {
          if (prev != null)
            prev.hnext = ts.hnext;
          else
            st.buckets[b] = ts.hnext;
          st.count--;
          return;
        }
        prev = p;
      }
    }
  }
}
