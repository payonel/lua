package lua.api;

import java.nio.charset.StandardCharsets;

import lua.bytes.ByteView;

/* consumer text: a Java String; crosses into the engine as UTF-8 bytes */
public final class HostString
{
  private final String str;

  public HostString()
  {
    this("");
  }

  public HostString(String s)
  {
    this.str = s;
  }

  public HostString(ByteView bytes)
  {
    this.str = from_bytes(bytes).str;
  }

  public ByteView to_utf8()
  {
    return new ByteView(str.getBytes(StandardCharsets.UTF_8));
  }

  public String to_os()
  {
    return str;
  }

  public static HostString from_bytes(ByteView bytes)
  {
    return new HostString(new String(bytes.array(), bytes.offset(), bytes.size(), StandardCharsets.UTF_8));
  }

  public boolean equals(String s)
  {
    return str.equals(s);
  }

  @Override
  public boolean equals(Object o)
  {
    return (o instanceof HostString h && str.equals(h.str)) || (o instanceof String s && str.equals(s));
  }

  @Override
  public int hashCode()
  {
    return str.hashCode();
  }

  @Override
  public String toString()
  {
    return str;
  }
}
