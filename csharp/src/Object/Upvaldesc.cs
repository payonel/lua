using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Object;

/*
** Description of an upvalue for function prototypes
*/
public sealed class Upvaldesc
{
  public TString name;    /* upvalue name (for debug information) */
  public bool instack; /* whether it is in stack (register) */
  public int idx;         /* index of upvalue (in stack or in outer function's list) */
  public sbyte kind;       /* kind of corresponding variable */
}
