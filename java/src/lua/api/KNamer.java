package lua.api;

import static lua.object.Lnames.*;

public final class KNamer
{
  public KNamer(String name, lua_KFunction k)
  {
    lua_registerk(name, k);
  }
}
