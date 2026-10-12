package lua.api;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import lua.bytes.ByteView;

public final class HostString
{
  private byte[] str = new byte[0];

  public HostString()
  {
  }

  public HostString(String s)
  {
    this.str = s.getBytes(StandardCharsets.UTF_8);
  }

  public HostString(ByteView bytes)
  {
    this.str = from_bytes(bytes).str;
  }

  public ByteView to_utf8()
  {
    return new ByteView(str, 0, str.length);
  }

  public String to_os()
  {
    return new String(str, StandardCharsets.UTF_8);
  }

  public static HostString from_bytes(ByteView bytes)
  {
    HostString s = new HostString();
    s.str = Arrays.copyOfRange(bytes.array(), bytes.offset(), bytes.offset() + bytes.size());
    return s;
  }

  public boolean equals(String s)
  {
    return Arrays.equals(str, s.getBytes(StandardCharsets.UTF_8));
  }

  @Override
  public boolean equals(Object o)
  {
    return (o instanceof HostString h && Arrays.equals(str, h.str)) || (o instanceof String s && equals(s));
  }

  @Override
  public int hashCode()
  {
    return Arrays.hashCode(str);
  }

  @Override
  public String toString()
  {
    return to_os();
  }
}
