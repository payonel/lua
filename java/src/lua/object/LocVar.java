package lua.object;

/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
public final class LocVar
{
  public TString varname;
  public int startpc; /* first point where variable is active */
  public int endpc;   /* first point where variable is dead */
}
