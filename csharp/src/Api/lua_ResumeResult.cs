using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Api;

public sealed class lua_ResumeResult
{
  public int status;
  public int nres;

  public lua_ResumeResult(int status, int nres)
  {
    this.status = status;
    this.nres = nres;
  }
}
