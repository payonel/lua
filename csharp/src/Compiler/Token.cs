using System;
using Payonel.LungoLua.Compat;
using TString = Payonel.LungoLua.Object.TString;

namespace Payonel.LungoLua.Compiler;

public sealed class Token
{
  public int token;
  public double r;
  public long i;
  public TString ts;

  public Token()
  {
  }

  public Token(Token o)
  {
    token = o.token;
    r = o.r;
    i = o.i;
    ts = o.ts;
  }
}
