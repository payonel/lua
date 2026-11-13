using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Api;

public sealed class HostString
{
  private sbyte[] str = new sbyte[0];

  public HostString()
  {
  }

  public HostString(string s)
  {
    this.str = s.getBytes(JCharsets.UTF_8);
  }

  public HostString(ByteView bytes)
  {
    this.str = from_bytes(bytes).str;
  }

  public ByteView to_utf8()
  {
    return new ByteView(str, 0, str.Length);
  }

  public string to_os()
  {
    return JString.of(str, JCharsets.UTF_8);
  }

  public static HostString from_bytes(ByteView bytes)
  {
    HostString s = new HostString();
    s.str = JArrays.copyOfRange(bytes.array(), bytes.offset(), bytes.offset() + bytes.size());
    return s;
  }

  public bool equals(string s)
  {
    return JArrays.equals(str, s.getBytes(JCharsets.UTF_8));
  }

  public override bool Equals(object o)
  {
    return (o is HostString h && JArrays.equals(str, h.str)) || (o is string s && equals(s));
  }

  public override int GetHashCode()
  {
    return JArrays.hashCode(str);
  }

  public override string ToString()
  {
    return to_os();
  }
}
