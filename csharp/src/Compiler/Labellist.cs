using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Compiler;

/* list of labels or gotos */
public sealed class Labellist
{
  public Labeldesc[] arr = new Labeldesc[0]; /* array */
  public int n;    /* number of entries in use */
  public int size; /* array size */

  public Labellist()
  {
  }

  public Labellist(Labellist o)
  {
    copyfrom(o);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void copyfrom(Labellist o)
  {
    arr = o.arr;
    n = o.n;
    size = o.size;
  }
}
