using System;
using Payonel.LungoLua.Compat;
using lua_Writer = Payonel.LungoLua.Api.lua_Writer;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using Lt = Payonel.LungoLua.Bytes.Lt;
using AbsLineInfo = Payonel.LungoLua.Object.AbsLineInfo;
using Proto = Payonel.LungoLua.Object.Proto;
using TString = Payonel.LungoLua.Object.TString;
using TValue = Payonel.LungoLua.Object.TValue;
using Table = Payonel.LungoLua.Object.Table;
using Cost = Payonel.LungoLua.Runtime.Cost;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Compiler.Lundump;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Lhack;
using static Payonel.LungoLua.Runtime.Lstate;

namespace Payonel.LungoLua.Compiler;

public sealed class Ldump
{
  private Ldump()
  {
  }

  private sealed class DumpState
  {
    internal lua_State L;
    internal lua_Writer writer;
    internal long offset; /* current position relative to beginning of dump */
    internal bool strip;
    internal int status;
    internal Table h;   /* table to track saved strings */
    internal long nstr; /* counter for counting saved strings */
  }

  /*
  ** All high-level dumps go through dumpVector; you can change it to
  ** change the endianness of the result
  */

  private static void dumpBlock(DumpState D, ByteView b)
  {
    if (D.status == 0)
    { /* do not write anything after an error */
      ByteView block = (b != null)
          ? b
          : null;
      lua_unlock(D.L);
      D.status = D.writer.write(D.L, block);
      lua_lock(D.L);
      D.offset += (b != null) ? b.size() : 0;
    }
  }

  private static void dumpVector(DumpState D, ByteView v)
  {
    dumpBlock(D, v);
  }

  private static void dumpVector(DumpState D, int[] v)
  {
    ByteOwner b = new ByteOwner(v.Length * Cost.Instruction);
    for (int k = 0; k < v.Length; k++)
      b.write_int32(v[k], k * Cost.Instruction);
    dumpBlock(D, b.view());
  }

  private static void dumpVector(DumpState D, AbsLineInfo[] v)
  {
    ByteOwner b = new ByteOwner(v.Length * Cost.AbsLineInfo);
    for (int k = 0; k < v.Length; k++)
    {
      b.write_int32(v[k].pc, k * Cost.AbsLineInfo);
      b.write_int32(v[k].line, k * Cost.AbsLineInfo + JInteger.BYTES);
    }
    dumpBlock(D, b.view());
  }

  private static void dumpVar(DumpState D, sbyte x)
  {
    ByteOwner b = new ByteOwner(1);
    b.set(0, x);
    dumpVector(D, b.view());
  }

  private static void dumpVar(DumpState D, int x)
  {
    ByteOwner b = new ByteOwner(JInteger.BYTES);
    b.write_int32(x, 0);
    dumpVector(D, b.view());
  }

  private static void dumpVar(DumpState D, long x)
  {
    ByteOwner b = new ByteOwner(JLong.BYTES);
    b.write_int64(x, 0);
    dumpVector(D, b.view());
  }

  private static void dumpVar(DumpState D, double x)
  {
    ByteOwner b = new ByteOwner(JDouble.BYTES);
    b.write_double(x, 0);
    dumpVector(D, b.view());
  }

  private static readonly ByteView paddingContent = new ByteView(new sbyte[JLong.BYTES]);

  /*
  ** Dump enough zeros to ensure that current position is a multiple of
  ** 'align'.
  */
  private static void dumpAlign(DumpState D, int align)
  {
    int padding = (int) (align - D.offset % align);
    if (padding < align)
    { /* padding == align means no padding */
      { if (JAssert.on && !( align <= JLong.BYTES)) throw new AssertionError(); }
      dumpBlock(D, paddingContent.substr(0, padding));
    }
    { if (JAssert.on && !( D.offset % align == 0)) throw new AssertionError(); }
  }

  private static void dumpByte(DumpState D, int y)
  {
    sbyte x = (sbyte) y;
    dumpVar(D, x);
  }

  /*
  ** size for 'dumpVarint' buffer: each byte can store up to 7 bits.
  ** (The "+6" rounds up the division.)
  */
  private const int DIBS = (64 + 6) / 7;

  /*
  ** Dumps an unsigned integer using the MSB Varint encoding
  */
  private static void dumpVarint(DumpState D, long x)
  {
    sbyte[] buff = new sbyte[DIBS];
    int n = 1;
    buff[DIBS - 1] = (sbyte) (x & 0x7f); /* fill least-significant byte */
    while ((x >>>= 7) != 0)              /* fill other bytes in reverse order */
      buff[DIBS - (++n)] = (sbyte) ((x & 0x7f) | 0x80);
    dumpVector(D, new ByteView(buff).substr(DIBS - n));
  }

  private static void dumpSize(DumpState D, int sz)
  {
    dumpVarint(D, sz);
  }

  private static void dumpInt(DumpState D, int x)
  {
    { if (JAssert.on && !( x >= 0)) throw new AssertionError(); }
    dumpVarint(D, x);
  }

  private static void dumpNumber(DumpState D, double x)
  {
    dumpVar(D, x);
  }

  /*
  ** Signed integers are coded to keep small values small. (Coding -1 as
  ** 0xfff...fff would use too many bytes to save a quite common value.)
  ** A non-negative x is coded as 2x; a negative x is coded as -2x - 1.
  ** (0 => 0; -1 => 1; 1 => 2; -2 => 3; 2 => 4; ...)
  */
  private static void dumpInteger(DumpState D, long x)
  {
    long cx = (x >= 0) ? 2L * x
                       : (2L * ~x) + 1;
    dumpVarint(D, cx);
  }

  private static void dumpString(DumpState D, TString ts)
  {
    if (ts == null)
    {
      dumpVarint(D, 0);
      dumpVarint(D, 0);
    }
    else
    {
      TValue idx = new TValue();
      int tag = D.h.luaH_getstr(ts, idx);
      if (!tagisempty(tag))
      {                            /* string already saved? */
        dumpVarint(D, 0);          /* reuse a saved string */
        dumpVarint(D, ivalue(idx)); /* index of saved string */
      }
      else
      {                                                /* must write and save the string */
        TValue key = new TValue(), value = new TValue(); /* to save the string in the hash */
        ByteView sv = ts.data.view();
        int size = sv.size();
        dumpSize(D, size + 1);
        dumpVector(D, sv.substr(0, size));
        dumpByte(D, 0);              /* include ending '\0' */
        D.nstr++;                    /* one more saved string */
        setsvalue(D.L, key, ts);     /* the string is the key */
        setivalue(value, D.nstr);    /* its index is the value */
        D.h.luaH_set(D.L, key, value); /* h[ts] = nstr */
        /* integer value does not need barrier */
      }
    }
  }

  private static void dumpCode(DumpState D, Proto f)
  {
    dumpInt(D, f.code.Length);
    dumpAlign(D, Cost.Instruction);
    { if (JAssert.on && !( f.code.Length != 0)) throw new AssertionError(); }
    dumpVector(D, f.code);
  }

  private static void dumpConstants(DumpState D, Proto f)
  {
    int i;
    int n = f.k.Length;
    dumpInt(D, n);
    for (i = 0; i < n; i++)
    {
      TValue o = f.k[i];
      int tt = ttypetag(o);
      dumpByte(D, tt);
      switch (tt)
      {
      case LUA_VNUMFLT:
        dumpNumber(D, fltvalue(o));
        break;
      case LUA_VNUMINT:
        dumpInteger(D, ivalue(o));
        break;
      case LUA_VSHRSTR:
        { if (JAssert.on && !( ttisstring(o))) throw new AssertionError(); }
        dumpString(D, tsvalue(o));
        break;
      default:
        { if (JAssert.on && !( tt == LUA_VNIL || tt == LUA_VFALSE || tt == LUA_VTRUE)) throw new AssertionError(); }
      break; }
    }
  }

  private static void dumpProtos(DumpState D, Proto f)
  {
    int i;
    int n = f.p.Length;
    dumpInt(D, n);
    for (i = 0; i < n; i++)
      dumpFunction(D, f.p[i]);
  }

  private static void dumpUpvalues(DumpState D, Proto f)
  {
    int i, n = f.upvalues.Length;
    dumpInt(D, n);
    for (i = 0; i < n; i++)
    {
      dumpByte(D, f.upvalues[i].instack ? 1 : 0);
      dumpByte(D, f.upvalues[i].idx);
      dumpByte(D, f.upvalues[i].kind);
    }
  }

  private static void dumpDebug(DumpState D, Proto f)
  {
    int i, n;
    n = D.strip ? 0 : f.lineinfo.Length;
    dumpInt(D, n);
    if (f.lineinfo.Length != 0)
      dumpVector(D, new ByteView(f.lineinfo, 0, n));
    n = D.strip ? 0 : f.abslineinfo.Length;
    dumpInt(D, n);
    if (n > 0)
    {
      /* 'abslineinfo' is an array of structures of int's */
      dumpAlign(D, JInteger.BYTES);
      dumpVector(D, JArrays.copyOf(f.abslineinfo, n));
    }
    n = D.strip ? 0 : f.locvars.Length;
    dumpInt(D, n);
    for (i = 0; i < n; i++)
    {
      dumpString(D, f.locvars[i].varname);
      dumpInt(D, f.locvars[i].startpc);
      dumpInt(D, f.locvars[i].endpc);
    }
    n = D.strip ? 0 : f.upvalues.Length;
    dumpInt(D, n);
    for (i = 0; i < n; i++)
      dumpString(D, f.upvalues[i].name);
  }

  private static void dumpFunction(DumpState D, Proto f)
  {
    dumpInt(D, f.linedefined);
    dumpInt(D, f.lastlinedefined);
    dumpByte(D, f.numparams);
    dumpByte(D, f.flag);
    dumpByte(D, f.maxstacksize);
    dumpCode(D, f);
    dumpConstants(D, f);
    dumpUpvalues(D, f);
    dumpProtos(D, f);
    dumpString(D, D.strip ? null : f.source);
    dumpDebug(D, f);
  }

  private static void dumpNumInfo(DumpState D, int v)
  {
    dumpByte(D, JInteger.BYTES);
    dumpVar(D, v);
  }

  private static void dumpNumInfo(DumpState D, long v)
  {
    dumpByte(D, JLong.BYTES);
    dumpVar(D, v);
  }

  private static void dumpNumInfo(DumpState D, double v)
  {
    dumpByte(D, JDouble.BYTES);
    dumpVar(D, v);
  }

  private static void dumpHeader(DumpState D)
  {
    dumpVector(D, LUA_SIGNATURE.substr(0, LUA_SIGNATURE.size()));
    dumpByte(D, LUAC_VERSION);
    dumpByte(D, LUAC_FORMAT);
    dumpVector(D, Lt.LUAC_DATA.substr(0, Lt.LUAC_DATA.size()));
    dumpNumInfo(D, LUAC_INT);
    dumpNumInfo(D, LUAC_INST);
    dumpNumInfo(D, (long) LUAC_INT);
    dumpNumInfo(D, LUAC_NUM);
  }

  /*
  ** dump Lua function as precompiled chunk
  */
  public static int luaU_dump(lua_State L, Proto f, lua_Writer w,
                              bool strip)
  {
    DumpState D = new DumpState();
    D.h = Table.luaH_new(L); /* aux. table to keep strings already dumped */
    setsloth(L, L.top, D.h); /* anchor it */
    L.top++;
    D.L = L;
    D.writer = w;
    D.offset = 0;
    D.strip = strip;
    D.status = 0;
    D.nstr = 0;
    dumpHeader(D);
    dumpByte(D, f.upvalues.Length);
    dumpFunction(D, f);
    dumpBlock(D, null); /* signal end of dump */
    return D.status;
  }
}
