package com.payonel.lungolua.compiler;

import com.payonel.lungolua.object.TString;

/* description of pending goto statements and label statements */
public final class Labeldesc
{
  public TString name; /* label identifier */
  public int pc;       /* position in code */
  public int line;     /* line where it appeared */
  public short nactvar; /* number of active variables in that position */
  public byte close;   /* true for goto that escapes upvalues */

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
