package lua.platform;

public final class Libm
{
  private Libm()
  {
  }

  public static double pow(double x, double y)
  {
    if (x == 1.0)
      return 1.0;
    if (x == -1.0 && Double.isInfinite(y))
      return 1.0;
    return StrictMath.pow(x, y);
  }

  public static double fmod(double x, double y)
  {
    return x % y;
  }

  public static double floor(double x)
  {
    return StrictMath.floor(x);
  }

  public static double ceil(double x)
  {
    return StrictMath.ceil(x);
  }

  public static double fabs(double x)
  {
    return Math.abs(x);
  }

  public static double sqrt(double x)
  {
    return StrictMath.sqrt(x);
  }

  public static double sin(double x)
  {
    return StrictMath.sin(x);
  }

  public static double cos(double x)
  {
    return StrictMath.cos(x);
  }

  public static double tan(double x)
  {
    return StrictMath.tan(x);
  }

  public static double asin(double x)
  {
    return StrictMath.asin(x);
  }

  public static double acos(double x)
  {
    return StrictMath.acos(x);
  }

  public static double atan2(double y, double x)
  {
    return StrictMath.atan2(y, x);
  }

  public static double exp(double x)
  {
    return StrictMath.exp(x);
  }

  public static double log(double x)
  {
    return StrictMath.log(x);
  }

  public static double log10(double x)
  {
    return StrictMath.log10(x);
  }

  public static double log2(double x)
  {
    if (x > 0 && x != Double.POSITIVE_INFINITY)
    {
      int e = Math.getExponent(x);
      if (e == Double.MIN_EXPONENT - 1)
      {
        long bits = Double.doubleToRawLongBits(x);
        if (Long.bitCount(bits) == 1)
          return Long.numberOfTrailingZeros(bits) - 1074;
      }
      else if (x == Math.scalb(1.0, e))
        return e;
    }
    return StrictMath.log(x) / StrictMath.log(2.0);
  }

  public static double ldexp(double x, int e)
  {
    return Math.scalb(x, e);
  }

  public static final class Frexp
  {
    public double m;
    public int e;
  }

  public static double frexp(double x, Frexp r)
  {
    if (x == 0 || Double.isNaN(x) || Double.isInfinite(x))
    {
      r.e = 0;
      return x;
    }
    int e = Math.getExponent(x);
    if (e == Double.MIN_EXPONENT - 1)
    {
      x = Math.scalb(x, 64);
      e = Math.getExponent(x) - 64;
      r.e = e + 1;
      return Math.scalb(x, -(Math.getExponent(x) + 1));
    }
    r.e = e + 1;
    return Math.scalb(x, -(e + 1));
  }
}
