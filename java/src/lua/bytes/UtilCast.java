package lua.bytes;

import java.util.HashMap;

/* util:: (util_cast.h): an object's identity as text, and the tests' integers as light userdata */
public final class UtilCast
{
  private UtilCast()
  {
  }

  public static int pointertext(ByteWrites out, int pos, Object p)
  {
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
  public static synchronized Object as_userdata(long val)
  {
    return userdatas.computeIfAbsent(val, v -> Long.valueOf(v));
  }
}
