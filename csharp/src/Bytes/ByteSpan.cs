using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

public sealed class ByteSpan : ByteWrites
{
  private readonly sbyte[] p;
  private readonly int o;
  private readonly int n;

  public ByteSpan() : this(new sbyte[0], 0, 0)
  {

  }

  internal ByteSpan(sbyte[] p, int o, int n)
  {
    this.p = p;
    this.o = o;
    this.n = n;
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
    return o;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteSpan subspan(int pos, int len)
  {
    { if (JAssert.on && !( pos <= n)) throw new AssertionError(); }
    return new ByteSpan(p, o + pos, (len == ByteView.npos) ? n - pos : JMath.min(len, n - pos));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteSpan subspan(int pos)
  {
    return subspan(pos, ByteView.npos);
  }
}
