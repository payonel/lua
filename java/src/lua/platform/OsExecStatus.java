package lua.platform;

public final class OsExecStatus
{
  public boolean exited;
  public int code;

  public OsExecStatus(boolean exited, int code)
  {
    this.exited = exited;
    this.code = code;
  }
}
