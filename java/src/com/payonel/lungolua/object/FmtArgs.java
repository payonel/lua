package com.payonel.lungolua.object;

import static com.payonel.lungolua.object.Lobject.MAXFMTARGS;

import com.payonel.lungolua.bytes.ByteView;

public final class FmtArgs
{
  public int n = 0;
  public final FmtArg[] arg = new FmtArg[MAXFMTARGS];

  public FmtArgs()
  {
    for (int k = 0; k < MAXFMTARGS; k++)
      arg[k] = new FmtArg();
  }

  public FmtArgs str(ByteView s)
  {
    assert n < MAXFMTARGS;
    assert s.array() != null;
    arg[n].tag = 'S';
    arg[n].s = s;
    n++;
    return this;
  }

  public FmtArgs chr(int c)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'c';
    arg[n].i = c;
    n++;
    return this;
  }

  public FmtArgs num(int d)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'd';
    arg[n].i = d;
    n++;
    return this;
  }

  public FmtArgs lint(long i)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'I';
    arg[n].i = i;
    n++;
    return this;
  }

  public FmtArgs flt(double f)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'f';
    arg[n].n = f;
    n++;
    return this;
  }

  public FmtArgs ptr(Object p)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'p';
    arg[n].p = p;
    n++;
    return this;
  }

  public FmtArgs utf8(long u)
  {
    assert n < MAXFMTARGS;
    arg[n].tag = 'U';
    arg[n].i = u;
    n++;
    return this;
  }
}
