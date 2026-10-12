package lua.api;

import static lua.object.Lnames.*;

public final class CClosureNamer
{
  public CClosureNamer(lua_CClosureKind kind)
  {
    lua_registercclosure(kind);
  }
}
