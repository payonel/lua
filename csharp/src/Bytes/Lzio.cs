using System;
using Payonel.LungoLua.Compat;
using lua_Reader = Payonel.LungoLua.Api.lua_Reader;
using AbsLineInfo = Payonel.LungoLua.Object.AbsLineInfo;
using Cost = Payonel.LungoLua.Runtime.Cost;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Bytes.Bytes;
using static Payonel.LungoLua.Bytes.Lload;
using static Payonel.LungoLua.Runtime.Lhack;

namespace Payonel.LungoLua.Bytes;

public sealed class Lzio
{
  private Lzio()
  {
  }

  public const int EOZ = -1; /* end of stream */

  public static int[] luaZ_overlay_Instruction(ByteView block)
  {
    int[] r = new int[block.size() / Cost.Instruction];
    for (int i = 0; i < r.Length; i++)
      r[i] = block.toInt32(i * Cost.Instruction);
    return r;
  }

  public static sbyte[] luaZ_overlay_ls_byte(ByteView block)
  {
    return block.toArray();
  }

  public static AbsLineInfo[] luaZ_overlay_AbsLineInfo(ByteView block)
  {
    AbsLineInfo[] r = new AbsLineInfo[block.size() / Cost.AbsLineInfo];
    for (int i = 0; i < r.Length; i++)
    {
      r[i] = new AbsLineInfo();
      r[i].pc = block.toInt32(i * Cost.AbsLineInfo);
      r[i].line = block.toInt32(i * Cost.AbsLineInfo + 4);
    }
    return r;
  }

  public static int luaZ_fill(ZIO z)
  {
    lua_State L = z.L;
    ByteView chunk = null;
    if (z.resuming)
    { /* the read a freeze stopped (the leaf of the frames): the reader function gave its piece */
      luaZ_restore(z.rz, LF_FILL);
      luaZ_resumed(z.rz);
      lua_unlock(L);
      chunk = luaZ_resumeread(z.rz);
      lua_lock(L);
    }
    else
    {
      LoadFrame l = new LoadFrame();
      lua_unlock(L);
      try
      {
        chunk = z.reader.read(L);
      }
      catch (Exception e)
      {
        luaZ_record(z.rz, LF_FILL, l);
        throw e;
      }
      lua_lock(L);
    }
    if (chunk == null || chunk.empty())
      return EOZ;
    z.chunk = chunk;
    z.pos = 1;
    return z.chunk.uat(0);
  }

  public static void luaZ_init(lua_State L, ZIO z, lua_Reader reader)
  {
    z.L = L;
    z.reader = reader;
    z.chunk = Lt.empty_str;
    z.pos = 0;
  }

  /* --------------------------------------------------------------- read --- */

  /*
  ** (With no frame of its own, in a load that a freeze can stop: made
  ** again, it goes the same way, the buffer empty, see lload.h.)
  */
  private static bool checkbuffer(ZIO z)
  {
    if (z.pos == z.chunk.size())
    {                          /* no bytes in buffer? */
      if (luaZ_fill(z) == EOZ) /* try to read more */
        return false;          /* no more input */
      else
        z.pos--; /* luaZ_fill consumed first byte; put it back */
    }
    return true; /* now buffer has something */
  }

  /* the frame of 'luaZ_read' (see lload.h) */
  private sealed class ReadFrame : LoadFrame
  {
    internal int n = 0;
    internal int outpos = 0;

    internal ReadFrame()
    {
    }

    internal ReadFrame(ReadFrame o) : base(o)
    {

      n = o.n;
      outpos = o.outpos;
    }

    public override LoadFrame copy()
    {
      return new ReadFrame(this);
    }

    public override int size()
    {
      return 40;
    }
  }

  /* read next n bytes */
  public static int luaZ_read(ZIO z, ByteSpan b)
  {
    ReadFrame l = new ReadFrame();
    if (z.resuming)
      l = new ReadFrame((ReadFrame) luaZ_restore(z.rz, LF_ZREAD));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
          case 0:
            l.n = b.size();
            l.site = 1;
          goto case 1; case 1: /* the loop */
            if (l.n == 0)
              return 0;
            l.site = 2;
          goto case 2; case 2:
          {
            if (!checkbuffer(z))
              return l.n; /* no more input; return number of missing bytes */
            int avail = z.chunk.size() - z.pos;
            int m = (l.n <= avail) ? l.n : avail;
            copy_bytes(b.subspan(l.outpos, m), z.chunk.substr(z.pos, m));
            z.pos += m;
            l.outpos += m;
            l.n -= m;
            l.site = 1;
            continue;
          }
          default:
            luaZ_badstate(z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(z.rz, LF_ZREAD, l);
      throw e;
    }
  }

  /* (with no frame of its own: see 'checkbuffer') */
  public static ByteView luaZ_getaddr(ZIO z, int n)
  {
    ByteView res;
    if (!checkbuffer(z))
      return null; /* no more input */
    if (z.chunk.size() - z.pos < n) /* not enough bytes? */
      return null; /* block not whole; cannot give an address */
    res = z.chunk.substr(z.pos, n);
    z.pos += n; /* consume these bytes */
    return res;
  }
}
