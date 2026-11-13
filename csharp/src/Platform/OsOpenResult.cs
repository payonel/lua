using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

public struct OsOpenResult
{
  public OsFile f;
  public int err;

  public OsOpenResult(OsFile f, int err)
  {
    this.f = f;
    this.err = err;
  }
}
