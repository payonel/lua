package lua.api;

import lua.bytes.ByteView;
import lua.runtime.CallInfo;

public final class lua_Debug
{
  public int event;
  public HostString name = new HostString();
  public HostString namewhat = new HostString();
  public HostString what = new HostString(); /* (S) 'Lua', 'C', 'main', 'tail' */
  public ByteView source = ByteView.EMPTY;   /* (S) */
  public int currentline;      /* (l) */
  public int linedefined;      /* (S) */
  public int lastlinedefined;  /* (S) */
  public int nups;             /* (u) number of upvalues */
  public int nparams;          /* (u) number of parameters */
  public byte isvararg;        /* (u) */
  public int extraargs;        /* (t) number of extra arguments */
  public byte istailcall;      /* (t) */
  public int ftransfer;        /* (r) index of first value transferred */
  public int ntransfer;        /* (r) number of transferred values */
  public HostString short_src = new HostString(); /* (S) */
  /* private part */
  public CallInfo i_ci; /* active function */

  public lua_Debug copy()
  {
    lua_Debug d = new lua_Debug();
    d.event = event;
    d.name = name;
    d.namewhat = namewhat;
    d.what = what;
    d.source = source;
    d.currentline = currentline;
    d.linedefined = linedefined;
    d.lastlinedefined = lastlinedefined;
    d.nups = nups;
    d.nparams = nparams;
    d.isvararg = isvararg;
    d.extraargs = extraargs;
    d.istailcall = istailcall;
    d.ftransfer = ftransfer;
    d.ntransfer = ntransfer;
    d.short_src = short_src;
    d.i_ci = i_ci;
    return d;
  }
}
