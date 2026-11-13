using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Runtime;

/* type of protected functions, to be ran by 'runprotected' */
public interface ProtectedCall
{
  void run(lua_State L);
}
