package com.payonel.lungolua.bytes;

import java.util.Arrays;

public final class InternalString
{
  private ByteOwner bytes_ = new ByteOwner();
  private int len_ = 0;

  public InternalString()
  {
  }

  public InternalString(ByteView src, int off, int len)
  {
    off = Math.min(off, src.size());
    len = Math.min(len, src.size() - off);
    bytes_ = new ByteOwner(src.substr(off, len));
    len_ = len;
  }

  public InternalString(ByteView src)
  {
    this(src, 0, src.size());
  }

  /* takes 'block' as its bytes: the first 'len' of them */
  public InternalString(ByteOwner block, int len)
  {
    bytes_ = block;
    len_ = len;
    assert len <= bytes_.size();
  }

  /* the move: 'o' is left empty */
  public static InternalString move(InternalString o)
  {
    InternalString s = new InternalString(o.bytes_, o.len_);
    o.bytes_ = new ByteOwner();
    o.len_ = 0;
    return s;
  }

  public int size()
  {
    return len_;
  }

  public boolean empty()
  {
    return len_ == 0;
  }

  public int byteAt(int i)
  {
    return bytes_.array()[i] & 0xFF;
  }

  public ByteView view()
  {
    return (bytes_.array() == null) ? ByteView.EMPTY : new ByteView(bytes_.array(), 0, len_);
  }

  /* its bytes, for its maker to write before the string is shared */
  public ByteSpan area()
  {
    return bytes_.span().subspan(0, len_);
  }

  public boolean equals(InternalString o)
  {
    if (len_ != o.len_)
      return false;
    return len_ == 0 || Arrays.equals(bytes_.array(), 0, len_, o.bytes_.array(), 0, len_);
  }

  @Override
  public boolean equals(Object o)
  {
    return o instanceof InternalString s && equals(s);
  }

  @Override
  public int hashCode()
  {
    return hash();
  }

  public int compare(InternalString o)
  {
    return view().compare(o.view());
  }

  public static int hashBytes(byte[] str, int off, int l)
  {
    int h = 0x5bd1e995 ^ l;
    for (int i = 0; i < l; i++)
      h ^= (h << 5) + (h >>> 2) + (str[off + i] & 0xFF);
    return h;
  }

  public int hash()
  {
    return (bytes_.array() == null) ? hashBytes(new byte[0], 0, 0) : hashBytes(bytes_.array(), 0, len_);
  }

  public InternalString substr(int off, int len)
  {
    assert off <= len_;
    return new InternalString(view(), off, len);
  }

  public InternalString substr(int off)
  {
    return substr(off, len_ - off);
  }

  public InternalString concat(ByteView o)
  {
    ByteOwner b = new ByteOwner(len_ + o.size());
    b.copy_from(view(), 0);
    b.copy_from(o, len_);
    return new InternalString(b, len_ + o.size());
  }

  public static int sv_byteat(ByteView s, int i)
  {
    return (i < s.size()) ? s.uat(i) : -1;
  }
}
