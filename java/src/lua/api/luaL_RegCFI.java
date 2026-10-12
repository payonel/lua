package lua.api;

import lua.bytes.ByteView;
import lua.object.CFuncInfo;

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
