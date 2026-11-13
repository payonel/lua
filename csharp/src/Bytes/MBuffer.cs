using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Bytes;

public sealed class MBuffer
{
  private ByteOwner bytes_ = new ByteOwner();
  private int len_ = 0;
  private lua_State L_ = null;

  public MBuffer()
  {
  }

  public MBuffer(lua_State L)
  {
    L_ = L;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void destroy()
  {
    if (bytes_.held())
      luaM_alloc_char_span(L_.l_G, L_, bytes_, 0, false);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void attach(lua_State L)
  {
    { if (JAssert.on && !( bytes_.empty() || L == L_)) throw new AssertionError(); }
    L_ = L;
  }

  public void growTo(long cap)
  {
    if (cap == bytes_.size())
      return;
    { if (JAssert.on && !( L_ != null)) throw new AssertionError(); }
    bytes_ = luaM_alloc_char_span(L_.l_G, L_, bytes_, cap, true);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void append(sbyte c)
  {
    if (len_ + 1 > bytes_.size())
      growTo(len_ + 1);
    bytes_.set(len_++, c);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void reset()
  {
    len_ = 0;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void drop(int k)
  {
    len_ -= k;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int size()
  {
    return len_;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int capacity()
  {
    return bytes_.size();
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteView view()
  {
    return new ByteView(bytes_.array(), 0, len_);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteSpan area()
  {
    return bytes_.span();
  }

  /* its bytes go elsewhere (their count is its size); it is left empty */
  public ByteOwner release()
  {
    len_ = 0;
    ByteOwner b = ByteOwner.move(bytes_);
    bytes_ = new ByteOwner();
    return b;
  }

  /* its bytes go elsewhere, counted no more as its own; it is left empty */
  public ByteOwner handover()
  {
    if (bytes_.held())
      luaM_alloc_char_span(L_.l_G, L_, bytes_, 0, false);
    len_ = 0;
    ByteOwner b = ByteOwner.move(bytes_);
    bytes_ = new ByteOwner();
    return b;
  }

  /* takes bytes given by 'release' */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void adopt(ByteOwner b, int len)
  {
    { if (JAssert.on && !( bytes_.empty())) throw new AssertionError(); }
    bytes_ = b;
    len_ = len;
  }
}
