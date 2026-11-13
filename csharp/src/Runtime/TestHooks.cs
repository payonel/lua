using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Runtime;

public interface TestHooks
{
  void luai_tracegctest(lua_State L, bool first);

  lua_State luai_newstate();

  void luai_openlibstest(lua_State L);
}
