package com.payonel.lungolua.platform;

public final class OsOpenResult
{
  public OsFile f;
  public int err;

  public OsOpenResult(OsFile f, int err)
  {
    this.f = f;
    this.err = err;
  }
}
