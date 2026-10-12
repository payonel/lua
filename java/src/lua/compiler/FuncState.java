package lua.compiler;

import lua.object.Proto;
import lua.object.Table;

/* state needed to generate code for a given function */
public final class FuncState
{
  public Proto f;          /* current function header */
  public FuncState prev;   /* enclosing function */
  public LexState ls;      /* lexical state */
  public BlockCnt bl;      /* chain of current blocks */
  public Table kcache;     /* cache for reusing constants */
  public int pc;           /* next position to code (equivalent to 'ncode') */
  public int lasttarget;   /* 'label' of last 'jump label' */
  public int previousline; /* last line that was saved in 'lineinfo' */
  public int nk;           /* number of elements in 'k' */
  public int np;           /* number of elements in 'p' */
  public int nabslineinfo; /* number of elements in 'abslineinfo' */
  public int firstlocal;   /* index of first local var (in Dyndata array) */
  public int firstlabel;   /* index of first label (in 'dyd->label->arr') */
  public short ndebugvars; /* number of elements in 'f->locvars' */
  public short nactvar;    /* number of active variable declarations */
  public int nups;         /* number of upvalues */
  public int freereg;      /* first free register */
  public int iwthabs;      /* instructions issued since last absolute line info */
  public byte needclose;   /* function needs to close upvalues when returning */

  public FuncState()
  {
  }

  public FuncState(FuncState o)
  {
    copyfrom(o);
  }

  public void copyfrom(FuncState o)
  {
    f = o.f;
    prev = o.prev;
    ls = o.ls;
    bl = o.bl;
    kcache = o.kcache;
    pc = o.pc;
    lasttarget = o.lasttarget;
    previousline = o.previousline;
    nk = o.nk;
    np = o.np;
    nabslineinfo = o.nabslineinfo;
    firstlocal = o.firstlocal;
    firstlabel = o.firstlabel;
    ndebugvars = o.ndebugvars;
    nactvar = o.nactvar;
    nups = o.nups;
    freereg = o.freereg;
    iwthabs = o.iwthabs;
    needclose = o.needclose;
  }
}
