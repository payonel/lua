package lua.bytes;

public abstract class ByteWrites
{
  public abstract int size();

  public abstract byte[] array();

  public abstract int offset();

  public int apply(ByteView from, int pos)
  {
    assert from.len + pos <= size();
    System.arraycopy(from.a, from.off, array(), offset() + pos, from.len);
    return pos + from.len;
  }

  public int apply(ByteView from)
  {
    return apply(from, 0);
  }

  public void write_int32(int v, int pos)
  {
    byte[] d = array();
    int o = offset() + pos;
    for (int i = 0; i < 4; i++)
    {
      d[o + i] = (byte) (v & 0xFF);
      v >>>= 8;
    }
  }

  public void write_int64(long v, int pos)
  {
    byte[] d = array();
    int o = offset() + pos;
    for (int i = 0; i < 8; i++)
    {
      d[o + i] = (byte) (v & 0xFF);
      v >>>= 8;
    }
  }

  public void write_double(double v, int pos)
  {
    write_int64(Double.doubleToRawLongBits(v), pos);
  }

  public void write_float(float v, int pos)
  {
    write_int32(Float.floatToRawIntBits(v), pos);
  }

  public void copy_from(ByteView src, int pos)
  {
    assert pos + src.len <= size();
    if (src.len != 0)
      System.arraycopy(src.a, src.off, array(), offset() + pos, src.len);
  }

  public void copy_from(ByteView src)
  {
    copy_from(src, 0);
  }

  public void fill(byte v, int pos, int len)
  {
    int s = size();
    assert pos <= s;
    len = (len == ByteView.npos) ? s - pos : Math.min(len, s - pos);
    java.util.Arrays.fill(array(), offset() + pos, offset() + pos + len, v);
  }

  public void fill(byte v)
  {
    fill(v, 0, ByteView.npos);
  }

  public void move_from(ByteView src, int pos)
  {
    assert pos + src.len <= size();
    System.arraycopy(src.a, src.off, array(), offset() + pos, src.len);
  }

  public void reverse(int pos, int len)
  {
    int s = size();
    assert pos <= s;
    len = (len == ByteView.npos) ? s - pos : Math.min(len, s - pos);
    byte[] p = array();
    int o = offset();
    for (int i = 0; i < len / 2; ++i)
    {
      int ei = o + pos + len - 1 - i;
      byte tmp = p[ei];
      p[ei] = p[o + i + pos];
      p[o + i + pos] = tmp;
    }
  }

  public void reverse()
  {
    reverse(0, ByteView.npos);
  }

  /* to_chars of an integer in 'base', then a '\0'; returns its length */
  private int write_integer(long value, boolean unsigned, int base, int pos, boolean prefix)
  {
    assert pos < size();
    byte[] d = array();
    int o = offset();
    int start = pos;
    if (prefix)
    {
      assert base == 16;
      d[o + pos] = '0';
      d[o + pos + 1] = 'x';
      start = pos + 2;
    }
    String s = unsigned ? Long.toUnsignedString(value, base) : Long.toString(value, base);
    int n = s.length();
    assert start + n < size();
    for (int i = 0; i < n; i++)
      d[o + start + i] = (byte) s.charAt(i);
    d[o + start + n] = 0;
    return (start + n) - pos;
  }

  public int write_number(long value, int base, int pos)
  {
    return write_integer(value, false, base, pos, false);
  }

  public int write_number(long value, int base)
  {
    return write_number(value, base, 0);
  }

  public int write_number(long value)
  {
    return write_number(value, 10, 0);
  }

  /* an unsigned value ('unsigned long' in the C++) */
  public int write_unsigned(long value, int base, int pos, boolean prefix)
  {
    return write_integer(value, true, base, pos, prefix);
  }

  /* to_chars of a float in format 'fmt' with 'precision', then a '\0' */
  public int write_number(double value, CharsFormat fmt, int precision, int pos)
  {
    assert pos < size();
    byte[] r = ToChars.format(value, fmt, precision);
    assert pos + r.length < size();
    System.arraycopy(r, 0, array(), offset() + pos, r.length);
    array()[offset() + pos + r.length] = 0;
    return r.length;
  }

  public int write_number(double value, CharsFormat fmt, int precision)
  {
    return write_number(value, fmt, precision, 0);
  }

  public ByteView view()
  {
    return new ByteView(array(), offset(), size());
  }

  public ByteView str_view()
  {
    ByteView sv = view();
    int n = sv.find(0);
    return sv.substr(0, n == ByteView.npos ? sv.size() : n);
  }

  public byte get(int i)
  {
    assert i < size();
    return array()[offset() + i];
  }

  public void set(int i, int b)
  {
    assert i < size();
    array()[offset() + i] = (byte) b;
  }

  public boolean empty()
  {
    return size() == 0;
  }
}
