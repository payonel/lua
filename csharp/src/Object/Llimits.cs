using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using LuaConf = Payonel.LungoLua.Api.LuaConf;
using Libm = Payonel.LungoLua.Platform.Libm;

namespace Payonel.LungoLua.Object;

public sealed class Llimits
{
  private Llimits()
  {
  }

  public const long MAX_LMEM = JLong.MAX_VALUE;
  public const long MAX_SIZET = -1L;
  public const long MAX_SIZE = LuaConf.LUA_MAXINTEGER;
  public const int MAX_JARRAY = JInteger.MAX_VALUE - 8;

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool ispow2(long x)
  {
    return (x & (x - 1)) == 0;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static int point2uint(object p)
  {
    return JSystem.identityHashCode(p);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numadd(object L, double a, double b)
  {
    return a + b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numsub(object L, double a, double b)
  {
    return a - b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_nummul(object L, double a, double b)
  {
    return a * b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numdiv(object L, double a, double b)
  {
    return a / b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numpow(object L, double a, double b)
  {
    return (b == 2) ? a * a : Libm.pow(a, b);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numidiv(object L, double a, double b)
  {
    return JMath.floor(luai_numdiv(L, a, b));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_numunm(object L, double a)
  {
    return -a;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numeq(double a, double b)
  {
    return a == b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numlt(double a, double b)
  {
    return a < b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numle(double a, double b)
  {
    return a <= b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numgt(double a, double b)
  {
    return a > b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numge(double a, double b)
  {
    return a >= b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool luai_numisnan(double a)
  {
    return !(a == a);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static double luai_nummod(object L, double a, double b)
  {
    double m = a % b; /* fmod */
    if ((m > 0) ? b < 0 : (m < 0 && b > 0))
      m += b;
    return m;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_umod(long a, long b)
  {
    return JLong.remainderUnsigned(a, b);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool l_ult(long a, long b)
  {
    return JLong.compareUnsigned(a, b) < 0;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intadd(long a, long b)
  {
    return a + b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intsub(long a, long b)
  {
    return a - b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intmul(long a, long b)
  {
    return a * b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intband(long a, long b)
  {
    return a & b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intbor(long a, long b)
  {
    return a | b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intbxor(long a, long b)
  {
    return a ^ b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intunm(long a)
  {
    return -a;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static long l_intbnot(long a)
  {
    return ~a;
  }

  public struct IntResult
  {
    public long v;
  }

  public struct NumResult
  {
    public double v;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static bool lua_numbertointeger(double n, ref IntResult p)
  {
    if (n >= (double) LuaConf.LUA_MININTEGER && n < -(double) LuaConf.LUA_MININTEGER)
    {
      p.v = (long) n;
      return true;
    }
    return false;
  }
}
