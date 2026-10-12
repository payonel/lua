package lua.platform;

public final class OsSystemResult
{
  public int stat;
  public int err;

  public OsSystemResult(int stat, int err)
  {
    this.stat = stat;
    this.err = err;
  }
}
