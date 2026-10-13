package com.payonel.lungolua.bytes;

/* byte_owner, and byte_inline<N> (a ByteOwner of N bytes) */
public final class ByteOwner extends ByteWrites
{
  private byte[] p;
  private int n;

  public ByteOwner()
  {
    this.p = null;
    this.n = 0;
  }

  public ByteOwner(int n)
  {
    this.p = new byte[n];
    this.n = n;
  }

  public ByteOwner(ByteView view)
  {
    this(view.len);
    apply(view);
  }

  /* takes 'a' as its bytes */
  public static ByteOwner adopt(byte[] a)
  {
    ByteOwner b = new ByteOwner();
    b.p = a;
    b.n = a.length;
    return b;
  }

  /* the move: 'o' is left empty */
  public static ByteOwner move(ByteOwner o)
  {
    ByteOwner b = new ByteOwner();
    b.p = o.p;
    b.n = o.n;
    o.p = null;
    o.n = 0;
    return b;
  }

  @Override
  public int size()
  {
    return n;
  }

  @Override
  public byte[] array()
  {
    return p;
  }

  @Override
  public int offset()
  {
    return 0;
  }

  /* data() != nullptr */
  public boolean held()
  {
    return p != null;
  }

  public ByteSpan span()
  {
    return new ByteSpan(p == null ? new byte[0] : p, 0, n);
  }
}
