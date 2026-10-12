package lua.compiler;

import static lua.object.Lobject.*;

import lua.object.TString;
import lua.object.TValue;

/* description of an active variable */
public final class Vardesc
{
  public final TValue k = new TValue(); /* constant value (if it is a compile-time constant) */
  public byte kind;
  public int ridx;     /* register holding the variable */
  public short pidx;   /* index of the variable in the Proto's 'locvars' array */
  public TString name; /* variable name */

  public Vardesc()
  {
  }

  public Vardesc(Vardesc o)
  {
    copyfrom(o);
  }

  public void copyfrom(Vardesc o)
  {
    copyobj(k, o.k);
    kind = o.kind;
    ridx = o.ridx;
    pidx = o.pidx;
    name = o.name;
  }
}
