using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

public sealed class OsSystemResult
{
  public int stat;
  public int err;

  public OsSystemResult(int stat, int err)
  {
    this.stat = stat;
    this.err = err;
  }
}
