package com.payonel.lungolua.api;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.object.CFuncInfo;

public final class luaL_RegCFI
{
  public ByteView name;
  public CFuncInfo cfi;

  public luaL_RegCFI(ByteView name, CFuncInfo cfi)
  {
    this.name = name;
    this.cfi = cfi;
  }
}
