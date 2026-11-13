using System;
using System.Globalization;
using System.Numerics;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

/* std::to_chars and std::from_chars for doubles, as the C++ uses them */
public sealed class ToChars
{
  private ToChars()
  {
  }

  public enum Errc
  {
    ok,
    invalid_argument,
    result_out_of_range
  }

  /* byte_view::parse_result */
  public sealed class ParseResult
  {
    public readonly int next;
    public readonly Errc ec;
    public readonly double result;

    internal ParseResult(int next, Errc ec, double result)
    {
      this.next = next;
      this.ec = ec;
      this.result = result;
    }
  }

  /* a double's exact decimal value: unscaled * 10^-scale (Java's BigDecimal) */
  private struct Dec
  {
    public BigInteger unscaled;
    public int scale;

    public Dec(BigInteger u, int s)
    {
      unscaled = u;
      scale = s;
    }

    public static Dec of(double d)
    {
      long bits = BitConverter.DoubleToInt64Bits(d) & 0x7fffffffffffffffL;
      int be = (int)(bits >> 52);
      long m = bits & 0xfffffffffffffL;
      int e;
      if (be == 0)
        e = -1074;
      else
      {
        m |= 1L << 52;
        e = be - 1075;
      }
      if (m == 0)
        return new Dec(BigInteger.Zero, 0);
      if (e >= 0)
        return new Dec(new BigInteger(m) << e, 0);
      return new Dec(new BigInteger(m) * BigInteger.Pow(5, -e), -e);
    }

    public int signum()
    {
      return unscaled.Sign;
    }

    public int precision()
    {
      if (unscaled.IsZero)
        return 1;
      return unscaled.ToString(CultureInfo.InvariantCulture).Length;
    }

    /* divide 'u' by 10^n, rounding half to even */
    private static BigInteger divround(BigInteger u, int n)
    {
      BigInteger p = BigInteger.Pow(10, n);
      BigInteger q = BigInteger.DivRem(u, p, out BigInteger r);
      BigInteger twice = r * 2;
      int c = twice.CompareTo(p);
      if (c > 0 || (c == 0 && !q.IsEven))
        q += 1;
      return q;
    }

    public Dec setScale(int p)
    {
      if (p >= scale)
        return new Dec(unscaled * BigInteger.Pow(10, p - scale), p);
      return new Dec(divround(unscaled, scale - p), p);
    }

    public Dec round(int digits)
    {
      int prec = precision();
      if (unscaled.IsZero || prec <= digits)
        return this;
      int drop = prec - digits;
      BigInteger u = divround(unscaled, drop);
      int s = scale - drop;
      if (u.ToString(CultureInfo.InvariantCulture).Length > digits)
      {
        u /= 10;
        s -= 1;
      }
      return new Dec(u, s);
    }

    public string toPlainString()
    {
      string u = unscaled.ToString(CultureInfo.InvariantCulture);
      if (scale <= 0)
        return scale == 0 || unscaled.IsZero ? u : u + new string('0', -scale);
      if (u.Length <= scale)
        u = new string('0', scale - u.Length + 1) + u;
      return u.Substring(0, u.Length - scale) + "." + u.Substring(u.Length - scale);
    }
  }

  /* to_chars(value, fmt, precision): as printf's %.*g, %.*e or %.*f */
  public static sbyte[] format(double v, CharsFormat fmt, int precision)
  {
    bool neg = JDouble.doubleToRawLongBits(v) < 0;
    string s;
    if (JDouble.isNaN(v))
      s = neg ? "-nan" : "nan";
    else if (JDouble.isInfinite(v))
      s = neg ? "-inf" : "inf";
    else
    {
      Dec d = Dec.of(v);
      string body;
      switch (fmt)
      {
      case CharsFormat.@fixed: body = @fixed(d, precision); break;
      case CharsFormat.scientific: body = scientific(d, precision); break;
      case CharsFormat.general: body = general(d, precision); break;
      default: throw new NotSupportedException("hex to_chars");
      }
      s = neg ? "-" + body : body;
    }
    return s.getBytes(JCharsets.ISO_8859_1);
  }

  private static string @fixed(Dec d, int p)
  {
    return d.setScale(p).toPlainString();
  }

  /* the exponent of 'd' written with 'digits' significant digits */
  private static int exponent(Dec d, int digits)
  {
    if (d.signum() == 0)
      return 0;
    Dec r = d.round(digits);
    return r.precision() - r.scale - 1;
  }

  private static string scientific(Dec d, int p)
  {
    StringBuilder sb = new StringBuilder();
    int e;
    string digits;
    if (d.signum() == 0)
    {
      e = 0;
      digits = "0".repeat(p + 1);
    }
    else
    {
      Dec r = d.round(p + 1);
      e = r.precision() - r.scale - 1;
      string u = r.unscaled.ToString(CultureInfo.InvariantCulture);
      if (u.length() < p + 1)
        u = u + "0".repeat(p + 1 - u.length());
      digits = u.substring(0, p + 1);
    }
    sb.append(digits.charAt(0));
    if (p > 0)
      sb.append('.').append(digits, 1, p + 1);
    sb.append('e').append(e < 0 ? '-' : '+');
    int ae = JMath.abs(e);
    if (ae < 10)
      sb.append('0');
    sb.append(ae);
    return sb.toString();
  }

  private static string general(Dec d, int p)
  {
    int P = (p == 0) ? 1 : p;
    int X = exponent(d, P);
    string s;
    if (P > X && X >= -4)
      s = @fixed(d, P - 1 - X);
    else
      s = scientific(d, P - 1);
    return stripzeros(s);
  }

  private static string stripzeros(string s)
  {
    int e = s.indexOf('e');
    string mant = (e < 0) ? s : s.substring(0, e);
    string exp = (e < 0) ? "" : s.substring(e);
    if (mant.indexOf('.') >= 0)
    {
      int n = mant.length();
      while (mant.charAt(n - 1) == '0')
        n--;
      if (mant.charAt(n - 1) == '.')
        n--;
      mant = mant.substring(0, n);
    }
    return mant + exp;
  }

  private static bool matchword(ByteView s, int p, string w)
  {
    if (p + w.length() > s.size())
      return false;
    for (int i = 0; i < w.length(); i++)
      if (JCharacter.toLowerCase((char) s.uat(p + i)) != w.charAt(i))
        return false;
    return true;
  }

  private static bool isdigit(ByteView s, int i)
  {
    return i < s.size() && s.uat(i) >= '0' && s.uat(i) <= '9';
  }

  /* from_chars(s from 'pos', general): a decimal number, "inf" or "nan" */
  public static ParseResult parse(ByteView s, int pos)
  {
    int p = pos;
    bool neg = false;
    if (p < s.size() && s.uat(p) == '-')
    {
      neg = true;
      p++;
    }
    if (matchword(s, p, "inf"))
    {
      int q = p + (matchword(s, p, "infinity") ? 8 : 3);
      return new ParseResult(q, Errc.ok, neg ? JDouble.NEGATIVE_INFINITY : JDouble.POSITIVE_INFINITY);
    }
    if (matchword(s, p, "nan"))
    {
      int q = p + 3;
      if (q < s.size() && s.uat(q) == '(')
      {
        int r = q + 1;
        while (r < s.size() && (JCharacter.isLetterOrDigit((char) s.uat(r)) || s.uat(r) == '_'))
          r++;
        if (r < s.size() && s.uat(r) == ')')
          q = r + 1;
      }
      return new ParseResult(q, Errc.ok, neg ? -JDouble.NaN : JDouble.NaN);
    }
    int start = p;
    bool nonzero = false;
    int ndigits = 0;
    while (isdigit(s, p))
    {
      if (s.uat(p) != '0')
        nonzero = true;
      p++;
      ndigits++;
    }
    if (p < s.size() && s.uat(p) == '.')
    {
      p++;
      while (isdigit(s, p))
      {
        if (s.uat(p) != '0')
          nonzero = true;
        p++;
        ndigits++;
      }
    }
    if (ndigits == 0)
      return new ParseResult(pos, Errc.invalid_argument, 0.0);
    if (p < s.size() && (s.uat(p) == 'e' || s.uat(p) == 'E'))
    {
      int q = p + 1;
      if (q < s.size() && (s.uat(q) == '+' || s.uat(q) == '-'))
        q++;
      if (isdigit(s, q))
      {
        while (isdigit(s, q))
          q++;
        p = q;
      }
    }
    string text = s.substr(start, p - start).toString();
    double v;
    try
    {
      v = JDouble.parseDouble(text);
    }
    catch (FormatException e)
    {
      return new ParseResult(pos, Errc.invalid_argument, 0.0);
    }
    if (JDouble.isInfinite(v) || (v == 0.0 && nonzero))
      return new ParseResult(p, Errc.result_out_of_range, 0.0);
    return new ParseResult(p, Errc.ok, neg ? -v : v);
  }
}
