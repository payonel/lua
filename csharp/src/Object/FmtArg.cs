using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Object;

public sealed class FmtArg
{
  public sbyte tag;
  public ByteView s;
  public long i;
  public double n;
  public object p;
}
