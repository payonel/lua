using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

public sealed class OsExecStatus
{
  public bool exited;
  public int code;

  public OsExecStatus(bool exited, int code)
  {
    this.exited = exited;
    this.code = code;
  }
}
