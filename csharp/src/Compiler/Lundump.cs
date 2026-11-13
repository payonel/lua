using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteSpan = Payonel.LungoLua.Bytes.ByteSpan;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using LoadFrame = Payonel.LungoLua.Bytes.LoadFrame;
using LoadResume = Payonel.LungoLua.Bytes.LoadResume;
using Lt = Payonel.LungoLua.Bytes.Lt;
using MBuffer = Payonel.LungoLua.Bytes.MBuffer;
using ZIO = Payonel.LungoLua.Bytes.ZIO;
using AbsLineInfo = Payonel.LungoLua.Object.AbsLineInfo;
using FmtArgs = Payonel.LungoLua.Object.FmtArgs;
using LClosure = Payonel.LungoLua.Object.LClosure;
using LocVar = Payonel.LungoLua.Object.LocVar;
using Proto = Payonel.LungoLua.Object.Proto;
using TString = Payonel.LungoLua.Object.TString;
using TValue = Payonel.LungoLua.Object.TValue;
using Table = Payonel.LungoLua.Object.Table;
using Upvaldesc = Payonel.LungoLua.Object.Upvaldesc;
using Cost = Payonel.LungoLua.Runtime.Cost;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaConf;
using static Payonel.LungoLua.Bytes.InternalString;
using static Payonel.LungoLua.Bytes.Lload;
using static Payonel.LungoLua.Bytes.Lzio;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Llimits;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Compiler;

public sealed class Lundump
{
  private Lundump()
  {
  }

  /* data to catch conversion errors */
  public const int LUAC_INT = -0x5678;
  public const int LUAC_INST = 0x12345678;
  public const double LUAC_NUM = -370.5;

  /*
  ** Encode major-minor version in one byte, one nibble for each
  */
  public const int LUAC_VERSION = LUA_VERSION_MAJOR_N * 16 + LUA_VERSION_MINOR_N;

  public const int LUAC_FORMAT = 0; /* this is the official format */

  private sealed class LoadState
  {
    internal lua_State L = null;
    internal ZIO Z = null;
    internal ByteView name = ByteView.EMPTY;
    internal Table h = null;      /* list for string reuse */
    internal long offset = 0;     /* current position relative to beginning of dump */
    internal long nstr = 0;       /* number of strings in the list */
    internal bool @fixed = false; /* dump is fixed in memory */
    internal MBuffer buff = null; /* for long strings (kept with a load a freeze stopped) */

    internal LoadState()
    {
    }

    internal LoadState(LoadState o)
    {
      L = o.L;
      Z = o.Z;
      name = o.name;
      h = o.h;
      offset = o.offset;
      nstr = o.nstr;
      @fixed = o.@fixed;
      buff = o.buff;
    }
  }

  /*
  ** A load that a freeze can stop (see lload.h): a function with calls a
  ** freeze can pass through runs as a state machine, its state and its
  ** locals in its frame ('l'). Each such call is made at a state of its own,
  ** the one its function goes to again when it is made again after the
  ** thaw; a freeze passing records the frame. A check point is a leaf: made
  ** again, the function goes on after it. A function with no frame of its
  ** own (as 'loadByte') goes the same way when made again.
  */

  /* the frames of the binary loader's functions (see lload.h) */
  private sealed class VarintFrame : LoadFrame
  {
    internal long x = 0;

    internal VarintFrame()
    {
    }

    internal VarintFrame(VarintFrame o) : base(o)
    {

      x = o.x;
    }

    public override LoadFrame copy()
    {
      return new VarintFrame(this);
    }

    public override int size()
    {
      return 32;
    }
  }

  private sealed class LoadNumberFrame : LoadFrame
  {
    internal ByteOwner x = new ByteOwner(JDouble.BYTES);

    internal LoadNumberFrame()
    {
    }

    internal LoadNumberFrame(LoadNumberFrame o) : base(o)
    {

      x = new ByteOwner(o.x.view());
    }

    public override LoadFrame copy()
    {
      return new LoadNumberFrame(this);
    }

    public override int size()
    {
      return 32;
    }
  }

  private sealed class LoadStringFrame : LoadFrame
  {
    internal int len = 0; /* (the string's size) */
    internal ByteOwner buff = new ByteOwner(41);

    internal LoadStringFrame()
    {
    }

    internal LoadStringFrame(LoadStringFrame o) : base(o)
    {

      len = o.len;
      buff = new ByteOwner(o.buff.view());
    }

    public override LoadFrame copy()
    {
      return new LoadStringFrame(this);
    }

    public override int size()
    {
      return 80;
    }
  }

  /* (of the functions with a count and an index) */
  private sealed class CountFrame : LoadFrame
  {
    internal int n = 0;
    internal int i = 0;

    internal CountFrame()
    {
    }

    internal CountFrame(CountFrame o) : base(o)
    {

      n = o.n;
      i = o.i;
    }

    public override LoadFrame copy()
    {
      return new CountFrame(this);
    }

    public override int size()
    {
      return 32;
    }
  }

  private sealed class LiteralFrame : LoadFrame
  {
    internal ByteOwner buff = new ByteOwner(LUA_SIGNATURE.size() + Lt.LUAC_DATA.size()); /* larger than both */

    internal LiteralFrame()
    {
    }

    internal LiteralFrame(LiteralFrame o) : base(o)
    {

      buff = new ByteOwner(o.buff.view());
    }

    public override LoadFrame copy()
    {
      return new LiteralFrame(this);
    }

    public override int size()
    {
      return 40;
    }
  }

  private sealed class CheckNumFrame : LoadFrame
  {
    internal ByteOwner raw = new ByteOwner(JLong.BYTES > JDouble.BYTES ? JLong.BYTES : JDouble.BYTES); /* (the bytes read) */

    internal CheckNumFrame()
    {
    }

    internal CheckNumFrame(CheckNumFrame o) : base(o)
    {

      raw = new ByteOwner(o.raw.view());
    }

    public override LoadFrame copy()
    {
      return new CheckNumFrame(this);
    }

    public override int size()
    {
      return 32;
    }
  }

  private sealed class UndumpFrame : LoadFrame
  {
    internal LoadState S = new LoadState();
    internal LClosure cl = null;

    internal UndumpFrame()
    {
    }

    internal UndumpFrame(UndumpFrame o) : base(o)
    {

      S = new LoadState(o.S);
      cl = o.cl;
    }

    public override LoadFrame copy()
    {
      return new UndumpFrame(this);
    }

    public override int size()
    {
      return 104;
    }
  }

  /* a check point of the load: a freeze can stop it there (the leaf of the frames) */
  private static void checkpoint(LoadState S, LoadFrame l, int site)
  {
    LoadResume rz = S.Z.rz;
    if (rz != null && luaZ_checkdue(rz, S.L))
    {
      l.site = site;
      luaZ_checkpoint(rz);
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static RuntimeException error(LoadState S, ByteView why)
  {
    luaO_pushfstring(S.L, Lt.s_bad_binary_format_s, new FmtArgs().str(S.name).str(why));
    throw luaD_throw(S.L, (sbyte) LUA_ERRSYNTAX);
  }

  /*
  ** All high-level loads go through loadVector; you can change it to
  ** adapt to the endianness of the input
  */

  /* (with no frame of its own: made again, it reads the rest of 'b') */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void loadBlock(LoadState S, ByteSpan b)
  {
    if (luaZ_read(S.Z, b) != 0)
      error(S, Lt.truncated_chunk);
    S.offset += b.size();
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void loadVector(LoadState S, ByteSpan v)
  {
    loadBlock(S, v);
  }

  private static void loadVector(LoadState S, int[] v)
  {
    ByteOwner b = new ByteOwner(v.Length * Cost.Instruction);
    for (int k = 0; k < v.Length; k++)
      b.write_int32(v[k], k * Cost.Instruction);
    try
    {
      loadBlock(S, b.span());
    }
    finally
    {
      ByteView bv = b.view();
      for (int k = 0; k < v.Length; k++)
        v[k] = bv.toInt32(k * Cost.Instruction);
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void loadVector(LoadState S, sbyte[] v)
  {
    loadBlock(S, ByteOwner.adopt(v).span());
  }

  private static void loadVector(LoadState S, AbsLineInfo[] v)
  {
    ByteOwner b = new ByteOwner(v.Length * Cost.AbsLineInfo);
    for (int k = 0; k < v.Length; k++)
    {
      b.write_int32(v[k].pc, k * Cost.AbsLineInfo);
      b.write_int32(v[k].line, k * Cost.AbsLineInfo + JInteger.BYTES);
    }
    try
    {
      loadBlock(S, b.span());
    }
    finally
    {
      ByteView bv = b.view();
      for (int k = 0; k < v.Length; k++)
      {
        v[k].pc = bv.toInt32(k * Cost.AbsLineInfo);
        v[k].line = bv.toInt32(k * Cost.AbsLineInfo + JInteger.BYTES);
      }
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void loadVar(LoadState S, ByteOwner x)
  {
    loadVector(S, x.span());
  }

  /* (with no frame of its own: made again, it goes the same way; the padding's bytes are not kept) */
  private static void loadAlign(LoadState S, int align)
  {
    int padding = (int) (align - S.offset % align);
    if (padding < align)
    { /* (padding == align) means no padding */
      ByteOwner paddingContent = new ByteOwner(JLong.BYTES);
      loadBlock(S, paddingContent.span().subspan(0, padding));
      { if (JAssert.on && !( S.offset % align == 0)) throw new AssertionError(); }
    }
  }

  private static int[] getaddr_Instruction(LoadState S, int n)
  {
    int size = n * Cost.Instruction;
    ByteView block = luaZ_getaddr(S.Z, size);
    S.offset += size;
    if (block == null)
      error(S, Lt.truncated_fixed_buffer);
    return luaZ_overlay_Instruction(block);
  }

  private static sbyte[] getaddr_ls_byte(LoadState S, int n)
  {
    int size = n * Cost.Byte;
    ByteView block = luaZ_getaddr(S.Z, size);
    S.offset += size;
    if (block == null)
      error(S, Lt.truncated_fixed_buffer);
    return luaZ_overlay_ls_byte(block);
  }

  private static AbsLineInfo[] getaddr_AbsLineInfo(LoadState S, int n)
  {
    int size = n * Cost.AbsLineInfo;
    ByteView block = luaZ_getaddr(S.Z, size);
    S.offset += size;
    if (block == null)
      error(S, Lt.truncated_fixed_buffer);
    return luaZ_overlay_AbsLineInfo(block);
  }

  /* (with no frame of its own: made again, it goes the same way) */
  private static sbyte loadByte(LoadState S)
  {
    int b = (S.Z.pos < S.Z.chunk.size())
                ? S.Z.chunk.uat(S.Z.pos++)
                : luaZ_fill(S.Z);
    if (b == EOZ)
      error(S, Lt.truncated_chunk);
    S.offset++;
    return (sbyte) b;
  }

  private static long loadVarint(LoadState S, long limit)
  {
    VarintFrame l = new VarintFrame();
    if (S.Z.resuming)
      l = new VarintFrame((VarintFrame) luaZ_restore(S.Z.rz, LF_LOADVARINT));
    limit >>>= 7;
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0: /* the loop */
        {
          int b = loadByte(S);
          if (JLong.compareUnsigned(l.x, limit) > 0)
            error(S, Lt.integer_overflow);
          l.x = (l.x << 7) | (b & 0x7f);
          if ((b & 0x80) == 0)
            return l.x;
          continue;
        }
        default:
          luaZ_badstate(S.Z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADVARINT, l);
      throw e;
    }
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int loadSize(LoadState S)
  {
    return (int) loadVarint(S, MAX_SIZE);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static int loadInt(LoadState S)
  {
    return (int) loadVarint(S, JInteger.MAX_VALUE);
  }

  private static double loadNumber(LoadState S)
  {
    LoadNumberFrame l = new LoadNumberFrame();
    if (S.Z.resuming)
      l = new LoadNumberFrame((LoadNumberFrame) luaZ_restore(S.Z.rz, LF_LOADNUMBER));
    try
    {
      switch (l.site)
      {
      case 0:
        loadVar(S, l.x);
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADNUMBER, l);
      throw e;
    }
    return l.x.view().toDouble(0);
  }

  private static long loadInteger(LoadState S)
  {
    long cx = loadVarint(S, LUA_MAXUNSIGNED);
    /* decode unsigned to signed */
    if ((cx & 1) != 0)
      return ~(cx >>> 1);
    else
      return cx >>> 1;
  }

  /*
  ** Load a nullable string into slot 'sl' from prototype 'p'. The
  ** assignment to the slot and the barrier must be performed before any
  ** possible GC activity, to anchor the string. (Both 'loadVector' and
  ** 'luaH_setint' can call the GC.)
  */
  private static void loadString(LoadState S, Proto p, Consumer<TString> setslot)
  {
    LoadStringFrame l = new LoadStringFrame();
    lua_State L = S.L;
    TString ts = null;
    TValue sv = new TValue();
    if (S.Z.resuming)
      l = new LoadStringFrame((LoadStringFrame) luaZ_restore(S.Z.rz, LF_LOADSTRING));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.len = loadSize(S);
          if (l.len == 0) /* previously saved string? */
            l.site = 1;
          else if ((l.len -= 1) <= 40)
            l.site = 2;
          else
          {
            S.buff.growTo(l.len + 1);
            l.site = 3;
          }
          continue;
        case 1:
        {
          long idx = loadVarint(S, LUA_MAXUNSIGNED); /* get its index */
          TValue stv = new TValue();
          if (idx == 0)
          { /* no string? */
            setslot.accept(null);
            return;
          }
          if (novariant(S.h.luaH_getint(idx, stv)) != LUA_TSTRING)
            error(S, Lt.invalid_string_index);
          { if (JAssert.on && !( ttisstring(stv))) throw new AssertionError(); }
          ts = tsvalue(stv); /* get its value */
          setslot.accept(ts);
          luaC_objbarrier(L, p, ts);
          return; /* do not save it again */
        }
        case 2:
        {
          loadVector(S, l.buff.span().subspan(0, l.len + 1)); /* load string into buffer */
          ByteView buffstr = l.buff.view().substr(0, l.len);
          ts = TString.luaS_createstr(L, buffstr); /* create string */
          setslot.accept(ts);
          luaC_objbarrier(L, p, ts);
          break;
        }
        case 3:
          loadVector(S, S.buff.area().subspan(0, l.len + 1));
          ts = TString.luaS_takeblock(L, S.buff.handover(), l.len); /* the buffer's bytes are the string's */
          setslot.accept(ts);
          luaC_objbarrier(L, p, ts);
          break;
        default:
          luaZ_badstate(S.Z.rz);
        break; }
        break; /* (the string is made) */
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADSTRING, l);
      throw e;
    }
    /* add string to list of saved strings */
    S.nstr++;
    setsvalue(L, sv, ts);
    S.h.luaH_setint(L, S.nstr, sv);
    { if (JAssert.on && !( novariant(S.h.tt) >= LUA_TSTRING)) throw new AssertionError(); }
    luaC_objbarrierback(L, S.h, ts);
  }

  private static void loadCode(LoadState S, Proto f)
  {
    CountFrame l = new CountFrame();
    if (S.Z.resuming)
      l = new CountFrame((CountFrame) luaZ_restore(S.Z.rz, LF_LOADCODE));
    try
    {
      switch (l.site)
      {
      case 0:
        l.n = loadInt(S);
        l.site = 1;
      goto case 1; case 1:
        loadAlign(S, Cost.Instruction);
        if (S.@fixed)
        {
          f.code = getaddr_Instruction(S, l.n);
          break;
        }
        f.code = luaM_alloc_Instruction_span(S.L.l_G, S.L, new int[0], l.n, true);
        l.site = 2;
      goto case 2; case 2:
        loadVector(S, f.code);
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADCODE, l);
      throw e;
    }
  }

  private static void loadConstants(LoadState S, Proto f)
  {
    CountFrame l = new CountFrame();
    if (S.Z.resuming)
    {
      l = new CountFrame((CountFrame) luaZ_restore(S.Z.rz, LF_LOADCONSTANTS));
      if (l.site == 2)
      { /* (the check point is behind it) */
        luaZ_resumed(S.Z.rz);
        l.site = 3;
      }
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.n = loadInt(S);
          f.k = luaM_alloc_TValue_span(S.L.l_G, S.L, new TValue[0], l.n, true);
          for (l.i = 0; l.i < l.n; l.i++)
            setnilvalue(f.k[l.i]);
          l.i = 0;
          l.site = 1;
        goto case 1; case 1: /* the loop */
          if (l.i >= l.n)
            return;
          checkpoint(S, l, 2);
          l.site = 3;
        goto case 3; case 3:
          switch (loadByte(S))
          {
          case LUA_VNIL:
            setnilvalue(f.k[l.i]);
            break;
          case LUA_VFALSE:
            f.k[l.i].tt_ = LUA_VFALSE;
            break;
          case LUA_VTRUE:
            f.k[l.i].tt_ = LUA_VTRUE;
            break;
          case LUA_VNUMFLT:
            l.site = 4;
            continue;
          case LUA_VNUMINT:
            l.site = 5;
            continue;
          case LUA_VSHRSTR:
            { if (JAssert.on && !( f.source == null)) throw new AssertionError(); }
            l.site = 6;
            continue;
          default:
            error(S, Lt.invalid_constant);
          break; }
          l.i++;
          l.site = 1;
          continue;
        case 4:
        {
          double x = loadNumber(S);
          setfltvalue(f.k[l.i], x);
          l.i++;
          l.site = 1;
          continue;
        }
        case 5:
        {
          long x = loadInteger(S);
          setivalue(f.k[l.i], x);
          l.i++;
          l.site = 1;
          continue;
        }
        case 6:
          loadString(S, f, ts => { f.source = ts; }); /* use 'source' to anchor string */
          if (f.source == null)
            error(S, Lt.bad_format_for_constant_string);
          setsvalue(S.L, f.k[l.i], f.source); /* save it in the right place */
          f.source = null;
          l.i++;
          l.site = 1;
          continue;
        default:
          luaZ_badstate(S.Z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADCONSTANTS, l);
      throw e;
    }
  }

  private static void loadProtos(LoadState S, Proto f)
  {
    CountFrame l = new CountFrame();
    if (S.Z.resuming)
      l = new CountFrame((CountFrame) luaZ_restore(S.Z.rz, LF_LOADPROTOS));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.n = loadInt(S);
          f.p = luaM_alloc_Proto_ptr_span(S.L.l_G, S.L, new Proto[0], l.n, true);
          for (l.i = 0; l.i < l.n; l.i++)
            f.p[l.i] = null;
          l.i = 0;
          l.site = 1;
        goto case 1; case 1: /* the loop */
          if (l.i >= l.n)
            return;
          f.p[l.i] = Proto.luaF_newproto(S.L);
          luaC_objbarrier(S.L, f, f.p[l.i]);
          l.site = 2;
        goto case 2; case 2:
          loadFunction(S, f.p[l.i]);
          l.i++;
          l.site = 1;
          continue;
        default:
          luaZ_badstate(S.Z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADPROTOS, l);
      throw e;
    }
  }

  /*
  ** Load the upvalues for a function. The names must be filled first,
  ** because the filling of the other fields can raise read errors and
  ** the creation of the error message can call an emergency collection;
  ** in that case all prototypes must be consistent for the GC.
  */
  private static void loadUpvalues(LoadState S, Proto f)
  {
    CountFrame l = new CountFrame();
    if (S.Z.resuming)
      l = new CountFrame((CountFrame) luaZ_restore(S.Z.rz, LF_LOADUPVALUES));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.n = loadInt(S);
          f.upvalues = luaM_alloc_Upvaldesc_span(S.L.l_G, S.L, new Upvaldesc[0], l.n, true);
          for (l.i = 0; l.i < l.n; l.i++) /* make array valid for GC */
            f.upvalues[l.i].name = null;
          l.i = 0;
          l.site = 1;
        goto case 1; case 1: /* the loop (following calls can raise errors) */
          if (l.i >= l.n)
            return;
          l.site = 2;
        goto case 2; case 2:
          f.upvalues[l.i].instack = loadByte(S) != 0;
          l.site = 3;
        goto case 3; case 3:
          f.upvalues[l.i].idx = loadByte(S) & 0xFF;
          l.site = 4;
        goto case 4; case 4:
          f.upvalues[l.i].kind = loadByte(S);
          l.i++;
          l.site = 1;
          continue;
        default:
          luaZ_badstate(S.Z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADUPVALUES, l);
      throw e;
    }
  }

  private static void loadDebug(LoadState S, Proto f)
  {
    CountFrame l = new CountFrame();
    if (S.Z.resuming)
    {
      l = new CountFrame((CountFrame) luaZ_restore(S.Z.rz, LF_LOADDEBUG));
      if (l.site == 7)
      { /* (the check point is behind it) */
        luaZ_resumed(S.Z.rz);
        l.site = 8;
      }
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.n = loadInt(S);
          if (S.@fixed)
          {
            f.lineinfo = getaddr_ls_byte(S, l.n);
            l.site = 2;
            continue;
          }
          f.lineinfo = luaM_alloc_ls_byte_span(S.L.l_G, S.L, new sbyte[0], l.n, true);
          l.site = 1;
        goto case 1; case 1:
          loadVector(S, f.lineinfo);
          l.site = 2;
        goto case 2; case 2:
          l.n = loadInt(S);
          if (l.n <= 0)
          {
            l.site = 5;
            continue;
          }
          l.site = 3;
        goto case 3; case 3:
          loadAlign(S, JInteger.BYTES);
          if (S.@fixed)
          {
            f.abslineinfo = getaddr_AbsLineInfo(S, l.n);
            l.site = 5;
            continue;
          }
          f.abslineinfo = luaM_alloc_AbsLineInfo_span(S.L.l_G, S.L, new AbsLineInfo[0], l.n, true);
          l.site = 4;
        goto case 4; case 4:
          loadVector(S, f.abslineinfo);
          l.site = 5;
        goto case 5; case 5:
          l.n = loadInt(S);
          f.locvars = luaM_alloc_LocVar_span(S.L.l_G, S.L, new LocVar[0], l.n, true);
          for (l.i = 0; l.i < l.n; l.i++)
            f.locvars[l.i].varname = null;
          l.i = 0;
          l.site = 6;
        goto case 6; case 6: /* the loop */
          if (l.i >= l.n)
          {
            l.site = 11;
            continue;
          }
          checkpoint(S, l, 7);
          l.site = 8;
        goto case 8; case 8:
        {
          int i = l.i;
          loadString(S, f, ts => { f.locvars[i].varname = ts; });
          l.site = 9;
        }
        goto case 9; case 9:
          f.locvars[l.i].startpc = loadInt(S);
          l.site = 10;
        goto case 10; case 10:
          f.locvars[l.i].endpc = loadInt(S);
          l.i++;
          l.site = 6;
          continue;
        case 11:
          l.n = loadInt(S);
          if (l.n != 0)                 /* does it have debug information? */
            l.n = f.upvalues.Length; /* must be this many */
          l.i = 0;
          l.site = 12;
        goto case 12; case 12: /* the loop */
          if (l.i >= l.n)
            return;
          l.site = 13;
        goto case 13; case 13:
        {
          int i = l.i;
          loadString(S, f, ts => { f.upvalues[i].name = ts; });
          l.i++;
          l.site = 12;
          continue;
        }
        default:
          luaZ_badstate(S.Z.rz);
        break; }
      }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADDEBUG, l);
      throw e;
    }
  }

  private static void loadFunction(LoadState S, Proto f)
  {
    LoadFrame l = new LoadFrame();
    if (S.Z.resuming)
    {
      l = new LoadFrame(luaZ_restore(S.Z.rz, LF_LOADFUNCTION));
      if (l.site == 1)
      { /* (the check point is behind it) */
        luaZ_resumed(S.Z.rz);
        l.site = 2;
      }
    }
    try
    {
      switch (l.site)
      {
      case 0:
        checkpoint(S, l, 1);
        l.site = 2;
      goto case 2; case 2:
        f.linedefined = loadInt(S);
        l.site = 3;
      goto case 3; case 3:
        f.lastlinedefined = loadInt(S);
        l.site = 4;
      goto case 4; case 4:
        f.numparams = loadByte(S) & 0xFF;
        l.site = 5;
      goto case 5; case 5:
        /* get only the meaningful flags */
        f.flag = (sbyte) (loadByte(S) & ~PF_FIXED);
        if (S.@fixed)
          f.flag |= PF_FIXED; /* signal that code is fixed */
        l.site = 6;
      goto case 6; case 6:
        f.maxstacksize = loadByte(S) & 0xFF;
        l.site = 7;
      goto case 7; case 7:
        loadCode(S, f);
        l.site = 8;
      goto case 8; case 8:
        loadConstants(S, f);
        l.site = 9;
      goto case 9; case 9:
        loadUpvalues(S, f);
        l.site = 10;
      goto case 10; case 10:
        loadProtos(S, f);
        l.site = 11;
      goto case 11; case 11:
        loadString(S, f, ts => { f.source = ts; });
        l.site = 12;
      goto case 12; case 12:
        loadDebug(S, f);
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_LOADFUNCTION, l);
      throw e;
    }
  }

  private static void checkliteral(LoadState S, ByteView s, ByteView msg)
  {
    LiteralFrame l = new LiteralFrame();
    int len = s.size();
    if (S.Z.resuming)
      l = new LiteralFrame((LiteralFrame) luaZ_restore(S.Z.rz, LF_CHECKLITERAL));
    try
    {
      switch (l.site)
      {
      case 0:
        loadVector(S, l.buff.span().subspan(0, len));
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_CHECKLITERAL, l);
      throw e;
    }
    if (!s.equals(l.buff.view().substr(0, len)))
      error(S, msg);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static RuntimeException numerror(LoadState S, ByteView what, ByteView tname)
  {
    ByteView msg = luaO_pushfstring(S.L, Lt.s_s_mismatch, new FmtArgs().str(tname).str(what)).view();
    throw error(S, msg);
  }

  /* (with no frame of its own: made again, it goes the same way) */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void checknumsize(LoadState S, int size, ByteView tname)
  {
    if (size != loadByte(S))
      numerror(S, Lt.size, tname);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static void checknumformat(LoadState S, bool eq, ByteView tname)
  {
    if (!eq)
      numerror(S, Lt.format, tname);
  }

  /* (its frame keeps the bytes read, as they come: the number is made of them at the end) */
  private static void checknum(LoadState S, int v, ByteView tname)
  {
    CheckNumFrame l = new CheckNumFrame();
    if (S.Z.resuming)
      l = new CheckNumFrame((CheckNumFrame) luaZ_restore(S.Z.rz, LF_CHECKNUM));
    try
    {
      switch (l.site)
      {
      case 0:
        checknumsize(S, JInteger.BYTES, tname);
        l.site = 1;
      goto case 1; case 1:
        loadVector(S, l.raw.span().subspan(0, JInteger.BYTES));
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_CHECKNUM, l);
      throw e;
    }
    int i = l.raw.view().toInt32(0);
    checknumformat(S, i == v, tname);
  }

  private static void checknum(LoadState S, long v, ByteView tname)
  {
    CheckNumFrame l = new CheckNumFrame();
    if (S.Z.resuming)
      l = new CheckNumFrame((CheckNumFrame) luaZ_restore(S.Z.rz, LF_CHECKNUM));
    try
    {
      switch (l.site)
      {
      case 0:
        checknumsize(S, JLong.BYTES, tname);
        l.site = 1;
      goto case 1; case 1:
        loadVector(S, l.raw.span().subspan(0, JLong.BYTES));
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_CHECKNUM, l);
      throw e;
    }
    long i = l.raw.view().toInt64(0);
    checknumformat(S, i == v, tname);
  }

  private static void checknum(LoadState S, double v, ByteView tname)
  {
    CheckNumFrame l = new CheckNumFrame();
    if (S.Z.resuming)
      l = new CheckNumFrame((CheckNumFrame) luaZ_restore(S.Z.rz, LF_CHECKNUM));
    try
    {
      switch (l.site)
      {
      case 0:
        checknumsize(S, JDouble.BYTES, tname);
        l.site = 1;
      goto case 1; case 1:
        loadVector(S, l.raw.span().subspan(0, JDouble.BYTES));
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_CHECKNUM, l);
      throw e;
    }
    double i = l.raw.view().toDouble(0);
    checknumformat(S, i == v, tname);
  }

  private static void checkHeader(LoadState S)
  {
    LoadFrame l = new LoadFrame();
    /* skip 1st char (already read and checked) */
    ByteView sig_tail = LUA_SIGNATURE.substr(1);
    if (S.Z.resuming)
      l = new LoadFrame(luaZ_restore(S.Z.rz, LF_CHECKHEADER));
    try
    {
      switch (l.site)
      {
      case 0:
        checkliteral(S, sig_tail, Lt.not_a_binary_chunk);
        l.site = 1;
      goto case 1; case 1:
        if (loadByte(S) != LUAC_VERSION)
          error(S, Lt.version_mismatch);
        l.site = 2;
      goto case 2; case 2:
        if (loadByte(S) != LUAC_FORMAT)
          error(S, Lt.format_mismatch);
        l.site = 3;
      goto case 3; case 3:
        checkliteral(S, Lt.LUAC_DATA, Lt.corrupted_chunk);
        l.site = 4;
      goto case 4; case 4:
        checknum(S, LUAC_INT, Lt.int_kw);
        l.site = 5;
      goto case 5; case 5:
        checknum(S, LUAC_INST, Lt.instruction);
        l.site = 6;
      goto case 6; case 6:
        checknum(S, (long) LUAC_INT, Lt.Lua_integer);
        l.site = 7;
      goto case 7; case 7:
        checknum(S, LUAC_NUM, Lt.Lua_number);
        break;
      default:
        luaZ_badstate(S.Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(S.Z.rz, LF_CHECKHEADER, l);
      throw e;
    }
  }

  /*
  ** Load precompiled chunk.
  */
  public static LClosure luaU_undump(lua_State L, ZIO Z, Table anchor, MBuffer buff, ByteView name, bool @fixed)
  {
    UndumpFrame l = new UndumpFrame();
    int c0 = sv_byteat(name, 0);
    if (c0 == '@' || c0 == '=')
    {
      name = name.substr(1);
    }
    else if (c0 == LUA_SIGNATURE.at(0))
    {
      name = Lt.binary_string;
    }
    if (Z.resuming)
    { /* (the state again with this load's stream and buffer) */
      l = new UndumpFrame((UndumpFrame) luaZ_restore(Z.rz, LF_UNDUMP));
      l.S.L = L;
      l.S.Z = Z;
      l.S.h = anchor;
      l.S.name = name;
      l.S.buff = buff;
    }
    else
    {
      l.S.name = name;
      l.S.L = L;
      l.S.Z = Z;
      l.S.@fixed = @fixed;
      l.S.buff = buff;
      l.S.offset = 1; /* fist byte was already read */
    }
    LoadState S = l.S;
    try
    {
      switch (l.site)
      {
      case 0:
        checkHeader(S);
        l.S.h = anchor;
        l.S.nstr = 0;
        l.site = 1;
      goto case 1; case 1:
      {
        int nup = loadByte(S) & 0xFF;
        l.cl = LClosure.luaF_newLclosure(L, nup);
        luaZ_madefunction(Z.rz, l.cl);
        { if (JAssert.on && !( novariant(l.cl.tt) >= LUA_TSTRING)) throw new AssertionError(); }
        luaD_anchorobj(L, anchor, l.cl);
        l.cl.p = Proto.luaF_newproto(L);
        luaC_objbarrier(L, l.cl, l.cl.p);
        l.site = 2;
      }
      goto case 2; case 2:
        loadFunction(S, l.cl.p);
        break;
      default:
        luaZ_badstate(Z.rz);
      break; }
    }
    catch (Exception e)
    {
      luaZ_record(Z.rz, LF_UNDUMP, l);
      throw e;
    }
    if (l.cl.upvals.Length != l.cl.p.upvalues.Length)
      error(S, Lt.corrupted_chunk);
    return l.cl;
  }
}
