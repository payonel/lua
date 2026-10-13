package com.payonel.lungolua.platform;

import com.payonel.lungolua.bytes.ByteView;

public final class OsDate
{
  public int year;
  public int month;
  public int day;
  public int hour;
  public int min;
  public int sec;
  public int yday;
  public int wday;
  public int isdst;
  public long gmtoff;
  public ByteView zone;

  public OsDate()
  {
  }

  /* the copy (C++'s struct assignment) */
  public OsDate(OsDate o)
  {
    year = o.year;
    month = o.month;
    day = o.day;
    hour = o.hour;
    min = o.min;
    sec = o.sec;
    yday = o.yday;
    wday = o.wday;
    isdst = o.isdst;
    gmtoff = o.gmtoff;
    zone = o.zone;
  }
}
