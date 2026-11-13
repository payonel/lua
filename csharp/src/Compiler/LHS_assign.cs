using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Compiler;

/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
public sealed class LHS_assign
{
  internal LHS_assign prev;
  internal readonly expdesc v = new expdesc(); /* variable (global, local, upvalue, or indexed) */

  internal LHS_assign()
  {
  }

  internal LHS_assign(LHS_assign o)
  {
    copyfrom(o);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  internal void copyfrom(LHS_assign o)
  {
    prev = o.prev;
    v.copyfrom(o.v);
  }
}
