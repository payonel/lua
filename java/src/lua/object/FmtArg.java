package lua.object;

import lua.bytes.ByteView;

public final class FmtArg
{
  public byte tag;
  public ByteView s;
  public long i;
  public double n;
  public Object p;
}
