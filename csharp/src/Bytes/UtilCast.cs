using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

/* util:: (util_cast.h): an object's identity as text, and the tests' integers as light userdata */
public sealed class UtilCast
{
  private UtilCast()
  {
  }

  public static int pointertext(ByteWrites @out, int pos, object p)
  {
    if (p is long v && isuserdata(p))
      return @out.write_unsigned(v, 16, pos, true);
    return @out.write_unsigned(JInteger.toUnsignedLong(JSystem.identityHashCode(p)), 16, pos, true);
  }

  public static InternalString func_to_string(object pf)
  {
    ByteOwner buf = new ByteOwner(20);
    int n = pointertext(buf, 0, pf);
    return new InternalString(buf.view().substr(0, n));
  }

  private static readonly HashMap<long, object> userdatas = new ();

  /* the same integer gives the same light userdata */
  /* (the box is the identity: one box per integer, as Java's Long objects) */
  private static bool isuserdata(object p)
  {
    return p is long v && ReferenceEquals(userdatas.get(v), p);
  }

  public static long as_integer(object p)
  {
    if (p == null)
      return 0;
    if (p is long v && isuserdata(p))
      return v;
    return JInteger.toUnsignedLong(JSystem.identityHashCode(p));
  }

  public static object as_userdata(long val)
  {
    if (val == 0)
      return null;
    object box = userdatas.get(val);
    if (box == null)
    {
      box = val;
      userdatas.put(val, box);
    }
    return box;
  }
}
