package com.payonel.lungolua.bytes;

import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;

/* std::to_chars and std::from_chars for doubles, as the C++ uses them */
public final class ToChars
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
  public static final class ParseResult
  {
    public final int next;
    public final Errc ec;
    public final double result;

    ParseResult(int next, Errc ec, double result)
    {
      this.next = next;
      this.ec = ec;
      this.result = result;
    }
  }

  /* to_chars(value, fmt, precision): as printf's %.*g, %.*e or %.*f */
  public static byte[] format(double v, CharsFormat fmt, int precision)
  {
    boolean neg = Double.doubleToRawLongBits(v) < 0;
    String s;
    if (Double.isNaN(v))
      s = neg ? "-nan" : "nan";
    else if (Double.isInfinite(v))
      s = neg ? "-inf" : "inf";
    else
    {
      BigDecimal d = new BigDecimal(Math.abs(v));
      String body = switch (fmt)
      {
        case fixed -> fixed(d, precision);
        case scientific -> scientific(d, precision);
        case general -> general(d, precision);
        case hex -> throw new UnsupportedOperationException("hex to_chars");
      };
      s = neg ? "-" + body : body;
    }
    return s.getBytes(StandardCharsets.ISO_8859_1);
  }

  private static String fixed(BigDecimal d, int p)
  {
    return d.setScale(p, RoundingMode.HALF_EVEN).toPlainString();
  }

  /* the exponent of 'd' written with 'digits' significant digits */
  private static int exponent(BigDecimal d, int digits)
  {
    if (d.signum() == 0)
      return 0;
    BigDecimal r = d.round(new MathContext(digits, RoundingMode.HALF_EVEN));
    return r.precision() - r.scale() - 1;
  }

  private static String scientific(BigDecimal d, int p)
  {
    StringBuilder sb = new StringBuilder();
    int e;
    String digits;
    if (d.signum() == 0)
    {
      e = 0;
      digits = "0".repeat(p + 1);
    }
    else
    {
      BigDecimal r = d.round(new MathContext(p + 1, RoundingMode.HALF_EVEN));
      e = r.precision() - r.scale() - 1;
      String u = r.unscaledValue().toString();
      if (u.length() < p + 1)
        u = u + "0".repeat(p + 1 - u.length());
      digits = u.substring(0, p + 1);
    }
    sb.append(digits.charAt(0));
    if (p > 0)
      sb.append('.').append(digits, 1, p + 1);
    sb.append('e').append(e < 0 ? '-' : '+');
    int ae = Math.abs(e);
    if (ae < 10)
      sb.append('0');
    sb.append(ae);
    return sb.toString();
  }

  private static String general(BigDecimal d, int p)
  {
    int P = (p == 0) ? 1 : p;
    int X = exponent(d, P);
    String s;
    if (P > X && X >= -4)
      s = fixed(d, P - 1 - X);
    else
      s = scientific(d, P - 1);
    return stripzeros(s);
  }

  private static String stripzeros(String s)
  {
    int e = s.indexOf('e');
    String mant = (e < 0) ? s : s.substring(0, e);
    String exp = (e < 0) ? "" : s.substring(e);
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

  private static boolean matchword(ByteView s, int p, String w)
  {
    if (p + w.length() > s.size())
      return false;
    for (int i = 0; i < w.length(); i++)
      if (Character.toLowerCase((char) s.uat(p + i)) != w.charAt(i))
        return false;
    return true;
  }

  private static boolean isdigit(ByteView s, int i)
  {
    return i < s.size() && s.uat(i) >= '0' && s.uat(i) <= '9';
  }

  /* from_chars(s from 'pos', general): a decimal number, "inf" or "nan" */
  public static ParseResult parse(ByteView s, int pos)
  {
    int p = pos;
    boolean neg = false;
    if (p < s.size() && s.uat(p) == '-')
    {
      neg = true;
      p++;
    }
    if (matchword(s, p, "inf"))
    {
      int q = p + (matchword(s, p, "infinity") ? 8 : 3);
      return new ParseResult(q, Errc.ok, neg ? Double.NEGATIVE_INFINITY : Double.POSITIVE_INFINITY);
    }
    if (matchword(s, p, "nan"))
    {
      int q = p + 3;
      if (q < s.size() && s.uat(q) == '(')
      {
        int r = q + 1;
        while (r < s.size() && (Character.isLetterOrDigit((char) s.uat(r)) || s.uat(r) == '_'))
          r++;
        if (r < s.size() && s.uat(r) == ')')
          q = r + 1;
      }
      return new ParseResult(q, Errc.ok, neg ? -Double.NaN : Double.NaN);
    }
    int start = p;
    boolean nonzero = false;
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
    String text = s.substr(start, p - start).toString();
    double v;
    try
    {
      v = Double.parseDouble(text);
    }
    catch (NumberFormatException e)
    {
      return new ParseResult(pos, Errc.invalid_argument, 0.0);
    }
    if (Double.isInfinite(v) || (v == 0.0 && nonzero))
      return new ParseResult(p, Errc.result_out_of_range, 0.0);
    return new ParseResult(p, Errc.ok, neg ? -v : v);
  }
}
