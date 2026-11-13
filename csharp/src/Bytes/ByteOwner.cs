using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

/* byte_owner, and byte_inline<N> (a ByteOwner of N bytes) */
public sealed class ByteOwner : ByteWrites
{
  private sbyte[] p;
  private int n;

  public ByteOwner()
  {
    this.p = null;
    this.n = 0;
  }

  public ByteOwner(int n)
  {
    this.p = new sbyte[n];
    this.n = n;
  }

  public ByteOwner(ByteView view) : this(view.len)
  {

    apply(view);
  }

  /* takes 'a' as its bytes */
  public static ByteOwner adopt(sbyte[] a)
  {
    ByteOwner b = new ByteOwner();
    b.p = a;
    b.n = a.Length;
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

  public override int size()
  {
    return n;
  }

  public override sbyte[] array()
  {
    return p;
  }

  public override int offset()
  {
    return 0;
  }

  /* data() != nullptr */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public bool held()
  {
    return p != null;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteSpan span()
  {
    return new ByteSpan(p == null ? new sbyte[0] : p, 0, n);
  }
}
