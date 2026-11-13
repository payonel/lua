using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Object;

/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
public sealed class LocVar
{
  public TString varname;
  public int startpc; /* first point where variable is active */
  public int endpc;   /* first point where variable is dead */
}
