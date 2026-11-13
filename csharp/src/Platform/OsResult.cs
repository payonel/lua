using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

public struct OsResult
{
  public bool ok;
  public int err;

  public OsResult(bool ok, int err)
  {
    this.ok = ok;
    this.err = err;
  }
}
