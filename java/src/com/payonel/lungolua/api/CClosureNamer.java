package com.payonel.lungolua.api;

import static com.payonel.lungolua.object.Lnames.*;

public final class CClosureNamer
{
  public CClosureNamer(lua_CClosureKind kind)
  {
    lua_registercclosure(kind);
  }
}
