using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Runtime;

public sealed class lua_longjmp
{
  public lua_longjmp previous;
  public int level;
  public volatile sbyte status; /* error code */
  public bool exitpoint;    /* where the exit of the budget callback ends */
}
