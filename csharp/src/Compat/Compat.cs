// The Java library calls the transcription keeps, with Java's semantics
// (docs/csharp-port.md, Transcription). Only what the sources use.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;

namespace Payonel.LungoLua.Compat;

/* the system properties of the Java build ('-Dname=value' before the program's arguments) */
public static class SysProps
{
  private static readonly Dictionary<string, string> props = new();

  /* takes the leading '-Dname=value' arguments; returns the rest */
  public static string[] parse(string[] args)
  {
    int i = 0;
    while (i < args.Length && args[i].StartsWith("-D", StringComparison.Ordinal))
    {
      string a = args[i].Substring(2);
      int eq = a.IndexOf('=');
      if (eq < 0)
        props[a] = "";
      else
        props[a.Substring(0, eq)] = a.Substring(eq + 1);
      i++;
    }
    string[] rest = new string[args.Length - i];
    Array.Copy(args, i, rest, 0, rest.Length);
    return rest;
  }

  public static string getProperty(string name)
  {
    return props.TryGetValue(name, out string v) ? v : null;
  }

  public static string getProperty(string name, string def)
  {
    return props.TryGetValue(name, out string v) ? v : def;
  }

  public static void setProperty(string name, string value)
  {
    props[name] = value;
  }

  public static bool getBoolean(string name)
  {
    return string.Equals(getProperty(name), "true", StringComparison.OrdinalIgnoreCase);
  }
}

/* Java's 'assert' statements run in the debug build (the JVM's -ea) */
public static class JAssert
{
  public static readonly bool on;

  static JAssert()
  {
    on = SysProps.getBoolean("lua.debug");
  }
}

public class RuntimeException : Exception
{
  public RuntimeException() { }
  public RuntimeException(string msg) : base(msg) { }
  public RuntimeException(string msg, Exception cause) : base(msg, cause) { }
  public RuntimeException(Exception cause) : base(cause?.Message, cause) { }
  /* Java's (message, cause, enableSuppression, writableStackTrace) */
  public RuntimeException(string msg, Exception cause, bool s, bool w) : base(msg, cause) { }
}

public class JError : Exception
{
  public JError() { }
  public JError(string msg) : base(msg) { }
  public JError(string msg, Exception cause) : base(msg, cause) { }
}

public class AssertionError : JError
{
  public AssertionError() : base("assertion failed") { }
  public AssertionError(string msg) : base(msg) { }
  public AssertionError(object msg) : base(JString.valueOf(msg)) { }
}

public class JStackOverflowError : JError
{
  public JStackOverflowError() { }
}

public class JInterruptedException : Exception
{
}

public class ArithmeticException : Exception
{
  public ArithmeticException(string msg) : base(msg) { }
}

public static class JInteger
{
  public const int MAX_VALUE = int.MaxValue;
  public const int MIN_VALUE = int.MinValue;
  public const int BYTES = 4;
  public const int SIZE = 32;

  public static long toUnsignedLong(int x) { return (long)(uint)x; }
  public static int compareUnsigned(int a, int b) { return ((uint)a).CompareTo((uint)b); }
  public static int compare(int a, int b) { return a < b ? -1 : a == b ? 0 : 1; }
  public static int parseInt(string s) { return int.Parse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture); }
  public static int parseInt(string s, int radix)
  {
    if (radix == 10) return parseInt(s);
    return Convert.ToInt32(s, radix);
  }
  public static string toString(int x) { return x.ToString(CultureInfo.InvariantCulture); }
  public static string toHexString(int x) { return ((uint)x).ToString("x", CultureInfo.InvariantCulture); }
  public static int numberOfLeadingZeros(int x) { return System.Numerics.BitOperations.LeadingZeroCount((uint)x); }
  public static int numberOfTrailingZeros(int x) { return x == 0 ? 32 : System.Numerics.BitOperations.TrailingZeroCount(x); }
  public static int bitCount(int x) { return System.Numerics.BitOperations.PopCount((uint)x); }
  public static int signum(int x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
  public static int? valueOf(int x) { return x; }
}

public static class JLong
{
  public const long MAX_VALUE = long.MaxValue;
  public const long MIN_VALUE = long.MinValue;
  public const int BYTES = 8;
  public const int SIZE = 64;

  public static int compareUnsigned(long a, long b) { return ((ulong)a).CompareTo((ulong)b); }
  public static long divideUnsigned(long a, long b) { return (long)((ulong)a / (ulong)b); }
  public static long remainderUnsigned(long a, long b) { return (long)((ulong)a % (ulong)b); }
  public static string toString(long x) { return x.ToString(CultureInfo.InvariantCulture); }
  public static string toString(long x, int radix)
  {
    if (x < 0) return "-" + toUnsignedString(-x, radix);
    return toUnsignedString(x, radix);
  }
  public static string toUnsignedString(long x) { return ((ulong)x).ToString(CultureInfo.InvariantCulture); }
  /* Java's digits: 0-9 then a-z */
  public static string toUnsignedString(long x, int radix)
  {
    ulong u = (ulong)x;
    if (u == 0) return "0";
    var c = new System.Collections.Generic.List<char>();
    while (u != 0)
    {
      int d = (int)(u % (ulong)radix);
      c.Add((char)(d < 10 ? '0' + d : 'a' + d - 10));
      u /= (ulong)radix;
    }
    c.Reverse();
    return new string(c.ToArray());
  }
  public static string toHexString(long x) { return ((ulong)x).ToString("x", CultureInfo.InvariantCulture); }
  public static long parseLong(string s) { return long.Parse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture); }
  public static long parseUnsignedLong(string s) { return (long)ulong.Parse(s, NumberStyles.None, CultureInfo.InvariantCulture); }
  public static long parseUnsignedLong(string s, int radix)
  {
    if (radix == 10) return parseUnsignedLong(s);
    return (long)Convert.ToUInt64(s, radix);
  }
  public static int numberOfTrailingZeros(long x) { return x == 0 ? 64 : System.Numerics.BitOperations.TrailingZeroCount(x); }
  public static int numberOfLeadingZeros(long x) { return System.Numerics.BitOperations.LeadingZeroCount((ulong)x); }
  public static int bitCount(long x) { return System.Numerics.BitOperations.PopCount((ulong)x); }
  public static long? valueOf(long x) { return x; }
  public static int compare(long a, long b) { return a < b ? -1 : a == b ? 0 : 1; }
  public static int signum(long x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
  public static long highestOneBit(long x) { return x == 0 ? 0 : (long)(1UL << (63 - System.Numerics.BitOperations.LeadingZeroCount((ulong)x))); }
}

public static class JDouble
{
  public const double POSITIVE_INFINITY = double.PositiveInfinity;
  public const double NEGATIVE_INFINITY = double.NegativeInfinity;
  public const double NaN = double.NaN;
  public const double MAX_VALUE = double.MaxValue;
  public const double MIN_VALUE = double.Epsilon;
  public const double MIN_NORMAL = 2.2250738585072014E-308;
  public const int BYTES = 8;
  public const int SIZE = 64;
  public const int PRECISION = 53;
  public const int MIN_EXPONENT = -1022;
  public const int MAX_EXPONENT = 1023;

  public static long doubleToRawLongBits(double d) { return BitConverter.DoubleToInt64Bits(d); }
  public static long doubleToLongBits(double d) { return double.IsNaN(d) ? 0x7ff8000000000000L : BitConverter.DoubleToInt64Bits(d); }
  public static double longBitsToDouble(long l) { return BitConverter.Int64BitsToDouble(l); }
  public static bool isInfinite(double d) { return double.IsInfinity(d); }
  public static bool isNaN(double d) { return double.IsNaN(d); }
  public static bool isFinite(double d) { return double.IsFinite(d); }
  public static double parseDouble(string s) { return double.Parse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture); }
  public static int compare(double a, double b) { return a.CompareTo(b); }
  public static string toString(double d) { return JString.javaDouble(d); }
  public static double? valueOf(double d) { return d; }
}

public static class JFloat
{
  public static float intBitsToFloat(int i) { return BitConverter.Int32BitsToSingle(i); }
  public static int floatToRawIntBits(float f) { return BitConverter.SingleToInt32Bits(f); }
}

public static class JBoolean
{
  public static bool getBoolean(string name) { return SysProps.getBoolean(name); }
  public static bool parseBoolean(string s) { return string.Equals(s, "true", StringComparison.OrdinalIgnoreCase); }
  public static string toString(bool b) { return b ? "true" : "false"; }
}

public static class JByte
{
  public const int SIZE = 8;
  public const int BYTES = 1;
  public static int toUnsignedInt(sbyte b) { return b & 0xff; }
}

public static class JShort
{
  public const short MAX_VALUE = short.MaxValue;
  public const short MIN_VALUE = short.MinValue;
}

public static class JCharacter
{
  public static char toLowerCase(char c) { return char.ToLowerInvariant(c); }
  public static char toUpperCase(char c) { return char.ToUpperInvariant(c); }
  public static bool isLetterOrDigit(char c) { return char.IsLetterOrDigit(c); }
  public static bool isDigit(char c) { return char.IsDigit(c); }
  public static bool isLetter(char c) { return char.IsLetter(c); }
  public static bool isWhitespace(char c) { return char.IsWhiteSpace(c); }
}

/* java.lang.Math: no exceptions for overflow (abs of the minimum is the minimum) */
public static class JMath
{
  public static int abs(int x) { return x < 0 ? -x : x; }
  public static long abs(long x) { return x < 0 ? -x : x; }
  public static double abs(double x) { return Math.Abs(x); }
  public static int min(int a, int b) { return a <= b ? a : b; }
  public static long min(long a, long b) { return a <= b ? a : b; }
  public static double min(double a, double b) { return Math.Min(a, b); }
  public static int max(int a, int b) { return a >= b ? a : b; }
  public static long max(long a, long b) { return a >= b ? a : b; }
  public static double max(double a, double b) { return Math.Max(a, b); }
  public static double floor(double x) { return Math.Floor(x); }
  public static double ceil(double x) { return Math.Ceiling(x); }
  public static double sqrt(double x) { return Math.Sqrt(x); }
  public static double pow(double x, double y) { return Math.Pow(x, y); }
  public static double log(double x) { return Math.Log(x); }
  public static double exp(double x) { return Math.Exp(x); }
  public static double rint(double x) { return Math.Round(x, MidpointRounding.ToEven); }
  public static long round(double x) { return (long)Math.Floor(x + 0.5); }
  public static int floorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a ^ b) < 0)) q--; return q; }
  public static long floorDiv(long a, long b) { long q = a / b; if ((a % b != 0) && ((a ^ b) < 0)) q--; return q; }
  public static int floorMod(int a, int b) { int m = a % b; if (m != 0 && ((m ^ b) < 0)) m += b; return m; }
  public static long floorMod(long a, long b) { long m = a % b; if (m != 0 && ((m ^ b) < 0)) m += b; return m; }
  public static int addExact(int a, int b) { return checked(a + b); }
  public static long addExact(long a, long b) { return checked(a + b); }
  public static int multiplyExact(int a, int b) { return checked(a * b); }
  public static long multiplyExact(long a, long b) { return checked(a * b); }
  public static int toIntExact(long a) { return checked((int)a); }
  public static long multiplyHigh(long a, long b) { return Math.BigMul(a, b, out _); }
  /* Math.getExponent: the unbiased exponent (MIN_EXPONENT - 1 for zero and subnormals, MAX_EXPONENT + 1 for inf/NaN) */
  public static int getExponent(double d)
  {
    return (int)((BitConverter.DoubleToInt64Bits(d) >> 52) & 0x7ff) - 1023;
  }
  /* Math.scalb: d * 2^n, rounded once */
  public static double scalb(double d, int n) { return Math.ScaleB(d, n); }
  public static double signum(double d) { return d > 0 ? 1.0 : d < 0 ? -1.0 : d; }
  public static double copySign(double m, double s) { return Math.CopySign(m, s); }
  public static double ulp(double d) { return Math.BitIncrement(Math.Abs(d)) - Math.Abs(d); }
}

/*
** java.lang.StrictMath. Java's is fdlibm, so that results do not depend on
** the JVM; .NET's Math is the C library's (glibc on Linux, as the C++
** build): see docs/csharp-port.md (Math).
*/
public static class JStrictMath
{
  public static double log(double x) { return Math.Log(x); }
  public static double log10(double x) { return Math.Log10(x); }
  public static double tan(double x) { return Math.Tan(x); }
  public static double sqrt(double x) { return Math.Sqrt(x); }
  public static double sin(double x) { return Math.Sin(x); }
  public static double cos(double x) { return Math.Cos(x); }
  public static double pow(double x, double y) { return Math.Pow(x, y); }
  public static double floor(double x) { return Math.Floor(x); }
  public static double ceil(double x) { return Math.Ceiling(x); }
  public static double exp(double x) { return Math.Exp(x); }
  public static double atan(double x) { return Math.Atan(x); }
  public static double atan2(double y, double x) { return Math.Atan2(y, x); }
  public static double asin(double x) { return Math.Asin(x); }
  public static double acos(double x) { return Math.Acos(x); }
  public static double abs(double x) { return Math.Abs(x); }
}

public static class JObjects
{
  public static new bool Equals(object a, object b) { return object.Equals(a, b); }
  public static bool equals(object a, object b) { return object.Equals(a, b); }
  public static int hashCode(object o) { return o == null ? 0 : o.GetHashCode(); }
  public static T requireNonNull<T>(T o) where T : class { if (o == null) throw new NullReferenceException(); return o; }
  public static T requireNonNull<T>(T o, string msg) where T : class { if (o == null) throw new NullReferenceException(msg); return o; }
}

public static class JArrays
{
  public static bool equals<T>(T[] a, T[] b)
  {
    if (a == b) return true;
    if (a == null || b == null || a.Length != b.Length) return false;
    var cmp = EqualityComparer<T>.Default;
    for (int i = 0; i < a.Length; i++)
      if (!cmp.Equals(a[i], b[i])) return false;
    return true;
  }
  public static T[] copyOf<T>(T[] a, int n)
  {
    T[] r = new T[n];
    Array.Copy(a, r, Math.Min(n, a.Length));
    return r;
  }
  public static T[] copyOfRange<T>(T[] a, int from, int to)
  {
    T[] r = new T[to - from];
    Array.Copy(a, from, r, 0, Math.Min(to, a.Length) - from);
    return r;
  }
  public static void fill<T>(T[] a, T v) { Array.Fill(a, v); }
  public static void fill<T>(T[] a, int from, int to, T v) { Array.Fill(a, v, from, to - from); }
  public static bool equals<T>(T[] a, int af, int at, T[] b, int bf, int bt)
  {
    if (at - af != bt - bf) return false;
    var cmp = EqualityComparer<T>.Default;
    for (int i = 0; i < at - af; i++)
      if (!cmp.Equals(a[af + i], b[bf + i])) return false;
    return true;
  }
  public static int mismatch<T>(T[] a, int af, int at, T[] b, int bf, int bt)
  {
    int n = Math.Min(at - af, bt - bf);
    var cmp = EqualityComparer<T>.Default;
    for (int i = 0; i < n; i++)
      if (!cmp.Equals(a[af + i], b[bf + i])) return i;
    return at - af == bt - bf ? -1 : n;
  }
  public static int hashCode(sbyte[] a)
  {
    if (a == null) return 0;
    int h = 1;
    foreach (sbyte b in a) h = 31 * h + b;
    return h;
  }
  public static List<T> asList<T>(params T[] a) { return new List<T>(a); }
  public static void sort<T>(T[] a) { Array.Sort(a); }
  public static void sort<T>(T[] a, Comparison<T> c) { Array.Sort(a, c); }
  public static void sort<T>(T[] a, int from, int to) { Array.Sort(a, from, to - from); }
  public static string toString(sbyte[] a)
  {
    if (a == null) return "null";
    var sb = new System.Text.StringBuilder("[");
    for (int i = 0; i < a.Length; i++) { if (i > 0) sb.Append(", "); sb.Append(a[i]); }
    return sb.Append(']').ToString();
  }
}

public static class JCharsets
{
  public static readonly Encoding UTF_8 = new UTF8Encoding(false, false);
  public static readonly Encoding ISO_8859_1 = Encoding.Latin1;
  public static readonly Encoding US_ASCII = Encoding.ASCII;
}

/* java.lang.String's methods on string, and Object's on object */
public static class JStringExt
{
  public static int length(this string s) { return s.Length; }
  public static char charAt(this string s, int i) { return s[i]; }
  public static bool equals(this string s, object o) { return o is string t && string.Equals(s, t, StringComparison.Ordinal); }
  public static bool isEmpty(this string s) { return s.Length == 0; }
  public static bool isBlank(this string s) { return string.IsNullOrWhiteSpace(s); }
  public static string substring(this string s, int a) { return s.Substring(a); }
  public static string substring(this string s, int a, int b) { return s.Substring(a, b - a); }
  public static int indexOf(this string s, string t) { return s.IndexOf(t, StringComparison.Ordinal); }
  public static int indexOf(this string s, char c) { return s.IndexOf(c); }
  public static int indexOf(this string s, int c) { return s.IndexOf((char)c); }
  public static int indexOf(this string s, string t, int from) { return s.IndexOf(t, from, StringComparison.Ordinal); }
  public static int indexOf(this string s, char c, int from) { return s.IndexOf(c, from); }
  public static int lastIndexOf(this string s, string t) { return s.LastIndexOf(t, StringComparison.Ordinal); }
  public static int lastIndexOf(this string s, char c) { return s.LastIndexOf(c); }
  public static int lastIndexOf(this string s, int c) { return s.LastIndexOf((char)c); }
  public static int lastIndexOf(this string s, string t, int from)
  {
    for (int k = Math.Min(from, s.Length - t.Length); k >= 0; k--)
      if (string.CompareOrdinal(s, k, t, 0, t.Length) == 0) return k;
    return -1;
  }
  public static bool startsWith(this string s, string t) { return s.StartsWith(t, StringComparison.Ordinal); }
  public static bool startsWith(this string s, string t, int off) { return off >= 0 && off <= s.Length && s.Substring(off).StartsWith(t, StringComparison.Ordinal); }
  public static bool endsWith(this string s, string t) { return s.EndsWith(t, StringComparison.Ordinal); }
  public static bool contains(this string s, string t) { return s.Contains(t, StringComparison.Ordinal); }
  public static string trim(this string s) { return s.Trim(' ', '\t', '\n', '\r', '\f', '\v', '\0'); }
  public static string toLowerCase(this string s) { return s.ToLowerInvariant(); }
  public static string toUpperCase(this string s) { return s.ToUpperInvariant(); }
  public static string repeat(this string s, int n) { var b = new System.Text.StringBuilder(); for (int i = 0; i < n; i++) b.Append(s); return b.ToString(); }
  public static string replace(this string s, string a, string b) { return s.Replace(a, b, StringComparison.Ordinal); }
  public static string replace(this string s, char a, char b) { return s.Replace(a, b); }
  public static char[] toCharArray(this string s) { return s.ToCharArray(); }
  public static string[] split(this string s, string regex) { return System.Text.RegularExpressions.Regex.Split(s, regex); }
  public static bool matches(this string s, string regex) { return System.Text.RegularExpressions.Regex.IsMatch(s, "^(?:" + regex + ")$"); }
  public static int compareTo(this string s, string t) { return string.CompareOrdinal(s, t); }
  public static string intern(this string s) { return string.Intern(s); }
  public static sbyte[] getBytes(this string s, Encoding e) { return (sbyte[])(object)e.GetBytes(s); }
  public static sbyte[] getBytes(this string s) { return (sbyte[])(object)JCharsets.UTF_8.GetBytes(s); }
  public static int codePointAt(this string s, int i) { return char.ConvertToUtf32(s, i); }

  public static bool equals(this object a, object b) { return a == null ? b == null : a.Equals(b); }
  public static int hashCode(this object a) { return a.GetHashCode(); }
  public static string toString(this object a) { return a is bool b ? (b ? "true" : "false") : a is double d ? JString.javaDouble(d) : a.ToString(); }
}

public static class JString
{
  /* a string from bytes (Java's new String(bytes, charset)) */
  public static string of(sbyte[] b, Encoding e) { return e.GetString((byte[])(object)b); }
  public static string of(sbyte[] b, int off, int len, Encoding e) { return e.GetString((byte[])(object)b, off, len); }
  public static string of(sbyte[] b) { return JCharsets.UTF_8.GetString((byte[])(object)b); }
  public static string of(char[] c) { return new string(c); }
  public static string of(char[] c, int off, int len) { return new string(c, off, len); }

  public static string valueOf(object o)
  {
    if (o == null) return "null";
    if (o is bool b) return b ? "true" : "false";
    if (o is double d) return javaDouble(d);
    if (o is float f) return javaDouble(f);
    if (o is IFormattable fm) return fm.ToString(null, CultureInfo.InvariantCulture);
    return o.ToString();
  }
  public static string valueOf(char c) { return c.ToString(); }
  public static string valueOf(int i) { return i.ToString(CultureInfo.InvariantCulture); }
  public static string valueOf(long i) { return i.ToString(CultureInfo.InvariantCulture); }
  public static string valueOf(double d) { return javaDouble(d); }
  public static string valueOf(bool b) { return b ? "true" : "false"; }
  public static string valueOf(char[] c) { return new string(c); }
  public static string join(string sep, IEnumerable<string> parts) { return string.Join(sep, parts); }
  public static string join(string sep, params string[] parts) { return string.Join(sep, parts); }

  /* Java's Double.toString */
  public static string javaDouble(double d)
  {
    if (double.IsNaN(d)) return "NaN";
    if (double.IsPositiveInfinity(d)) return "Infinity";
    if (double.IsNegativeInfinity(d)) return "-Infinity";
    if (d == 0) return BitConverter.DoubleToInt64Bits(d) < 0 ? "-0.0" : "0.0";
    double a = Math.Abs(d);
    string r = d.ToString("R", CultureInfo.InvariantCulture);
    if (a >= 1e-3 && a < 1e7)
    {
      if (r.Contains('E')) r = d.ToString("F17", CultureInfo.InvariantCulture).TrimEnd('0');
      if (!r.Contains('.')) r += ".0";
      if (r.EndsWith(".")) r += "0";
      return r;
    }
    string e = d.ToString("E16", CultureInfo.InvariantCulture);
    /* shortest digits */
    string s = r;
    int ei = s.IndexOf('E');
    string mant, exps;
    if (ei < 0)
    {
      /* r is plain: convert */
      int exp = (int)Math.Floor(Math.Log10(a));
      double m = d / Math.Pow(10, exp);
      mant = m.ToString("R", CultureInfo.InvariantCulture);
      exps = exp.ToString(CultureInfo.InvariantCulture);
    }
    else
    {
      mant = s.Substring(0, ei);
      exps = int.Parse(s.Substring(ei + 1), CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture);
    }
    if (!mant.Contains('.')) mant += ".0";
    return mant + "E" + exps;
  }

  public static string format(Locale l, string fmt, params object[] args) { return format(fmt, args); }

  /* java.util.Formatter: the conversions the sources use */
  public static string format(string fmt, params object[] args)
  {
    var sb = new System.Text.StringBuilder();
    int ai = 0;
    for (int i = 0; i < fmt.Length; i++)
    {
      char c = fmt[i];
      if (c != '%') { sb.Append(c); continue; }
      i++;
      string flags = "";
      while ("-+ 0#,(".IndexOf(fmt[i]) >= 0) { flags += fmt[i]; i++; }
      int width = -1, prec = -1;
      int st = i;
      while (char.IsDigit(fmt[i])) i++;
      if (i > st) width = int.Parse(fmt.Substring(st, i - st), CultureInfo.InvariantCulture);
      if (fmt[i] == '.')
      {
        i++; st = i;
        while (char.IsDigit(fmt[i])) i++;
        prec = int.Parse(fmt.Substring(st, i - st), CultureInfo.InvariantCulture);
      }
      char conv = fmt[i];
      string body;
      switch (conv)
      {
        case '%': body = "%"; break;
        case 'n': body = "\n"; break;
        case 's': case 'S':
          body = valueOf(args[ai++]);
          if (prec >= 0 && body.Length > prec) body = body.Substring(0, prec);
          if (conv == 'S') body = body.ToUpperInvariant();
          break;
        case 'c': { object o = args[ai++]; body = o is char ch ? ch.ToString() : ((char)Convert.ToInt32(o, CultureInfo.InvariantCulture)).ToString(); break; }
        case 'b': body = valueOf(args[ai++] is bool bb ? bb : args[ai - 1] != null); break;
        case 'd':
        {
          object o = args[ai++];
          long v = Convert.ToInt64(o, CultureInfo.InvariantCulture);
          body = v.ToString(flags.Contains(',') ? "N0" : "D", CultureInfo.InvariantCulture);
          if (flags.Contains('+') && v >= 0) body = "+" + body;
          else if (flags.Contains(' ') && v >= 0) body = " " + body;
          break;
        }
        case 'x': case 'X':
        {
          object o = args[ai++];
          string h = o switch
          {
            sbyte b => ((byte)b).ToString("x"),
            short s2 => ((ushort)s2).ToString("x"),
            int n => ((uint)n).ToString("x"),
            long l => ((ulong)l).ToString("x"),
            _ => Convert.ToUInt64(o, CultureInfo.InvariantCulture).ToString("x"),
          };
          if (flags.Contains('#')) h = "0x" + h;
          body = conv == 'X' ? h.ToUpperInvariant() : h;
          break;
        }
        case 'o': body = Convert.ToString(Convert.ToInt64(args[ai++], CultureInfo.InvariantCulture), 8); break;
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a':
        {
          double v = Convert.ToDouble(args[ai++], CultureInfo.InvariantCulture);
          if (prec < 0) prec = 6;
          if (double.IsNaN(v)) body = "NaN";
          else if (double.IsInfinity(v)) body = v > 0 ? (flags.Contains('+') ? "+Infinity" : "Infinity") : "-Infinity";
          else if (conv == 'f') body = v.ToString("F" + prec, CultureInfo.InvariantCulture);
          else if (conv == 'e' || conv == 'E')
          {
            string t = v.ToString((conv == 'e' ? "e" : "E") + prec, CultureInfo.InvariantCulture);
            /* .NET gives three exponent digits; Java at least two */
            int ep = t.IndexOfAny(new[] { 'e', 'E' });
            string mant = t.Substring(0, ep + 2);
            string ex = t.Substring(ep + 2).TrimStart('0');
            if (ex.Length < 2) ex = ex.PadLeft(2, '0');
            body = mant + ex;
          }
          else body = v.ToString("G" + Math.Max(prec, 1), CultureInfo.InvariantCulture);
          if (flags.Contains('+') && v >= 0 && !body.StartsWith("+")) body = "+" + body;
          break;
        }
        default: throw new FormatException("format conversion %" + conv);
      }
      if (width > body.Length)
      {
        if (flags.Contains('-')) body = body.PadRight(width);
        else if (flags.Contains('0') && conv != 's')
        {
          bool neg = body.StartsWith("-") || body.StartsWith("+");
          string sign = neg ? body.Substring(0, 1) : "";
          string digits = neg ? body.Substring(1) : body;
          if (digits.StartsWith("0x")) { sign += "0x"; digits = digits.Substring(2); }
          body = sign + digits.PadLeft(width - sign.Length, '0');
        }
        else body = body.PadLeft(width);
      }
      sb.Append(body);
    }
    return sb.ToString();
  }
}

/* java.util.Locale: the formatting is the invariant culture's (Locale.ROOT) */
public sealed class Locale
{
  public static readonly Locale ROOT = new Locale();
  private Locale() { }
}

/* java.lang.Runtime: the shutdown hooks run at the process's exit (what one throws is printed, as
   a thread's uncaught exception); 'halt' runs them too (.NET has no exit without them) */
public sealed class JRuntime
{
  private static readonly JRuntime instance = new JRuntime();
  public static JRuntime getRuntime() { return instance; }
  public void addShutdownHook(Runnable hook)
  {
    AppDomain.CurrentDomain.ProcessExit += (s, e) =>
    {
      try { hook(); }
      catch (Exception x) { Console.Error.WriteLine("Exception in shutdown hook: " + x); }
    };
  }
  public void halt(int status) { Environment.Exit(status); }
}

/* java.lang.StringBuilder */
public sealed class StringBuilder
{
  private readonly System.Text.StringBuilder b;
  public StringBuilder() { b = new System.Text.StringBuilder(); }
  public StringBuilder(int cap) { b = new System.Text.StringBuilder(cap); }
  public StringBuilder(string s) { b = new System.Text.StringBuilder(s); }
  public StringBuilder append(string s) { b.Append(s ?? "null"); return this; }
  public StringBuilder append(char c) { b.Append(c); return this; }
  public StringBuilder append(int i) { b.Append(i.ToString(CultureInfo.InvariantCulture)); return this; }
  public StringBuilder append(long i) { b.Append(i.ToString(CultureInfo.InvariantCulture)); return this; }
  public StringBuilder append(double d) { b.Append(JString.javaDouble(d)); return this; }
  public StringBuilder append(bool v) { b.Append(v ? "true" : "false"); return this; }
  public StringBuilder append(object o) { b.Append(JString.valueOf(o)); return this; }
  public StringBuilder append(char[] c) { b.Append(c); return this; }
  public StringBuilder append(char[] c, int off, int len) { b.Append(c, off, len); return this; }
  public StringBuilder append(string s, int start, int end) { b.Append(s, start, end - start); return this; }
  public StringBuilder insert(int at, string s) { b.Insert(at, s); return this; }
  public StringBuilder insert(int at, char c) { b.Insert(at, c); return this; }
  public StringBuilder reverse() { char[] a = b.ToString().ToCharArray(); Array.Reverse(a); b.Clear(); b.Append(a); return this; }
  public StringBuilder deleteCharAt(int i) { b.Remove(i, 1); return this; }
  public StringBuilder delete(int s, int e) { b.Remove(s, Math.Min(e, b.Length) - s); return this; }
  public int length() { return b.Length; }
  public char charAt(int i) { return b[i]; }
  public void setCharAt(int i, char c) { b[i] = c; }
  public void setLength(int n) { if (n < b.Length) b.Length = n; else b.Append('\0', n - b.Length); }
  public bool isEmpty() { return b.Length == 0; }
  public override string ToString() { return b.ToString(); }
  public string toString() { return b.ToString(); }
}

/* java.util.ArrayList (and List) */
public class ArrayList<T> : IEnumerable<T>
{
  protected readonly List<T> l;
  public ArrayList() { l = new List<T>(); }
  public ArrayList(int cap) { l = new List<T>(cap); }
  public ArrayList(IEnumerable<T> c) { l = new List<T>(c); }
  public bool add(T x) { l.Add(x); return true; }
  public void add(int i, T x) { l.Insert(i, x); }
  public bool addAll(IEnumerable<T> c) { l.AddRange(c); return true; }
  public T get(int i) { return l[i]; }
  public T set(int i, T x) { T o = l[i]; l[i] = x; return o; }
  public int size() { return l.Count; }
  public bool isEmpty() { return l.Count == 0; }
  public T remove(int i) { T o = l[i]; l.RemoveAt(i); return o; }
  public bool removeItem(T x) { return l.Remove(x); }
  public void clear() { l.Clear(); }
  public bool contains(T x) { return l.Contains(x); }
  public int indexOf(T x) { return l.IndexOf(x); }
  public T[] toArray() { return l.ToArray(); }
  public T[] toArray(T[] _) { return l.ToArray(); }
  public ArrayList<T> subList(int a, int b) { return new ArrayList<T>(l.GetRange(a, b - a)); }
  public void sort(Comparison<T> c) { l.Sort(c); }
  public IEnumerator<T> GetEnumerator() { return l.GetEnumerator(); }
  IEnumerator IEnumerable.GetEnumerator() { return l.GetEnumerator(); }
}

public class List<T> : System.Collections.Generic.List<T>
{
  public List() { }
  public List(IEnumerable<T> c) : base(c) { }
  public List(int cap) : base(cap) { }
}

public sealed class MapEntry<K, V>
{
  private readonly K k; private readonly V v;
  public MapEntry(K k, V v) { this.k = k; this.v = v; }
  public K getKey() { return k; }
  public V getValue() { return v; }
}

/* java.util.HashMap: get answers default (null) for a missing key */
public class HashMap<K, V> : IEnumerable<MapEntry<K, V>>
{
  protected readonly Dictionary<K, V> d;
  public HashMap() { d = new Dictionary<K, V>(); }
  public HashMap(IEqualityComparer<K> c) { d = new Dictionary<K, V>(c); }
  public V get(K k) { return k != null && d.TryGetValue(k, out V v) ? v : default; }
  public V computeIfAbsent(K k, Func<K, V> f)
  {
    if (d.TryGetValue(k, out V v)) return v;
    v = f(k);
    if (v != null) d[k] = v;
    return v;
  }
  public V getOrDefault(K k, V def) { return k != null && d.TryGetValue(k, out V v) ? v : def; }
  public V put(K k, V v) { d.TryGetValue(k, out V o); d[k] = v; return o; }
  public V putIfAbsent(K k, V v) { if (d.TryGetValue(k, out V o)) return o; d[k] = v; return default; }
  public bool containsKey(K k) { return k != null && d.ContainsKey(k); }
  public V remove(K k) { if (d.Remove(k, out V o)) return o; return default; }
  public int size() { return d.Count; }
  public bool isEmpty() { return d.Count == 0; }
  public void clear() { d.Clear(); }
  public IEnumerable<K> keySet() { return d.Keys; }
  public IEnumerable<V> values() { return d.Values; }
  public IEnumerable<MapEntry<K, V>> entrySet() { foreach (var kv in d) yield return new MapEntry<K, V>(kv.Key, kv.Value); }
  public IEnumerator<MapEntry<K, V>> GetEnumerator() { return entrySet().GetEnumerator(); }
  IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
}

public sealed class IdentityComparer<K> : IEqualityComparer<K>
{
  public static readonly IdentityComparer<K> instance = new();
  public bool Equals(K a, K b) { return ReferenceEquals(a, b); }
  public int GetHashCode(K o) { return RuntimeHelpers.GetHashCode(o); }
}

public class IdentityHashMap<K, V> : HashMap<K, V>
{
  public IdentityHashMap() : base(IdentityComparer<K>.instance) { }
}

public class LinkedHashSet<T> : IEnumerable<T>
{
  private readonly List<T> order = new();
  private readonly HashSet<T> set = new();
  public bool add(T x) { if (!set.Add(x)) return false; order.Add(x); return true; }
  public bool contains(T x) { return set.Contains(x); }
  public int size() { return set.Count; }
  public IEnumerator<T> GetEnumerator() { return order.GetEnumerator(); }
  IEnumerator IEnumerable.GetEnumerator() { return order.GetEnumerator(); }
}

public sealed class BitSet
{
  private readonly BitArray bits;
  public BitSet() { bits = new BitArray(64); }
  public BitSet(int n) { bits = new BitArray(Math.Max(n, 1)); }
  private void ensure(int i) { if (i >= bits.Length) bits.Length = Math.Max(i + 1, bits.Length * 2); }
  public void set(int i) { ensure(i); bits[i] = true; }
  public void set(int i, bool v) { ensure(i); bits[i] = v; }
  public void clear(int i) { if (i < bits.Length) bits[i] = false; }
  public void clear() { bits.SetAll(false); }
  public bool get(int i) { return i < bits.Length && bits[i]; }
  public int nextSetBit(int i) { for (; i < bits.Length; i++) if (bits[i]) return i; return -1; }
  public int nextClearBit(int i) { for (; i < bits.Length; i++) if (!bits[i]) return i; return i; }
}

public sealed class OptionalLong
{
  private static readonly OptionalLong EMPTY = new(false, 0);
  private readonly bool present; private readonly long v;
  private OptionalLong(bool p, long v) { present = p; this.v = v; }
  public static OptionalLong of(long v) { return new OptionalLong(true, v); }
  public static OptionalLong empty() { return EMPTY; }
  public bool isPresent() { return present; }
  public bool isEmpty() { return !present; }
  public long getAsLong() { if (!present) throw new InvalidOperationException("No value present"); return v; }
  public long orElse(long o) { return present ? v : o; }
}

public sealed class OptionalDouble
{
  private static readonly OptionalDouble EMPTY = new(false, 0);
  private readonly bool present; private readonly double v;
  private OptionalDouble(bool p, double v) { present = p; this.v = v; }
  public static OptionalDouble of(double v) { return new OptionalDouble(true, v); }
  public static OptionalDouble empty() { return EMPTY; }
  public bool isPresent() { return present; }
  public bool isEmpty() { return !present; }
  public double getAsDouble() { if (!present) throw new InvalidOperationException("No value present"); return v; }
  public double orElse(double o) { return present ? v : o; }
}

public sealed class OptionalInt
{
  private static readonly OptionalInt EMPTY = new(false, 0);
  private readonly bool present; private readonly int v;
  private OptionalInt(bool p, int v) { present = p; this.v = v; }
  public static OptionalInt of(int v) { return new OptionalInt(true, v); }
  public static OptionalInt empty() { return EMPTY; }
  public bool isPresent() { return present; }
  public bool isEmpty() { return !present; }
  public int getAsInt() { if (!present) throw new InvalidOperationException("No value present"); return v; }
  public int orElse(int o) { return present ? v : o; }
}

public sealed class Optional<T> where T : class
{
  private readonly T v;
  private Optional(T v) { this.v = v; }
  public static Optional<T> ofNullable(T v) { return new Optional<T>(v); }
  public static Optional<T> of(T v) { return new Optional<T>(v); }
  public static Optional<T> empty() { return new Optional<T>(null); }
  public bool isPresent() { return v != null; }
  public bool isEmpty() { return v == null; }
  public T get() { return v; }
  public T orElse(T o) { return v ?? o; }
}

public delegate void Consumer<T>(T x);
public delegate R BiFunction<A, B, R>(A a, B b);
public delegate R Function<A, R>(A a);
public delegate T Supplier<T>();
public delegate void Runnable();

public static class JFunctionExt
{
  public static void accept<T>(this Consumer<T> f, T x) { f(x); }
  public static R apply<A, B, R>(this BiFunction<A, B, R> f, A a, B b) { return f(a, b); }
  public static R apply<A, R>(this Function<A, R> f, A a) { return f(a); }
  public static T get<T>(this Supplier<T> f) { return f(); }
  public static void run(this Runnable f) { f(); }
}

/* java.lang.System */
public static class JSystem
{
  public static void arraycopy(Array src, int srcPos, Array dst, int dstPos, int len)
  {
    Array.Copy(src, srcPos, dst, dstPos, len);
  }
  public static string getProperty(string name) { return SysProps.getProperty(name); }
  public static string getProperty(string name, string def) { return SysProps.getProperty(name, def); }
  public static string getenv(string name) { return Environment.GetEnvironmentVariable(name); }
  public static int identityHashCode(object o) { return RuntimeHelpers.GetHashCode(o); }
  public static void exit(int code) { Environment.Exit(code); }
  private static readonly long t0 = Environment.TickCount64;
  public static long currentTimeMillis() { return DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(); }
  public static long nanoTime() { return System.Diagnostics.Stopwatch.GetTimestamp() * (1_000_000_000L / System.Diagnostics.Stopwatch.Frequency); }
  public static readonly PrintStream @out = new PrintStream(Console.OpenStandardOutput());
  public static readonly PrintStream err = new PrintStream(Console.OpenStandardError());
  public static void gc() { GC.Collect(); }
}

/* java.io.PrintStream over a stream: text as UTF-8 */
public sealed class PrintStream
{
  private readonly System.IO.Stream s;
  public PrintStream(System.IO.Stream s) { this.s = s; }
  public void print(string x) { byte[] b = Encoding.UTF8.GetBytes(x ?? "null"); s.Write(b, 0, b.Length); }
  public void print(object x) { print(JString.valueOf(x)); }
  public void print(char c) { print(c.ToString()); }
  public void print(int i) { print(JString.valueOf(i)); }
  public void print(long i) { print(JString.valueOf(i)); }
  public void println(string x) { print(x); print("\n"); flush(); }
  public void println(object x) { println(JString.valueOf(x)); }
  public void println(int i) { println(JString.valueOf(i)); }
  public void println(long i) { println(JString.valueOf(i)); }
  public void println() { print("\n"); flush(); }
  public void printf(string fmt, params object[] args) { print(JString.format(fmt, args)); }
  public PrintStream format(string fmt, params object[] args) { print(JString.format(fmt, args)); return this; }
  public void write(sbyte[] b, int off, int len) { s.Write((byte[])(object)b, off, len); }
  public void write(int b) { s.WriteByte((byte)b); }
  public void flush() { s.Flush(); }
}

public static class JThread
{
  public static void sleep(long ms) { System.Threading.Thread.Sleep((int)ms); }
}
