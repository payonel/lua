package lua.bytes;

public final class ByteSpan extends ByteWrites
{
  private final byte[] p;
  private final int o;
  private final int n;

  public ByteSpan()
  {
    this(new byte[0], 0, 0);
  }

  ByteSpan(byte[] p, int o, int n)
  {
    this.p = p;
    this.o = o;
    this.n = n;
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
    return o;
  }

  public ByteSpan subspan(int pos, int len)
  {
    assert pos <= n;
    return new ByteSpan(p, o + pos, (len == ByteView.npos) ? n - pos : Math.min(len, n - pos));
  }

  public ByteSpan subspan(int pos)
  {
    return subspan(pos, ByteView.npos);
  }
}
