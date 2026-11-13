using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

public sealed class Libm
{
  private Libm()
  {
  }

  public static double pow(double x, double y)
  {
    if (x == 1.0)
      return 1.0;
    if (x == -1.0 && JDouble.isInfinite(y))
      return 1.0;
    return JStrictMath.pow(x, y);
  }

  public static double fmod(double x, double y)
  {
    return x % y;
  }

  public static double floor(double x)
  {
    return JStrictMath.floor(x);
  }

  public static double ceil(double x)
  {
    return JStrictMath.ceil(x);
  }

  public static double fabs(double x)
  {
    return JMath.abs(x);
  }

  public static double sqrt(double x)
  {
    return JStrictMath.sqrt(x);
  }

  public static double sin(double x)
  {
    return JStrictMath.sin(x);
  }

  public static double cos(double x)
  {
    return JStrictMath.cos(x);
  }

  public static double tan(double x)
  {
    return JStrictMath.tan(x);
  }

  public static double asin(double x)
  {
    return JStrictMath.asin(x);
  }

  public static double acos(double x)
  {
    return JStrictMath.acos(x);
  }

  public static double atan2(double y, double x)
  {
    return JStrictMath.atan2(y, x);
  }

  public static double exp(double x)
  {
    return JStrictMath.exp(x);
  }

  public static double log(double x)
  {
    return JStrictMath.log(x);
  }

  public static double log10(double x)
  {
    return JStrictMath.log10(x);
  }

  public static double log2(double x)
  {
    if (x > 0 && x != JDouble.POSITIVE_INFINITY)
    {
      int e = JMath.getExponent(x);
      if (e == JDouble.MIN_EXPONENT - 1)
      {
        long bits = JDouble.doubleToRawLongBits(x);
        if (JLong.bitCount(bits) == 1)
          return JLong.numberOfTrailingZeros(bits) - 1074;
      }
      else if (x == JMath.scalb(1.0, e))
        return e;
    }
    return JStrictMath.log(x) / JStrictMath.log(2.0);
  }

  public static double ldexp(double x, int e)
  {
    return JMath.scalb(x, e);
  }

  public sealed class Frexp
  {
    public double m;
    public int e;
  }

  public static double frexp(double x, Frexp r)
  {
    if (x == 0 || JDouble.isNaN(x) || JDouble.isInfinite(x))
    {
      r.e = 0;
      return x;
    }
    int e = JMath.getExponent(x);
    if (e == JDouble.MIN_EXPONENT - 1)
    {
      x = JMath.scalb(x, 64);
      e = JMath.getExponent(x) - 64;
      r.e = e + 1;
      return JMath.scalb(x, -(JMath.getExponent(x) + 1));
    }
    r.e = e + 1;
    return JMath.scalb(x, -(e + 1));
  }
}
