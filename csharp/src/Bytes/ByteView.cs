using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

public sealed class ByteView
{
  public const int npos = -1;
  public static readonly ByteView EMPTY = new ByteView(new sbyte[0], 0, 0);

  internal readonly sbyte[] a;
  internal readonly int off;
  internal readonly int len;

  private static readonly sbyte[] NONE = new sbyte[0];

  public ByteView(sbyte[] a, int off, int len)
  {
    this.a = (a == null) ? NONE : a;
    this.off = off;
    this.len = len;
  }

  public ByteView(sbyte[] a) : this(a, 0, a.Length)
  {

  }

  /* an ASCII text as bytes (the engine's literals) */
  public static ByteView of(string ascii)
  {
    return new ByteView(ascii.getBytes(JCharsets.ISO_8859_1));
  }

  /* a C string: the bytes up to the first '\0' */
  public static ByteView cstring(sbyte[] a, int off)
  {
    int e = off;
    while (e < a.Length && a[e] != 0)
      e++;
    return new ByteView(a, off, e - off);
  }

  public int size()
  {
    return len;
  }

  public sbyte[] array()
  {
    return a;
  }

  public int offset()
  {
    return off;
  }

  public bool empty()
  {
    return len == 0;
  }

  /* operator[]: the byte, as C's 'char' (signed) */
  public sbyte at(int i)
  {
    return a[off + i];
  }

  /* the byte as an unsigned value */
  public int uat(int i)
  {
    return a[off + i] & 0xFF;
  }

  public bool ends_with(int c)
  {
    return len > 0 && a[off + len - 1] == (sbyte) c;
  }

  public bool ends_with(ByteView s)
  {
    return s.len <= len && regionEquals(len - s.len, s);
  }

  public bool starts_with(ByteView s)
  {
    return s.len <= len && regionEquals(0, s);
  }

  public bool starts_with(int c)
  {
    return len > 0 && a[off] == (sbyte) c;
  }

  private bool regionEquals(int pos, ByteView s)
  {
    for (int i = 0; i < s.len; i++)
      if (a[off + pos + i] != s.a[s.off + i])
        return false;
    return true;
  }

  public int find_first_not_of(ByteView chars, int pos)
  {
    for (int i = pos; i < len; i++)
      if (chars.find(a[off + i], 0) == npos)
        return i;
    return npos;
  }

  public int find_first_not_of(ByteView chars)
  {
    return find_first_not_of(chars, 0);
  }

  public int find_first_of(ByteView chars, int pos)
  {
    for (int i = pos; i < len; i++)
      if (chars.find(a[off + i], 0) != npos)
        return i;
    return npos;
  }

  public int find_first_of(ByteView chars)
  {
    return find_first_of(chars, 0);
  }

  public int find(int c, int pos)
  {
    sbyte b = (sbyte) c;
    for (int i = JMath.max(pos, 0); i < len; i++)
      if (a[off + i] == b)
        return i;
    return npos;
  }

  public int find(int c)
  {
    return find(c, 0);
  }

  public int find(ByteView s, int pos)
  {
    if (pos > len)
      return npos;
    if (s.len == 0)
      return pos;
    for (int i = pos; i + s.len <= len; i++)
      if (regionEquals(i, s))
        return i;
    return npos;
  }

  public int find(ByteView s)
  {
    return find(s, 0);
  }

  /* std::string_view::compare(pos, count, s): <0, 0, >0, bytes unsigned */
  public int compare(int pos, int count, ByteView s)
  {
    int n = JMath.min(count, len - pos);
    return compareBytes(a, off + pos, n, s.a, s.off, s.len);
  }

  public int compare(ByteView s)
  {
    return compareBytes(a, off, len, s.a, s.off, s.len);
  }

  internal static int compareBytes(sbyte[] x, int xo, int xn, sbyte[] y, int yo, int yn)
  {
    int n = JMath.min(xn, yn);
    for (int i = 0; i < n; i++)
    {
      int c = (x[xo + i] & 0xFF) - (y[yo + i] & 0xFF);
      if (c != 0)
        return c;
    }
    return JInteger.compare(xn, yn);
  }

  public bool equals(ByteView other)
  {
    return other != null && len == other.len && regionEquals(0, other);
  }

  public override bool Equals(object o)
  {
    return o is ByteView v && equals(v);
  }

  public override int GetHashCode()
  {
    int h = 1;
    for (int i = 0; i < len; i++)
      h = 31 * h + a[off + i];
    return h;
  }

  public ByteView substr(int pos, int count)
  {
    if (pos < 0 || pos > len)
      throw new IndexOutOfRangeException("substr");
    int n = (count < 0) ? len - pos : JMath.min(count, len - pos);
    return new ByteView(a, off + pos, n);
  }

  public ByteView substr(int pos)
  {
    return substr(pos, npos);
  }

  public ToChars.ParseResult parse_number(int pos, CharsFormat fmt)
  {
    { if (JAssert.on && !( fmt == CharsFormat.general)) throw new AssertionError(); }
    return ToChars.parse(this, pos);
  }

  public int toInt32(int pos)
  {
    int bits = 0;
    for (int i = 4; i-- > 0;)
      bits = (bits << 8) | (a[off + pos + i] & 0xFF);
    return bits;
  }

  public long toInt64(int pos)
  {
    long bits = 0;
    for (int i = 8; i-- > 0;)
      bits = (bits << 8) | (a[off + pos + i] & 0xFF);
    return bits;
  }

  public double toDouble(int pos)
  {
    return JDouble.longBitsToDouble(toInt64(pos));
  }

  public float toFloat(int pos)
  {
    return JFloat.intBitsToFloat(toInt32(pos));
  }

  /* a copy of the bytes */
  public sbyte[] toArray()
  {
    sbyte[] r = new sbyte[len];
    JSystem.arraycopy(a, off, r, 0, len);
    return r;
  }

  /* the bytes as ISO-8859-1 text (for diagnostics and the host boundary) */

  public override string ToString()
  {
    return JString.of(a, off, len, JCharsets.ISO_8859_1);
  }
}
