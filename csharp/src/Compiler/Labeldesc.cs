using System;
using Payonel.LungoLua.Compat;
using TString = Payonel.LungoLua.Object.TString;

namespace Payonel.LungoLua.Compiler;

/* description of pending goto statements and label statements */
public sealed class Labeldesc
{
  public TString name; /* label identifier */
  public int pc;       /* position in code */
  public int line;     /* line where it appeared */
  public short nactvar; /* number of active variables in that position */
  public bool close; /* true for goto that escapes upvalues */

  public Labeldesc()
  {
  }

  public Labeldesc(Labeldesc o)
  {
    copyfrom(o);
  }

  public void copyfrom(Labeldesc o)
  {
    name = o.name;
    pc = o.pc;
    line = o.line;
    nactvar = o.nactvar;
    close = o.close;
  }
}
