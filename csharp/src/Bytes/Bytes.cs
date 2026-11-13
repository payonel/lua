using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

public sealed class Bytes
{
  private Bytes()
  {
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void copy_bytes(ByteSpan dst, ByteView src)
  {
    { if (JAssert.on && !( src.size() <= dst.size())) throw new AssertionError(); }
    if (!src.empty())
      JSystem.arraycopy(src.array(), src.offset(), dst.array(), dst.offset(), src.size());
  }
}
