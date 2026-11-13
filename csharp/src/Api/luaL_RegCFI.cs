using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using CFuncInfo = Payonel.LungoLua.Object.CFuncInfo;

namespace Payonel.LungoLua.Api;

public sealed class luaL_RegCFI
{
  public ByteView name;
  public CFuncInfo cfi;

  public luaL_RegCFI(ByteView name, CFuncInfo cfi)
  {
    this.name = name;
    this.cfi = cfi;
  }
}
