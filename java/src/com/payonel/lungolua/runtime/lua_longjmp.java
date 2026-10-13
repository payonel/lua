package com.payonel.lungolua.runtime;

public final class lua_longjmp
{
  public lua_longjmp previous;
  public int level;
  public volatile byte status; /* error code */
  public boolean exitpoint;    /* where the exit of the budget callback ends */
}
