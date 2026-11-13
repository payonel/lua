using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

public abstract class ByteWrites
{
  public abstract int size();

  public abstract sbyte[] array();

  public abstract int offset();

  public int apply(ByteView from, int pos)
  {
    { if (JAssert.on && !( from.len + pos <= size())) throw new AssertionError(); }
    if (from.len != 0)
      JSystem.arraycopy(from.a, from.off, array(), offset() + pos, from.len);
    return pos + from.len;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int apply(ByteView from)
  {
    return apply(from, 0);
  }

  public void write_int32(int v, int pos)
  {
    sbyte[] d = array();
    int o = offset() + pos;
    for (int i = 0; i < 4; i++)
    {
      d[o + i] = (sbyte) (v & 0xFF);
      v >>>= 8;
    }
  }

  public void write_int64(long v, int pos)
  {
    sbyte[] d = array();
    int o = offset() + pos;
    for (int i = 0; i < 8; i++)
    {
      d[o + i] = (sbyte) (v & 0xFF);
      v >>>= 8;
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void write_double(double v, int pos)
  {
    write_int64(JDouble.doubleToRawLongBits(v), pos);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void write_float(float v, int pos)
  {
    write_int32(JFloat.floatToRawIntBits(v), pos);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void copy_from(ByteView src, int pos)
  {
    { if (JAssert.on && !( pos + src.len <= size())) throw new AssertionError(); }
    if (src.len != 0)
      JSystem.arraycopy(src.a, src.off, array(), offset() + pos, src.len);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void copy_from(ByteView src)
  {
    copy_from(src, 0);
  }

  public void fill(sbyte v, int pos, int len)
  {
    int s = size();
    { if (JAssert.on && !( pos <= s)) throw new AssertionError(); }
    len = (len == ByteView.npos) ? s - pos : JMath.min(len, s - pos);
    JArrays.fill(array(), offset() + pos, offset() + pos + len, v);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void fill(sbyte v)
  {
    fill(v, 0, ByteView.npos);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void move_from(ByteView src, int pos)
  {
    { if (JAssert.on && !( pos + src.len <= size())) throw new AssertionError(); }
    if (src.len != 0)
      JSystem.arraycopy(src.a, src.off, array(), offset() + pos, src.len);
  }

  public void reverse(int pos, int len)
  {
    int s = size();
    { if (JAssert.on && !( pos <= s)) throw new AssertionError(); }
    len = (len == ByteView.npos) ? s - pos : JMath.min(len, s - pos);
    sbyte[] p = array();
    int o = offset();
    for (int i = 0; i < len / 2; ++i)
    {
      int ei = o + pos + len - 1 - i;
      sbyte tmp = p[ei];
      p[ei] = p[o + i + pos];
      p[o + i + pos] = tmp;
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void reverse()
  {
    reverse(0, ByteView.npos);
  }

  /* to_chars of an integer in 'base', then a '\0'; returns its length */
  private int write_integer(long value, bool unsigned, int @base, int pos, bool prefix)
  {
    { if (JAssert.on && !( pos < size())) throw new AssertionError(); }
    sbyte[] d = array();
    int o = offset();
    int start = pos;
    if (prefix)
    {
      { if (JAssert.on && !( @base == 16)) throw new AssertionError(); }
      d[o + pos] = (sbyte)('0');
      d[o + pos + 1] = (sbyte)('x');
      start = pos + 2;
    }
    string s = unsigned ? JLong.toUnsignedString(value, @base) : JLong.toString(value, @base);
    int n = s.length();
    { if (JAssert.on && !( start + n < size())) throw new AssertionError(); }
    for (int i = 0; i < n; i++)
      d[o + start + i] = (sbyte) s.charAt(i);
    d[o + start + n] = 0;
    return (start + n) - pos;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int write_number(long value, int @base, int pos)
  {
    return write_integer(value, false, @base, pos, false);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int write_number(long value, int @base)
  {
    return write_number(value, @base, 0);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int write_number(long value)
  {
    return write_number(value, 10, 0);
  }

  /* an unsigned value ('unsigned long' in the C++) */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int write_unsigned(long value, int @base, int pos, bool prefix)
  {
    return write_integer(value, true, @base, pos, prefix);
  }

  /* to_chars of a float in format 'fmt' with 'precision', then a '\0' */
  public int write_number(double value, CharsFormat fmt, int precision, int pos)
  {
    { if (JAssert.on && !( pos < size())) throw new AssertionError(); }
    sbyte[] r = ToChars.format(value, fmt, precision);
    { if (JAssert.on && !( pos + r.Length < size())) throw new AssertionError(); }
    JSystem.arraycopy(r, 0, array(), offset() + pos, r.Length);
    array()[offset() + pos + r.Length] = 0;
    return r.Length;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public int write_number(double value, CharsFormat fmt, int precision)
  {
    return write_number(value, fmt, precision, 0);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteView view()
  {
    return new ByteView(array(), offset(), size());
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public ByteView str_view()
  {
    ByteView sv = view();
    int n = sv.find(0);
    return sv.substr(0, n == ByteView.npos ? sv.size() : n);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public sbyte get(int i)
  {
    { if (JAssert.on && !( i < size())) throw new AssertionError(); }
    return array()[offset() + i];
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public void set(int i, int b)
  {
    { if (JAssert.on && !( i < size())) throw new AssertionError(); }
    array()[offset() + i] = (sbyte) b;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public bool empty()
  {
    return size() == 0;
  }
}
