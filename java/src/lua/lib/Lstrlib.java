package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.api.LuaConf.*;
import static lua.bytes.InternalString.sv_byteat;
import static lua.bytes.UtilCast.pointertext;
import static lua.gc.Lgc.*;
import static lua.lib.Lstrlib.KOption.*;
import static lua.object.Llimits.*;
import static lua.object.Lnames.*;
import static lua.object.Lobject.*;
import static lua.platform.Libm.*;
import static lua.platform.LocaleUtil.*;
import static lua.runtime.Lstate.*;

import java.nio.ByteOrder;

import lua.api.CClosureNamer;
import lua.api.HostString;
import lua.api.KNamer;
import lua.api.luaL_Box;
import lua.api.luaL_Buffer;
import lua.api.luaL_RegCFI;
import lua.api.lua_CClosureKind;
import lua.api.lua_CFunction;
import lua.api.lua_KFunction;
import lua.api.lua_ObjectFactory;
import lua.api.lua_UserObject;
import lua.api.lua_Writer;
import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.ByteWrites;
import lua.bytes.CharsFormat;
import lua.bytes.FmtScan;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.object.CClosure;
import lua.object.CFuncInfo;
import lua.object.TValue;
import lua.object.Udata;
import lua.runtime.lua_State;

public final class Lstrlib
{
  private Lstrlib()
  {
  }

  /*
  ** maximum number of captures that a pattern can do during
  ** pattern-matching. This limit is arbitrary, but must fit in
  ** an unsigned char.
  */
  private static final int LUA_MAXCAPTURES = 32;

  private static int str_len(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    lua_pushinteger(L, s.size());
    return 1;
  }

  private static final lua_CFunction str_len = Lstrlib::str_len;

  /*
  ** translate a relative initial string position
  ** (negative means back from end): clip result to [1, inf).
  ** The length of any string in Lua must fit in a lua_Integer,
  ** so there are no overflows in the casts.
  ** The inverted comparison avoids a possible overflow
  ** computing '-pos'.
  */
  private static long posrelatI(long pos, int len)
  {
    if (pos > 0)
      return pos;
    else if (pos == 0)
      return 1;
    else if (pos < -(long) len) /* inverted comparison */
      return 1;                 /* clip to 1 */
    else
      return len + pos + 1;
  }

  /*
  ** Gets an optional ending string position from argument 'arg',
  ** with default value 'def'.
  ** Negative means back from end: clip result to [0, len]
  */
  private static int getendpos(lua_State L, int arg, long def,
                               int len)
  {
    long pos = luaL_optinteger(L, arg, def);
    if (pos > (long) len)
      return len;
    else if (pos >= 0)
      return (int) pos;
    else if (pos < -(long) len)
      return 0;
    else
      return (int) (len + pos + 1);
  }

  private static int str_sub(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    int l = s.size();
    long start = posrelatI(luaL_checkinteger(L, 2), l);
    int end = getendpos(L, 3, -1, l);
    if (start <= end)
      lua_pushlstring(L, s.view(), (int) start - 1, (end - (int) start) + 1);
    else
      lua_pushistringref(L, Lt.empty_str);
    return 1;
  }

  private static final lua_CFunction str_sub = Lstrlib::str_sub;

  private static int str_reverse(lua_State L)
  {
    int i;
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      InternalString s = luaL_checkistring(L, 1);
      int l = s.size();
      int off = luaL_buffinitsize(L, b, l);
      ByteSpan p = luaL_buffaddr(b);
      for (i = 0; i < l; i++)
        p.set(off + i, (byte) s.byteAt(l - i - 1));
      luaL_pushresultsize(b, l);
      return 1;
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_reverse = Lstrlib::str_reverse;

  private static int str_lower(lua_State L)
  {
    int i;
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      InternalString s = luaL_checkistring(L, 1);
      int l = s.size();
      int off = luaL_buffinitsize(L, b, l);
      ByteSpan p = luaL_buffaddr(b);
      for (i = 0; i < l; i++)
        p.set(off + i, (byte) locale_tolower(s.byteAt(i)));
      luaL_pushresultsize(b, l);
      return 1;
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_lower = Lstrlib::str_lower;

  private static int str_upper(lua_State L)
  {
    int i;
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      InternalString s = luaL_checkistring(L, 1);
      int l = s.size();
      int off = luaL_buffinitsize(L, b, l);
      ByteSpan p = luaL_buffaddr(b);
      for (i = 0; i < l; i++)
        p.set(off + i, (byte) locale_toupper(s.byteAt(i)));
      luaL_pushresultsize(b, l);
      return 1;
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_upper = Lstrlib::str_upper;

  /*
  ** MAX_SIZE is limited both by size_t and lua_Integer.
  ** When x <= MAX_SIZE, x can be safely cast to size_t or lua_Integer.
  */
  private static int str_rep(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    ByteView sv = s.view();
    long n = luaL_checkinteger(L, 2);
    InternalString sep = luaL_optistring(L, 3, Lt.empty_str_i);
    ByteView sepsv = sep.view();
    if (n <= 0 || (sv.size() | sepsv.size()) == 0)
    {
      lua_pushistringref(L, Lt.empty_str); /* no repetitions or both strings empty */
    }
    else if (sv.size() > MAX_SIZE - sepsv.size() || ((long) sv.size() + sepsv.size()) > MAX_SIZE / n)
    {
      return luaL_errori(L, Lt.resulting_string_too_large);
    }
    else
    {
      long totallen = (n * ((long) sv.size() + sepsv.size())) - sepsv.size();
      luaL_Buffer b = new luaL_Buffer();
      try
      {
        int off = luaL_buffinitsize(L, b, totallen);
        ByteSpan p = luaL_buffaddr(b);
        int pos = off;
        while (n-- > 1)
        { /* first n-1 copies (followed by separator) */
          p.copy_from(sv, pos);
          pos += sv.size();
          if (sepsv.size() > 0)
          { /* empty 'memcpy' is not that cheap */
            p.copy_from(sepsv, pos);
            pos += sepsv.size();
          }
        }
        p.copy_from(sv, pos); /* last copy without separator */
        luaL_pushresultsize(b, (int) totallen);
      }
      finally
      {
        b.release();
      }
    }
    return 1;
  }

  private static final lua_CFunction str_rep = Lstrlib::str_rep;

  private static int str_byte(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    int l = s.size();
    long pi = luaL_optinteger(L, 2, 1);
    long posi = posrelatI(pi, l);
    int pose = getendpos(L, 3, pi, l);
    int n, i;
    if (posi > pose)
      return 0;                                    /* empty interval; return no values */
    if (pose - posi >= Integer.MAX_VALUE) /* arithmetic overflow? */
      return luaL_errori(L, Lt.string_slice_too_long);
    n = (int) (pose - posi) + 1;
    luaL_checkstacki(L, n, Lt.string_slice_too_long);
    for (i = 0; i < n; i++)
      lua_pushinteger(L, s.byteAt((int) posi + i - 1));
    return n;
  }

  private static final lua_CFunction str_byte = Lstrlib::str_byte;

  private static int str_char(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    int i;
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      int off = luaL_buffinitsize(L, b, n);
      ByteSpan p = luaL_buffaddr(b);
      for (i = 1; i <= n; i++)
      {
        long c = luaL_checkinteger(L, i);
        luaL_argchecki(L, Long.compareUnsigned(c, 255) <= 0, i, Lt.value_out_of_range);
        p.set(off + i - 1, (byte) c);
      }
      luaL_pushresultsize(b, n);
      return 1;
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_char = Lstrlib::str_char;

  /*
  ** Buffer to store the result of 'string.dump'. It must be initialized
  ** after the call to 'lua_dump', to ensure that the function is on the
  ** top of the stack when 'lua_dump' is called. ('luaL_buffinit' might
  ** push stuff.)
  */
  private static final class str_Writer implements lua_Writer
  {
    int init; /* true iff buffer has been initialized */
    luaL_Buffer B = new luaL_Buffer();

    @Override
    public int write(lua_State L, ByteView b)
    {
      return writer(L, b, this);
    }
  }

  private static int writer(lua_State L, ByteView b, str_Writer state)
  {
    if (state.init == 0)
    {
      state.init = 1;
      luaL_buffinit(L, state.B);
    }
    if (b == null)
    {
      luaL_pushresult(state.B); /* push result */
      lua_replace(L, 1);        /* move it to reserved slot */
    }
    else
      luaL_addlstring(state.B, b, 0, b.size());
    return 0;
  }

  private static int str_dump(lua_State L)
  {
    str_Writer state = new str_Writer();
    try
    {
      int strip = lua_toboolean(L, 2);
      luaL_argchecki(L, lua_type(L, 1) == LUA_TFUNCTION && lua_iscfunction(L, 1) == 0, 1, Lt.Lua_function_expected);
      /* ensure function is on the top of the stack and vacate slot 1 */
      lua_pushvalue(L, 1);
      state.init = 0;
      lua_dump(L, state, strip);
      lua_settop(L, 1); /* leave final result on top */
      return 1;
    }
    finally
    {
      state.B.release();
    }
  }

  private static final lua_CFunction str_dump = Lstrlib::str_dump;

  /*
  ** {======================================================
  ** METAMETHODS
  ** =======================================================
  */

  private static int tonum(lua_State L, int arg)
  {
    if (lua_type(L, arg) == LUA_TNUMBER)
    { /* already a number? */
      lua_pushvalue(L, arg);
      return 1;
    }
    else
    { /* check whether it is a numerical string */
      InternalString s = lua_toistring(L, arg);
      return (s != null && lua_stringtonumber(L, new HostString(s.view())) == s.size() + 1) ? 1 : 0;
    }
  }

  /*
  ** To be here, either the first operand was a string or the first
  ** operand didn't have a corresponding metamethod. (Otherwise, that
  ** other metamethod would have been called.) So, if this metamethod
  ** doesn't work, the only other option would be for the second
  ** operand to have a different metamethod.
  */
  /* (the other operand's metamethod returned) */
  private static int arithk(lua_State L, int status, long ctx)
  {
    return 1;
  }

  private static final lua_KFunction arithk = Lstrlib::arithk;

  private static final KNamer knamer_arithk = new KNamer("string.arithk", arithk);

  private static void trymt(lua_State L, ByteView mtkey, ByteView opname)
  {
    lua_settop(L, 2); /* back to the original arguments */
    if (lua_type(L, 2) == LUA_TSTRING ||
        luaL_getmetafield(L, 2, new HostString(mtkey)) == 0)
    {
      ByteView t1 = lua_typenamei(L, lua_type(L, -2));
      ByteView t2 = lua_typenamei(L, lua_type(L, -1));
      lua_pushlstring(L, opname, 0, opname.size());
      lua_pushlstring(L, t1, 0, t1.size());
      lua_pushlstring(L, t2, 0, t2.size());
      luaL_errori(L, Lt.attempt_to_s_a_s_with_a_s);
    }
    lua_insert(L, -3); /* put metamethod before arguments */
    luaE_freezeonly(L);
    lua_callk(L, 2, 1, 0, arithk); /* call metamethod */
  }

  private static int arith(lua_State L, int op, ByteView mtname)
  {
    if (tonum(L, 1) != 0 && tonum(L, 2) != 0)
      lua_arith(L, op); /* result will be on the top */
    else
    {
      ByteView opname = mtname.substr(2);
      trymt(L, mtname, opname);
    }
    return 1;
  }

  private static int arith_add(lua_State L)
  {
    return arith(L, LUA_OPADD, Lt.mm_add);
  }

  private static final lua_CFunction arith_add = Lstrlib::arith_add;

  private static int arith_sub(lua_State L)
  {
    return arith(L, LUA_OPSUB, Lt.mm_sub);
  }

  private static final lua_CFunction arith_sub = Lstrlib::arith_sub;

  private static int arith_mul(lua_State L)
  {
    return arith(L, LUA_OPMUL, Lt.mm_mul);
  }

  private static final lua_CFunction arith_mul = Lstrlib::arith_mul;

  private static int arith_mod(lua_State L)
  {
    return arith(L, LUA_OPMOD, Lt.mm_mod);
  }

  private static final lua_CFunction arith_mod = Lstrlib::arith_mod;

  private static int arith_pow(lua_State L)
  {
    return arith(L, LUA_OPPOW, Lt.mm_pow);
  }

  private static final lua_CFunction arith_pow = Lstrlib::arith_pow;

  private static int arith_div(lua_State L)
  {
    return arith(L, LUA_OPDIV, Lt.mm_div);
  }

  private static final lua_CFunction arith_div = Lstrlib::arith_div;

  private static int arith_idiv(lua_State L)
  {
    return arith(L, LUA_OPIDIV, Lt.mm_idiv);
  }

  private static final lua_CFunction arith_idiv = Lstrlib::arith_idiv;

  private static int arith_unm(lua_State L)
  {
    return arith(L, LUA_OPUNM, Lt.mm_unm);
  }

  private static final lua_CFunction arith_unm = Lstrlib::arith_unm;

  private static final CFuncInfo cfi_arith_add = new CFuncInfo(arith_add, "string.arith_add");
  private static final CFuncInfo cfi_arith_sub = new CFuncInfo(arith_sub, "string.arith_sub");
  private static final CFuncInfo cfi_arith_mul = new CFuncInfo(arith_mul, "string.arith_mul");
  private static final CFuncInfo cfi_arith_mod = new CFuncInfo(arith_mod, "string.arith_mod");
  private static final CFuncInfo cfi_arith_pow = new CFuncInfo(arith_pow, "string.arith_pow");
  private static final CFuncInfo cfi_arith_div = new CFuncInfo(arith_div, "string.arith_div");
  private static final CFuncInfo cfi_arith_idiv = new CFuncInfo(arith_idiv, "string.arith_idiv");
  private static final CFuncInfo cfi_arith_unm = new CFuncInfo(arith_unm, "string.arith_unm");

  private static final luaL_RegCFI[] stringmetamethods = {
    new luaL_RegCFI(Lt.mm_add, cfi_arith_add),
    new luaL_RegCFI(Lt.mm_sub, cfi_arith_sub),
    new luaL_RegCFI(Lt.mm_mul, cfi_arith_mul),
    new luaL_RegCFI(Lt.mm_mod, cfi_arith_mod),
    new luaL_RegCFI(Lt.mm_pow, cfi_arith_pow),
    new luaL_RegCFI(Lt.mm_div, cfi_arith_div),
    new luaL_RegCFI(Lt.mm_idiv, cfi_arith_idiv),
    new luaL_RegCFI(Lt.mm_unm, cfi_arith_unm),
    new luaL_RegCFI(Lt.mm_index, null),
  }; /* placeholder */

  /* }====================================================== */

  /*
  ** {======================================================
  ** PATTERN MATCHING
  ** =======================================================
  */

  private static final int CAP_UNFINISHED = -1;
  private static final int CAP_POSITION = -2;

  private static final int NOMATCH = -1;

  private static final class MatchState
  {
    InternalString src;
    InternalString p;
    int src_end;
    int p_end;
    lua_State L;
    int matchdepth; /* control for recursive depth (to avoid C stack overflow) */
    int level;      /* total number of captures (finished or unfinished) */
    boolean budgeted;  /* is there a budget (the calls of 'match' are its steps)? */
    boolean goingon;   /* is the next call of 'match' one going on ('goon')? */
    static final class CaptureSlot
    {
      int init;
      int len; /* length or special value (CAP_*) */
    }
    final CaptureSlot[] capture = new CaptureSlot[LUA_MAXCAPTURES];
    /* where a check point keeps the match ('match_checkpoint') */
    int boxidx;            /* the stack index of its match box */
    MatchBox box;          /* the box, once made */
    MatchBox rec;          /* the box a freeze records the frames in, or nullptr */
    int nrec;              /* the frames left to record */
    lua_KFunction k;       /* the caller's continuation */
    long ctx;
    int origin;            /* where the caller's match started */
    luaL_Buffer b;         /* 'gsub': its buffer, kept in a box below the match box... */
    luaL_Box bbox;         /* ...this one, once made */
    final long[] loop = new long[3]; /* 'gsub': its last match, count and change flag */

    MatchState()
    {
      for (int i = 0; i < LUA_MAXCAPTURES; i++)
        capture[i] = new CaptureSlot();
    }
  }

  /* recursive function */

  private static int srcbyte(MatchState ms, int i)
  {
    return (i < ms.src_end) ? ms.src.byteAt(i) : 0;
  }

  private static int patbyte(MatchState ms, int i)
  {
    return (i < ms.p_end) ? ms.p.byteAt(i) : 0;
  }

  /* maximum recursion depth for 'match' */
  private static final int MAXCCALLS = 200;

  /*
  ** A match goes through check points: each call of 'match' is a step of the
  ** budget, and where the budget is spent the callback runs and may freeze.
  ** The match then stops where it is, kept in a match box (a user object at
  ** 'boxidx', made at the first check point): the position the call of
  ** 'match' began with, the captures, and a frame for each call of 'match'
  ** waiting for the one it made (outermost first), recorded as the freeze
  ** leaves them. After the thaw, the caller's continuation makes those calls
  ** again from their frames ('match_resume') and the match goes on as if
  ** nothing had happened.
  */
  private static final byte MF_START = 1; /* 'start_capture': undoes its capture if the match fails */
  private static final byte MF_END = 2;   /* 'end_capture' of capture 'a': undoes its close */
  private static final byte MF_OPT = 3;   /* '?' at 'a' (subject), 'b' (its class end): goes on without it */
  private static final byte MF_MAX = 4;   /* 'max_expand' at 'a', 'b' with 'c' repetitions: tries fewer */
  private static final byte MF_MIN = 5;   /* 'min_expand' at 'a', class 'b' to 'c': tries one more */
  private static final int MF_SIZE = 1 + 3 * Long.BYTES;   /* kind, a, b, c */
  private static final int MB_CAPSIZE = 2 * Long.BYTES; /* init, len */
  private static final int MB_BYTES = LUA_MAXCAPTURES * MB_CAPSIZE + MAXCCALLS * MF_SIZE;
  private static final int MB_S = 0;       /* the position the stopped call of 'match' began with... */
  private static final int MB_P = 1;       /* ...in the subject and in the pattern */
  private static final int MB_LEVEL = 2;   /* the captures (as many) */
  private static final int MB_NFRAMES = 3; /* the calls waiting */
  private static final int MB_ORIGIN = 4;  /* where the caller's match started */
  private static final int MB_NINTS = 5;
  private static final int MATCHBOX_SIZE = 6800; /* (its accounted size) */

  private static final class MatchBox implements lua_UserObject
  {
    final long[] ints = new long[MB_NINTS];
    final ByteOwner data = new ByteOwner(MB_BYTES); /* the captures, then the frames */

    int used()
    {
      return (int) ints[MB_LEVEL] * MB_CAPSIZE + (int) ints[MB_NFRAMES] * MF_SIZE;
    }

    @Override
    public String persistkind()
    {
      return "string.match";
    }

    @Override
    public int persistints(long[] out)
    {
      for (int i = 0; i < MB_NINTS; i++)
        out[i] = ints[i];
      return MB_NINTS;
    }

    @Override
    public ByteView persistbytes()
    {
      return data.view().substr(0, used());
    }
  }

  /* a match box from its state, checked to be one (not yet against its subject) */
  private static lua_UserObject makematchbox(lua_State L, long[] ints, int nints,
                                             ByteView bytes)
  {
    if (nints != MB_NINTS || ints[MB_LEVEL] < 0 || ints[MB_LEVEL] > LUA_MAXCAPTURES ||
        ints[MB_NFRAMES] < 0 || ints[MB_NFRAMES] >= MAXCCALLS)
      return null;
    MatchBox mb = new MatchBox();
    for (int i = 0; i < MB_NINTS; i++)
      mb.ints[i] = ints[i];
    if (bytes.size() != mb.used())
      return null;
    mb.data.copy_from(bytes, 0);
    return mb;
  }

  private static final lua_ObjectFactory makematchbox = Lstrlib::makematchbox;

  private static final class MatchBoxKind
  {
    MatchBoxKind()
    {
      lua_registerobjectkind("string.match", makematchbox);
    }
  }

  private static final MatchBoxKind matchboxkind = new MatchBoxKind();

  /* records the frame of a call of 'match' a freeze leaves (see 'match_checkpoint') */
  private static void record(MatchState ms, byte kind, int a, int b, int c)
  {
    MatchBox mb = ms.rec;
    if (mb == null || ms.nrec == 0)
      return; /* (not a freeze of a check point) */
    ms.nrec--;
    int d = (int) mb.ints[MB_LEVEL] * MB_CAPSIZE + ms.nrec * MF_SIZE;
    mb.data.set(d, kind);
    mb.data.write_int64(a, d + 1);
    mb.data.write_int64(b, d + 1 + Long.BYTES);
    mb.data.write_int64(c, d + 1 + 2 * Long.BYTES);
  }

  /*
  ** A check point at the start of a call of 'match' at 's', 'p': the budget is
  ** spent. The match box (and, for 'gsub', the box of its buffer below it)
  ** is made at its index the first time. The callback runs; if it freezes,
  ** the calls of 'match' record their frames as the freeze leaves them.
  */
  private static void match_checkpoint(MatchState ms, int s, int p)
  {
    lua_State L = ms.L;
    if (ms.b != null && ms.bbox == null)
    { /* (the caller's arguments stay below the boxes) */
      lua_settop(L, ms.boxidx - 2);
      ms.bbox = luaL_newbox(L);
    }
    if (ms.box == null)
    {
      lua_settop(L, ms.boxidx - 1);
      MatchBox o = new MatchBox();
      MatchBox nb = o;
      lua_newuserobject(L, o, MATCHBOX_SIZE, 0);
      ms.box = nb;
    }
    MatchBox mb = ms.box;
    luaL_Box bb = (ms.b != null) ? ms.bbox : null;
    int nframes = MAXCCALLS - 1 - ms.matchdepth;
    mb.ints[MB_S] = s;
    mb.ints[MB_P] = p;
    mb.ints[MB_LEVEL] = ms.level;
    mb.ints[MB_NFRAMES] = nframes;
    mb.ints[MB_ORIGIN] = ms.origin;
    for (int i = 0; i < ms.level; i++)
    {
      mb.data.write_int64(ms.capture[i].init, i * MB_CAPSIZE);
      mb.data.write_int64(ms.capture[i].len, i * MB_CAPSIZE + Long.BYTES);
    }
    if (bb != null)
    {
      bb.ints[0] = ms.origin;
      for (int i = 0; i < 3; i++)
        bb.ints[i + 1] = ms.loop[i];
      luaL_buffsave(ms.b, bb);
    }
    ms.rec = mb;
    ms.nrec = nframes;
    lua_checkpointk(L, ms.ctx, ms.k);
    ms.rec = null; /* (no freeze) */
    if (bb != null)
      luaL_buffload(ms.b, bb);
  }

  private static final byte L_ESC = '%';

  private static int check_capture(MatchState ms, int l)
  {
    l -= '1';
    if (l < 0 || l >= ms.level ||
        ms.capture[l].len == CAP_UNFINISHED)
    {
      lua_pushinteger(ms.L, l + 1);
      return luaL_errori(ms.L, Lt.invalid_capture_index_d);
    }
    return l;
  }

  private static int capture_to_close(MatchState ms)
  {
    int level = ms.level;
    for (level--; level >= 0; level--)
      if (ms.capture[level].len == CAP_UNFINISHED)
        return level;
    return luaL_errori(ms.L, Lt.invalid_pattern_capture);
  }

  private static int classend(MatchState ms, int p)
  {
    switch (patbyte(ms, p++))
    {
    case L_ESC:
    {
      if (p == ms.p_end)
        luaL_errori(ms.L, Lt.malformed_pattern_ends_with);
      return p + 1;
    }
    case '[':
    {
      if (patbyte(ms, p) == '^')
        p++;
      do
      { /* look for a ']' */
        if (p == ms.p_end)
          luaL_errori(ms.L, Lt.malformed_pattern_missing);
        if (patbyte(ms, p++) == L_ESC && p < ms.p_end)
          p++; /* skip escapes (e.g. '%]') */
      } while (patbyte(ms, p) != ']');
      return p + 1;
    }
    default:
    {
      return p;
    }
    }
  }

  private static int match_class(int c, int cl)
  {
    int res;
    int lower = locale_tolower(cl);
    if (lower == 'z')
      res = (c == 0) ? 1 : 0; /* deprecated option */
    else
      res = locale_matchclass(c, lower);
    if (res == 0 && lower == cl)
      return (cl == c) ? 1 : 0; /* not a known class and lowercase — literal match */
    return (locale_islower(cl) != 0 ? res : (res == 0 ? 1 : 0));
  }

  private static int matchbracketclass(MatchState ms, int c, int p,
                                       int ec)
  {
    int sig = 1;
    if (patbyte(ms, p + 1) == '^')
    {
      sig = 0;
      p++; /* skip the '^' */
    }
    while (++p < ec)
    {
      if (patbyte(ms, p) == L_ESC)
      {
        p++;
        if (match_class(c, patbyte(ms, p)) != 0)
          return sig;
      }
      else if ((patbyte(ms, p + 1) == '-') && (p + 2 < ec))
      {
        p += 2;
        if (patbyte(ms, p - 2) <= c && c <= patbyte(ms, p))
          return sig;
      }
      else if (patbyte(ms, p) == c)
        return sig;
    }
    return (sig == 0) ? 1 : 0;
  }

  private static int singlematch(MatchState ms, int s, int p,
                                 int ep)
  {
    if (s >= ms.src_end)
      return 0;
    else
    {
      int c = srcbyte(ms, s);
      switch (patbyte(ms, p))
      {
      case '.':
        return 1; /* matches any char */
      case L_ESC:
        return match_class(c, patbyte(ms, p + 1));
      case '[':
        return matchbracketclass(ms, c, p, ep - 1);
      default:
        return (patbyte(ms, p) == c) ? 1 : 0;
      }
    }
  }

  private static int matchbalance(MatchState ms, int s,
                                  int p)
  {
    if (p >= ms.p_end - 1)
      luaL_errori(ms.L, Lt.malformed_pattern_missing_arguments_to_b);
    if (srcbyte(ms, s) != patbyte(ms, p))
      return NOMATCH;
    else
    {
      int b = patbyte(ms, p);
      int e = patbyte(ms, p + 1);
      int cont = 1;
      while (++s < ms.src_end)
      {
        if (srcbyte(ms, s) == e)
        {
          if (--cont == 0)
            return s + 1;
        }
        else if (srcbyte(ms, s) == b)
          cont++;
      }
    }
    return NOMATCH; /* string ends out of balance */
  }

  /* keeps trying to match with 'i' repetitions, then fewer */
  private static int max_expand_from(MatchState ms, int s, int ep, int i)
  {
    while (i >= 0)
    {
      int res;
      try
      {
        res = match(ms, s + i, ep + 1);
      }
      catch (Throwable e)
      {
        record(ms, MF_MAX, s, ep, i);
        throw e;
      }
      if (res != NOMATCH)
        return res;
      i--; /* else didn't match; reduce 1 repetition to try again */
    }
    return NOMATCH;
  }

  private static int max_expand(MatchState ms, int s,
                                int p, int ep)
  {
    int i = 0; /* counts maximum expand for item */
    while (singlematch(ms, s + i, p, ep) != 0)
      i++;
    /* keeps trying to match with the maximum repetitions */
    return max_expand_from(ms, s, ep, i);
  }

  private static int min_expand(MatchState ms, int s,
                                int p, int ep)
  {
    for (;;)
    {
      int res;
      try
      {
        res = match(ms, s, ep + 1);
      }
      catch (Throwable e)
      {
        record(ms, MF_MIN, s, p, ep);
        throw e;
      }
      if (res != NOMATCH)
        return res;
      else if (singlematch(ms, s, p, ep) != 0)
        s++; /* try with one more repetition */
      else
        return NOMATCH;
    }
  }

  private static int start_capture(MatchState ms, int s,
                                   int p, int what)
  {
    int res;
    int level = ms.level;
    if (level >= LUA_MAXCAPTURES)
      luaL_errori(ms.L, Lt.too_many_captures);
    ms.capture[level].init = s;
    ms.capture[level].len = what;
    ms.level = level + 1;
    try
    {
      res = match(ms, s, p);
    }
    catch (Throwable e)
    {
      record(ms, MF_START, 0, 0, 0);
      throw e;
    }
    if (res == NOMATCH) /* match failed? */
      ms.level--;       /* undo capture */
    return res;
  }

  private static int end_capture(MatchState ms, int s,
                                 int p)
  {
    int l = capture_to_close(ms);
    int res;
    ms.capture[l].len = s - ms.capture[l].init; /* close capture */
    try
    {
      res = match(ms, s, p);
    }
    catch (Throwable e)
    {
      record(ms, MF_END, l, 0, 0);
      throw e;
    }
    if (res == NOMATCH)                    /* match failed? */
      ms.capture[l].len = CAP_UNFINISHED; /* undo capture */
    return res;
  }

  private static int match_capture(MatchState ms, int s, int l)
  {
    l = check_capture(ms, l);
    int len = ms.capture[l].len;
    ByteView msv = ms.src.view();
    if ((ms.src_end - s) >= len &&
        msv.substr(ms.capture[l].init, len).equals(msv.substr(s, len)))
      return s + len;
    else
      return NOMATCH;
  }

  private static int match(MatchState ms, int s, int p)
  {
    if (ms.matchdepth-- == 0)
      luaL_errori(ms.L, Lt.pattern_too_complex);
    if (ms.budgeted)
    {
      if (ms.goingon)
        ms.goingon = false; /* (a call going on after a freeze: see 'goon') */
      else if (luaE_spent(ms.L, 1))
        match_checkpoint(ms, s, p);
    }
    for (;;)
    {
      if (p == ms.p_end) /* end of pattern? */
        break;
      if (patbyte(ms, p) == '(')
      {                                /* start capture */
        if (patbyte(ms, p + 1) == ')') /* position capture? */
          s = start_capture(ms, s, p + 2, CAP_POSITION);
        else
          s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
        break;
      }
      else if (patbyte(ms, p) == ')')
      { /* end capture */
        s = end_capture(ms, s, p + 1);
        break;
      }
      else if (patbyte(ms, p) == '$' && (p + 1) == ms.p_end)
      {
        s = (s == ms.src_end) ? s : NOMATCH; /* check end of string */
        break;
      }
      else if (patbyte(ms, p) == L_ESC && patbyte(ms, p + 1) == 'b')
      { /* balanced string? */
        s = matchbalance(ms, s, p + 2);
        if (s == NOMATCH) /* fail */
          break;
        p += 4;
        continue; /* return match(ms, s, p + 4); */
      }
      else if (patbyte(ms, p) == L_ESC && patbyte(ms, p + 1) == 'f')
      { /* frontier? */
        int ep;
        int previous;
        p += 2;
        if (patbyte(ms, p) != '[')
          luaL_errori(ms.L, Lt.missing_after_f_in_pattern);
        ep = classend(ms, p); /* points to what is next */
        previous = (s == 0) ? '\0' : srcbyte(ms, s - 1);
        if (matchbracketclass(ms, previous, p, ep - 1) == 0 &&
            matchbracketclass(ms, srcbyte(ms, s), p, ep - 1) != 0)
        {
          p = ep;
          continue; /* return match(ms, s, ep); */
        }
        s = NOMATCH; /* match failed */
        break;
      }
      else if (patbyte(ms, p) == L_ESC && '0' <= patbyte(ms, p + 1) &&
               patbyte(ms, p + 1) <= '9')
      { /* capture results (%0-%9)? */
        s = match_capture(ms, s, patbyte(ms, p + 1));
        if (s == NOMATCH)
          break;
        p += 2;
        continue; /* return match(ms, s, p + 2) */
      }
      else
      {                                  /* pattern class plus optional suffix */
        int ep = classend(ms, p);        /* points to optional suffix */
        /* does not match at least once? */
        if (singlematch(ms, s, p, ep) == 0)
        {
          int sfx = patbyte(ms, ep);
          if (sfx == '*' || sfx == '?' || sfx == '-')
          { /* accept empty? */
            p = ep + 1;
            continue; /* return match(ms, s, ep + 1); */
          }
          else           /* '+' or no suffix */
            s = NOMATCH; /* fail */
        }
        else
        { /* matched once */
          switch (patbyte(ms, ep))
          { /* handle optional suffix */
          case '?':
          { /* optional */
            int res;
            try
            {
              res = match(ms, s + 1, ep + 1);
            }
            catch (Throwable e)
            {
              record(ms, MF_OPT, s, ep, 0);
              throw e;
            }
            if (res != NOMATCH)
              s = res;
            else
            {
              p = ep + 1;
              continue; /* else return match(ms, s, ep + 1); */
            }
            break;
          }
          case '+': /* 1 or more repetitions */
            s++;    /* 1 match already done */
            /* FALLTHROUGH */
          case '*': /* 0 or more repetitions */
            s = max_expand(ms, s, p, ep);
            break;
          case '-': /* 0 or more repetitions (minimum) */
            s = min_expand(ms, s, p, ep);
            break;
          default: /* no suffix */
            s++;
            p = ep;
            continue; /* return match(ms, s + 1, ep); */
          }
        }
        break;
      }
    }
    ms.matchdepth++;
    return s;
  }

  /*
  ** A call of 'match' goes on from 's', 'p' after its start (its check point
  ** behind it): 'match' runs again, as that same call (at its depth, with no
  ** step taken).
  */
  private static int goon(MatchState ms, int s, int p)
  {
    ms.matchdepth++; /* ('match' takes it again) */
    ms.goingon = true;
    s = match(ms, s, p);
    ms.matchdepth--;
    return s;
  }

  /*
  ** A match stopped at a check point goes on (see 'match_checkpoint'): the
  ** call of 'match' of frame 'k' gets the result of the one it made (frame
  ** 'k + 1', or the stopped one, which goes on from where it began) and goes
  ** on as it would have. Returns the result of that call.
  */
  private static int resume_call(MatchState ms, ByteView frames, int k, int n,
                                 int s, int p)
  {
    if (k == n)
    { /* the stopped call: its check point is behind it */
      s = goon(ms, s, p);
      ms.matchdepth++;
      return s;
    }
    int d = k * MF_SIZE;
    byte kind = frames.at(d);
    int a = (int) frames.toInt64(d + 1);
    int b = (int) frames.toInt64(d + 1 + Long.BYTES);
    int c = (int) frames.toInt64(d + 1 + 2 * Long.BYTES);
    int res;
    try
    {
      res = resume_call(ms, frames, k + 1, n, s, p);
    }
    catch (Throwable e)
    {
      record(ms, kind, a, b, c);
      throw e;
    }
    switch (kind)
    {
    case MF_START:
      if (res == NOMATCH)
        ms.level--; /* undo capture */
      break;
    case MF_END:
      if (res == NOMATCH)
        ms.capture[a].len = CAP_UNFINISHED; /* undo capture */
      break;
    case MF_OPT:
      if (res == NOMATCH)
        res = goon(ms, a, b + 1); /* without the optional item */
      break;
    case MF_MAX:
      if (res == NOMATCH)
        res = max_expand_from(ms, a, b, c - 1);
      break;
    default: /* MF_MIN */
      if (res == NOMATCH)
        res = singlematch(ms, a, b, c) != 0 ? min_expand(ms, a + 1, b, c) : NOMATCH;
      break;
    }
    ms.matchdepth++;
    return res;
  }

  /*
  ** Checks the stopped match in box 'mb' against the subject and pattern of
  ** 'ms' (a box made from outside the process is not trusted to fit them).
  */
  private static boolean match_fits(MatchState ms, MatchBox mb)
  {
    long[] v = mb.ints;
    int level = (int) v[MB_LEVEL];
    int n = (int) v[MB_NFRAMES];
    if (v[MB_S] < 0 || v[MB_S] > ms.src_end || v[MB_P] < 0 || v[MB_P] > ms.p_end ||
        v[MB_ORIGIN] < 0 || v[MB_ORIGIN] > ms.src_end)
      return false;
    for (int i = 0; i < level; i++)
    {
      long init = mb.data.view().toInt64(i * MB_CAPSIZE);
      long len = mb.data.view().toInt64(i * MB_CAPSIZE + Long.BYTES);
      if (init < 0 || init > ms.src_end ||
          (len != CAP_UNFINISHED && len != CAP_POSITION && (len < 0 || len > ms.src_end - init)))
        return false;
    }
    int starts = 0;
    ByteView frames = mb.data.view().substr(level * MB_CAPSIZE);
    for (int k = 0; k < n; k++)
    {
      int d = k * MF_SIZE;
      long a = frames.toInt64(d + 1);
      long b = frames.toInt64(d + 1 + Long.BYTES);
      long c = frames.toInt64(d + 1 + 2 * Long.BYTES);
      switch (frames.at(d))
      {
      case MF_START:
        starts++;
        break;
      case MF_END:
        if (a < 0 || a >= level)
          return false;
        break;
      case MF_OPT:
        if (a < 0 || a >= ms.src_end || b < 0 || b >= ms.p_end)
          return false;
        break;
      case MF_MAX:
        if (a < 0 || a > ms.src_end || b < 0 || b >= ms.p_end || c < 0 || c > ms.src_end - a)
          return false;
        break;
      case MF_MIN:
        if (a < 0 || a > ms.src_end || b < 0 || c <= b || c >= ms.p_end)
          return false;
        break;
      default:
        return false;
      }
    }
    return starts <= level;
  }

  /*
  ** The match in the box at 'ms->boxidx', stopped by a freeze, goes on:
  ** returns what the caller's call of 'match' returns.
  */
  private static int match_resume(MatchState ms)
  {
    MatchBox mb = (lua_touserobject(ms.L, ms.boxidx) instanceof MatchBox m) ? m : null;
    if (mb == null || !match_fits(ms, mb))
      luaL_errori(ms.L, Lt.invalid_match_state);
    ms.box = mb;
    ms.budgeted = true; /* (it was) */
    ms.goingon = false;
    ms.origin = (int) mb.ints[MB_ORIGIN];
    ms.level = (int) mb.ints[MB_LEVEL];
    for (int i = 0; i < ms.level; i++)
    {
      ms.capture[i].init = (int) mb.data.view().toInt64(i * MB_CAPSIZE);
      ms.capture[i].len = (int) mb.data.view().toInt64(i * MB_CAPSIZE + Long.BYTES);
    }
    int n = (int) mb.ints[MB_NFRAMES];
    ms.matchdepth = MAXCCALLS - 1 - n;
    /* (each call reads its frame before the next is made, so all are read
       before a check point can write the box again) */
    ByteView frames = mb.data.view().substr(ms.level * MB_CAPSIZE);
    return resume_call(ms, frames, 0, n, (int) mb.ints[MB_S], (int) mb.ints[MB_P]);
  }

  /*
  ** The plain search of 'lmemfind' from 'p' on, with 'l1' places left for
  ** the first character of 's2' ('l2' the length of the rest). Each place it
  ** compares at is a step of the budget, and where the budget is spent a
  ** check point of 'find' (the place kept in a box at 5, made there then).
  */
  private static int lmemfind_from(lua_State L, ByteView s1, int p, int l1,
                                   ByteView s2, int l2, luaL_Box box)
  {
    while (l1 > 0)
    {
      int init = s1.find(s2.at(0), p);
      if (init == ByteView.npos || init >= p + l1)
        break;
      init++; /* 1st char is already checked */
      if (s1.substr(init, l2).equals(s2.substr(1, l2)))
        return init - 1;
      else
      {
        l1 -= init - p;
        p = init;
      }
      if (luaE_spent(L, 1))
      {
        if (box == null)
        {
          lua_settop(L, 4); /* (the arguments stay below it) */
          box = luaL_newbox(L);
        }
        box.ints[0] = (long) p;
        box.ints[1] = (long) l1;
        lua_checkpointk(L, 2, str_findk);
      }
    }
    return ByteView.npos; /* not found */
  }

  private static int lmemfind(lua_State L, ByteView s1, int off1, int l1,
                              ByteView s2, int l2)
  {
    if (l2 == 0)
      return off1; /* empty strings are everywhere */
    else if (l2 > l1)
      return ByteView.npos; /* avoids a negative 'l1' */
    else
    {
      l2--;
      l1 = l1 - l2; /* 's2' cannot be found after that */
      return lmemfind_from(L, s1, off1, l1, s2, l2, null);
    }
  }

  private static final class Capture
  {
    int init;
    int len;

    Capture(int init, int len)
    {
      this.init = init;
      this.len = len;
    }
  }

  private static Capture get_onecapture(MatchState ms, int i, int s,
                                        int e)
  {
    if (i >= ms.level)
    {
      if (i != 0)
      {
        lua_pushinteger(ms.L, i + 1);
        luaL_errori(ms.L, Lt.invalid_capture_index_d);
      }
      return new Capture(s, (e - s));
    }
    else
    {
      int capl = ms.capture[i].len;
      if (capl == CAP_UNFINISHED)
        luaL_errori(ms.L, Lt.unfinished_capture);
      else if (capl == CAP_POSITION)
        lua_pushinteger(ms.L, (long) ms.capture[i].init + 1);
      return new Capture(ms.capture[i].init, capl);
    }
  }

  /*
  ** Push the i-th capture on the stack.
  */
  private static void push_onecapture(MatchState ms, int i, int s,
                                      int e)
  {
    Capture cap = get_onecapture(ms, i, s, e);
    if (cap.len != CAP_POSITION)
      lua_pushlstring(ms.L, ms.src.view(), cap.init, cap.len);
    /* else position was already pushed */
  }

  private static int push_captures(MatchState ms, int s, int e)
  {
    int i;
    int nlevels = (ms.level == 0 && s != NOMATCH) ? 1 : ms.level;
    luaL_checkstacki(ms.L, nlevels, Lt.too_many_captures);
    for (i = 0; i < nlevels; i++)
      push_onecapture(ms, i, s, e);
    return nlevels; /* number of strings pushed */
  }

  /* check whether pattern has no special characters */
  private static int nospecials(ByteView p)
  {
    return (p.find_first_of(Lt.pattern_specials) == ByteView.npos) ? 1 : 0;
  }

  private static void prepstate(MatchState ms, lua_State L,
                                InternalString s, InternalString p)
  {
    ms.L = L;
    ms.src = s;
    ms.src_end = s.size();
    ms.p = p;
    ms.p_end = p.size();
    ms.box = null;
    ms.rec = null;
    ms.b = null;
    ms.bbox = null;
  }

  /* where the check points of a match keep it, and how its caller goes on */
  private static void prepcheck(MatchState ms, int boxidx, lua_KFunction k, long ctx)
  {
    ms.boxidx = boxidx;
    ms.k = k;
    ms.ctx = ctx;
  }

  /*
  ** (Re)prepare state for a match, setting fields that change during
  ** each match.
  */
  private static void reprepstate(MatchState ms)
  {
    ms.matchdepth = MAXCCALLS;
    ms.level = 0;
    /* (no Lua code runs in a match, but at its check points: a match with
       no budget gets none) */
    ms.budgeted = (ms.L.hookmask & LUA_MASKBUDGET) != 0;
    ms.goingon = false;
  }

  private static int str_find_aux(lua_State L, int find)
  {
    InternalString s = luaL_checkistring(L, 1);
    InternalString p = luaL_checkistring(L, 2);
    int ls = s.size();
    int lp = p.size();
    long init = posrelatI(luaL_optinteger(L, 3, 1), ls) - 1;
    if (init > ls)
    {                   /* start after string's end? */
      luaL_pushfail(L); /* cannot find anything */
      return 1;
    }
    /* explicit request or no special characters? */
    if (find != 0 && (lua_toboolean(L, 4) != 0 || nospecials(p.view()) != 0))
    {
      /* do a plain search */
      int pos = lmemfind(L, s.view(), (int) init, ls - (int) init, p.view(), lp);
      if (pos != ByteView.npos)
      {
        lua_pushinteger(L, (long) pos + 1);
        lua_pushinteger(L, pos + lp);
        return 2;
      }
    }
    else
    {
      MatchState ms = new MatchState();
      prepstate(ms, L, s, p);
      prepcheck(ms, 5, str_findk, find); /* (a match box at 5, after the arguments) */
      return find_loop(L, ms, find, (int) init, NOMATCH, false);
    }
    luaL_pushfail(L); /* not found */
    return 1;
  }

  /*
  ** The matches of 'find' and 'match' from 's1' on ('first' when the match
  ** from 's1' is already 'res').
  */
  private static int find_loop(lua_State L, MatchState ms, int find, int s1, int res, boolean first)
  {
    int anchor = (ms.p_end > 0 && ms.p.byteAt(0) == '^') ? 1 : 0;
    int pinit = (anchor != 0) ? 1 : 0; /* skip anchor character */
    do
    {
      if (!first)
      {
        reprepstate(ms);
        ms.origin = s1;
        res = match(ms, s1, pinit);
      }
      first = false;
      if (res != NOMATCH)
      {
        if (find != 0)
        {
          lua_pushinteger(L, (long) s1 + 1); /* start */
          lua_pushinteger(L, res);           /* end */
          return push_captures(ms, NOMATCH, 0) + 2;
        }
        else
          return push_captures(ms, s1, res);
      }
    } while (s1++ < ms.src_end && anchor == 0);
    luaL_pushfail(L); /* not found */
    return 1;
  }

  /*
  ** 'find' and 'match' go on after a freeze in a match (context: 'find'), or
  ** 'find' after a freeze in a plain search (context 2, its place in a box
  ** at 5)
  */
  private static int str_findk(lua_State L, int status, long ctx)
  {
    if (ctx == 2)
    {
      InternalString s = luaL_checkistring(L, 1);
      InternalString p = luaL_checkistring(L, 2);
      luaL_Box box = (lua_touserobject(L, 5) instanceof luaL_Box x) ? x : null;
      int ls = s.size();
      int l2 = p.size() - 1;
      if (box == null || p.size() == 0 || box.ints[0] < 0 || box.ints[1] < 0 ||
          l_intadd(box.ints[0], box.ints[1]) != (long) (ls - l2))
        luaL_errori(L, Lt.invalid_match_state);
      int pos = lmemfind_from(L, s.view(), (int) box.ints[0],
                              (int) box.ints[1], p.view(), l2, box);
      if (pos == ByteView.npos)
      {
        luaL_pushfail(L); /* not found */
        return 1;
      }
      lua_pushinteger(L, (long) pos + 1);
      lua_pushinteger(L, pos + p.size());
      return 2;
    }
    MatchState ms = new MatchState();
    prepstate(ms, L, luaL_checkistring(L, 1), luaL_checkistring(L, 2));
    prepcheck(ms, 5, str_findk, ctx);
    int res = match_resume(ms);
    return find_loop(L, ms, (int) ctx, ms.origin, res, true);
  }

  private static final lua_KFunction str_findk = Lstrlib::str_findk;

  private static final KNamer knamer_str_findk = new KNamer("string.str_findk", str_findk);

  private static int str_find(lua_State L)
  {
    return str_find_aux(L, 1);
  }

  private static final lua_CFunction str_find = Lstrlib::str_find;

  private static int str_match(lua_State L)
  {
    return str_find_aux(L, 0);
  }

  private static final lua_CFunction str_match = Lstrlib::str_match;

  /* state for 'gmatch' */
  private static final int GMATCHSTATE_SIZE = 584;

  /*
  ** The state of a 'gmatch' iterator is where it is; each call makes its
  ** matches with a state of its own (the budget callback, called in a match,
  ** can call the iterator again)
  */
  private static final class GMatchState implements lua_UserObject
  {
    InternalString subject; /* (kept by the closure's first two upvalues) */
    InternalString pattern;
    int src;       /* current position */
    int p;
    long lastmatch;
  }

  /* the matches of a 'gmatch' iterator from 'src' on ('first': see 'find_loop') */
  private static int gmatch_loop(GMatchState gm, MatchState ms, int src, int e, boolean first)
  {
    for (; src <= ms.src_end; src++)
    {
      if (!first)
      {
        reprepstate(ms);
        ms.origin = src;
        e = match(ms, src, gm.p);
      }
      first = false;
      if (e != NOMATCH && e != gm.lastmatch)
      {
        gm.lastmatch = e;
        gm.src = e;
        return push_captures(ms, src, e);
      }
    }
    return 0; /* not found */
  }

  private static int gmatch_aux(lua_State L)
  {
    GMatchState gm = (GMatchState) lua_touserobject(L, lua_upvalueindex(3));
    MatchState ms = new MatchState();
    prepstate(ms, L, gm.subject, gm.pattern);
    prepcheck(ms, 1, gmatchk, 0); /* (a match box at 1: its arguments are not used) */
    return gmatch_loop(gm, ms, gm.src, NOMATCH, false);
  }

  private static final lua_CFunction gmatch_aux = Lstrlib::gmatch_aux;

  /* a 'gmatch' iterator goes on after a freeze in a match */
  private static int gmatchk(lua_State L, int status, long ctx)
  {
    GMatchState gm = (GMatchState) lua_touserobject(L, lua_upvalueindex(3));
    MatchState ms = new MatchState();
    prepstate(ms, L, gm.subject, gm.pattern);
    prepcheck(ms, 1, gmatchk, 0);
    int e = match_resume(ms);
    return gmatch_loop(gm, ms, ms.origin, e, true);
  }

  private static final lua_KFunction gmatchk = Lstrlib::gmatchk;

  private static final KNamer knamer_gmatchk = new KNamer("string.gmatchk", gmatchk);

  /* the state of a gmatch iterator, kept outside the process: where it is */
  private static int gmatch_save(CClosure cl, long[] ints)
  {
    Udata u = uvalue(cl.upvalue[2]);
    GMatchState gm = (u.obj instanceof GMatchState g) ? g : null;
    if (!ttisstring(cl.upvalue[0]) || !ttisstring(cl.upvalue[1]) ||
        tsvalue(cl.upvalue[0]).data != gm.subject || tsvalue(cl.upvalue[1]).data != gm.pattern)
      return -1; /* (its strings were replaced: debug.setupvalue) */
    ints[0] = gm.src;
    ints[1] = gm.p;
    ints[2] = gm.lastmatch;
    return 3;
  }

  private static final lua_CClosureKind.Save gmatch_save = Lstrlib::gmatch_save;

  private static boolean gmatch_make(lua_State L, CClosure cl, long[] ints, int nints)
  {
    if (nints != 3 || !ttisstring(cl.upvalue[0]) || !ttisstring(cl.upvalue[1]))
      return false;
    InternalString s = tsvalue(cl.upvalue[0]).data;
    InternalString p = tsvalue(cl.upvalue[1]).data;
    if (ints[0] < 0 || ints[0] > (long) s.size() + 1 || ints[1] < 0 ||
        ints[1] > (long) p.size())
      return false;
    GMatchState o = new GMatchState();
    GMatchState gm = o;
    lua_newuserobject(L, o, GMATCHSTATE_SIZE, 0);
    gm.subject = s;
    gm.pattern = p;
    gm.src = (int) ints[0];
    gm.p = (int) ints[1];
    gm.lastmatch = ints[2];
    TValue u = getslot(L, L.top - 1);
    copyobj(cl.upvalue[2], u);
    luaC_barrier(L, cl, cl.upvalue[2]);
    lua_pop(L, 1);
    return true;
  }

  private static final lua_CClosureKind.Make gmatch_make = Lstrlib::gmatch_make;

  private static final CClosureNamer gmatch_kind = new CClosureNamer(new lua_CClosureKind("string.gmatch_aux", gmatch_aux, 3, gmatch_save, gmatch_make));

  private static int gmatch(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    InternalString p = luaL_checkistring(L, 2);
    int ls = s.size();
    long init = posrelatI(luaL_optinteger(L, 3, 1), ls) - 1;
    GMatchState gm;
    lua_settop(L, 2); /* keep strings on closure to avoid being collected */
    GMatchState o = new GMatchState();
    gm = o;
    lua_newuserobject(L, o, GMATCHSTATE_SIZE, 0);
    if (init > ls)   /* start after string's end? */
      init = ls + 1;
    gm.subject = s;
    gm.pattern = p;
    gm.src = (int) init;
    gm.p = 0;
    gm.lastmatch = NOMATCH;
    lua_pushcclosure(L, gmatch_aux, 3);
    return 1;
  }

  private static final lua_CFunction gmatch = Lstrlib::gmatch;

  private static void add_s(MatchState ms, luaL_Buffer b, int s,
                            int e)
  {
    lua_State L = ms.L;
    InternalString news = lua_toistring(L, 3);
    ByteView nv = news.view();
    FmtScan sc = new FmtScan(nv);
    while (sc.next())
    {
      luaL_addlstring(b, nv, sc.litStart, sc.litEnd - sc.litStart);
      if (sc.spec == L_ESC) /* '%%' */
        luaL_addchar(b, (byte) sc.spec);
      else if (sc.spec == '0') /* '%0' */
        luaL_addlstring(b, ms.src.view(), s, e - s);
      else if ('1' <= sc.spec && sc.spec <= '9')
      { /* '%n' */
        Capture cap = get_onecapture(ms, sc.spec - '1', s, e);
        if (cap.len == CAP_POSITION)
          luaL_addvalue(b); /* add position to accumulated result */
        else
          luaL_addlstring(b, ms.src.view(), cap.init, cap.len);
      }
      else
      {
        lua_pushinteger(L, L_ESC);
        luaL_errori(L, Lt.invalid_use_of_c_in_replacement_string);
      }
    }
    luaL_addlstring(b, nv, sc.litStart, sc.litEnd - sc.litStart);
  }

  /*
  ** The arguments of 'gsub' and what follows from them (its continuation
  ** gets them again).
  */
  private static final class GsubArgs
  {
    InternalString srcs; /* subject */
    InternalString pat;  /* pattern */
    int tr;              /* replacement type */
    long max_s;          /* max replacements */
    int anchor;
    int pinit; /* where the pattern starts (after an anchor) */
  }

  private static GsubArgs gsubargs(lua_State L)
  {
    GsubArgs a = new GsubArgs();
    a.srcs = luaL_checkistring(L, 1);
    a.pat = luaL_checkistring(L, 2);
    a.tr = lua_type(L, 3);
    a.max_s = luaL_optinteger(L, 4, (long) a.srcs.size() + 1);
    a.anchor = (a.pat.size() > 0 && a.pat.byteAt(0) == '^') ? 1 : 0;
    a.pinit = (a.anchor != 0) ? 1 : 0; /* skip anchor character */
    return a;
  }

  /* where a 'gsub' calling its replacement keeps its state (box at 5) */
  private static final int GS_S = 0;      /* the match: its start... */
  private static final int GS_E = 1;      /* ...and its end */
  private static final int GS_N = 2;      /* replacement count */
  private static final int GS_CHANGED = 3; /* change flag before the match */

  /*
  ** The replacement function or table gave the value on the top for the
  ** match 's'..'e': add it to the string buffer 'b'. Return true if the
  ** original string was changed. (Function calls and table indexing
  ** resulting in nil or false do not change the subject.)
  */
  private static int add_result(lua_State L, luaL_Buffer b, InternalString src,
                                int s, int e)
  {
    if (lua_toboolean(L, -1) == 0)
    {                /* nil or false? */
      lua_pop(L, 1); /* remove value */
      luaL_addlstring(b, src.view(), s, e - s); /* keep original text */
      return 0;                                 /* no changes */
    }
    else if (lua_isstring(L, -1) == 0)
    {
      lua_pushistringref(L, lua_typenamei(L, lua_type(L, -1)));
      return luaL_errori(L, Lt.invalid_replacement_value_a_s);
    }
    else
    {
      luaL_addvalue(b); /* add result to accumulator */
      return 1;         /* something changed */
    }
  }

  /*
  ** Add the replacement value to the string buffer 'b'. A replacement
  ** function or table is called with the state in 'box', for 'gsubk' to go
  ** on with after a freeze. Return true if the original string was changed.
  */
  private static int add_value(MatchState ms, luaL_Buffer b, luaL_Box box, int s,
                               int e, int tr, long n, int changed)
  {
    lua_State L = ms.L;
    switch (tr)
    {
    case LUA_TFUNCTION:
    { /* call the function */
      lua_pushvalue(L, 3);                 /* push the function */
      int nargs = push_captures(ms, s, e); /* all captures as arguments */
      box.ints[GS_S] = s;
      box.ints[GS_E] = e;
      box.ints[GS_N] = n;
      box.ints[GS_CHANGED] = changed;
      luaL_buffsave(b, box);
      lua_callk(L, nargs, 1, 0, gsubk); /* call it */
      luaL_buffload(b, box);
      break;
    }
    case LUA_TTABLE:
    {                               /* index the table */
      push_onecapture(ms, 0, s, e); /* first capture is the index */
      if (box == null)              /* (a table with no metatable: calls nothing) */
        lua_gettable(L, 3);
      else
      {
        box.ints[GS_S] = s;
        box.ints[GS_E] = e;
        box.ints[GS_N] = n;
        box.ints[GS_CHANGED] = changed;
        luaL_buffsave(b, box);
        lua_gettablek(L, 3, 0, gsubk);
        luaL_buffload(b, box);
      }
      break;
    }
    default:
    {                     /* LUA_TNUMBER or LUA_TSTRING */
      add_s(ms, b, s, e); /* add value to the buffer */
      return 1;           /* something changed */
    }
    }
    return add_result(L, b, ms.src, s, e);
  }

  private static int gsub_end(lua_State L, GsubArgs a, MatchState ms, luaL_Buffer b,
                              int src, long n, int changed)
  {
    if (changed == 0)      /* no changes? */
      lua_pushvalue(L, 1); /* return original string */
    else
    { /* something changed */
      luaL_addlstring(b, a.srcs.view(), src, ms.src_end - src);
      luaL_pushresult(b); /* create and return new string */
    }
    lua_pushinteger(L, n); /* number of substitutions */
    return 2;
  }

  /* the matches of 'gsub' from 'src' on ('first': see 'find_loop') */
  private static int gsub_loop(lua_State L, GsubArgs a, MatchState ms, luaL_Buffer b,
                               luaL_Box box, int src, long lastmatch,
                               long n, int changed, int e, boolean first)
  {
    while (n < a.max_s)
    {
      if (!first)
      {
        reprepstate(ms); /* (re)prepare state for new match */
        ms.origin = src;
        ms.loop[0] = lastmatch;
        ms.loop[1] = n;
        ms.loop[2] = changed;
        e = match(ms, src, a.pinit);
      }
      first = false;
      if (e != NOMATCH && e != lastmatch)
      { /* match? */
        n++;
        changed = (add_value(ms, b, box, src, e, a.tr, n, changed) != 0 || changed != 0) ? 1 : 0;
        lastmatch = e;
        src = e;
      }
      else if (src < ms.src_end) /* otherwise, skip one character */
        luaL_addchar(b, (byte) a.srcs.byteAt(src++));
      else
        break; /* end of subject */
      if (a.anchor != 0)
        break;
    }
    return gsub_end(L, a, ms, b, src, n, changed);
  }

  /*
  ** 'gsub' goes on after its replacement function or table (context 0), or
  ** after a freeze in a match (context 1, a match box at 6). The box at 5
  ** keeps its buffer and where it is (for a match: from where, its last
  ** match, count and change flag).
  */
  private static int gsubk(lua_State L, int status, long ctx)
  {
    GsubArgs a = gsubargs(L);
    luaL_Box box = luaL_tobox(L, 5);
    MatchState ms = new MatchState();
    prepstate(ms, L, a.srcs, a.pat);
    prepcheck(ms, 6, gsubk, 1);
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      luaL_buffload(b, box);
      ms.b = b;
      ms.bbox = box;
      if (ctx == 1)
      { /* (a check point of the match going on keeps these again) */
        long lastmatch = box.ints[GS_E];
        long n = box.ints[GS_N];
        int changed = (box.ints[GS_CHANGED] != 0) ? 1 : 0;
        ms.loop[0] = lastmatch;
        ms.loop[1] = n;
        ms.loop[2] = changed;
        int e = match_resume(ms);
        boolean calls = (a.tr == LUA_TFUNCTION || (a.tr == LUA_TTABLE && lua_isplaintable(L, 3) == 0));
        return gsub_loop(L, a, ms, b, calls ? box : null, ms.origin, lastmatch, n, changed, e, true);
      }
      long s = box.ints[GS_S];
      long e = box.ints[GS_E];
      if (s < 0 || s > e || e > ms.src_end)
        luaL_errori(L, Lt.invalid_match_state);
      long n = box.ints[GS_N];
      int changed = (add_result(L, b, a.srcs, (int) s, (int) e) != 0 || (int) box.ints[GS_CHANGED] != 0) ? 1 : 0;
      if (lua_gettop(L) == 6)
        ms.box = (lua_touserobject(L, 6) instanceof MatchBox m) ? m : null; /* (made by a check point before) */
      if (a.anchor != 0)
        return gsub_end(L, a, ms, b, (int) e, n, changed);
      return gsub_loop(L, a, ms, b, box, (int) e, e, n, changed, NOMATCH, false);
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_KFunction gsubk = Lstrlib::gsubk;

  private static final KNamer knamer_gsubk = new KNamer("string.gsubk", gsubk);

  private static int str_gsub(lua_State L)
  {
    GsubArgs a = gsubargs(L);
    int tr = a.tr;
    luaL_argexpectedi(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3, Lt.string_function_table);
    luaL_Box box = null;
    if (tr == LUA_TFUNCTION || (tr == LUA_TTABLE && lua_isplaintable(L, 3) == 0))
    { /* replacements call back: keep the state where a freeze finds it */
      lua_settop(L, 4);
      box = luaL_newbox(L);
      luaE_freezeonly(L);
    }
    MatchState ms = new MatchState();
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      prepstate(ms, L, a.srcs, a.pat);
      prepcheck(ms, 6, gsubk, 1); /* (boxes at 5 and 6, after the arguments) */
      ms.b = b;
      ms.bbox = box;
      return gsub_loop(L, a, ms, b, box, 0, NOMATCH, 0, 0, NOMATCH, false);
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_gsub = Lstrlib::str_gsub;

  /* }====================================================== */

  /*
  ** {======================================================
  ** STRING FORMAT
  ** =======================================================
  */


  /*
  ** Hexadecimal floating-point formatter
  */



  /*
  ** Number of bits that goes into the first digit. It can be any value
  ** between 1 and 4; the following definition tries to align the number
  ** to nibble boundaries by making what is left after that first digit a
  ** multiple of 4.
  */
  private static final int L_NBFD = (Double.PRECISION - 1) % 4 + 1;

  /*
  ** Add integer part of 'x' to buffer and return new 'x'
  */
  private static double adddigit(ByteWrites buff, int n, double x)
  {
    double dd = floor(x); /* get integer part from 'x' */
    int d = (int) dd;
    buff.set(n, (byte) (d < 10 ? d + '0' : d - 10 + 'a')); /* add to buffer */
    return x - dd;                                         /* return what is left */
  }

  private static int num2straux(ByteWrites area, int off, int sz, double x)
  {
    /* if 'inf' or 'NaN', format it like '%g' */
    if (x != x)
    {
      ByteView r = (Double.doubleToRawLongBits(x) < 0) ? Lt.minus_nan : Lt.nan;
      assert sz > r.size();
      area.copy_from(r, off);
      return r.size();
    }
    else if (x == Double.POSITIVE_INFINITY || x == -Double.POSITIVE_INFINITY)
    {
      ByteView r = (x < 0) ? Lt.minus_inf : Lt.inf;
      area.copy_from(r, off);
      return r.size();
    }
    else if (x == 0)
    {
      ByteView r = (Double.doubleToRawLongBits(x) < 0) ? Lt.neg_hex_zero : Lt.hex_zero;
      area.copy_from(r, off);
      return r.size();
    }
    else
    {
      int e;
      Frexp fe = new Frexp();
      double m = frexp(x, fe); /* 'x' fraction and exponent */
      e = fe.e;
      int n = 0;               /* character count */
      if (m < 0)
      {                             /* is number negative? */
        area.set(off + n++, '-'); /* add sign */
        m = -m;                     /* make it positive */
      }
      area.set(off + n++, '0');
      area.set(off + n++, 'x');                         /* add "0x" */
      m = adddigit(area, off + n++, m * (1 << L_NBFD)); /* add first digit */
      e -= L_NBFD;                                      /* this digit goes before the radix point */
      if (m > 0)
      {                                         /* more digits? */
        area.set(off + n++, locale_radixpoint()); /* add radix point */
        do
        { /* add as many digits as needed */
          m = adddigit(area, off + n++, m * 16);
        } while (m > 0);
      }
      area.set(off + n++, 'p');
      area.set(off + n++, (e >= 0) ? '+' : '-');
      n += area.write_number(e >= 0 ? e : -e, 10, off + n);
      assert n < sz;
      return n;
    }
  }

  private static int lua_number2strx(lua_State L, ByteWrites area, int off, int sz, ByteView fmt, double x)
  {
    int n = num2straux(area, off, sz, x);
    if (fmt.at(1) == 'A')
    {
      for (int i = 0; i < n; i++)
        area.set(off + i, (byte) locale_toupper(area.get(off + i) & 0xFF));
    }
    else if (fmt.at(1) != 'a')
    {
      return luaL_errori(L, Lt.modifiers_for_format_a_A_not_implemented);
    }
    return n;
  }


  /*
  ** Maximum size for items formatted with '%f'. This size is produced
  ** by format('%.99f', -maxfloat), and is equal to 99 + 3 ('-', '.',
  ** and '\0') + number of decimal digits to represent maxfloat (which
  ** is maximum exponent + 1). (99+3+1, adding some extra, 110)
  */
  private static final int MAX_ITEMF = 110 + 308;

  /*
  ** All formats except '%f' do not need that large limit.  The other
  ** float formats use exponents, so that they fit in the 99 limit for
  ** significant digits; 's' for large strings and 'q' add items directly
  ** to the buffer; all integer formats also fit in the 99 limit.  The
  ** worst case are floats: they may need 99 significant digits, plus
  ** '0x', '-', '.', 'e+XXXX', and '\0'. Adding some extra, 120.
  */
  private static final int MAX_ITEM = 120;

  /* valid flags in a format specification */
  /* valid flags for o, x, and X conversions */
  /* valid flags for d and i conversions */
  /* valid flags for u conversions */
  /* valid flags for c, p, and s conversions */

  /*
  ** Maximum size of each format specification (such as "%-099.99d"):
  ** Initial '%', flags (up to 5), width (2), period, precision (2),
  ** length modifier (8), conversion specifier, and final '\0', plus some
  ** extra.
  */
  private static final int MAX_FORMAT = 32;

  private static void addquoted(luaL_Buffer b, InternalString s)
  {
    int len = s.size();
    int i = 0;
    luaL_addchar(b, (byte) '"');
    for (; i < len; i++)
    {
      int c = s.byteAt(i);
      if (c == '"' || c == '\\' || c == '\n')
      {
        luaL_addchar(b, (byte) '\\');
        luaL_addchar(b, (byte) c);
      }
      else if (locale_matchclass(c, 'c') != 0)
      {
        ByteOwner dbuf = new ByteOwner(4);
        ByteView d = dbuf.view().substr(0, dbuf.write_number(c));
        luaL_addchar(b, (byte) '\\');
        if (locale_isdigit(sv_byteat(s.view(), i + 1)) != 0)
        {
          for (int j = d.size(); j < 3; j++)
            luaL_addchar(b, (byte) '0');
        }
        luaL_addlstring(b, d, 0, d.size());
      }
      else
        luaL_addchar(b, (byte) c);
    }
    luaL_addchar(b, (byte) '"');
  }

  /*
  ** Serialize a floating-point number in such a way that it can be
  ** scanned back by Lua. Use hexadecimal format for "common" numbers
  ** (to preserve precision); inf, -inf, and NaN are handled separately.
  ** (NaN cannot be expressed as a numeral, so we write '(0/0)' for it.)
  */
  private static int quotefloat(lua_State L, ByteWrites area, int off, double n)
  {
    ByteView s;                        /* for the fixed representations */
    if (n == Double.POSITIVE_INFINITY) /* inf? */
      s = Lt.huge_exp;
    else if (n == -Double.POSITIVE_INFINITY) /* -inf? */
      s = Lt.neg_huge_exp;
    else if (n != n) /* NaN? */
      s = Lt.n0_0;
    else
    { /* format number as hexadecimal */
      int nb = lua_number2strx(L, area, off, MAX_ITEM, Lt.fmt_a, n);
      ByteView region = area.view().substr(off, nb);
      if (region.find('.') == ByteView.npos)
      {                                   /* no dot? */
        byte point = locale_radixpoint(); /* try locale point */
        int ppos = region.find(point);
        if (ppos != ByteView.npos)
        {
          area.set(off + ppos, '.'); /* change it to a dot */
        }
      }
      return nb;
    }
    /* for the fixed representations */
    int l = s.size();
    assert l < MAX_ITEM;
    area.copy_from(s, off);
    area.set(off + l, '\0');
    return l;
  }

  private static void addliteral(lua_State L, luaL_Buffer b, int arg)
  {
    switch (lua_type(L, arg))
    {
    case LUA_TSTRING:
    {
      addquoted(b, lua_toistring(L, arg));
      break;
    }
    case LUA_TNUMBER:
    {
      int off = luaL_prepbuffsize(b, MAX_ITEM);
      ByteSpan area = luaL_buffaddr(b);
      int nb;
      if (lua_isinteger(L, arg) == 0) /* float? */
        nb = quotefloat(L, area, off, lua_tonumber(L, arg));
      else
      { /* integers */
        long n = lua_tointeger(L, arg);
        if (n == LUA_MININTEGER)
        {
          nb = area.write_unsigned(n, 16, off, true);
        }
        else
        {
          nb = area.write_number(n, 10, off);
        }
      }
      luaL_addsize(b, nb);
      break;
    }
    case LUA_TNIL:
    case LUA_TBOOLEAN:
    {
      luaL_toistring(L, arg);
      luaL_addvalue(b);
      break;
    }
    default:
    {
      luaL_argerrori(L, arg, Lt.value_has_no_literal_form);
    }
    }
  }

  private static int get2digits(ByteView s, int p)
  {
    if (locale_isdigit(sv_byteat(s, p)) != 0)
    {
      p++;
      if (locale_isdigit(sv_byteat(s, p)) != 0)
        p++; /* (2 digits at most) */
    }
    return p;
  }

  /*
  ** Check whether a conversion specification is valid. When called,
  ** first character in 'form' must be '%' and last character must
  ** be a valid conversion specifier. 'flags' are the accepted flags;
  ** 'precision' signals whether to accept a precision.
  */
  private static void checkformat(lua_State L, ByteView form,
                                  ByteView flags, int precision)
  {
    int p = 1;                                /* skip '%' */
    while (p < form.size() && flags.find(form.at(p)) != ByteView.npos)
      p++;                                    /* skip flags */
    if (sv_byteat(form, p) != '0')
    {                          /* a width cannot start with '0' */
      p = get2digits(form, p); /* skip width */
      if (sv_byteat(form, p) == '.' && precision != 0)
      {
        p++;
        p = get2digits(form, p); /* skip precision */
      }
    }
    if (!(p + 1 == form.size() && locale_isalpha(sv_byteat(form, p)) != 0))
    { /* did not go to the end? */
      lua_pushlstring(L, form, 0, form.size());
      luaL_errori(L, Lt.invalid_conversion_specification_s);
    }
  }

  /* where a conversion spec ends, and the length of its copy in 'form' */
  private static final class FormSpan
  {
    int end;
    int len;

    FormSpan(int end, int len)
    {
      this.end = end;
      this.len = len;
    }
  }

  private static FormSpan getformat(lua_State L, InternalString sf, int i, ByteWrites form)
  {
    /* spans flags, width, and precision ('0' is included as a flag) */
    ByteView sv = sf.view();
    ByteView fset = Lt.fmtflags_f_digits;
    int len = 0;
    while (i + len < sv.size() && fset.find(sv.at(i + len)) != ByteView.npos)
      len++;
    len++; /* adds following character (should be the specifier) */
    /* still needs space for '%', '\0', plus a length modifier */
    if (len >= MAX_FORMAT - 10)
      luaL_errori(L, Lt.invalid_format_too_long);
    form.set(0, '%');
    form.copy_from(sv.substr(i, len), 1);
    form.set(1 + len, '\0');
    return new FormSpan(i + len - 1, 1 + len);
  }

  private static final class FormatSpec
  {
    boolean left;
    boolean plus;
    boolean space;
    boolean alt;
    boolean zero;
    int width;
    int precision;
    byte conv;

    void assign(FormatSpec o)
    {
      left = o.left;
      plus = o.plus;
      space = o.space;
      alt = o.alt;
      zero = o.zero;
      width = o.width;
      precision = o.precision;
      conv = o.conv;
    }
  }

  private static void scanformat(ByteView form, FormatSpec spec)
  {
    int p = 1; /* skip '%' */
    spec.assign(new FormatSpec());
    spec.precision = -1;
    for (;; p++)
    {
      switch (sv_byteat(form, p))
      {
        case '-': spec.left = true; continue;
        case '+': spec.plus = true; continue;
        case ' ': spec.space = true; continue;
        case '#': spec.alt = true; continue;
        case '0': spec.zero = true; continue;
      }
      break;
    }
    while (locale_isdigit(sv_byteat(form, p)) != 0)
      spec.width = spec.width * 10 + (form.at(p++) - '0');
    if (sv_byteat(form, p) == '.')
    {
      p++;
      spec.precision = 0;
      while (locale_isdigit(sv_byteat(form, p)) != 0)
        spec.precision = spec.precision * 10 + (form.at(p++) - '0');
    }
    spec.conv = form.at(form.size() - 1);
  }

  private static int packformat(ByteWrites area, int off, int maxitem,
                                FormatSpec spec,
                                ByteView prefix, ByteView body,
                                boolean zeropadok)
  {
    int prefixlen = prefix.size();
    int bodylen = body.size();
    int content = prefixlen + bodylen;
    int width = spec.width;
    int total = (content > width) ? content : width;
    int pad = total - content;
    int pos = off;
    assert total < maxitem;
    if (spec.left)
    {
      area.copy_from(prefix, pos);
      pos += prefixlen;
      area.copy_from(body, pos);
      pos += bodylen;
      area.fill((byte) ' ', pos, pad);
    }
    else if (spec.zero && zeropadok)
    {
      area.copy_from(prefix, pos);
      pos += prefixlen;
      area.fill((byte) '0', pos, pad);
      pos += pad;
      area.copy_from(body, pos);
    }
    else
    {
      area.fill((byte) ' ', pos, pad);
      pos += pad;
      area.copy_from(prefix, pos);
      pos += prefixlen;
      area.copy_from(body, pos);
    }
    return total;
  }

  private static int formatint(lua_State L, int arg, ByteView form,
                               ByteWrites area, int off,
                               int maxitem, ByteView flags)
  {
    long n = luaL_checkinteger(L, arg);
    checkformat(L, form, flags, 1);
    FormatSpec spec = new FormatSpec();
    scanformat(form, spec);
    boolean issigned = (spec.conv == 'd' || spec.conv == 'i');
    int base = (spec.conv == 'o') ? 8
               : (spec.conv == 'x' || spec.conv == 'X') ? 16
                                                        : 10;
    long u = (issigned && n < 0)
                 ? 0L - n
                 : n;
    ByteOwner digits = new ByteOwner(24);
    int ndigits = digits.write_unsigned(u, base, 0, false);
    if (spec.conv == 'X')
    {
      for (int i = 0; i < ndigits; i++)
        digits.set(i, (byte) locale_toupper(digits.get(i) & 0xFF));
    }
    ByteOwner body = new ByteOwner(MAX_ITEM);
    int blen = 0;
    if (u == 0 && spec.precision == 0)
      ;
    else
    {
      int mindigits = (spec.precision >= 0) ? spec.precision : 1;
      while (blen + ndigits < mindigits)
        body.set(blen++, '0');
      body.copy_from(digits.view().substr(0, ndigits), blen);
      blen += ndigits;
    }
    if (spec.alt && spec.conv == 'o' && (blen == 0 || body.get(0) != '0'))
    {
      body.move_from(body.view().substr(0, blen), 1);
      body.set(0, '0');
      blen++;
    }
    byte[] prefix = new byte[2];
    int prefixlen = 0;
    if (issigned)
    {
      if (n < 0)
        prefix[prefixlen++] = '-';
      else if (spec.plus)
        prefix[prefixlen++] = '+';
      else if (spec.space)
        prefix[prefixlen++] = ' ';
    }
    else if (spec.alt && (spec.conv == 'x' || spec.conv == 'X') && u != 0)
    {
      prefix[prefixlen++] = '0';
      prefix[prefixlen++] = spec.conv;
    }
    return packformat(area, off, maxitem, spec,
                      new ByteView(prefix, 0, prefix.length).substr(0, prefixlen),
                      body.view().substr(0, blen), spec.precision < 0);
  }

  private static int formatfloat(lua_State L, int arg, ByteView form, ByteWrites area, int off, int maxitem)
  {
    double n = luaL_checknumber(L, arg);
    checkformat(L, form, Lt.fmtflags_f, 1);
    FormatSpec spec = new FormatSpec();
    scanformat(form, spec);
    byte lower = (byte) locale_tolower(spec.conv & 0xFF);
    int precision = (spec.precision >= 0) ? spec.precision : 6;
    byte[] prefix = new byte[1];
    int prefixlen = 0;
    if (Double.doubleToRawLongBits(n) < 0)
      prefix[prefixlen++] = '-';
    else if (spec.plus)
      prefix[prefixlen++] = '+';
    else if (spec.space)
      prefix[prefixlen++] = ' ';
    ByteOwner body = new ByteOwner(MAX_ITEMF);
    int blen;
    boolean zeropadok = true;
    if (Double.isInfinite(n) || Double.isNaN(n))
    {
      ByteView r = Double.isInfinite(n) ? Lt.inf : Lt.nan;
      blen = r.size();
      body.copy_from(r);
      zeropadok = false;
    }
    else
    {
      CharsFormat cf = (lower == 'f')   ? CharsFormat.fixed
                       : (lower == 'e') ? CharsFormat.scientific
                                        : CharsFormat.general;
      if (lower == 'g' && precision == 0)
        precision = 1;
      blen = body.write_number(fabs(n), cf, precision);
      if (spec.alt)
      {
        int epos = 0;
        boolean haspoint = false;
        while (epos < blen && body.get(epos) != 'e')
        {
          if (body.get(epos) == '.')
            haspoint = true;
          epos++;
        }
        if (!haspoint)
        {
          body.move_from(body.view().substr(epos, blen - epos), epos + 1);
          body.set(epos++, '.');
          blen++;
        }
        if (lower == 'g')
        {
          int sig = 0;
          boolean seennonzero = false;
          for (int i = 0; i < epos; i++)
          {
            if (locale_isdigit(body.get(i) & 0xFF) != 0)
            {
              if (body.get(i) != '0')
                seennonzero = true;
              if (seennonzero)
                sig++;
            }
          }
          if (sig == 0)
            sig = 1;
          int zeros = (precision > sig) ? precision - sig : 0;
          body.move_from(body.view().substr(epos, blen - epos), epos + zeros);
          body.fill((byte) '0', epos, zeros);
          blen += zeros;
        }
      }
    }
    if (spec.conv == 'E' || spec.conv == 'G')
    {
      for (int i = 0; i < blen; i++)
        body.set(i, (byte) locale_toupper(body.get(i) & 0xFF));
    }
    return packformat(area, off, maxitem, spec,
                      new ByteView(prefix, 0, prefix.length).substr(0, prefixlen),
                      body.view().substr(0, blen), zeropadok);
  }

  /* where a 'format' calling '__tostring' keeps its state (box after the
     arguments) */
  private static final int FMT_ITEM = 0; /* the item: where it starts (after its '%')... */
  private static final int FMT_ARG = 1;  /* ...and its argument */

  /*
  ** Whether the value at 'arg' has a '__tostring' to call for a format
  ** item: then the state goes into the box (made the first time) before
  ** the call, for 'formatk'.
  */
  private static luaL_Box formatsave(lua_State L, luaL_Buffer b, luaL_Box box, int top,
                                     int item, int arg)
  {
    if (luaL_getmetafieldi(L, arg, Lt.mm_tostring) == LUA_TNIL)
      return null;
    lua_pop(L, 1);
    if (box == null)
    {
      assert lua_gettop(L) == top;
      box = luaL_newbox(L);
      luaE_freezeonly(L);
    }
    box.ints[FMT_ITEM] = (long) item;
    box.ints[FMT_ARG] = arg;
    luaL_buffsave(b, box);
    return box;
  }

  /* the string 's' of a '%s' item, on the top: add it as the format says */
  private static int addstringitem(lua_State L, luaL_Buffer b, ByteView formv, int formlen,
                                   int arg, int boff, int maxitem, InternalString s)
  {
    int nb = 0;
    ByteView sv = s.view();
    if (formlen == 2)   /* no modifiers? */
      luaL_addvalue(b); /* keep entire string */
    else
    {
      luaL_argchecki(L, sv.find('\0') == ByteView.npos, arg, Lt.string_contains_zeros);
      checkformat(L, formv, Lt.minus_ch, 1);
      if (formv.find('.') == ByteView.npos && sv.size() >= 100)
      {
        /* no precision and string is too long to be formatted */
        luaL_addvalue(b); /* keep entire string */
      }
      else
      { /* format the string into 'buff' */
        FormatSpec spec = new FormatSpec();
        scanformat(formv, spec);
        int blen = (spec.precision >= 0 && spec.precision < sv.size())
                       ? spec.precision /* truncate */
                       : sv.size();
        nb = packformat(luaL_buffaddr(b), boff, maxitem, spec, Lt.empty_str, sv.substr(0, blen), false);
        lua_pop(L, 1);
      }
    }
    return nb;
  }

  /*
  ** Format from position 'i' of 'sf' on, the last argument used being 'arg'
  ** (of 'top'), into 'b'. With a box, it is at 'top + 1'.
  */
  private static int format_from(lua_State L, InternalString sf, int i, int arg, int top,
                                 luaL_Buffer b, luaL_Box box)
  {
    int sfl = sf.size();
    while (i < sfl)
    {
      if (sf.byteAt(i) != L_ESC)
        luaL_addchar(b, (byte) sf.byteAt(i++));
      else if (sv_byteat(sf.view(), ++i) == L_ESC)
      {
        luaL_addchar(b, (byte) sf.byteAt(i)); /* %% */
        i++;
      }
      else
      {                                                /* format item */
        ByteOwner form = new ByteOwner(MAX_FORMAT);    /* to store the format ('%...') */
        int maxitem = MAX_ITEM;                        /* maximum length for the result */
        int boff = luaL_prepbuffsize(b, maxitem);      /* to put result */
        int nb = 0;                                    /* number of bytes in result */
        int item = i;
        if (++arg > top)
          return luaL_argerrori(L, arg, Lt.no_value);
        FormSpan fs = getformat(L, sf, i, form);
        i = fs.end;
        ByteView formv = form.view().substr(0, fs.len);
        switch (sv_byteat(sf.view(), i++))
        {
        case 'c':
        {
          checkformat(L, formv, Lt.minus_ch, 0);
          FormatSpec spec = new FormatSpec();
          scanformat(formv, spec);
          byte[] buff = new byte[1];
          buff[0] = (byte) luaL_checkinteger(L, arg);
          ByteView body = new ByteView(buff, 0, 1);
          nb = packformat(luaL_buffaddr(b), boff, maxitem, spec, Lt.empty_str, body, false);
          break;
        }
        case 'd':
        case 'i':
          nb = formatint(L, arg, formv, luaL_buffaddr(b), boff, maxitem, Lt.fmt_flags_all);
          break;
        case 'u':
          nb = formatint(L, arg, formv, luaL_buffaddr(b), boff, maxitem, Lt.fmt_flags_zero);
          break;
        case 'o':
        case 'x':
        case 'X':
          nb = formatint(L, arg, formv, luaL_buffaddr(b), boff, maxitem, Lt.fmt_flags_hash);
          break;
        case 'a':
        case 'A':
          checkformat(L, formv, Lt.fmtflags_f, 1);
          nb = lua_number2strx(L, luaL_buffaddr(b), boff, maxitem, formv, luaL_checknumber(L, arg));
          break;
        case 'f':
          maxitem = MAX_ITEMF; /* extra space for '%f' */
          boff = luaL_prepbuffsize(b, maxitem);
          /* FALLTHROUGH */
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        {
          nb = formatfloat(L, arg, formv, luaL_buffaddr(b), boff, maxitem);
          break;
        }
        case 'p':
        {
          checkformat(L, formv, Lt.minus_ch, 0);
          FormatSpec spec = new FormatSpec();
          scanformat(formv, spec);
          ByteOwner body = new ByteOwner(MAX_ITEM);
          int blen;
          if (lua_islightcfunction(L, arg) != 0)
          {
            InternalString fa = lua_funcaddressi(L, arg);
            blen = fa.size();
            assert blen < body.size();
            body.copy_from(fa.view());
          }
          else
          {
            Object p = lua_topointer(L, arg);
            if (p == null)
            {
              ByteView nullrep = Lt.null_kw;
              blen = nullrep.size();
              body.copy_from(nullrep);
            }
            else
            {
              blen = pointertext(body, 0, p);
            }
          }
          ByteView bodybytes = body.view().substr(0, blen);
          nb = packformat(luaL_buffaddr(b), boff, maxitem, spec, Lt.empty_str, bodybytes, false);
          break;
        }
        case 'q':
        {
          if (fs.len != 2) /* modifiers? */
            return luaL_errori(L, Lt.specifier_q_cannot_have_modifiers);
          int t = lua_type(L, arg);
          luaL_Box saved;
          if ((t == LUA_TNIL || t == LUA_TBOOLEAN) &&
              (saved = formatsave(L, b, box, top, item, arg)) != null)
          { /* (a '__tostring' for nil or a boolean) */
            box = saved;
            luaL_toistringk(L, arg, 0, formatk);
            luaL_buffload(b, box);
            luaL_tostringresult(L);
            luaL_addvalue(b);
          }
          else
            addliteral(L, b, arg);
          break;
        }
        case 's':
        {
          luaL_Box saved = formatsave(L, b, box, top, item, arg);
          InternalString str;
          if (saved != null)
          {
            box = saved;
            luaL_toistringk(L, arg, 0, formatk);
            luaL_buffload(b, box);
            str = luaL_tostringresult(L);
          }
          else
          {
            luaL_tostringdefault(L, arg);
            str = lua_toistring(L, -1);
          }
          nb = addstringitem(L, b, formv, fs.len, arg, boff, maxitem, str);
          break;
        }
        default:
        { /* also treat cases 'pnLlh' */
          lua_pushlstring(L, formv, 0, formv.size());
          return luaL_errori(L, Lt.invalid_conversion_s_to_format);
        }
        }
        assert nb < maxitem;
        luaL_addsize(b, nb);
      }
    }
    luaL_pushresult(b);
    return 1;
  }

  /* 'format' goes on after a '__tostring' */
  private static int formatk(lua_State L, int status, long ctx)
  {
    int top = lua_gettop(L) - 2; /* (the arguments, the box, the string) */
    InternalString sf = lua_toistring(L, 1);
    luaL_Box box = luaL_tobox(L, top + 1);
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      luaL_buffload(b, box);
      int arg = (int) box.ints[FMT_ARG];
      ByteOwner form = new ByteOwner(MAX_FORMAT);
      FormSpan fs = getformat(L, sf, (int) box.ints[FMT_ITEM], form);
      int i = fs.end;
      if (sv_byteat(sf.view(), i++) == 'q')
      {
        luaL_tostringresult(L);
        luaL_addvalue(b);
      }
      else
      { /* 's' */
        int maxitem = MAX_ITEM;
        int boff = luaL_prepbuffsize(b, maxitem);
        int nb = addstringitem(L, b, form.view().substr(0, fs.len), fs.len, arg, boff, maxitem,
                               luaL_tostringresult(L));
        assert nb < maxitem;
        luaL_addsize(b, nb);
      }
      return format_from(L, sf, i, arg, top, b, box);
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_KFunction formatk = Lstrlib::formatk;

  private static final KNamer knamer_formatk = new KNamer("string.formatk", formatk);

  private static int str_format(lua_State L)
  {
    int top = lua_gettop(L);
    InternalString sf = luaL_checkistring(L, 1);
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      return format_from(L, sf, 0, 1, top, b, null);
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_format = Lstrlib::str_format;

  /* }====================================================== */

  /*
  ** {======================================================
  ** PACK/UNPACK
  ** =======================================================
  */

  /* value used for padding */
  private static final byte LUAL_PACKPADBYTE = 0x00;

  /* maximum size for the binary representation of an integer */
  private static final int MAXINTSIZE = 16;

  private static final int PACK_MAXALIGN = 16;

  /* number of bits in a character */
  private static final int NB = Byte.SIZE;

  /* mask for one character (NB 1's) */
  private static final int MC = (1 << NB) - 1;

  /* size of a lua_Integer */
  private static final int SZINT = 8;

  private static final int SZ_CHAR = 1;
  private static final int SZ_SHORT = 2;
  private static final int SZ_INT = 4;
  private static final int SZ_LONG = 8;
  private static final int SZ_INTEGER = 8;
  private static final int SZ_SIZET = 8;
  private static final int SZ_FLOAT = 4;
  private static final int SZ_NUMBER = 8;
  private static final int SZ_DOUBLE = 8;

  private static final boolean nativeendian_little = (ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN);

  private static final class Header
  {
    lua_State L;
    InternalString fmt; /* format string */
    int pos;
    int islittle;
    long maxalign;
  }

  private static int fmtbyte(Header h)
  {
    return sv_byteat(h.fmt.view(), h.pos);
  }

  /*
  ** options for pack/unpack
  */
  enum KOption
  {
    Kint,       /* signed integers */
    Kuint,      /* unsigned integers */
    Kfloat,     /* single-precision floating-point numbers */
    Knumber,    /* Lua "native" floating-point numbers */
    Kdouble,    /* double-precision floating-point numbers */
    Kchar,      /* fixed-length strings */
    Kstring,    /* strings with prefixed length */
    Kzstr,      /* zero-terminated strings */
    Kpadding,   /* padding */
    Kpaddalign, /* padding for alignment */
    Knop        /* no-op (configuration or spaces) */
  }

  /*
  ** Read an integer numeral from string 'fmt' or return 'df' if
  ** there is no numeral
  */
  private static int digit(int c)
  {
    return ('0' <= c && c <= '9') ? 1 : 0;
  }

  private static long getnum(Header h, long df)
  {
    if (digit(fmtbyte(h)) == 0) /* no number? */
      return df;                /* return default value */
    else
    {
      long a = 0;
      do
      {
        a = a * 10 + (fmtbyte(h) - '0');
        h.pos++;
      } while (digit(fmtbyte(h)) != 0 && a <= (MAX_SIZE - 9) / 10);
      return a;
    }
  }

  /*
  ** Read an integer numeral and raises an error if it is larger
  ** than the maximum size of integers.
  */
  private static long getnumlimit(Header h, long df)
  {
    long sz = getnum(h, df);
    if (Long.compareUnsigned(sz - 1, MAXINTSIZE) >= 0)
    {
      lua_pushinteger(h.L, sz);
      lua_pushinteger(h.L, MAXINTSIZE);
      return luaL_errori(h.L, Lt.integral_size_d_out_of_limits_1_d);
    }
    return sz;
  }

  /*
  ** Initialize Header
  */
  private static void initheader(lua_State L, Header h, InternalString fmt)
  {
    h.L = L;
    h.fmt = fmt;
    h.pos = 0;
    h.islittle = nativeendian_little ? 1 : 0;
    h.maxalign = 1;
  }

  /*
  ** Read and classify next option. 'size' is filled with option's size.
  */
  private static final class PackOption
  {
    KOption opt;
    long size;

    PackOption(KOption opt, long size)
    {
      this.opt = opt;
      this.size = size;
    }
  }

  private static PackOption getoption(Header h)
  {
    int opt = fmtbyte(h);
    h.pos++;
    switch (opt)
    {
    case 'b':
      return new PackOption(Kint, SZ_CHAR);
    case 'B':
      return new PackOption(Kuint, SZ_CHAR);
    case 'h':
      return new PackOption(Kint, SZ_SHORT);
    case 'H':
      return new PackOption(Kuint, SZ_SHORT);
    case 'l':
      return new PackOption(Kint, SZ_LONG);
    case 'L':
      return new PackOption(Kuint, SZ_LONG);
    case 'j':
      return new PackOption(Kint, SZ_INTEGER);
    case 'J':
      return new PackOption(Kuint, SZ_INTEGER);
    case 'T':
      return new PackOption(Kuint, SZ_SIZET);
    case 'f':
      return new PackOption(Kfloat, SZ_FLOAT);
    case 'n':
      return new PackOption(Knumber, SZ_NUMBER);
    case 'd':
      return new PackOption(Kdouble, SZ_DOUBLE);
    case 'i':
      return new PackOption(Kint, getnumlimit(h, SZ_INT));
    case 'I':
      return new PackOption(Kuint, getnumlimit(h, SZ_INT));
    case 's':
      return new PackOption(Kstring, getnumlimit(h, SZ_SIZET));
    case 'c':
    {
      long size = getnum(h, -1);
      if (size == -1L)
        luaL_errori(h.L, Lt.missing_size_for_format_option_c);
      return new PackOption(Kchar, size);
    }
    case 'z':
      return new PackOption(Kzstr, 0);
    case 'x':
      return new PackOption(Kpadding, 1);
    case 'X':
      return new PackOption(Kpaddalign, 0);
    case ' ':
      break;
    case '<':
      h.islittle = 1;
      break;
    case '>':
      h.islittle = 0;
      break;
    case '=':
      h.islittle = nativeendian_little ? 1 : 0;
      break;
    case '!':
    {
      h.maxalign = getnumlimit(h, PACK_MAXALIGN);
      break;
    }
    default:
      lua_pushinteger(h.L, opt);
      luaL_errori(h.L, Lt.invalid_format_option_c);
    }
    return new PackOption(Knop, 0);
  }

  /*
  ** Read, classify, and fill other details about the next option.
  ** 'psize' is filled with option's size, 'notoalign' with its
  ** alignment requirements.
  ** Local variable 'size' gets the size to be aligned. (Kpadal option
  ** always gets its full alignment, other options are limited by
  ** the maximum alignment ('maxalign'). Kchar option needs no alignment
  ** despite its size.
  */
  private static final class PackDetails
  {
    KOption opt;
    long size;
    long ntoalign;

    PackDetails(KOption opt, long size, long ntoalign)
    {
      this.opt = opt;
      this.size = size;
      this.ntoalign = ntoalign;
    }
  }

  private static PackDetails getdetails(Header h, long totalsize)
  {
    PackOption po = getoption(h);
    KOption opt = po.opt;
    long align = po.size; /* usually, alignment follows size */
    long ntoalign;
    if (opt == Kpaddalign)
    { /* 'X' gets alignment from following option */
      if (fmtbyte(h) <= 0)
        luaL_argerrori(h.L, 1, Lt.invalid_next_option_for_option_X);
      PackOption next = getoption(h);
      align = next.size;
      if (next.opt == Kchar || align == 0)
        luaL_argerrori(h.L, 1, Lt.invalid_next_option_for_option_X);
    }
    if (align <= 1 || opt == Kchar) /* need no alignment? */
      ntoalign = 0;
    else
    {
      if (align > h.maxalign) /* enforce maximum alignment */
        align = h.maxalign;
      if (!ispow2(align))
      {               /* not a power of 2? */
        ntoalign = 0; /* to avoid warnings */
        luaL_argerrori(h.L, 1, Lt.format_asks_for_alignment_not_power_of_2);
      }
      else
      {
        /* 'szmoda' = totalsize % align */
        long szmoda = totalsize & (align - 1);
        ntoalign = (align - szmoda) & (align - 1);
      }
    }
    return new PackDetails(opt, po.size, ntoalign);
  }

  /*
  ** Pack integer 'n' with 'size' bytes and 'islittle' endianness.
  ** The final 'if' handles the case when 'size' is larger than
  ** the size of a Lua integer, correcting the extra sign-extension
  ** bytes if necessary (by default they would be zeros).
  */
  private static void packint(luaL_Buffer b, long n,
                              int islittle, int size, int neg)
  {
    int off = luaL_prepbuffsize(b, size);
    ByteSpan buff = luaL_buffaddr(b);
    int i;
    buff.set(off + (islittle != 0 ? 0 : size - 1), (byte) (n & MC)); /* first byte */
    for (i = 1; i < size; i++)
    {
      n >>>= NB;
      buff.set(off + (islittle != 0 ? i : size - 1 - i), (byte) (n & MC));
    }
    if (neg != 0 && size > SZINT)
    {                                /* negative number need sign extension? */
      for (i = SZINT; i < size; i++) /* correct extra bytes */
        buff.set(off + (islittle != 0 ? i : size - 1 - i), (byte) MC);
    }
    luaL_addsize(b, size); /* add result to buffer */
  }

  private static void packFloat(luaL_Buffer b, float f, int islittle)
  {
    int pos = luaL_prepbuffsize(b, SZ_FLOAT);
    ByteSpan buff = luaL_buffaddr(b);
    luaL_addsize(b, SZ_FLOAT);
    buff.write_float(f, pos);
    if (islittle != (nativeendian_little ? 1 : 0))
    {
      buff.reverse(pos, SZ_FLOAT);
    }
  }

  private static void packDouble(luaL_Buffer b, double f, boolean islittle)
  {
    int pos = luaL_prepbuffsize(b, SZ_DOUBLE);
    ByteSpan buff = luaL_buffaddr(b);
    luaL_addsize(b, SZ_DOUBLE);
    buff.write_double(f, pos);
    if (islittle != nativeendian_little)
    {
      buff.reverse(pos, SZ_DOUBLE);
    }
  }

  private static double unpackDouble(ByteView data, int pos, boolean islittle)
  {
    data = data.substr(pos, SZ_DOUBLE);
    if (islittle != nativeendian_little)
    {
      ByteOwner r = new ByteOwner(data);
      r.reverse();
      data = r.view();
    }
    return data.toDouble(0);
  }

  private static float unpackFloat(ByteView data, int pos, boolean islittle)
  {
    data = data.substr(pos, SZ_FLOAT);
    if (islittle != nativeendian_little)
    {
      ByteOwner r = new ByteOwner(data);
      r.reverse();
      data = r.view();
    }
    return data.toFloat(0);
  }

  private static int str_pack(lua_State L)
  {
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      Header h = new Header();
      InternalString fmt = luaL_checkistring(L, 1); /* format string */
      int arg = 1;        /* current argument to pack */
      long totalsize = 0; /* accumulate total size of result */
      initheader(L, h, fmt);
      lua_pushnil(L); /* mark to separate arguments from string buffer */
      luaL_buffinit(L, b);
      while (fmtbyte(h) > 0)
      {
        PackDetails d = getdetails(h, totalsize);
        KOption opt = d.opt;
        long size = d.size;
        long ntoalign = d.ntoalign;
        luaL_argchecki(L, size + ntoalign <= MAX_SIZE - totalsize, arg, Lt.result_too_long);
        totalsize += ntoalign + size;
        while (ntoalign-- > 0)
          luaL_addchar(b, LUAL_PACKPADBYTE); /* fill alignment */
        arg++;
        switch (opt)
        {
        case Kint:
        { /* signed integers */
          long n = luaL_checkinteger(L, arg);
          if (size < SZINT)
          { /* need overflow check? */
            long lim = 1L << ((size * NB) - 1);
            luaL_argchecki(L, -lim <= n && n < lim, arg, Lt.integer_overflow);
          }
          packint(b, n, h.islittle, (int) size, (n < 0) ? 1 : 0);
          break;
        }
        case Kuint:
        { /* unsigned integers */
          long n = luaL_checkinteger(L, arg);
          if (size < SZINT) /* need overflow check? */
            luaL_argchecki(L, l_ult(n, 1L << (size * NB)), arg, Lt.unsigned_overflow);
          packint(b, n, h.islittle, (int) size, 0);
          break;
        }
        case Kfloat:
        {                                            /* C float */
          float f = (float) luaL_checknumber(L, arg); /* get argument */
          packFloat(b, f, h.islittle);
          break;
        }
        case Knumber:
        {                                       /* Lua float */
          double f = luaL_checknumber(L, arg); /* get argument */
          packDouble(b, f, h.islittle != 0);
          break;
        }
        case Kdouble:
        {                                       /* C double */
          double f = luaL_checknumber(L, arg); /* get argument */
          packDouble(b, f, h.islittle != 0);
          break;
        }
        case Kchar:
        { /* fixed-size string */
          ByteView s = luaL_checklstring(L, arg);
          int len = s.size();
          luaL_argchecki(L, len <= size, arg, Lt.string_longer_than_given_size);
          luaL_addlstring(b, s, 0, len); /* add string */
          if (len < size)
          {                           /* does it need padding? */
            long psize = size - len; /* pad size */
            int pos = luaL_prepbuffsize(b, psize);
            luaL_buffaddr(b).fill(LUAL_PACKPADBYTE, pos, (int) psize);
            luaL_addsize(b, (int) psize);
          }
          break;
        }
        case Kstring:
        { /* strings with length count */
          ByteView s = luaL_checklstring(L, arg);
          int len = s.size();
          luaL_argchecki(L, size >= SZ_INTEGER || len < (1L << (size * NB)), arg, Lt.string_length_does_not_fit_in_given_size);
          /* pack length */
          packint(b, len, h.islittle, (int) size, 0);
          luaL_addlstring(b, s, 0, len);
          totalsize += len;
          break;
        }
        case Kzstr:
        { /* zero-terminated string */
          ByteView s = luaL_checklstring(L, arg);
          int len = s.size();
          luaL_argchecki(L, s.find('\0') == ByteView.npos, arg, Lt.string_contains_zeros);
          luaL_addlstring(b, s, 0, len);
          luaL_addchar(b, (byte) '\0'); /* add zero at the end */
          totalsize += len + 1;
          break;
        }
        case Kpadding:
          luaL_addchar(b, LUAL_PACKPADBYTE); /* FALLTHROUGH */
        case Kpaddalign:
        case Knop:
          arg--; /* undo increment */
          break;
        }
      }
      luaL_pushresult(b);
      return 1;
    }
    finally
    {
      b.release();
    }
  }

  private static final lua_CFunction str_pack = Lstrlib::str_pack;

  private static int str_packsize(lua_State L)
  {
    Header h = new Header();
    InternalString fmt = luaL_checkistring(L, 1); /* format string */
    long totalsize = 0; /* accumulate total size of result */
    initheader(L, h, fmt);
    while (fmtbyte(h) > 0)
    {
      PackDetails d = getdetails(h, totalsize);
      KOption opt = d.opt;
      long size = d.size;
      long ntoalign = d.ntoalign;
      luaL_argchecki(L, opt != Kstring && opt != Kzstr, 1, Lt.variable_length_format);
      size += ntoalign; /* total space used by option */
      luaL_argchecki(L, totalsize <= MAX_SIZE - size, 1, Lt.format_result_too_large);
      totalsize += size;
    }
    lua_pushinteger(L, totalsize);
    return 1;
  }

  private static final lua_CFunction str_packsize = Lstrlib::str_packsize;

  /*
  ** Unpack an integer with 'size' bytes and 'islittle' endianness.
  ** If size is smaller than the size of a Lua integer and integer
  ** is signed, must do sign extension (propagating the sign to the
  ** higher bits); if size is larger than the size of a Lua integer,
  ** it must check the unread bytes to see whether they do not cause an
  ** overflow.
  */
  private static long unpackint(lua_State L, ByteView data, int off,
                                int islittle, int size, int issigned)
  {
    long res = 0;
    int i;
    int limit = (size <= SZINT) ? size : SZINT;
    for (i = limit - 1; i >= 0; i--)
    {
      res <<= NB;
      res |= (long) data.uat(off + (islittle != 0 ? i : size - 1 - i));
    }
    if (size < SZINT)
    { /* real size smaller than lua_Integer? */
      if (issigned != 0)
      { /* needs sign extension? */
        long mask = 1L << (size * NB - 1);
        res = (res ^ mask) - mask; /* do sign extension */
      }
    }
    else if (size > SZINT)
    { /* must check unread bytes */
      int mask = (issigned == 0 || res >= 0) ? 0 : MC;
      for (i = limit; i < size; i++)
      {
        if (data.uat(off + (islittle != 0 ? i : size - 1 - i)) != mask)
        {
          lua_pushinteger(L, size);
          luaL_errori(L, Lt.d_byte_integer_does_not_fit_into_Lua_Integer);
        }
      }
    }
    return res;
  }

  private static int str_unpack(lua_State L)
  {
    Header h = new Header();
    InternalString fmt = luaL_checkistring(L, 1);
    ByteView data = luaL_checklstring(L, 2);
    int ld = data.size();
    long pos = posrelatI(luaL_optinteger(L, 3, 1), ld) - 1;
    int n = 0; /* number of results */
    luaL_argchecki(L, pos <= ld, 3, Lt.initial_position_out_of_string);
    initheader(L, h, fmt);
    while (fmtbyte(h) > 0)
    {
      PackDetails d = getdetails(h, pos);
      KOption opt = d.opt;
      long size = d.size;
      long ntoalign = d.ntoalign;
      luaL_argchecki(L, ntoalign + size <= ld - pos, 2, Lt.data_string_too_short);
      pos += ntoalign; /* skip alignment */
      /* stack space for item + next position */
      luaL_checkstacki(L, 2, Lt.too_many_results);
      n++;
      switch (opt)
      {
      case Kint:
      case Kuint:
      {
        long res = unpackint(L, data, (int) pos, h.islittle,
                             (int) size, (opt == Kint) ? 1 : 0);
        lua_pushinteger(L, res);
        break;
      }
      case Kfloat:
      {
        float f = unpackFloat(data, (int) pos, h.islittle != 0);
        lua_pushnumber(L, f);
        break;
      }
      case Knumber:
      {
        double f = unpackDouble(data, (int) pos, h.islittle != 0);
        lua_pushnumber(L, f);
        break;
      }
      case Kdouble:
      {
        double f = unpackDouble(data, (int) pos, h.islittle != 0);
        lua_pushnumber(L, f);
        break;
      }
      case Kchar:
      {
        lua_pushlstring(L, data, (int) pos, (int) size);
        break;
      }
      case Kstring:
      {
        long len = unpackint(L, data, (int) pos,
                             h.islittle, (int) size, 0);
        luaL_argchecki(L, Long.compareUnsigned(len, ld - pos - size) <= 0, 2, Lt.data_string_too_short);
        lua_pushlstring(L, data, (int) (pos + size), (int) len);
        pos += len; /* skip string */
        break;
      }
      case Kzstr:
      {
        int zend = data.find('\0', (int) pos);
        luaL_argchecki(L, zend != ByteView.npos, 2, Lt.unfinished_string_for_format_z);
        int len = zend - (int) pos;
        lua_pushlstring(L, data, (int) pos, len);
        pos += len + 1; /* skip string plus final '\0' */
        break;
      }
      case Kpaddalign:
      case Kpadding:
      case Knop:
        n--; /* undo increment */
        break;
      }
      pos += size;
    }
    lua_pushinteger(L, pos + 1); /* next position */
    return n + 1;
  }

  private static final lua_CFunction str_unpack = Lstrlib::str_unpack;

  /* }====================================================== */

  private static final CFuncInfo cfi_str_byte = new CFuncInfo(str_byte, "string.str_byte");
  private static final CFuncInfo cfi_str_char = new CFuncInfo(str_char, "string.str_char");
  private static final CFuncInfo cfi_str_dump = new CFuncInfo(str_dump, "string.str_dump");
  private static final CFuncInfo cfi_str_find = new CFuncInfo(str_find, "string.str_find");
  private static final CFuncInfo cfi_str_format = new CFuncInfo(str_format, "string.str_format");
  private static final CFuncInfo cfi_gmatch = new CFuncInfo(gmatch, "string.gmatch");
  private static final CFuncInfo cfi_str_gsub = new CFuncInfo(str_gsub, "string.str_gsub");
  private static final CFuncInfo cfi_str_len = new CFuncInfo(str_len, "string.str_len");
  private static final CFuncInfo cfi_str_lower = new CFuncInfo(str_lower, "string.str_lower");
  private static final CFuncInfo cfi_str_match = new CFuncInfo(str_match, "string.str_match");
  private static final CFuncInfo cfi_str_rep = new CFuncInfo(str_rep, "string.str_rep");
  private static final CFuncInfo cfi_str_reverse = new CFuncInfo(str_reverse, "string.str_reverse");
  private static final CFuncInfo cfi_str_sub = new CFuncInfo(str_sub, "string.str_sub");
  private static final CFuncInfo cfi_str_upper = new CFuncInfo(str_upper, "string.str_upper");
  private static final CFuncInfo cfi_str_pack = new CFuncInfo(str_pack, "string.str_pack");
  private static final CFuncInfo cfi_str_packsize = new CFuncInfo(str_packsize, "string.str_packsize");
  private static final CFuncInfo cfi_str_unpack = new CFuncInfo(str_unpack, "string.str_unpack");

  private static final luaL_RegCFI[] strlib = {
    new luaL_RegCFI(Lt.byte_kw, cfi_str_byte),
    new luaL_RegCFI(Lt.char_kw, cfi_str_char),
    new luaL_RegCFI(Lt.dump, cfi_str_dump),
    new luaL_RegCFI(Lt.find, cfi_str_find),
    new luaL_RegCFI(Lt.format, cfi_str_format),
    new luaL_RegCFI(Lt.gmatch, cfi_gmatch),
    new luaL_RegCFI(Lt.gsub, cfi_str_gsub),
    new luaL_RegCFI(Lt.len, cfi_str_len),
    new luaL_RegCFI(Lt.lower, cfi_str_lower),
    new luaL_RegCFI(Lt.match, cfi_str_match),
    new luaL_RegCFI(Lt.rep, cfi_str_rep),
    new luaL_RegCFI(Lt.reverse, cfi_str_reverse),
    new luaL_RegCFI(Lt.sub, cfi_str_sub),
    new luaL_RegCFI(Lt.upper, cfi_str_upper),
    new luaL_RegCFI(Lt.pack, cfi_str_pack),
    new luaL_RegCFI(Lt.packsize, cfi_str_packsize),
    new luaL_RegCFI(Lt.unpack, cfi_str_unpack),
  };

  private static void createmetatable(lua_State L)
  {
    /* table to be metatable for strings */
    lua_createtable(L, 0, stringmetamethods.length - 1);
    luaL_setfuncscfi(L, stringmetamethods);
    lua_pushistringref(L, Lt.empty_str); /* dummy string */
    lua_pushvalue(L, -2);                /* copy table */
    lua_setmetatable(L, -2);             /* set table as metatable for strings */
    lua_pop(L, 1);                       /* pop dummy string */
    lua_pushvalue(L, -2);                /* get string library */
    lua_setfieldi(L, -2, Lt.mm_index);   /* metatable.__index = string */
    lua_pop(L, 1);                       /* pop metatable */
  }

  /*
  ** Open string library
  */
  public static int luaopen_string(lua_State L)
  {
    luaL_newlibcfi(L, strlib);
    createmetatable(L);
    return 1;
  }

  public static final lua_CFunction luaopen_string = Lstrlib::luaopen_string;
}
