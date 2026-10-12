package lua.object;

import lua.api.LuaConf;
import lua.platform.Libm;

public final class Llimits
{
  private Llimits()
  {
  }

  public static final long MAX_LMEM = Long.MAX_VALUE;
  public static final long MAX_SIZET = -1L;
  public static final long MAX_SIZE = LuaConf.LUA_MAXINTEGER;
  public static final int MAX_JARRAY = Integer.MAX_VALUE - 8;

  public static boolean ispow2(long x)
  {
    return (x & (x - 1)) == 0;
  }

  public static int point2uint(Object p)
  {
    return System.identityHashCode(p);
  }

  public static double luai_numadd(Object L, double a, double b)
  {
    return a + b;
  }

  public static double luai_numsub(Object L, double a, double b)
  {
    return a - b;
  }

  public static double luai_nummul(Object L, double a, double b)
  {
    return a * b;
  }

  public static double luai_numdiv(Object L, double a, double b)
  {
    return a / b;
  }

  public static double luai_numpow(Object L, double a, double b)
  {
    return (b == 2) ? a * a : Libm.pow(a, b);
  }

  public static double luai_numidiv(Object L, double a, double b)
  {
    return Math.floor(luai_numdiv(L, a, b));
  }

  public static double luai_numunm(Object L, double a)
  {
    return -a;
  }

  public static boolean luai_numeq(double a, double b)
  {
    return a == b;
  }

  public static boolean luai_numlt(double a, double b)
  {
    return a < b;
  }

  public static boolean luai_numle(double a, double b)
  {
    return a <= b;
  }

  public static boolean luai_numgt(double a, double b)
  {
    return a > b;
  }

  public static boolean luai_numge(double a, double b)
  {
    return a >= b;
  }

  public static boolean luai_numisnan(double a)
  {
    return !(a == a);
  }

  public static double luai_nummod(Object L, double a, double b)
  {
    double m = a % b; /* fmod */
    if ((m > 0) ? b < 0 : (m < 0 && b > 0))
      m += b;
    return m;
  }

  public static long l_umod(long a, long b)
  {
    return Long.remainderUnsigned(a, b);
  }

  public static boolean l_ult(long a, long b)
  {
    return Long.compareUnsigned(a, b) < 0;
  }

  public static long l_intadd(long a, long b)
  {
    return a + b;
  }

  public static long l_intsub(long a, long b)
  {
    return a - b;
  }

  public static long l_intmul(long a, long b)
  {
    return a * b;
  }

  public static long l_intband(long a, long b)
  {
    return a & b;
  }

  public static long l_intbor(long a, long b)
  {
    return a | b;
  }

  public static long l_intbxor(long a, long b)
  {
    return a ^ b;
  }

  public static long l_intunm(long a)
  {
    return -a;
  }

  public static long l_intbnot(long a)
  {
    return ~a;
  }

  public static final class IntResult
  {
    public long v;
  }

  public static final class NumResult
  {
    public double v;
  }

  public static int lua_numbertointeger(double n, IntResult p)
  {
    if (n >= (double) LuaConf.LUA_MININTEGER && n < -(double) LuaConf.LUA_MININTEGER)
    {
      p.v = (long) n;
      return 1;
    }
    return 0;
  }
}
