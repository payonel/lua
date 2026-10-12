package lua.api;

import static lua.bytes.Lload.*;
import static lua.bytes.Lzio.*;
import static lua.bytes.UtilCast.*;
import static lua.compiler.Ldump.*;
import static lua.gc.Lgc.*;
import static lua.object.Lfunc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.object.Lstring.*;
import static lua.object.Ltable.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Ldebug.*;
import static lua.vm.Lvm.*;

import java.util.OptionalDouble;
import java.util.OptionalLong;

import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.FmtScan;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.bytes.ZIO;
import lua.object.CClosure;
import lua.object.CFuncInfo;
import lua.object.FmtArgs;
import lua.object.GCObject;
import lua.object.LClosure;
import lua.object.Proto;
import lua.object.TString;
import lua.object.TValue;
import lua.object.Table;
import lua.object.Udata;
import lua.object.UpVal;
import lua.runtime.CallInfo;
import lua.runtime.ProtectedCall;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class LuaApi
{
  private LuaApi()
  {
  }

  public static final int LUA_VERSION_MAJOR_N = 5;
  public static final int LUA_VERSION_MINOR_N = 5;
  public static final int LUA_VERSION_RELEASE_N = 1;
  public static final int LUA_VERSION_NUM = LUA_VERSION_MAJOR_N * 100 + LUA_VERSION_MINOR_N;
  public static final int LUA_VERSION_RELEASE_NUM = LUA_VERSION_NUM * 100 + LUA_VERSION_RELEASE_N;
  public static final ByteView LUA_VERSION_MAJOR = ByteView.of("5");
  public static final ByteView LUA_VERSION_MINOR = ByteView.of("5");
  public static final ByteView LUA_VERSION_RELEASE = ByteView.of("1");
  public static final ByteView LUA_VERSION = ByteView.of("Lua 5.5");
  public static final ByteView LUA_RELEASE = ByteView.of("Lua 5.5.1");
  public static final ByteView LUA_COPYRIGHT = ByteView.of("Lua 5.5.1  Copyright (C) 1994-2026 Lua.org, PUC-Rio");
  public static final ByteView LUA_AUTHORS = ByteView.of("R. Ierusalimschy, L. H. de Figueiredo, W. Celes");
  public static final ByteView LUA_SIGNATURE = ByteView.of("\33Lua");
  public static final int LUA_MULTRET = -1;
  public static final int LUA_REGISTRYINDEX = -(Integer.MAX_VALUE / 2 + 1000);
  public static final int LUA_OK = 0;
  public static final int LUA_YIELD = 1;
  public static final int LUA_ERRRUN = 2;
  public static final int LUA_ERRSYNTAX = 3;
  public static final int LUA_ERRMEM = 4;
  public static final int LUA_ERRERR = 5;
  public static final int LUA_TNONE = -1;
  public static final int LUA_TNIL = 0;
  public static final int LUA_TBOOLEAN = 1;
  public static final int LUA_TLIGHTUSERDATA = 2;
  public static final int LUA_TNUMBER = 3;
  public static final int LUA_TSTRING = 4;
  public static final int LUA_TTABLE = 5;
  public static final int LUA_TFUNCTION = 6;
  public static final int LUA_TUSERDATA = 7;
  public static final int LUA_TTHREAD = 8;
  public static final int LUA_NUMTYPES = 9;
  public static final int LUA_MINSTACK = 20;
  public static final int LUA_RIDX_GLOBALS = 2;
  public static final int LUA_RIDX_MAINTHREAD = 3;
  public static final int LUA_RIDX_LAST = 3;
  public static final int LUA_OPADD = 0;
  public static final int LUA_OPSUB = 1;
  public static final int LUA_OPMUL = 2;
  public static final int LUA_OPMOD = 3;
  public static final int LUA_OPPOW = 4;
  public static final int LUA_OPDIV = 5;
  public static final int LUA_OPIDIV = 6;
  public static final int LUA_OPBAND = 7;
  public static final int LUA_OPBOR = 8;
  public static final int LUA_OPBXOR = 9;
  public static final int LUA_OPSHL = 10;
  public static final int LUA_OPSHR = 11;
  public static final int LUA_OPUNM = 12;
  public static final int LUA_OPBNOT = 13;
  public static final int LUA_OPEQ = 0;
  public static final int LUA_OPLT = 1;
  public static final int LUA_OPLE = 2;
  public static final int LUA_NOFREEZE_BUDGET = 1;
  public static final int LUA_NOFREEZE_CALL = 2;
  public static final int LUA_NOFREEZE_RESUME = 3;
  public static final int LUA_GCSTOP = 0;
  public static final int LUA_GCRESTART = 1;
  public static final int LUA_GCCOLLECT = 2;
  public static final int LUA_GCCOUNT = 3;
  public static final int LUA_GCCOUNTB = 4;
  public static final int LUA_GCSTEP = 5;
  public static final int LUA_GCISRUNNING = 6;
  public static final int LUA_GCGEN = 7;
  public static final int LUA_GCINC = 8;
  public static final int LUA_GCPARAM = 9;
  public static final int LUA_GCPMINORMUL = 0;
  public static final int LUA_GCPMAJORMINOR = 1;
  public static final int LUA_GCPMINORMAJOR = 2;
  public static final int LUA_GCPPAUSE = 3;
  public static final int LUA_GCPSTEPMUL = 4;
  public static final int LUA_GCPSTEPSIZE = 5;
  public static final int LUA_GCPN = 6;
  public static final int LUA_N2SBUFFSZ = 64;
  public static final int LUA_HOOKCALL = 0;
  public static final int LUA_HOOKRET = 1;
  public static final int LUA_HOOKLINE = 2;
  public static final int LUA_HOOKCOUNT = 3;
  public static final int LUA_HOOKTAILCALL = 4;
  public static final int LUA_MASKCALL = 1 << LUA_HOOKCALL;
  public static final int LUA_MASKRET = 1 << LUA_HOOKRET;
  public static final int LUA_MASKLINE = 1 << LUA_HOOKLINE;
  public static final int LUA_MASKCOUNT = 1 << LUA_HOOKCOUNT;

  public static void api_incr_top(lua_State L)
  {
    L.top++;
    assert L.top <= L.ci.top : "stack overflow";
  }

  public static void adjustresults(lua_State L, int nres)
  {
    if (nres <= LUA_MULTRET && L.ci.top < L.top)
      L.ci.top = L.top;
  }

  /* Ensure the stack has at least 'n' elements */
  public static void api_checknelems(lua_State L, int n)
  {
    assert n < (L.top - L.ci.func) : "not enough elements in the stack";
  }

  /* Ensure the stack has at least 'n' elements to be popped. (Some
  ** functions only update a slot after checking it for popping, but that
  ** is only an optimization for a pop followed by a push.)
  */
  public static void api_checkpop(lua_State L, int n)
  {
    assert n < L.top - L.ci.func && L.tbclist < L.top - n : "not enough free elements in the stack";
  }

  public static final ByteView lua_ident = Lt.lua_ident_str;

  /* test for pseudo index */
  private static int ispseudo(int i)
  {
    return (i <= LUA_REGISTRYINDEX) ? 1 : 0;
  }

  /* test for upvalue */
  private static int isupvalue(int i)
  {
    return (i < LUA_REGISTRYINDEX) ? 1 : 0;
  }

  private static final class IndexAddr
  {
    int slot;
    TValue row;

    IndexAddr(int slot, TValue row)
    {
      this.slot = slot;
      this.row = row;
    }
  }

  private static IndexAddr resolveindex(lua_State L, int idx)
  {
    CallInfo ci = L.ci;
    if (idx > 0)
    {
      int o = ci.func + idx;
      assert idx <= ci.top - (ci.func + 1) : "unacceptable index";
      if (o >= L.top)
        return new IndexAddr(0, L.l_G.nilvalue);
      else
        return new IndexAddr(o, null);
    }
    else if (ispseudo(idx) == 0)
    { /* negative index */
      assert idx != 0 && -idx <= L.top - (ci.func + 1) : "invalid index";
      return new IndexAddr(L.top + idx, null);
    }
    else if (idx == LUA_REGISTRYINDEX)
      return new IndexAddr(0, L.l_G.l_registry);
    else
    { /* upvalues */
      idx = LUA_REGISTRYINDEX - idx;
      assert idx <= MAXUPVAL + 1 : "upvalue index too large";
      if (slottag(L, ci.func) == ctb(LUA_VCCL))
      { /* C closure? */
        CClosure func = slotclCval(L, ci.func);
        return new IndexAddr(0, (idx <= func.upvalue.length) ? func.upvalue[idx - 1]
                                                               : L.l_G.nilvalue);
      }
      else
      { /* light C function or Lua function (through a hook)?) */
        assert slottag(L, ci.func) == ctb(LUA_VLCF) : "caller not a C function";
        return new IndexAddr(0, L.l_G.nilvalue); /* no upvalues */
      }
    }
  }

  private static TValue index2val(lua_State L, int idx)
  {
    IndexAddr a = resolveindex(L, idx);
    return (a.row != null) ? getobj(a.row) : getslot(L, a.slot);
  }

  private static int isvalidindex(lua_State L, int idx)
  {
    IndexAddr a = resolveindex(L, idx);
    return (a.row != L.l_G.nilvalue) ? 1 : 0;
  }

  private static void setindexval(lua_State L, int idx, TValue v)
  {
    IndexAddr a = resolveindex(L, idx);
    if (a.row == null)
      setslot(L, a.slot, v);
    else
    {
      assert a.row != L.l_G.nilvalue : "invalid index";
      setobj(L, a.row, v);
    }
  }

  /*
  ** Convert a valid actual index (not a pseudo-index) to its address.
  */
  private static int index2stack(lua_State L, int idx)
  {
    CallInfo ci = L.ci;
    if (idx > 0)
    {
      int o = ci.func + idx;
      assert o < L.top : "invalid index";
      return o;
    }
    else
    { /* non-positive index */
      assert idx != 0 && -idx <= L.top - (ci.func + 1) : "invalid index";
      assert ispseudo(idx) == 0 : "invalid index";
      return L.top + idx;
    }
  }

  public static int lua_checkstack(lua_State L, int n)
  {
    int res;
    CallInfo ci;
    lua_lock(L);
    ci = L.ci;
    assert n >= 0 : "negative 'n'";
    if (L.stack_last - L.top > n) /* stack large enough? */
      res = 1;                          /* yes; check is OK */
    else                                /* need to grow stack */
      res = luaD_growstack(L, n, 0);
    if (res != 0 && ci.top < L.top + n)
      ci.top = L.top + n; /* adjust frame top */
    lua_unlock(L);
    return res;
  }

  public static void lua_xmove(lua_State from, lua_State to, int n)
  {
    int i;
    if (from == to)
      return;
    lua_lock(to);
    api_checkpop(from, n);
    assert from.l_G == to.l_G : "moving among independent states";
    assert to.ci.top >= to.top + n : "stack overflow";
    from.top -= n;
    for (i = 0; i < n; i++)
    {
      copyslot(to, to.top, from, from.top + i);
      to.top++; /* stack already checked by previous 'api_check' */
    }
    lua_unlock(to);
  }

  public static lua_CFunction lua_atpanic(lua_State L, lua_CFunction panicf)
  {
    lua_CFunction old;
    lua_lock(L);
    old = L.l_G.panic;
    L.l_G.panic = panicf;
    lua_unlock(L);
    return old;
  }

  public static double lua_version(lua_State L)
  {
    return LUA_VERSION_NUM;
  }

  /*
  ** basic stack manipulation
  */

  /*
  ** convert an acceptable stack index into an absolute index
  */

  public static int lua_absindex(lua_State L, int idx)
  {
    return (idx > 0 || ispseudo(idx) != 0)
               ? idx
               : (int) (L.top - L.ci.func) + idx;
  }

  public static int lua_gettop(lua_State L)
  {
    return (int) (L.top - (L.ci.func + 1));
  }

  /*
  ** 'lua_settop' and 'lua_closeslot' with a continuation 'k' (or none): a
  ** '__close' they call may freeze (or yield), and 'finishapik' then
  ** closes what is left down to 'apifirst' and finishes the call.
  */
  private static void settopaux(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    CallInfo ci;
    int func, newtop;
    int diff; /* difference for new top */
    lua_lock(L);
    ci = L.ci;
    func = ci.func;
    if (idx >= 0)
    {
      assert idx <= ci.top - (func + 1) : "new top too large";
      diff = (int) (((func + 1) + idx) - L.top);
      for (; diff > 0; diff--)
        setslottag(L, L.top++, LUA_VNIL); /* clear new slots */
    }
    else
    {
      assert -(idx + 1) <= (L.top - (func + 1)) : "invalid new top";
      diff = idx + 1; /* will "subtract" index (as it is negative) */
    }
    newtop = L.top + diff;
    if (diff < 0 && L.tbclist >= newtop)
    {
      assert (ci.callstatus & CIST_TBC) != 0;
      boolean armed = armk(L, ctx, k, APIK_SETTOP);
      if (armed)
        ci.apifirst = newtop;
      newtop = luaF_close(L, newtop, (byte) CLOSEKTOP, armed ? closeyy(L) : 0);
      disarmk(L, armed);
    }
    L.top = newtop; /* correct top only after closing any upvalue */
    lua_unlock(L);
  }

  public static void lua_settop(lua_State L, int idx)
  {
    settopaux(L, idx, 0, null);
  }

  public static void lua_settopk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    settopaux(L, idx, ctx, k);
  }

  private static void closeslotaux(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    int level;
    lua_lock(L);
    level = index2stack(L, idx);
    assert (L.ci.callstatus & CIST_TBC) != 0 && (L.tbclist == level) : "no variable to close at given level";
    boolean armed = armk(L, ctx, k, APIK_CLOSESLOT);
    if (armed)
      L.ci.apifirst = level;
    level = luaF_close(L, level, (byte) CLOSEKTOP, armed ? closeyy(L) : 0);
    disarmk(L, armed);
    setslottag(L, level, LUA_VNIL);
    lua_unlock(L);
  }

  public static void lua_closeslot(lua_State L, int idx)
  {
    closeslotaux(L, idx, 0, null);
  }

  public static void lua_closeslotk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    closeslotaux(L, idx, ctx, k);
  }

  /*
  ** Reverse the stack segment from 'from' to 'to'
  ** (auxiliary to 'lua_rotate')
  ** Note that we move(copy) only the value inside the stack.
  ** (We do not move additional fields that may exist.)
  */
  private static void reverse(lua_State L, int from, int to)
  {
    for (; from < to; from++, to--)
    {
      TValue temp = getslot(L, from);
      copyslot(L, from, to);
      setslot(L, to, temp);
    }
  }

  /*
  ** Let x = AB, where A is a prefix of length 'n'. Then,
  ** rotate x n == BA. But BA == (A^r . B^r)^r.
  */

  public static void lua_rotate(lua_State L, int idx, int n)
  {
    int p, t, m;
    lua_lock(L);
    t = L.top - 1;           /* end of stack segment being rotated */
    p = index2stack(L, idx); /* start of segment */
    assert L.tbclist < p : "moving a to-be-closed slot";
    assert (n >= 0 ? n : -n) <= (t - p + 1) : "invalid 'n'";
    m = (n >= 0 ? t - n : p - n - 1); /* end of prefix */
    reverse(L, p, m);                 /* reverse the prefix with length 'n' */
    reverse(L, m + 1, t);             /* reverse the suffix */
    reverse(L, p, t);                 /* reverse the entire segment */
    lua_unlock(L);
  }

  public static void lua_copy(lua_State L, int fromidx, int toidx)
  {
    lua_lock(L);
    TValue fr = index2val(L, fromidx);
    assert isvalidindex(L, toidx) != 0 : "invalid index";
    setindexval(L, toidx, fr);
    if (isupvalue(toidx) != 0) /* function upvalue? */
    {
      int fv = L.ci.func;
      luaC_barrier(L, slotclCval(L, fv), fr);
    }
    /* LUA_REGISTRYINDEX does not need gc barrier
       (collector revisits it before finishing collection) */
    lua_unlock(L);
  }

  public static void lua_pushvalue(lua_State L, int idx)
  {
    lua_lock(L);
    TValue v = index2val(L, idx);
    setslot(L, L.top, v);
    api_incr_top(L);
    lua_unlock(L);
  }

  /*
  ** access functions (stack -> C)
  */

  public static int lua_type(lua_State L, int idx)
  {
    return ((isvalidindex(L, idx) != 0) ? novariant(index2val(L, idx).tt_)
                                         : LUA_TNONE);
  }

  public static HostString lua_typename(lua_State L, int t)
  {
    assert LUA_TNONE <= t && t < LUA_NUMTYPES : "invalid type";
    ByteView n = ttypename(t);
    return new HostString(n);
  }

  public static int lua_iscfunction(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return (ttislcf(o) || ttisCclosure(o)) ? 1 : 0;
  }

  public static int lua_islightcfunction(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return ttislcf(o) ? 1 : 0;
  }

  public static int lua_isinteger(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return ttisinteger(o) ? 1 : 0;
  }

  public static int lua_isnumber(lua_State L, int idx)
  {
    NumResult n = new NumResult();
    TValue o = index2val(L, idx);
    return tonumber(o, n);
  }

  public static int lua_isstring(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return (ttisstring(o) || cvt2str(o) != 0) ? 1 : 0;
  }

  public static int lua_isuserdata(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return (ttisfulluserdata(o) || ttislightuserdata(o)) ? 1 : 0;
  }

  public static int lua_rawequal(lua_State L, int index1, int index2)
  {
    TValue o1 = index2val(L, index1);
    TValue o2 = index2val(L, index2);
    return (isvalidindex(L, index1) != 0 && isvalidindex(L, index2) != 0)
               ? luaV_rawequalobj(o1, o2)
               : 0;
  }

  public static void lua_arith(lua_State L, int op)
  {
    lua_lock(L);
    if (op != LUA_OPUNM && op != LUA_OPBNOT)
      api_checkpop(L, 2); /* all other operations expect two operands */
    else
    { /* for unary operations, add fake 2nd operand */
      api_checkpop(L, 1);
      copyslot(L, L.top, L.top - 1);
      api_incr_top(L);
    }
    /* first operand at top - 2, second at top - 1; result go to top - 2 */
    luaO_arith(L, op, L.top - 2, L.top - 1, L.top - 2);
    L.top--; /* pop second operand */
    lua_unlock(L);
  }

  public static int lua_compare(lua_State L, int index1, int index2, int op)
  {
    int i = 0;
    lua_lock(L); /* may call tag method */
    TValue o1 = index2val(L, index1);
    TValue o2 = index2val(L, index2);
    if (isvalidindex(L, index1) != 0 && isvalidindex(L, index2) != 0)
    {
      switch (op)
      {
      case LUA_OPEQ:
        i = luaV_equalobj(L, o1, o2);
        break;
      case LUA_OPLT:
        i = luaV_lessthan(L, o1, o2);
        break;
      case LUA_OPLE:
        i = luaV_lessequal(L, o1, o2);
        break;
      default:
        assert false : "invalid option";
      }
    }
    lua_unlock(L);
    return i;
  }

  /*
  ** Whether the value at 'index1' is less than the one at 'index2', where no
  ** metamethod can run (two numbers, or two strings); -1 otherwise.
  */
  public static int lua_rawlessthan(lua_State L, int index1, int index2)
  {
    TValue o1 = index2val(L, index1);
    TValue o2 = index2val(L, index2);
    if ((ttisnumber(o1) && ttisnumber(o2)) || (ttisstring(o1) && ttisstring(o2)))
      return luaV_lessthan(L, o1, o2);
    return -1;
  }

  public static ByteView lua_numbertocstringi(lua_State L, int idx, ByteSpan buff)
  {
    TValue o = index2val(L, idx);
    if (ttisnumber(o))
    {
      int len = luaO_tostringbuff(o, buff);
      return new ByteView(buff.array(), buff.offset(), len);
    }
    else
      return null;
  }

  public static HostString lua_numbertocstring(lua_State L, int idx)
  {
    ByteOwner buff = new ByteOwner(LUA_N2SBUFFSZ);
    ByteView s = lua_numbertocstringi(L, idx, buff.span());
    if (s == null)
      return null;
    return new HostString(s);
  }

  public static int lua_stringtonumber(lua_State L, HostString s)
  {
    TValue v = new TValue();
    int sz = luaO_str2num(s.to_utf8(), v);
    if (sz != 0)
    {
      setslot(L, L.top, v);
      api_incr_top(L);
    }
    return sz;
  }

  public static OptionalDouble lua_tonumberx(lua_State L, int idx)
  {
    NumResult n = new NumResult();
    TValue o = index2val(L, idx);
    if (tonumber(o, n) == 0)
      return OptionalDouble.empty();
    return OptionalDouble.of(n.v);
  }

  public static OptionalLong lua_tointegerx(lua_State L, int idx)
  {
    IntResult res = new IntResult();
    TValue o = index2val(L, idx);
    if (tointeger(o, res) == 0)
      return OptionalLong.empty();
    return OptionalLong.of(res.v);
  }

  public static int lua_toboolean(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return !(ttisfalse(o) || ttisnil(o)) ? 1 : 0;
  }

  public static ByteView lua_tolstring(lua_State L, int idx)
  {
    lua_lock(L);
    TValue o = index2val(L, idx);
    if (!ttisstring(o))
    {
      if (cvt2str(o) == 0)
      {
        lua_unlock(L);
        return null;
      }
      luaO_tostring(L, o);
      setindexval(L, idx, o);
      luaC_checkGC(L);
    }
    lua_unlock(L);
    assert ttisstring(o);
    TString s = tsvalue(o);
    return s.data.view();
  }

  public static InternalString lua_toistring(lua_State L, int idx)
  {
    lua_lock(L);
    TValue o = index2val(L, idx);
    if (!ttisstring(o))
    {
      if (cvt2str(o) == 0)
      { /* not convertible? */
        lua_unlock(L);
        return null;
      }
      luaO_tostring(L, o);
      setindexval(L, idx, o);
      luaC_checkGC(L);
    }
    lua_unlock(L);
    assert ttisstring(o);
    return tsvalue(o).data;
  }

  public static long lua_rawlen(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    switch (ttypetag(o))
    {
    case LUA_VSHRSTR:
      return tsslen(tsvalue(o));
    case LUA_VUSERDATA:
    {
      Udata pu = uvalue(o);
      return pu.zb ? 0 : pu.data.size();
    }
    case LUA_VTABLE:
    {
      long res;
      lua_lock(L);
      res = hvalue(o).luaH_getn(L);
      lua_unlock(L);
      return res;
    }
    default:
      return 0;
    }
  }

  public static lua_CFunction lua_tocfunction(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    if (ttislcf(o))
      return fvalue(o);
    else if (ttisCclosure(o))
    {
      return clCvalue(o).f;
    }
    else
      return null; /* not a C function */
  }

  private static Object touserdata(TValue o)
  {
    switch (ttype(o))
    {
    case LUA_TUSERDATA:
      return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA:
      assert ttislightuserdata(o);
      return pvalue(o);
    default:
      return null;
    }
  }

  public static Object lua_touserdata(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return touserdata(o);
  }

  public static lua_State lua_tothread(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    if (!ttisthread(o))
      return null;
    return thvalue(o);
  }

  /*
  ** Returns a pointer to the internal representation of an object.
  ** Note that ISO C does not allow the conversion of a pointer to
  ** function to a 'void*', so the conversion here goes through
  ** a 'size_t'. (As the returned pointer is only informative, this
  ** conversion should not be a problem.)
  */

  public static Object lua_topointer(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    switch (ttypetag(o))
    {
    case LUA_VLCF:
      return o.ref;
    case LUA_VUSERDATA:
    case LUA_VLIGHTUSERDATA:
      return touserdata(o);
    default:
    {
      if (iscollectable(o))
        return gcvalue(o);
      else
        return null;
    }
    }
  }

  public static InternalString lua_funcaddressi(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    assert ttislcf(o);
    return func_to_string(fvalue(o));
  }

  public static ByteView lua_typenamei(lua_State L, int t)
  {
    assert LUA_TNONE <= t && t < LUA_NUMTYPES : "invalid type";
    return ttypename(t);
  }

  /*
  ** push functions (C -> stack)
  */

  public static void lua_pushnil(lua_State L)
  {
    lua_lock(L);
    setslottag(L, L.top, LUA_VNIL);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static void lua_pushnumber(lua_State L, double n)
  {
    lua_lock(L);
    setslotflt(L, L.top, n);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static void lua_pushinteger(lua_State L, long n)
  {
    lua_lock(L);
    setsloti(L, L.top, n);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static ByteView lua_pushlstring(lua_State L, ByteView s, int off, int len)
  {
    TString ts;
    lua_lock(L);
    if (len == 0)
    {
      s = Lt.empty_str;
      off = 0;
      len = s.size();
    }
    ts = TString.luaS_createstr(L, s.substr(off, len));
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
    return ts.data.view();
  }

  public static void lua_pushstring(lua_State L, HostString s)
  {
    if (s == null)
    {
      lua_pushnil(L);
      return;
    }
    lua_lock(L);
    ByteView bytes = s.to_utf8();
    TString ts = TString.luaS_createstr(L, bytes);
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static void lua_pushistringref(lua_State L, ByteView bytes)
  {
    lua_lock(L);
    TString ts = TString.luaS_createstr(L, bytes);
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static void lua_pushistring(lua_State L, InternalString s)
  {
    lua_lock(L);
    TString ts = TString.luaS_take(L, InternalString.move(s));
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static void lua_pushiblock(lua_State L, ByteOwner block, int len)
  {
    lua_lock(L);
    TString ts = TString.luaS_takeblock(L, ByteOwner.move(block), len);
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static void lua_pushistringext(lua_State L, InternalString s)
  {
    lua_lock(L);
    TString ts = TString.luaS_takeexternal(L, InternalString.move(s));
    setslots(L, L.top, ts);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static HostString lua_pushfstring(lua_State L, HostString fmt)
  {
    return new HostString(lua_pushfstringi(L, fmt.to_utf8()).view());
  }

  public static InternalString lua_pushfstringi(lua_State L, ByteView fmt)
  {
    InternalString ret;
    FmtArgs args = new FmtArgs();
    lua_lock(L);
    int n = luaO_fmtargcount(fmt);
    api_checknelems(L, n);
    FmtScan sc = new FmtScan(fmt);
    while (sc.next())
    {
      int slot = L.top - n + args.n;
      switch (sc.spec)
      {
      case 's':
      {
        int o = slot;
        if (novariant(slottag(L, o)) == LUA_TNUMBER)
        {
          TValue v = getslot(L, o);
          luaO_tostring(L, v);
          setslot(L, o, v);
        }
        assert novariant(slottag(L, o)) == LUA_TSTRING : "string expected";
        args.str(slottsval(L, o).data.view());
        break;
      }
      case 'c':
      {
        int o = slot;
        assert slottag(L, o) == LUA_VNUMINT : "integer expected";
        args.chr((int) slotival(L, o));
        break;
      }
      case 'd':
      {
        int o = slot;
        assert slottag(L, o) == LUA_VNUMINT : "integer expected";
        args.num((int) slotival(L, o));
        break;
      }
      case 'I':
      {
        int o = slot;
        assert slottag(L, o) == LUA_VNUMINT : "integer expected";
        args.lint(slotival(L, o));
        break;
      }
      case 'f':
      {
        int o = slot;
        assert slottag(L, o) == LUA_VNUMFLT : "float expected";
        args.flt(slotfltval(L, o));
        break;
      }
      case 'p':
      {
        args.ptr(lua_topointer(L, args.n - n));
        break;
      }
      case 'U':
      {
        int o = slot;
        assert slottag(L, o) == LUA_VNUMINT : "integer expected";
        args.utf8(slotival(L, o));
        break;
      }
      default:
        break;
      }
    }
    InternalString is = luaO_pushvfstring(L, fmt, args);
    if (is == null)
      luaD_throw(L, (byte) LUA_ERRMEM);
    ret = is;
    if (n > 0)
    {
      copyslot(L, L.top - 1 - n, L.top - 1);
      L.top -= n;
    }
    luaC_checkGC(L);
    lua_unlock(L);
    return ret;
  }

  public static void lua_pushcclosure(lua_State L, lua_CFunction fn, int n)
  {
    lua_lock(L);
    if (n == 0)
    {
      CFuncInfo cfi = lua_createcfuncinfo(L, fn);
      setslotf(L, L.top, cfi);
      api_incr_top(L);
    }
    else
    {
      int i;
      CClosure cl;
      api_checkpop(L, n);
      assert n <= MAXUPVAL : "upvalue index too large";
      cl = CClosure.luaF_newCclosure(L, n);
      cl.f = fn;
      for (i = 0; i < n; i++)
      {
        slot2obj(L, cl.upvalue[i], L.top - n + i);
        /* does not need barrier because closure is white */
        assert iswhite(cl) != 0;
      }
      L.top -= n;
      setslotclC(L, L.top, cl);
      api_incr_top(L);
      luaC_checkGC(L);
    }
    lua_unlock(L);
  }

  public static void lua_pushboolean(lua_State L, int b)
  {
    lua_lock(L);
    if (b != 0)
      setslottag(L, L.top, LUA_VTRUE);
    else
      setslottag(L, L.top, LUA_VFALSE);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static void lua_pushlightuserdata(lua_State L, Object p)
  {
    lua_lock(L);
    setslotp(L, L.top, p);
    api_incr_top(L);
    lua_unlock(L);
  }

  /* pushes thread 'th' */
  public static void lua_pushthreadi(lua_State L, lua_State th)
  {
    lua_lock(L);
    setslotth(L, L.top, th);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static int lua_pushthread(lua_State L)
  {
    lua_lock(L);
    setslotth(L, L.top, L);
    api_incr_top(L);
    lua_unlock(L);
    return (mainthread(L.l_G) == L) ? 1 : 0;
  }

  /*
  ** Whether a call with a continuation from 'ci' may yield: the thread can
  ** yield and the function does not keep calls from yielding (CIST_FZONLY).
  */
  private static boolean yieldablek(lua_State L, CallInfo ci)
  {
    return yieldable(L) != 0 && (ci.callstatus & CIST_FZONLY) == 0;
  }

  /*
  ** API calls with continuations: while one runs, a metamethod it calls may
  ** yield or freeze, and 'finishCcall' finishes the call as 'op' says
  ** (APIK_*) before calling the continuation. Where the call cannot yield,
  ** the metamethod can still freeze (CIST_NYCALL counts its call). Without
  ** a continuation, it is the plain call.
  */
  private static boolean armk(lua_State L, long ctx, lua_KFunction k, byte op)
  {
    CallInfo ci = L.ci;
    assert k == null || isLua(ci) == 0 : "cannot use continuations inside hooks";
    if (k == null)
      return false;
    ci.k = k;
    ci.ctx = ctx;
    ci.apiop = op;
    ci.callstatus |= yieldablek(L, ci) ? CIST_APIK : (CIST_APIK | CIST_NYCALL);
    return true;
  }

  private static void disarmk(lua_State L, boolean armed)
  {
    if (armed)
      L.ci.callstatus &= ~(CIST_APIK | CIST_NYCALL);
  }

  /* how an API call armed by 'armk' calls '__close' (see 'luaF_close') */
  private static int closeyy(lua_State L)
  {
    return ((L.ci.callstatus & CIST_NYCALL) != 0) ? 2 : 1;
  }

  /*
  ** get functions (Lua -> stack)
  */

  private static int auxgetstr(lua_State L, TValue t, ByteView k,
                               long ctx, lua_KFunction kf)
  {
    byte tag;
    TValue res = new TValue();
    TString str = TString.luaS_createstr(L, k);
    if (!ttistable(t))
      tag = LUA_VNOTABLE;
    else
    {
      tag = hvalue(t).luaH_getstr(str, res);
    }
    if (!tagisempty(tag))
    {
      setslot(L, L.top, res);
      api_incr_top(L);
    }
    else
    {
      setslots(L, L.top, str);
      api_incr_top(L);
      TValue key = getslot(L, L.top - 1);
      boolean armed = armk(L, ctx, kf, APIK_OVERKEY);
      tag = luaV_finishget(L, t, key, L.top - 1, tag, NOSLOT);
      disarmk(L, armed);
    }
    lua_unlock(L);
    return novariant(tag);
  }

  private static int auxgetstr(lua_State L, TValue t, ByteView k)
  {
    return auxgetstr(L, t, k, 0, null);
  }

  /*
  ** The following function assumes that the registry cannot be a weak
  ** table; so, an emergency collection while using the global table
  ** cannot collect it.
  */
  private static void getGlobalTable(lua_State L, TValue gt)
  {
    Table registry = hvalue(L.l_G.l_registry);
    byte tag = registry.luaH_getint(LUA_RIDX_GLOBALS, gt);
    assert novariant(tag) == LUA_TTABLE : "global table must exist";
  }

  public static int lua_getglobal(lua_State L, HostString name)
  {
    return lua_getglobali(L, name.to_utf8());
  }

  public static int lua_getglobali(lua_State L, ByteView name)
  {
    TValue gt = new TValue();
    lua_lock(L);
    getGlobalTable(L, gt);
    return auxgetstr(L, gt, name);
  }

  public static int lua_getglobalik(lua_State L, ByteView name, long ctx, lua_KFunction k)
  {
    TValue gt = new TValue();
    lua_lock(L);
    getGlobalTable(L, gt);
    return auxgetstr(L, gt, name, ctx, k);
  }

  public static int lua_getglobalk(lua_State L, HostString name, long ctx, lua_KFunction k)
  {
    return lua_getglobalik(L, name.to_utf8(), ctx, k);
  }

  private static int auxgettable(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    byte tag;
    lua_lock(L);
    api_checkpop(L, 1);
    TValue t = index2val(L, idx);
    if (!ttistable(t))
      tag = LUA_VNOTABLE;
    else
    {
      TValue key = getslot(L, L.top - 1);
      TValue res = new TValue();
      tag = hvalue(t).luaH_get(key, res);
      if (!tagisempty(tag))
        setslot(L, L.top - 1, res);
    }
    if (tagisempty(tag))
    {
      TValue key = getslot(L, L.top - 1);
      boolean armed = armk(L, ctx, k, APIK_OVERKEY);
      tag = luaV_finishget(L, t, key, L.top - 1, tag, NOSLOT);
      disarmk(L, armed);
    }
    lua_unlock(L);
    return novariant(tag);
  }

  public static int lua_gettable(lua_State L, int idx)
  {
    return auxgettable(L, idx, 0, null);
  }

  public static int lua_getfield(lua_State L, int idx, HostString k)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    return auxgetstr(L, t, k.to_utf8());
  }

  private static int auxgeti(lua_State L, int idx, long n, long ctx, lua_KFunction k)
  {
    byte tag;
    lua_lock(L);
    TValue t = index2val(L, idx);
    TValue res = new TValue();
    if (!ttistable(t))
      tag = LUA_VNOTABLE;
    else
    {
      Table h = hvalue(t);
      long u = l_intsub(n, 1);
      if (l_ult(u, h.asize()))
      {
        tag = getArrTag(h, u);
        if (!tagisempty(tag))
          farr2val(h, u, tag, res);
      }
      else
        tag = h.luaH_getint(n, res);
    }
    if (tagisempty(tag))
    {
      TValue key = new TValue();
      setivalue(key, n);
      boolean armed = armk(L, ctx, k, APIK_PUSH);
      tag = luaV_finishget(L, t, key, L.top, tag, NOSLOT);
      disarmk(L, armed);
    }
    else
      setslot(L, L.top, res);
    api_incr_top(L);
    lua_unlock(L);
    return novariant(tag);
  }

  public static int lua_geti(lua_State L, int idx, long n)
  {
    return auxgeti(L, idx, n, 0, null);
  }

  private static int finishrawget(lua_State L, byte tag)
  {
    if (tagisempty(tag)) /* avoid copying empty items to the stack */
      setslottag(L, L.top, LUA_VNIL);
    api_incr_top(L);
    lua_unlock(L);
    return novariant(tag);
  }

  private static Table gettable(lua_State L, int idx)
  {
    TValue t = index2val(L, idx);
    assert ttistable(t) : "table expected";
    return hvalue(t);
  }

  public static int lua_rawget(lua_State L, int idx)
  {
    Table t;
    byte tag;
    lua_lock(L);
    api_checkpop(L, 1);
    t = gettable(L, idx);
    {
      TValue key = getslot(L, L.top - 1);
      TValue res = new TValue();
      tag = t.luaH_get(key, res);
      if (!tagisempty(tag))
        setslot(L, L.top - 1, res);
    }
    L.top--; /* pop key */
    return finishrawget(L, tag);
  }

  public static int lua_rawgeti(lua_State L, int idx, long n)
  {
    Table t;
    byte tag;
    lua_lock(L);
    t = gettable(L, idx);
    {
      TValue res = new TValue();
      long u = l_intsub(n, 1);
      if (l_ult(u, t.asize()))
      {
        tag = getArrTag(t, u);
        if (!tagisempty(tag))
          farr2val(t, u, tag, res);
      }
      else
        tag = t.luaH_getint(n, res);
      if (!tagisempty(tag))
        setslot(L, L.top, res);
    }
    return finishrawget(L, tag);
  }

  public static int lua_rawgetp(lua_State L, int idx, Object p)
  {
    Table t;
    TValue k = new TValue();
    lua_lock(L);
    t = gettable(L, idx);
    setpvalue(k, p);
    TValue res = new TValue();
    byte tag = t.luaH_get(k, res);
    if (!tagisempty(tag))
      setslot(L, L.top, res);
    return finishrawget(L, tag);
  }

  public static void lua_createtable(lua_State L, int narray, int nrec)
  {
    Table t;
    lua_lock(L);
    t = Table.luaH_new(L);
    setsloth(L, L.top, t);
    api_incr_top(L);
    if (narray > 0 || nrec > 0)
      t.luaH_resize(L, narray, nrec);
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static int lua_getmetatable(lua_State L, int objindex)
  {
    Table mt;
    int res = 0;
    lua_lock(L);
    TValue obj = index2val(L, objindex);
    switch (ttype(obj))
    {
    case LUA_TTABLE:
      mt = hvalue(obj).metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj).metatable;
      break;
    default:
      mt = L.l_G.mt[ttype(obj)];
      break;
    }
    if (mt != null)
    {
      setsloth(L, L.top, mt);
      api_incr_top(L);
      res = 1;
    }
    lua_unlock(L);
    return res;
  }

  public static int lua_getiuservalue(lua_State L, int idx, int n)
  {
    int t;
    lua_lock(L);
    TValue o = index2val(L, idx);
    assert ttisfulluserdata(o) : "full userdata expected";
    Udata u = uvalue(o);
    if (n <= 0 || n > u.uv.length)
    {
      setslottag(L, L.top, LUA_VNIL);
      t = LUA_TNONE;
    }
    else
    {
      setslot(L, L.top, u.uv[n - 1]);
      t = novariant(slottag(L, L.top));
    }
    api_incr_top(L);
    lua_unlock(L);
    return t;
  }

  /*
  ** set functions (stack -> Lua)
  */

  /*
  ** t[k] = value at the top of the stack (where 'k' is a string)
  */
  private static void auxsetstr(lua_State L, TValue t, ByteView k,
                                long ctx, lua_KFunction kf)
  {
    int hres;
    TString str = TString.luaS_createstr(L, k);
    api_checkpop(L, 1);
    if (!ttistable(t))
      hres = HNOTATABLE;
    else
    {
      TValue val = getslot(L, L.top - 1);
      hres = hvalue(t).luaH_psetstr(str, val);
    }
    if (hres == HOK)
    {
      TValue val = getslot(L, L.top - 1);
      luaV_finishfastset(L, t, val);
      L.top--; /* pop value */
    }
    else
    {
      setslots(L, L.top, str); /* push 'str' (to make it a TValue) */
      api_incr_top(L);
      TValue key = getslot(L, L.top - 1);
      TValue val = getslot(L, L.top - 2);
      boolean armed = armk(L, ctx, kf, APIK_POP2);
      luaV_finishset(L, t, key, val, hres, NOSLOT);
      disarmk(L, armed);
      L.top -= 2; /* pop value and key */
    }
    lua_unlock(L); /* lock done by caller */
  }

  private static void auxsetstr(lua_State L, TValue t, ByteView k)
  {
    auxsetstr(L, t, k, 0, null);
  }

  public static void lua_setglobal(lua_State L, HostString name)
  {
    lua_setglobali(L, name.to_utf8());
  }

  public static void lua_setglobali(lua_State L, ByteView name)
  {
    TValue gt = new TValue();
    lua_lock(L); /* unlock done in 'auxsetstr' */
    getGlobalTable(L, gt);
    auxsetstr(L, gt, name);
  }

  public static void lua_setglobalik(lua_State L, ByteView name, long ctx, lua_KFunction k)
  {
    TValue gt = new TValue();
    lua_lock(L); /* unlock done in 'auxsetstr' */
    getGlobalTable(L, gt);
    auxsetstr(L, gt, name, ctx, k);
  }

  public static void lua_setglobalk(lua_State L, HostString name, long ctx, lua_KFunction k)
  {
    lua_setglobalik(L, name.to_utf8(), ctx, k);
  }

  private static void auxsettable(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    int hres;
    lua_lock(L);
    api_checkpop(L, 2);
    TValue t = index2val(L, idx);
    TValue key = getslot(L, L.top - 2);
    TValue val = getslot(L, L.top - 1);
    if (!ttistable(t))
      hres = HNOTATABLE;
    else
    {
      hres = hvalue(t).luaH_pset(key, val);
    }
    if (hres == HOK)
      luaV_finishfastset(L, t, val);
    else
    {
      boolean armed = armk(L, ctx, k, APIK_POP2);
      luaV_finishset(L, t, key, val, hres, NOSLOT);
      disarmk(L, armed);
    }
    L.top -= 2; /* pop index and value */
    lua_unlock(L);
  }

  public static void lua_settable(lua_State L, int idx)
  {
    auxsettable(L, idx, 0, null);
  }

  public static void lua_setfield(lua_State L, int idx, HostString k)
  {
    lua_lock(L); /* unlock done in 'auxsetstr' */
    TValue t = index2val(L, idx);
    auxsetstr(L, t, k.to_utf8());
  }

  public static int lua_getfieldi(lua_State L, int idx, ByteView k)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    return auxgetstr(L, t, k);
  }

  public static void lua_setfieldi(lua_State L, int idx, ByteView k)
  {
    lua_lock(L); /* unlock done in 'auxsetstr' */
    TValue t = index2val(L, idx);
    auxsetstr(L, t, k);
  }

  private static void auxseti(lua_State L, int idx, long n, long ctx, lua_KFunction k)
  {
    int hres;
    lua_lock(L);
    api_checkpop(L, 1);
    TValue t = index2val(L, idx);
    TValue val = getslot(L, L.top - 1);
    if (!ttistable(t))
      hres = HNOTATABLE;
    else
    {
      Table h = hvalue(t);
      long u = l_intsub(n, 1);
      if (l_ult(u, h.asize()))
      {
        byte tag = getArrTag(h, u);
        if (checknoTM(h.metatable, TM_NEWINDEX) != 0 || !tagisempty(tag))
        {
          fval2arr(h, u, val);
          hres = HOK;
        }
        else
          hres = ~(int) u;
      }
      else
        hres = h.luaH_psetint(n, val);
    }
    if (hres == HOK)
      luaV_finishfastset(L, t, val);
    else
    {
      TValue temp = new TValue();
      setivalue(temp, n);
      boolean armed = armk(L, ctx, k, APIK_POP1);
      luaV_finishset(L, t, temp, val, hres, NOSLOT);
      disarmk(L, armed);
    }
    L.top--; /* pop value */
    lua_unlock(L);
  }

  public static void lua_seti(lua_State L, int idx, long n)
  {
    auxseti(L, idx, n, 0, null);
  }

  /*
  ** {======================================================
  ** Bulk work on plain tables, for the table library: tables with no
  ** metatable, whose reads and writes call nothing ('lua_geti' and
  ** 'lua_seti' do them the same way). A step of the budget each element;
  ** the work stops after the element where the budget is spent (see
  ** 'lua_spend').
  ** =======================================================
  */

  /* is the value at 'idx' a table with no metatable? */
  public static int lua_isplaintable(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    return (ttistable(o) && hvalue(o).metatable == null) ? 1 : 0;
  }

  /* t[n], as 'lua_geti' reads a table with no metatable */
  private static void plaingeti(Table h, long n, TValue res)
  {
    long u = l_intsub(n, 1);
    byte tag;
    if (l_ult(u, h.asize()))
    {
      tag = getArrTag(h, u);
      if (!tagisempty(tag))
        farr2val(h, u, tag, res);
    }
    else
      tag = h.luaH_getint(n, res);
    if (tagisempty(tag))
      setnilvalue(res);
  }

  /* t[n] = val, as 'lua_seti' writes a table with no metatable */
  private static void plainseti(lua_State L, TValue t, long n, TValue val)
  {
    Table h = hvalue(t);
    int hres;
    long u = l_intsub(n, 1);
    if (l_ult(u, h.asize()))
    {
      fval2arr(h, u, val); /* (no '__newindex') */
      hres = HOK;
    }
    else
      hres = h.luaH_psetint(n, val);
    if (hres == HOK)
      luaV_finishfastset(L, t, val);
    else
    {
      TValue key = new TValue();
      setivalue(key, n);
      luaV_finishset(L, t, key, val, hres, NOSLOT);
    }
  }

  /* pushes t[i], t[i + 1], ... (at most 'n'); returns how many */
  public static int lua_plaingetrange(lua_State L, int idx, long i, int n)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    assert ttistable(t) && hvalue(t).metatable == null;
    assert n <= L.stack_last - L.top : "stack overflow";
    int k = 0;
    while (k < n)
    {
      TValue v = new TValue();
      plaingeti(hvalue(t), i + k, v);
      setslot(L, L.top, v);
      L.top++;
      k++;
      if (luaE_spent(L, 1))
        break;
    }
    lua_unlock(L);
    return k;
  }

  /*
  ** t2[d + k] = t1[s + k], for 'k' from 0 up to 'n' - 1 ('up') or from
  ** 'n' - 1 down to 0; returns how many it moved.
  */
  public static long lua_plainmove(lua_State L, int idx1, long s, int idx2, long d,
                                   long n, int up)
  {
    lua_lock(L);
    TValue t1 = index2val(L, idx1);
    TValue t2 = index2val(L, idx2);
    assert ttistable(t1) && hvalue(t1).metatable == null;
    assert ttistable(t2) && hvalue(t2).metatable == null;
    long k = 0;
    while (k < n)
    {
      long off = (up != 0) ? k : n - 1 - k;
      TValue v = new TValue();
      plaingeti(hvalue(t1), s + off, v);
      plainseti(L, t2, d + off, v);
      k++;
      if (luaE_spent(L, 1))
        break;
    }
    lua_unlock(L);
    return k;
  }

  /* }====================================================== */

  private static void aux_rawset(lua_State L, int idx, TValue key, int n)
  {
    Table t;
    lua_lock(L);
    api_checkpop(L, n);
    t = gettable(L, idx);
    TValue val = getslot(L, L.top - 1);
    t.luaH_set(L, key, val);
    invalidateTMcache(t);
    assert novariant(t.tt) >= LUA_TSTRING;
    luaC_barrierback(L, t, val);
    L.top -= n;
    lua_unlock(L);
  }

  public static void lua_rawset(lua_State L, int idx)
  {
    TValue key = getslot(L, L.top - 2);
    aux_rawset(L, idx, key, 2);
  }

  public static void lua_rawsetp(lua_State L, int idx, Object p)
  {
    TValue k = new TValue();
    setpvalue(k, p);
    aux_rawset(L, idx, k, 1);
  }

  public static void lua_rawseti(lua_State L, int idx, long n)
  {
    Table t;
    lua_lock(L);
    api_checkpop(L, 1);
    t = gettable(L, idx);
    TValue val = getslot(L, L.top - 1);
    t.luaH_setint(L, n, val);
    assert novariant(t.tt) >= LUA_TSTRING;
    luaC_barrierback(L, t, val);
    L.top--;
    lua_unlock(L);
  }

  public static int lua_setmetatable(lua_State L, int objindex)
  {
    Table mt;
    lua_lock(L);
    api_checkpop(L, 1);
    TValue obj = index2val(L, objindex);
    if (tagisempty(slottag(L, L.top - 1)))
      mt = null;
    else
    {
      assert slottag(L, L.top - 1) == ctb(LUA_VTABLE) : "table expected";
      mt = slothval(L, L.top - 1);
    }
    switch (ttype(obj))
    {
    case LUA_TTABLE:
    {
      hvalue(obj).metatable = mt;
      if (mt != null)
      {
        assert iscollectable(obj);
        luaC_objbarrier(L, gcvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    case LUA_TUSERDATA:
    {
      uvalue(obj).metatable = mt;
      if (mt != null)
      {
        luaC_objbarrier(L, uvalue(obj), mt);
        assert iscollectable(obj);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    default:
    {
      L.l_G.mt[ttype(obj)] = mt;
      break;
    }
    }
    L.top--;
    lua_unlock(L);
    return 1;
  }

  public static int lua_setiuservalue(lua_State L, int idx, int n)
  {
    int res;
    lua_lock(L);
    api_checkpop(L, 1);
    TValue o = index2val(L, idx);
    assert ttisfulluserdata(o) : "full userdata expected";
    Udata u = uvalue(o);
    if (!(n >= 1 && n <= u.uv.length))
      res = 0;
    else
    {
      TValue v = getslot(L, L.top - 1);
      setobj(L, u.uv[n - 1], v);
      assert iscollectable(o);
      luaC_barrierback(L, gcvalue(o), v);
      res = 1;
    }
    L.top--;
    lua_unlock(L);
    return res;
  }

  /*
  ** 'load' and 'call' functions (run Lua code)
  */

  private static void checkresults(lua_State L, int na, int nr)
  {
    assert (nr == LUA_MULTRET || (nr <= na || L.ci.top >= L.top + (nr - na))) : "results from function overflow current stack size";
    assert LUA_MULTRET <= nr && nr <= MAXRESULTS : "invalid number of results";
  }

  private static final class CallS implements ProtectedCall
  { /* data to 'f_call' */
    int func;
    int nresults;

    @Override
    public void run(lua_State L)
    {
      f_call(L, this);
    }
  }

  private static void f_call(lua_State L, CallS c)
  {
    luaD_callnoyield(L, c.func, c.nresults);
  }

  /*
  ** A call or protected call of the host, from the base level of 'L', where
  ** a freeze of 'L' ends: 'L' keeps the call, to go on with when it thaws
  ** ('lua_thawk'), and the call returns as if its function returned nothing.
  */
  private static void hostfrozen(lua_State L, byte kind, int func, int nresults,
                                 int ef)
  {
    global_State g = L.l_G;
    assert g.freezetarget == L;
    g.freezetarget = null;
    luaE_setfrozen(L, kind);
    L.frozenfunc = func;
    L.frozennres = nresults;
    L.frozenerrfunc = ef;
  }

  /*
  ** The calls with continuations of the running C function freeze but do
  ** not yield: what it calls stays as non-yieldable as in a function without
  ** continuations, while a freeze passes through and the continuation goes
  ** on after the thaw.
  */
  public static void lua_freezeonly(lua_State L)
  {
    assert isLua(L.ci) == 0 && L.ci != L.base_ci;
    luaE_freezeonly(L);
  }

  /*
  ** C work: 'n' steps of the budget. Returns true when the budget is spent:
  ** the running C function then calls 'lua_checkpointk' where it can go on
  ** after a freeze (or leaves the callback to the next budget point).
  */
  public static int lua_spend(lua_State L, int n)
  {
    return luaE_spent(L, n) ? 1 : 0;
  }

  /*
  ** A check point of the running C function, where the budget is spent:
  ** the budget callback runs, and when it freezes the thread, the function
  ** goes on in 'k' after the thaw (with status LUA_YIELD and 'ctx'), as
  ** from a call.
  */
  public static void lua_checkpointk(lua_State L, long ctx, lua_KFunction k)
  {
    lua_lock(L);
    if (L.l_G.budgetL != null)
    { /* (a thread the callback runs: no callback inside the callback, as at
         the budget points of instructions; its budget is seen to later) */
      lua_unlock(L);
      return;
    }
    CallInfo ci = L.ci;
    assert isLua(ci) == 0 && ci != L.base_ci && k != null;
    ci.k = k;
    ci.ctx = ctx;
    lua_State co = luaD_budget(L);
    if (co != null)
      luaD_freeze(L, co); /* ('finishCcall' goes on with 'k') */
    lua_unlock(L);
  }

  /*
  ** Calls through C that may freeze: a call by the host (from the base level)
  ** ends a freeze; a call with a continuation lets a freeze pass through,
  ** even where it cannot yield (CIST_NYCALL counts it); a call without one
  ** cannot freeze.
  */
  public static void lua_callk(lua_State L, int nargs, int nresults,
                               long ctx, lua_KFunction k)
  {
    int func;
    lua_lock(L);
    assert k == null || isLua(L.ci) == 0 : "cannot use continuations inside hooks";
    api_checkpop(L, nargs + 1);
    assert L.status == LUA_OK : "cannot do calls on non-normal thread";
    checkresults(L, nargs, nresults);
    func = L.top - (nargs + 1);
    CallInfo ci = L.ci;
    luaE_spend(L, 1); /* a hand-off */
    if (ci == L.base_ci)
    { /* a call of the host */
      CallS c = new CallS();
      c.func = func;
      c.nresults = nresults;
      byte status = luaD_rawrunprotected(L, c);
      if (status == LUA_FREEZE && L.l_G.freezekill)
      { /* its chain is destroyed: the call fails */
        boolean landed = luaD_killed(L);
        assert landed;
        L.top--; /* (one error object) */
        luaD_throw(L, (byte) LUA_ERRRUN);
      }
      if (status == LUA_FREEZE)
      {
        hostfrozen(L, FZ_CALL, func, nresults, L.errfunc);
        lua_unlock(L);
        return;
      }
      if (status != LUA_OK)
        luaD_throw(L, status); /* not protected */
    }
    else if (k == null)
    { /* a call that cannot freeze */
      L.nfz++;
      luaD_callnoyield(L, func, nresults);
      L.nfz--;
    }
    else
    {
      ci.k = k;     /* save continuation */
      ci.ctx = ctx; /* save context */
      if (yieldablek(L, ci))
        luaD_call(L, func, nresults); /* do the call */
      else
      {
        ci.callstatus |= CIST_NYCALL;
        luaD_callnoyield(L, func, nresults);
        ci.callstatus &= ~CIST_NYCALL;
      }
    }
    adjustresults(L, nresults);
    lua_unlock(L);
  }

  public static int lua_gettablek(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    return auxgettable(L, idx, ctx, k);
  }

  public static int lua_getfieldik(lua_State L, int idx, ByteView key, long ctx, lua_KFunction k)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    return auxgetstr(L, t, key, ctx, k);
  }

  public static void lua_setfieldik(lua_State L, int idx, ByteView key, long ctx, lua_KFunction k)
  {
    lua_lock(L); /* unlock done in 'auxsetstr' */
    TValue t = index2val(L, idx);
    auxsetstr(L, t, key, ctx, k);
  }

  public static int lua_getfieldk(lua_State L, int idx, HostString key, long ctx, lua_KFunction k)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    return auxgetstr(L, t, key.to_utf8(), ctx, k);
  }

  public static int lua_getik(lua_State L, int idx, long n, long ctx, lua_KFunction k)
  {
    return auxgeti(L, idx, n, ctx, k);
  }

  public static void lua_settablek(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    auxsettable(L, idx, ctx, k);
  }

  public static void lua_setfieldk(lua_State L, int idx, HostString key, long ctx, lua_KFunction k)
  {
    lua_lock(L); /* unlock done in 'auxsetstr' */
    TValue t = index2val(L, idx);
    auxsetstr(L, t, key.to_utf8(), ctx, k);
  }

  public static void lua_setik(lua_State L, int idx, long n, long ctx, lua_KFunction k)
  {
    auxseti(L, idx, n, ctx, k);
  }

  public static int lua_comparek(lua_State L, int idx1, int idx2, int op, long ctx, lua_KFunction k)
  {
    idx1 = lua_absindex(L, idx1);
    idx2 = lua_absindex(L, idx2);
    TValue o1 = index2val(L, idx1);
    TValue o2 = index2val(L, idx2);
    boolean mayrun; /* may it call a metamethod? */
    if (op == LUA_OPEQ)
      mayrun = ttype(o1) == ttype(o2) && (ttistable(o1) || ttisfulluserdata(o1));
    else
      mayrun = !((ttisnumber(o1) && ttisnumber(o2)) || (ttisstring(o1) && ttisstring(o2)));
    boolean armed = mayrun && armk(L, ctx, k, APIK_BOOL);
    int r = lua_compare(L, idx1, idx2, op);
    disarmk(L, armed);
    lua_pushboolean(L, r);
    return r;
  }

  public static void lua_lenk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    TValue o = index2val(L, idx);
    boolean mayrun = !(ttisstring(o) || (ttistable(o) && hvalue(o).metatable == null));
    boolean armed = mayrun && armk(L, ctx, k, APIK_PUSH);
    lua_len(L, idx);
    disarmk(L, armed);
  }

  public static void lua_arithk(lua_State L, int op, long ctx, lua_KFunction k)
  {
    boolean armed = armk(L, ctx, k, APIK_ARITH);
    lua_arith(L, op);
    disarmk(L, armed);
  }

  public static void lua_concatk(lua_State L, int n, long ctx, lua_KFunction k)
  {
    boolean armed = armk(L, ctx, k, APIK_CONCAT);
    if (armed)
      L.ci.apifirst = L.top - n;
    lua_concat(L, n);
    disarmk(L, armed);
  }

  /*
  ** Execute a protected call.
  */
  /*
  ** A protected call with a continuation where the thread cannot yield: a
  ** 'conventional' protected call; a freeze passing through makes it one the
  ** thread recovers itself, as when it can yield.
  */
  private static byte pcallnoyield(lua_State L, CallS cp, int func, int nresults,
                                   long ctx, lua_KFunction k)
  {
    CallS c = cp;
    CallInfo ci = L.ci;
    byte status;
    ci.k = k;     /* save continuation */
    ci.ctx = ctx; /* save context */
    c.nresults = nresults;
    int olderrfunc = L.errfunc;
    byte oldah = L.allowhook;
    status = luaD_pcall(L, c, c.func, func, false);
    if (status == LUA_FREEZE)
    {
      ci.funcidx = (int) c.func;
      ci.old_errfunc = olderrfunc;
      L.errfunc = func;
      setoah(ci, oldah);
      ci.callstatus |= CIST_YPCALL | CIST_NYCALL;
      luaD_throw(L, LUA_FREEZE); /* go on freezing */
    }
    if (status != LUA_OK)
    { /* it failed: close its variables so that a '__close' can freeze, with the
         frame one that recovers, as when the thread can yield (its error is
         in 'finishpcallk'); a '__close' still cannot yield here */
      ci.funcidx = (int) c.func;
      ci.old_errfunc = olderrfunc;
      setoah(ci, oldah);
      setcistrecst(ci, status);
      ci.callstatus |= CIST_YPCALL | CIST_NYCALL;
      status = luaD_closeprotected(L, c.func, status, 2); /* (freezes, cannot yield) */
      luaD_seterrorobj(L, status, c.func);
      luaD_shrinkstack(L); /* restore stack size in case of overflow */
      ci.callstatus &= ~(CIST_YPCALL | CIST_NYCALL);
      setcistrecst(ci, LUA_OK);
      L.errfunc = olderrfunc;
    }
    return status;
  }

  public static int lua_pcallk(lua_State L, int nargs, int nresults, int errfunc,
                               long ctx, lua_KFunction k)
  {
    CallS c = new CallS();
    byte status;
    int func;
    lua_lock(L);
    assert k == null || isLua(L.ci) == 0 : "cannot use continuations inside hooks";
    api_checkpop(L, nargs + 1);
    assert L.status == LUA_OK : "cannot do calls on non-normal thread";
    checkresults(L, nargs, nresults);
    if (errfunc == 0)
      func = 0;
    else
    {
      int o = index2stack(L, errfunc);
      assert novariant(slottag(L, o)) == LUA_TFUNCTION : "error handler must be a function";
      func = o;
    }
    c.func = L.top - (nargs + 1); /* function to be called */
    CallInfo ci = L.ci;
    luaE_spend(L, 1); /* a hand-off */
    if (ci == L.base_ci)
    { /* a call of the host */
      c.nresults = nresults;
      int olderrfunc = L.errfunc;
      status = luaD_pcall(L, c, c.func, func, false);
      if (status != LUA_OK && status != LUA_FREEZE)
      { /* it failed: its variables close, where a '__close' can freeze */
        status = luaD_hostclose(L, c.func, status);
        if (status != LUA_FREEZE)
        {
          luaD_seterrorobj(L, status, c.func);
          luaD_shrinkstack(L); /* restore stack size in case of overflow */
          L.errfunc = olderrfunc;
        }
        else if (!L.l_G.freezekill)
        { /* a freeze lands here: frozen in the close, which a thaw finishes */
          hostfrozen(L, FZ_HOSTCLOSE, c.func, nresults, L.errfunc);
          L.errfunc = olderrfunc;
          lua_unlock(L);
          return LUA_OK;
        }
      }
      if (status == LUA_FREEZE && L.l_G.freezekill)
      { /* its chain is destroyed: the call fails */
        boolean landed = luaD_killed(L);
        assert landed;
        L.top--; /* (one error object) */
        L.errfunc = olderrfunc;
        lua_unlock(L);
        return LUA_ERRRUN;
      }
      if (status == LUA_FREEZE)
      {
        hostfrozen(L, FZ_PCALL, c.func, nresults, L.errfunc); /* (the handler in effect where it froze) */
        L.errfunc = olderrfunc;
        lua_unlock(L);
        return LUA_OK;
      }
    }
    else if (k == null)
    { /* a call that cannot freeze */
      c.nresults = nresults;
      L.nfz++;
      status = luaD_pcall(L, c, c.func, func);
      L.nfz--;
      if (status == LUA_FREEZE)
        luaD_throw(L, LUA_FREEZE); /* (the chain is destroyed: nothing here freezes) */
    }
    else if (!yieldablek(L, ci))
      status = pcallnoyield(L, c, func, nresults, ctx, k);
    else
    { /* prepare continuation (call is already protected by 'resume') */
      ci.k = k;     /* save continuation */
      ci.ctx = ctx; /* save context */
      /* save information for error recovery */
      ci.funcidx = (int) c.func;
      ci.old_errfunc = L.errfunc;
      L.errfunc = func;
      setoah(ci, L.allowhook);       /* save value of 'allowhook' */
      ci.callstatus |= CIST_YPCALL;  /* function can do error recovery */
      luaD_call(L, c.func, nresults); /* do the call */
      ci.callstatus &= ~CIST_YPCALL;
      L.errfunc = ci.old_errfunc;
      status = LUA_OK; /* if it is here, there were no errors */
    }
    adjustresults(L, nresults);
    lua_unlock(L);
    return status;
  }

  /*
  ** A protected call from a C function, where the thread cannot yield, that
  ** keeps the message handler in effect: an error calls it where it happens,
  ** as if the call were not protected (the reader function of 'load', called
  ** inside the parse). A freeze can pass through: 'k' goes on after the thaw.
  */
  public static int lua_pcallkeep(lua_State L, int nargs, int nresults, long ctx, lua_KFunction k)
  {
    CallS c = new CallS();
    lua_lock(L);
    api_checkpop(L, nargs + 1);
    checkresults(L, nargs, nresults);
    assert k != null && L.ci != L.base_ci && isLua(L.ci) == 0 && !yieldablek(L, L.ci);
    c.func = L.top - (nargs + 1);
    luaE_spend(L, 1); /* a hand-off */
    byte status = pcallnoyield(L, c, L.errfunc, nresults, ctx, k);
    adjustresults(L, nresults);
    lua_unlock(L);
    return status;
  }

  /* the function a load made, on the top, gets the global table as its environment */
  private static void setloadenv(lua_State L)
  {
    int s = L.top - 1;
    LClosure f = slotclLval(L, s); /* get new function */
    if (f.upvals.length >= 1)
    { /* does it have an upvalue? */
      /* get global table from registry */
      TValue gt = new TValue();
      getGlobalTable(L, gt);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      upvalsetval(L, f.upvals[0], gt);
      luaC_barrier(L, f.upvals[0], gt);
    }
  }

  public static int lua_loadi(lua_State L, lua_Reader reader, ByteView chunkname, ByteView mode)
  {
    ZIO z = new ZIO();
    byte status;
    lua_lock(L);
    luaC_checkGC(L);
    luaZ_init(L, z, reader);
    status = luaD_protectedparser(L, z, chunkname, mode);
    if (status == LUA_OK) /* no errors? */
      setloadenv(L);
    lua_unlock(L);
    return status;
  }

  public static int lua_loadk(lua_State L, lua_LoadSpec ls)
  {
    lua_lock(L);
    luaC_checkGC(L);
    lua_unlock(L);
    int status = luaZ_loadk(L, ls, false); /* (it locks around the parse) */
    if (status == LUA_OK)
    {
      lua_lock(L);
      setloadenv(L);
      lua_unlock(L);
    }
    return status;
  }

  public static int lua_loadkread(lua_State L, lua_LoadSpec ls, lua_Reader source)
  {
    if ((L.hookmask & LUA_MASKBUDGET) == 0)
      return 0;
    luaZ_loadread(L, ls, source);
    return 1;
  }

  public static int lua_loadkresume(lua_State L, int status, lua_LoadSpec ls)
  {
    if (status != LUA_OK && status != LUA_YIELD)
      return status; /* (the reader function failed after the thaw: its error on the top) */
    status = luaZ_loadk(L, ls, true);
    if (status == LUA_OK)
    {
      lua_lock(L);
      setloadenv(L);
      lua_unlock(L);
    }
    return status;
  }

  public static int lua_load(lua_State L, lua_Reader reader,
                             HostString chunkname,
                             HostString mode)
  {
    ByteView name = (chunkname != null) ? chunkname.to_utf8() : null;
    ByteView m = (mode != null) ? mode.to_utf8() : null;
    return lua_loadi(L, reader, name, m);
  }

  /*
  ** Dump a Lua function, calling 'writer' to write its parts. Ensure
  ** the stack returns with its original size.
  */

  public static int lua_dump(lua_State L, lua_Writer writer, int strip)
  {
    int status;
    int otop = L.top; /* original top */
    int f = L.top - 1;           /* function to be dumped */
    lua_lock(L);
    api_checkpop(L, 1);
    assert slottag(L, f) == ctb(LUA_VLCL) : "Lua function expected";
    status = luaU_dump(L, slotclLval(L, f).p, writer, strip);
    L.top = (otop); /* restore top */
    lua_unlock(L);
    return status;
  }

  public static int lua_status(lua_State L)
  {
    return L.status;
  }

  /* is the thread running a '__gc' that its group's thread called (see 'gcdrain')? */
  private static boolean inuserfinalizer(lua_State L)
  {
    for (CallInfo ci = L.ci; ci != null; ci = ci.previous)
      if ((ci.callstatus & CIST_FIN) != 0)
        return true;
    return false;
  }

  public static int lua_gc(lua_State L, int what, long stepsize, int param, int value)
  {
    int res = 0;
    global_State g = L.l_G;
    if ((g.gcstp & (GCSTPGC | GCSTPCLS)) != 0) /* internal stop? */
      return -1;                               /* all options are invalid when stopped */
    if (inuserfinalizer(L))                    /* (the collector is not stopped for it: it can freeze) */
      return -1;                               /* but as in stock Lua, the options are invalid there */
    lua_lock(L);
    switch (what)
    {
    case LUA_GCSTOP:
    {
      g.gcstp = (byte) GCSTPUSR; /* stopped by the user */
      break;
    }
    case LUA_GCRESTART:
    {
      luaE_setdebt(g, 0);
      g.gcstp = 0; /* (other bits must be zero here) */
      break;
    }
    case LUA_GCCOLLECT:
    {
      luaC_fullgc(L, 0);
      break;
    }
    case LUA_GCCOUNT:
    {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = (int) (gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB:
    {
      res = (int) (gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCSTEP:
    {
      byte oldstp = g.gcstp;
      long n = stepsize;
      long newdebt;
      int work = 0; /* true if GC did some work */
      g.gcstp = 0;  /* allow GC to run (other bits must be zero here) */
      if (n <= 0)
        newdebt = 0;                      /* force to run one basic step */
      else if (g.GCdebt >= n - MAX_LMEM) /* no overflow? */
        newdebt = g.GCdebt - n;
      else                   /* overflow */
        newdebt = -MAX_LMEM; /* set debt to miminum value */
      luaE_setdebt(g, newdebt);
      {
        if (L.l_G.GCdebt <= 0)
        {
          luaC_step(L);
          work = 1;
        }
      }
      if (work != 0 && g.gcstate == GCSpause) /* end of cycle? */
        res = 1;                               /* signal it */
      g.gcstp = oldstp;                        /* restore previous state */
      break;
    }
    case LUA_GCISRUNNING:
    {
      res = gcrunning(g);
      break;
    }
    case LUA_GCGEN:
    {
      res = (g.gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_GENMINOR);
      break;
    }
    case LUA_GCINC:
    {
      res = (g.gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_INC);
      break;
    }
    case LUA_GCPARAM:
    {
      assert 0 <= param && param < LUA_GCPN : "invalid parameter";
      res = (int) luaO_applyparam(g.gcparams[param], 100);
      if (value >= 0)
        g.gcparams[param] = luaO_codeparam(value);
      break;
    }
    default:
      res = -1; /* invalid option */
    }
    lua_unlock(L);
    return res;
  }

  /*
  ** miscellaneous functions
  */

  public static int lua_error(lua_State L)
  {
    lua_lock(L);
    TValue errobj = getslot(L, L.top - 1);
    api_checkpop(L, 1);
    /* error object is the memory error message? */
    if (ttisstring(errobj) &&
        eqshrstr(tsvalue(errobj), L.l_G.memerrmsg) != 0)
      luaD_throw(L, (byte) LUA_ERRMEM); /* raise a memory error */
    else
      luaG_errormsg(L); /* raise a regular error */
    /* code unreachable; will unlock when control actually leaves the kernel */
    return 0; /* to avoid warnings */
  }

  public static int lua_next(lua_State L, int idx)
  {
    Table t;
    int more;
    lua_lock(L);
    api_checkpop(L, 1);
    t = gettable(L, idx);
    more = t.luaH_next(L, L.top - 1);
    if (more != 0)
      api_incr_top(L);
    else      /* no more elements */
      L.top--; /* pop key */
    lua_unlock(L);
    return more;
  }

  public static void lua_toclose(lua_State L, int idx)
  {
    int o;
    lua_lock(L);
    o = index2stack(L, idx);
    assert L.tbclist < o : "given index below or equal a marked one";
    luaF_newtbcupval(L, o);        /* create new to-be-closed upvalue */
    L.ci.callstatus |= CIST_TBC; /* mark that function has TBC slots */
    lua_unlock(L);
  }

  public static void lua_concat(lua_State L, int n)
  {
    lua_lock(L);
    api_checknelems(L, n);
    if (n > 0)
    {
      luaV_concat(L, n);
      luaC_checkGC(L);
    }
    else
    {                                                                   /* nothing to concatenate */
      setslots(L, L.top, TString.luaS_createstr(L, Lt.empty_str)); /* push empty string */
      api_incr_top(L);
    }
    lua_unlock(L);
  }

  public static void lua_len(lua_State L, int idx)
  {
    lua_lock(L);
    TValue t = index2val(L, idx);
    luaV_objlen(L, L.top, t, NOSLOT);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static lua_Alloc lua_getallocf(lua_State L)
  {
    lua_lock(L);
    lua_Alloc a = L.l_G.frealloc;
    lua_unlock(L);
    return a;
  }

  public static void lua_setallocf(lua_State L, lua_Alloc f)
  {
    lua_lock(L);
    L.l_G.frealloc = f;
    lua_unlock(L);
  }

  public static void lua_setwarnf(lua_State L, lua_WarnFunction f)
  {
    lua_lock(L);
    L.l_G.warnf = f;
    lua_unlock(L);
  }

  public static void lua_warning(lua_State L, HostString msg, int tocont)
  {
    lua_warningi(L, msg.to_utf8(), tocont);
  }

  public static void lua_warningi(lua_State L, ByteView msg, int tocont)
  {
    lua_lock(L);
    luaE_warning(L, msg, tocont);
    lua_unlock(L);
  }

  public static void lua_newuserobject(lua_State L, lua_UserObject obj, long size, int nuvalue)
  {
    Udata u;
    lua_lock(L);
    assert 0 <= nuvalue && nuvalue < Short.MAX_VALUE : "invalid value";
    try
    {
      u = Udata.luaS_newudata(L, 0, nuvalue);
      setslotu(L, L.top, u);
      api_incr_top(L);
      luaM_takeobj(L, size);
    }
    catch (Throwable e)
    {
      obj.freed(L);
      throw e;
    }
    u.objsize = size;
    u.obj = obj;
    luaC_checkGC(L);
    lua_unlock(L);
  }

  public static lua_UserObject lua_touserobject(lua_State L, int idx)
  {
    TValue o = index2val(L, idx);
    if (!checktype(o, LUA_TUSERDATA))
      return null;
    return uvalue(o).obj;
  }

  public static Object lua_newuserdatauv(lua_State L, int size, int nuvalue)
  {
    Udata u;
    lua_lock(L);
    assert 0 <= nuvalue && nuvalue < Short.MAX_VALUE : "invalid value";
    u = Udata.luaS_newudata(L, size, nuvalue);
    setslotu(L, L.top, u);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
    return getudatamem(u);
  }

  private static final class UpvalAddr
  {
    TValue row;
    UpVal uv;
    GCObject owner;

    UpvalAddr(TValue row, UpVal uv, GCObject owner)
    {
      assign(row, uv, owner);
    }

    void assign(TValue row, UpVal uv, GCObject owner)
    {
      this.row = row;
      this.uv = uv;
      this.owner = owner;
    }
  }

  private static InternalString aux_upvalue(TValue fi, int n, UpvalAddr addr)
  {
    switch (ttypetag(fi))
    {
    case LUA_VCCL:
    { /* C closure */
      CClosure f = clCvalue(fi);
      if (!(n >= 1 && n <= f.upvalue.length))
        return null; /* 'n' not in [1, f->nupvalues] */
      assert novariant(f.tt) >= LUA_TSTRING;
      addr.assign(f.upvalue[n - 1], null, f);
      return Lt.empty_str_i;
    }
    case LUA_VLCL:
    { /* Lua closure */
      LClosure f = clLvalue(fi);
      TString name;
      Proto p = f.p;
      if (!(n >= 1 && n <= p.upvalues.length))
        return null; /* 'n' not in [1, p->sizeupvalues] */
      assert novariant(f.upvals[n - 1].tt) >= LUA_TSTRING;
      addr.assign(null, f.upvals[n - 1], f.upvals[n - 1]);
      name = p.upvalues[n - 1].name;
      return (name == null) ? Lt.no_name_i : name.data;
    }
    default:
      return null; /* not a closure */
    }
  }

  public static InternalString lua_getupvaluei(lua_State L, int funcindex, int n)
  {
    InternalString name;
    UpvalAddr addr = new UpvalAddr(null, null, null);
    lua_lock(L);
    TValue fi = index2val(L, funcindex);
    name = aux_upvalue(fi, n, addr);
    if (name != null)
    {
      TValue v = (addr.uv != null) ? upvalgetval(addr.uv) : getobj(addr.row);
      setslot(L, L.top, v);
      api_incr_top(L);
    }
    lua_unlock(L);
    return name;
  }

  public static HostString lua_getupvalue(lua_State L, int funcindex, int n)
  {
    InternalString name = lua_getupvaluei(L, funcindex, n);
    if (name == null)
      return null;
    return new HostString(name.view());
  }

  public static InternalString lua_setupvaluei(lua_State L, int funcindex, int n)
  {
    InternalString name;
    UpvalAddr addr = new UpvalAddr(null, null, null);
    lua_lock(L);
    TValue fi = index2val(L, funcindex);
    api_checknelems(L, 1);
    name = aux_upvalue(fi, n, addr);
    if (name != null)
    {
      L.top--;
      TValue v = getslot(L, L.top);
      if (addr.uv != null)
        upvalsetval(L, addr.uv, v);
      else
      {
        setobj(L, addr.row, v);
      }
      luaC_barrier(L, addr.owner, v);
    }
    lua_unlock(L);
    return name;
  }

  public static HostString lua_setupvalue(lua_State L, int funcindex, int n)
  {
    InternalString name = lua_setupvaluei(L, funcindex, n);
    if (name == null)
      return null;
    return new HostString(name.view());
  }

  private static UpVal getupval(lua_State L, int fidx, int n)
  {
    TValue fi = index2val(L, fidx);
    assert ttisLclosure(fi) : "Lua function expected";
    LClosure f = clLvalue(fi);
    if (1 <= n && n <= f.p.upvalues.length)
      return f.upvals[n - 1]; /* get its upvalue */
    else
      return null;
  }

  public static Object lua_upvalueid(lua_State L, int fidx, int n)
  {
    TValue fi = index2val(L, fidx);
    switch (ttypetag(fi))
    {
    case LUA_VLCL:
    { /* lua closure */
      return getupval(L, fidx, n);
    }
    case LUA_VCCL:
    { /* C closure */
      CClosure f = clCvalue(fi);
      if (1 <= n && n <= f.upvalue.length)
        return f.upvalue[n - 1];
      /* else */
    } /* FALLTHROUGH */
    case LUA_VLCF:
      return null; /* light C functions have no upvalues */
    default:
    {
      assert false : "function expected";
      return null;
    }
    }
  }

  public static void lua_upvaluejoin(lua_State L, int fidx1, int n1,
                                     int fidx2, int n2)
  {
    UpVal uv1 = getupval(L, fidx1, n1);
    UpVal uv2 = getupval(L, fidx2, n2);
    TValue fi1 = index2val(L, fidx1);
    LClosure f1 = clLvalue(fi1);
    assert uv1 != null && uv2 != null : "invalid upvalue index";
    f1.upvals[n1 - 1] = uv2;
    luaC_objbarrier(L, f1, uv2);
  }

  public static int lua_upvalueindex(int i)
  {
    return LUA_REGISTRYINDEX - i;
  }

  public static void lua_call(lua_State L, int nargs, int nresults)
  {
    lua_callk(L, nargs, nresults, 0, null);
  }

  public static int lua_pcall(lua_State L, int nargs, int nresults, int errfunc)
  {
    return lua_pcallk(L, nargs, nresults, errfunc, 0, null);
  }

  public static int lua_yield(lua_State L, int nresults)
  {
    return lua_yieldk(L, nresults, 0, null);
  }

  public static Object lua_getextra(lua_State L)
  {
    return L.extra;
  }

  public static void lua_setextra(lua_State L, Object p)
  {
    L.extra = p;
  }

  public static double lua_tonumber(lua_State L, int idx)
  {
    return lua_tonumberx(L, idx).orElse(0);
  }

  public static long lua_tointeger(lua_State L, int idx)
  {
    return lua_tointegerx(L, idx).orElse(0);
  }

  public static void lua_pop(lua_State L, int n)
  {
    lua_settop(L, -n - 1);
  }

  public static void lua_newtable(lua_State L)
  {
    lua_createtable(L, 0, 0);
  }

  public static void lua_pushcfunction(lua_State L, lua_CFunction f)
  {
    lua_pushcclosure(L, f, 0);
  }

  public static CFuncInfo lua_createcfuncinfo(lua_State L, lua_CFunction fn)
  {
    return CFuncInfo.luaF_newCFuncInfo(L, fn);
  }

  public static void lua_pushcfuncinfo(lua_State L, CFuncInfo cfi)
  {
    lua_lock(L);
    setslotf(L, L.top, cfi);
    api_incr_top(L);
    lua_unlock(L);
  }

  public static void lua_register(lua_State L, HostString name, lua_CFunction f)
  {
    lua_pushcfunction(L, f);
    lua_setglobal(L, name);
  }

  public static int lua_isfunction(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TFUNCTION) ? 1 : 0;
  }

  public static int lua_istable(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TTABLE) ? 1 : 0;
  }

  public static int lua_islightuserdata(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TLIGHTUSERDATA) ? 1 : 0;
  }

  public static int lua_isnil(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TNIL) ? 1 : 0;
  }

  public static int lua_isboolean(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TBOOLEAN) ? 1 : 0;
  }

  public static int lua_isthread(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TTHREAD) ? 1 : 0;
  }

  public static int lua_isnone(lua_State L, int idx)
  {
    return (lua_type(L, idx) == LUA_TNONE) ? 1 : 0;
  }

  public static int lua_isnoneornil(lua_State L, int idx)
  {
    return (lua_type(L, idx) <= 0) ? 1 : 0;
  }

  public static void lua_pushliteral(lua_State L, HostString s)
  {
    lua_pushstring(L, s);
  }

  public static void lua_pushglobaltable(lua_State L)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  }

  public static HostString lua_tostring(lua_State L, int idx)
  {
    ByteView s = lua_tolstring(L, idx);
    if (s == null)
      return null;
    return new HostString(s);
  }

  public static void lua_insert(lua_State L, int idx)
  {
    lua_rotate(L, idx, 1);
  }

  public static void lua_remove(lua_State L, int idx)
  {
    lua_rotate(L, idx, -1);
    lua_pop(L, 1);
  }

  public static void lua_replace(lua_State L, int idx)
  {
    lua_copy(L, -1, idx);
    lua_pop(L, 1);
  }

  public static Object lua_newuserdata(lua_State L, int sz)
  {
    return lua_newuserdatauv(L, sz, 1);
  }

  public static int lua_resetthread(lua_State L)
  {
    return lua_closethread(L, null);
  }

  /* }====================================================================== */
}
