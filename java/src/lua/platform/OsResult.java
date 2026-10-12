package lua.platform;

public final class OsResult
{
  public boolean ok;
  public int err;

  public OsResult(boolean ok, int err)
  {
    this.ok = ok;
    this.err = err;
  }
}
