package com.payonel.lungolua.api;

import static com.payonel.lungolua.object.Lnames.*;

public final class KNamer
{
  public KNamer(String name, lua_KFunction k)
  {
    lua_registerk(name, k);
  }
}
