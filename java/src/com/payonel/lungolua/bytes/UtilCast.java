package com.payonel.lungolua.bytes;

import java.util.HashMap;

/* util:: (util_cast.h): an object's identity as text, and the tests' integers as light userdata */
public final class UtilCast
{
  private UtilCast()
  {
  }

  public static int pointertext(ByteWrites out, int pos, Object p)
  {
    if (p instanceof Long v && isuserdata(v))
      return out.write_unsigned(v, 16, pos, true);
    return out.write_unsigned(Integer.toUnsignedLong(System.identityHashCode(p)), 16, pos, true);
  }

  public static InternalString func_to_string(Object pf)
  {
    ByteOwner buf = new ByteOwner(20);
    int n = pointertext(buf, 0, pf);
    return new InternalString(buf.view().substr(0, n));
  }

  private static final HashMap<Long, Object> userdatas = new HashMap<>();

  /* the same integer gives the same light userdata */
  private static synchronized boolean isuserdata(Long v)
  {
    return userdatas.get(v) == v;
  }

  public static long as_integer(Object p)
  {
    if (p == null)
      return 0;
    if (p instanceof Long v && isuserdata(v))
      return v;
    return Integer.toUnsignedLong(System.identityHashCode(p));
  }

  public static synchronized Object as_userdata(long val)
  {
    if (val == 0)
      return null;
    return userdatas.computeIfAbsent(val, v -> Long.valueOf(v));
  }
}
