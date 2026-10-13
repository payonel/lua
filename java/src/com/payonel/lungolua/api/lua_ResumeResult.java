package com.payonel.lungolua.api;

public final class lua_ResumeResult
{
  public int status;
  public int nres;

  public lua_ResumeResult(int status, int nres)
  {
    this.status = status;
    this.nres = nres;
  }
}
