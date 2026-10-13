package com.payonel.lungolua.bytes;

import java.nio.charset.StandardCharsets;

public final class ByteView
{
  public static final int npos = -1;
  public static final ByteView EMPTY = new ByteView(new byte[0], 0, 0);

  final byte[] a;
  final int off;
  final int len;

  private static final byte[] NONE = new byte[0];

  public ByteView(byte[] a, int off, int len)
  {
    this.a = (a == null) ? NONE : a;
    this.off = off;
    this.len = len;
  }

  public ByteView(byte[] a)
  {
    this(a, 0, a.length);
  }

  /* an ASCII text as bytes (the engine's literals) */
  public static ByteView of(String ascii)
  {
    return new ByteView(ascii.getBytes(StandardCharsets.ISO_8859_1));
  }

  /* a C string: the bytes up to the first '\0' */
  public static ByteView cstring(byte[] a, int off)
  {
    int e = off;
    while (e < a.length && a[e] != 0)
      e++;
    return new ByteView(a, off, e - off);
  }

  public int size()
  {
    return len;
  }

  public byte[] array()
  {
    return a;
  }

  public int offset()
  {
    return off;
  }

  public boolean empty()
  {
    return len == 0;
  }

  /* operator[]: the byte, as C's 'char' (signed) */
  public byte at(int i)
  {
    return a[off + i];
  }

  /* the byte as an unsigned value */
  public int uat(int i)
  {
    return a[off + i] & 0xFF;
  }

  public boolean ends_with(int c)
  {
    return len > 0 && a[off + len - 1] == (byte) c;
  }

  public boolean ends_with(ByteView s)
  {
    return s.len <= len && regionEquals(len - s.len, s);
  }

  public boolean starts_with(ByteView s)
  {
    return s.len <= len && regionEquals(0, s);
  }

  public boolean starts_with(int c)
  {
    return len > 0 && a[off] == (byte) c;
  }

  private boolean regionEquals(int pos, ByteView s)
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
    byte b = (byte) c;
    for (int i = Math.max(pos, 0); i < len; i++)
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
    int n = Math.min(count, len - pos);
    return compareBytes(a, off + pos, n, s.a, s.off, s.len);
  }

  public int compare(ByteView s)
  {
    return compareBytes(a, off, len, s.a, s.off, s.len);
  }

  static int compareBytes(byte[] x, int xo, int xn, byte[] y, int yo, int yn)
  {
    int n = Math.min(xn, yn);
    for (int i = 0; i < n; i++)
    {
      int c = (x[xo + i] & 0xFF) - (y[yo + i] & 0xFF);
      if (c != 0)
        return c;
    }
    return Integer.compare(xn, yn);
  }

  public boolean equals(ByteView other)
  {
    return other != null && len == other.len && regionEquals(0, other);
  }

  @Override
  public boolean equals(Object o)
  {
    return o instanceof ByteView v && equals(v);
  }

  @Override
  public int hashCode()
  {
    int h = 1;
    for (int i = 0; i < len; i++)
      h = 31 * h + a[off + i];
    return h;
  }

  public ByteView substr(int pos, int count)
  {
    if (pos < 0 || pos > len)
      throw new IndexOutOfBoundsException("substr");
    int n = (count < 0) ? len - pos : Math.min(count, len - pos);
    return new ByteView(a, off + pos, n);
  }

  public ByteView substr(int pos)
  {
    return substr(pos, npos);
  }

  public ToChars.ParseResult parse_number(int pos, CharsFormat fmt)
  {
    assert fmt == CharsFormat.general;
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
    return Double.longBitsToDouble(toInt64(pos));
  }

  public float toFloat(int pos)
  {
    return Float.intBitsToFloat(toInt32(pos));
  }

  /* a copy of the bytes */
  public byte[] toArray()
  {
    byte[] r = new byte[len];
    System.arraycopy(a, off, r, 0, len);
    return r;
  }

  /* the bytes as ISO-8859-1 text (for diagnostics and the host boundary) */
  @Override
  public String toString()
  {
    return new String(a, off, len, StandardCharsets.ISO_8859_1);
  }
}
