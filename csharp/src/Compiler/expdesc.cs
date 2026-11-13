using System;
using Payonel.LungoLua.Compat;
using TString = Payonel.LungoLua.Object.TString;

namespace Payonel.LungoLua.Compiler;

public sealed class expdesc
{
  public int k;
  public long ival;      /* for VKINT */
  public double nval;    /* for VKFLT */
  public TString strval; /* for VKSTR */
  public int info;       /* for generic use */

  public sealed class Ind
  {                    /* for indexed variables */
    public int idx;    /* index (R or "long" K) */
    public int t;      /* table (register or upvalue) */
    public bool ro; /* true if variable is read-only */
    public int keystr; /* index in 'k' of string key, or -1 if not a string */
  }

  public readonly Ind ind = new Ind();

  public sealed class Var
  {                  /* for local variables */
    public int ridx; /* register holding the variable */
    public short vidx; /* index in 'actvar.arr' */
  }

  public readonly Var var = new Var();
  public int t; /* patch list of 'exit when true' */
  public int f; /* patch list of 'exit when false' */

  public expdesc()
  {
  }

  public expdesc(expdesc o)
  {
    copyfrom(o);
  }

  public void copyfrom(expdesc o)
  {
    k = o.k;
    ival = o.ival;
    nval = o.nval;
    strval = o.strval;
    info = o.info;
    ind.idx = o.ind.idx;
    ind.t = o.ind.t;
    ind.ro = o.ind.ro;
    ind.keystr = o.ind.keystr;
    var.ridx = o.var.ridx;
    var.vidx = o.var.vidx;
    t = o.t;
    f = o.f;
  }
}
