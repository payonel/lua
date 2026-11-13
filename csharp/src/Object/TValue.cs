using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Object;

/* a tagged value: never shared; 'getobj'/'copyobj' copy its fields */
public class TValue
{
  public long prim;
  public object @ref;
  public sbyte tt_;

  public TValue()
  {
  }

  public TValue(long prim, object @ref, sbyte tt)
  {
    this.prim = prim;
    this.@ref = @ref;
    this.tt_ = tt;
  }
}
