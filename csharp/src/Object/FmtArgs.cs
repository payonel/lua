using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using static Payonel.LungoLua.Object.Lobject;

namespace Payonel.LungoLua.Object;

public sealed class FmtArgs
{
  public int n = 0;
  public readonly FmtArg[] arg = new FmtArg[MAXFMTARGS];

  public FmtArgs()
  {
    for (int k = 0; k < MAXFMTARGS; k++)
      arg[k] = new FmtArg();
  }

  public FmtArgs str(ByteView s)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    { if (JAssert.on && !( s.array() != null)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('S');
    arg[n].s = s;
    n++;
    return this;
  }

  public FmtArgs chr(int c)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('c');
    arg[n].i = c;
    n++;
    return this;
  }

  public FmtArgs num(int d)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('d');
    arg[n].i = d;
    n++;
    return this;
  }

  public FmtArgs lint(long i)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('I');
    arg[n].i = i;
    n++;
    return this;
  }

  public FmtArgs flt(double f)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('f');
    arg[n].n = f;
    n++;
    return this;
  }

  public FmtArgs ptr(object p)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('p');
    arg[n].p = p;
    n++;
    return this;
  }

  public FmtArgs utf8(long u)
  {
    { if (JAssert.on && !( n < MAXFMTARGS)) throw new AssertionError(); }
    arg[n].tag = (sbyte)('U');
    arg[n].i = u;
    n++;
    return this;
  }
}
