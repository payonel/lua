package lua.compiler;

import lua.object.TString;

public final class Token
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
