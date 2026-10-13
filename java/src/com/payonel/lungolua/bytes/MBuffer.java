package com.payonel.lungolua.bytes;

import static com.payonel.lungolua.runtime.Lmem.*;

import com.payonel.lungolua.runtime.lua_State;

public final class MBuffer
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

  public void destroy()
  {
    if (bytes_.held())
      luaM_alloc_char_span(L_.l_G, L_, bytes_, 0, false);
  }

  public void attach(lua_State L)
  {
    assert bytes_.empty() || L == L_;
    L_ = L;
  }

  public void growTo(long cap)
  {
    if (cap == bytes_.size())
      return;
    assert L_ != null;
    bytes_ = luaM_alloc_char_span(L_.l_G, L_, bytes_, cap, true);
  }

  public void append(byte c)
  {
    if (len_ + 1 > bytes_.size())
      growTo(len_ + 1);
    bytes_.set(len_++, c);
  }

  public void reset()
  {
    len_ = 0;
  }

  public void drop(int k)
  {
    len_ -= k;
  }

  public int size()
  {
    return len_;
  }

  public int capacity()
  {
    return bytes_.size();
  }

  public ByteView view()
  {
    return new ByteView(bytes_.array(), 0, len_);
  }

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
  public void adopt(ByteOwner b, int len)
  {
    assert bytes_.empty();
    bytes_ = b;
    len_ = len;
  }
}
