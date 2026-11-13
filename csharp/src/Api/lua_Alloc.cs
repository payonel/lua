using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Api;

public interface lua_Alloc
{
  bool permit(long osize, long nsize, int tag);
}
