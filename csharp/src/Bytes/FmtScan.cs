using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

public sealed class FmtScan
{
  public ByteView fmt;
  public int pos = 0;
  public int litStart = 0;
  public int litEnd = 0;
  public int escPos = 0;
  public int spec = -1;

  public FmtScan(ByteView fmt)
  {
    this.fmt = fmt;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public bool next()
  {
    litStart = pos;
    int e = fmt.find('%', pos);
    if (e == ByteView.npos)
    {
      litEnd = pos = fmt.size();
      return false;
    }
    litEnd = escPos = e;
    spec = InternalString.sv_byteat(fmt, e + 1);
    pos = (spec < 0) ? fmt.size() : e + 2;
    return true;
  }
}
