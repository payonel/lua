package com.payonel.lungolua.bytes;

public final class Bytes
{
  private Bytes()
  {
  }

  public static void copy_bytes(ByteSpan dst, ByteView src)
  {
    assert src.size() <= dst.size();
    if (!src.empty())
      System.arraycopy(src.array(), src.offset(), dst.array(), dst.offset(), src.size());
  }
}
