package com.payonel.lungolua.runtime;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;
import static com.payonel.lungolua.vm.Lvm.*;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;

public final class Ltm
{
  private Ltm()
  {
  }

  /*
   * WARNING: if you change the order of this enumeration,
   * grep "ORDER TM" and "ORDER OP"
   */
  public static final int TM_INDEX = 0;
  public static final int TM_NEWINDEX = 1;
  public static final int TM_GC = 2;
  public static final int TM_MODE = 3;
  public static final int TM_LEN = 4;
  public static final int TM_EQ = 5; /* last tag method with fast access */
  public static final int TM_ADD = 6;
  public static final int TM_SUB = 7;
  public static final int TM_MUL = 8;
  public static final int TM_MOD = 9;
  public static final int TM_POW = 10;
  public static final int TM_DIV = 11;
  public static final int TM_IDIV = 12;
  public static final int TM_BAND = 13;
  public static final int TM_BOR = 14;
  public static final int TM_BXOR = 15;
  public static final int TM_SHL = 16;
  public static final int TM_SHR = 17;
  public static final int TM_UNM = 18;
  public static final int TM_BNOT = 19;
  public static final int TM_LT = 20;
  public static final int TM_LE = 21;
  public static final int TM_CONCAT = 22;
  public static final int TM_CALL = 23;
  public static final int TM_CLOSE = 24;
  public static final int TM_N = 25; /* number of elements in the enum */

  /*
  ** Mask with 1 in all fast-access methods. A 1 in any of these bits
  ** in the flag of a (meta)table means the metatable does not have the
  ** corresponding metamethod field. (Bit 6 of the flag indicates that
  ** the table is using the dummy node.)
  */
  public static final byte maskflags = (byte) ~(~0 << (TM_EQ + 1));

  /*
  ** Test whether there is no tagmethod.
  ** (Because tagmethods use raw accesses, the result may be an "empty" nil.)
  */
  public static int notm(TValue tm)
  {
    return (ttype(tm) == LUA_TNIL) ? 1 : 0;
  }

  public static int checknoTM(Table mt, int e)
  {
    return (mt == null || (mt.flags & (1 << e)) != 0) ? 1 : 0;
  }

  public static ByteView ttypename(int x)
  {
    return luaT_typenames_[x + 1];
  }

  public static final ByteView[] luaT_typenames_ = {
      Lt.no_value,
      Lt.nil, Lt.boolean_kw, Lt.userdata, Lt.number_2,
      Lt.string_3, Lt.table, Lt.function, Lt.userdata,
      Lt.thread, Lt.upvalue, Lt.proto, Lt.budget /* these last cases are used for tests only */
  };

  private static final ByteView[] luaT_eventname = {/* ORDER TM */
      Lt.mm_index, Lt.mm_newindex,
      Lt.mm_gc, Lt.mm_mode, Lt.mm_len, Lt.mm_eq,
      Lt.mm_add, Lt.mm_sub, Lt.mm_mul, Lt.mm_mod, Lt.mm_pow,
      Lt.mm_div, Lt.mm_idiv,
      Lt.mm_band, Lt.mm_bor, Lt.mm_bxor, Lt.mm_shl, Lt.mm_shr,
      Lt.mm_unm, Lt.mm_bnot, Lt.mm_lt, Lt.mm_le,
      Lt.mm_concat, Lt.mm_call, Lt.mm_close};

  public static void luaT_init(lua_State L)
  {
    assert luaT_eventname.length == TM_N;
    for (int i = 0; i < TM_N; i++)
    {
      ByteView bytes = luaT_eventname[i];
      L.l_G.tmname[i] = TString.luaS_createstr(L, bytes);
      assert novariant(L.l_G.tmname[i].tt) >= LUA_TSTRING;
      luaC_fix(L, L.l_G.tmname[i]); /* never collect these names */
    }
  }

  /*
  ** function to be used with macro "fasttm": optimized for absence of
  ** tag methods
  */
  public static TValue luaT_gettm(Table events, int event, TString ename)
  {
    TValue tm = events.luaH_Hgetshortstr(ename);
    assert event <= TM_EQ;
    if (notm(tm) != 0)
    {                                      /* no tag method? */
      events.flags |= (byte) (1 << event); /* cache this fact */
      return null;
    }
    else
      return tm;
  }

  public static TValue luaT_gettmbyobj(lua_State L, TValue o, int event)
  {
    Table mt;
    switch (ttype(o))
    {
    case LUA_TTABLE:
      mt = hvalue(o).metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(o).metatable;
      break;
    default:
      mt = L.l_G.mt[ttype(o)];
    }
    return (mt != null ? mt.luaH_Hgetshortstr(L.l_G.tmname[event]) : L.l_G.nilvalue);
  }

  public static TValue luaT_gettmbyobj(lua_State L, int o, int event)
  {
    TValue v = getslot(L, o);
    return luaT_gettmbyobj(L, v, event);
  }

  /*
  ** Return the name of the type of an object. For tables and userdata
  ** with metatable, use their '__name' metafield, if present.
  */
  public static ByteView luaT_objtypename(lua_State L, TValue o)
  {
    Table mt;
    if ((ttistable(o) && (mt = hvalue(o).metatable) != null) ||
        (ttisfulluserdata(o) && (mt = uvalue(o).metatable) != null))
    {
      TValue name = mt.luaH_Hgetshortstr(TString.luaS_createstr(L, Lt.mm_name));
      if (ttisstring(name))           /* is '__name' a string? */
      {
        return tsvalue(name).data.view(); /* use it as type name */
      }
    }
    return ttypename(ttype(o)); /* else use standard type name */
  }

  public static void luaT_callTM(lua_State L, TValue f, TValue p1,
                                 TValue p2, TValue p3)
  {
    int func = L.top;
    setslot(L, func, f);      /* push function (assume EXTRA_STACK) */
    setslot(L, func + 1, p1); /* 1st argument */
    setslot(L, func + 2, p2); /* 2nd argument */
    setslot(L, func + 3, p3); /* 3rd argument */
    L.top = func + 4;
    /* metamethod may yield only when called from Lua code, or from an API
       call with a continuation that can yield; it may freeze also from one
       that cannot (CIST_NYCALL counts it) */
    int st = L.ci.callstatus & (CIST_APIK | CIST_NYCALL);
    if (isLuacode(L.ci) != 0 || st == CIST_APIK)
      luaD_call(L, func, 0);
    else if (st != 0)
      luaD_callnoyield(L, func, 0);
    else
    { /* (cannot freeze) */
      L.nfz++;
      luaD_callnoyield(L, func, 0);
      L.nfz--;
    }
  }

  public static byte luaT_callTMres(lua_State L, TValue f, TValue p1,
                                    TValue p2, int res)
  {
    int result = res;
    int func = L.top;
    setslot(L, func, f);      /* push function (assume EXTRA_STACK) */
    setslot(L, func + 1, p1); /* 1st argument */
    setslot(L, func + 2, p2); /* 2nd argument */
    L.top += 3;
    /* metamethod may yield only when called from Lua code, or from an API
       call with a continuation that can yield; it may freeze also from one
       that cannot (CIST_NYCALL counts it) */
    int st = L.ci.callstatus & (CIST_APIK | CIST_NYCALL);
    if (isLuacode(L.ci) != 0 || st == CIST_APIK)
      luaD_call(L, func, 1);
    else if (st != 0)
      luaD_callnoyield(L, func, 1);
    else
    { /* (cannot freeze) */
      L.nfz++;
      luaD_callnoyield(L, func, 1);
      L.nfz--;
    }
    res = result;
    copyslot(L, res, --L.top); /* move result to its place */
    return withvariant(slottag(L, res));     /* return tag of the result */
  }

  private static int callbinTM(lua_State L, TValue p1, TValue p2,
                               int res, int event)
  {
    TValue tm = luaT_gettmbyobj(L, p1, event); /* try first operand */
    if (notm(tm) != 0)
      tm = luaT_gettmbyobj(L, p2, event); /* try second operand */
    if (notm(tm) != 0)
      return -1; /* tag method not found */
    else         /* call tag method and return the tag of the result */
      return luaT_callTMres(L, tm, p1, p2, res);
  }

  public static void luaT_trybinTM(lua_State L, TValue p1, TValue p2,
                                   int res, int event, int src1, int src2)
  {
    if (callbinTM(L, p1, p2, res, event) < 0)
    {
      switch (event)
      {
      case TM_BAND:
      case TM_BOR:
      case TM_BXOR:
      case TM_SHL:
      case TM_SHR:
      case TM_BNOT:
      {
        if (ttisnumber(p1) && ttisnumber(p2))
          luaG_tointerror(L, p1, p2, src1, src2);
        else
          luaG_opinterror(L, p1, p2, Lt.perform_bitwise_operation_on, src1, src2);
      }
      /* calls never return, but to avoid warnings: */ /* FALLTHROUGH */
      default:
        luaG_opinterror(L, p1, p2, Lt.perform_arithmetic_on, src1, src2);
      }
    }
  }

  /*
  ** The use of 'p1' after 'callbinTM' is safe because, when a tag
  ** method is not found, 'callbinTM' cannot change the stack.
  */
  public static void luaT_tryconcatTM(lua_State L)
  {
    int p1 = L.top - 2; /* first argument */
    TValue v1 = getslot(L, p1);
    TValue v2 = getslot(L, p1 + 1);
    if (callbinTM(L, v1, v2, p1, TM_CONCAT) < 0)
      luaG_concaterror(L, v1, v2, p1, p1 + 1);
  }

  public static void luaT_trybinassocTM(lua_State L, TValue p1, TValue p2,
                                        int flip, int res, int event, int src1)
  {
    if (flip != 0)
      luaT_trybinTM(L, p2, p1, res, event, NOSLOT, src1);
    else
      luaT_trybinTM(L, p1, p2, res, event, src1, NOSLOT);
  }

  public static void luaT_trybiniTM(lua_State L, TValue p1, long i2,
                                    int flip, int res, int event, int src1)
  {
    TValue aux = new TValue();
    setivalue(aux, i2);
    luaT_trybinassocTM(L, p1, aux, flip, res, event, src1);
  }

  public static void luaT_trybinTM(lua_State L, int p1, int p2, int res, int event)
  {
    TValue v1 = getslot(L, p1);
    TValue v2 = getslot(L, p2);
    luaT_trybinTM(L, v1, v2, res, event, p1, p2);
  }

  public static void luaT_trybinassocTM(lua_State L, int p1, TValue p2,
                                        int flip, int res, int event)
  {
    TValue v1 = getslot(L, p1);
    luaT_trybinassocTM(L, v1, p2, flip, res, event, p1);
  }

  public static void luaT_trybiniTM(lua_State L, int p1, long i2,
                                    int flip, int res, int event)
  {
    TValue v1 = getslot(L, p1);
    luaT_trybiniTM(L, v1, i2, flip, res, event, p1);
  }

  /*
  ** Calls an order tag method.
  */
  public static int luaT_callorderTM(lua_State L, TValue p1, TValue p2,
                                     int event)
  {
    int tag = callbinTM(L, p1, p2, L.top, event); /* try original event */
    if (tag >= 0)                                  /* found tag method? */
      return (!(tag == LUA_VFALSE || tagisempty(tag))) ? 1 : 0;
    luaG_ordererror(L, p1, p2); /* no metamethod found */
    return 0;                   /* to avoid warnings */
  }

  public static int luaT_callorderiTM(lua_State L, TValue p1, int v2,
                                      int flip, int isfloat, int event)
  {
    TValue aux = new TValue();
    TValue p2;
    if (isfloat != 0)
    {
      setfltvalue(aux, v2);
    }
    else
      setivalue(aux, v2);
    if (flip != 0)
    { /* arguments were exchanged? */
      p2 = p1;
      p1 = aux; /* correct them */
    }
    else
      p2 = aux;
    return luaT_callorderTM(L, p1, p2, event);
  }

  public static int luaT_callorderiTM(lua_State L, int p1, int v2,
                                      int flip, int isfloat, int event)
  {
    TValue v1 = getslot(L, p1);
    return luaT_callorderiTM(L, v1, v2, flip, isfloat, event);
  }

  /*
  ** Create a vararg table at the top of the stack, with 'n' elements
  ** starting at 'f'.
  */
  private static void createvarargtab(lua_State L, int f, int n)
  {
    int i;
    TValue key = new TValue(), value = new TValue();
    Table t = Table.luaH_new(L);
    setsloth(L, L.top, t);
    L.top++;
    t.luaH_resize(L, n, 1);
    ByteView bytes = Lt.n_char;
    setsvalue(L, key, TString.luaS_createstr(L, bytes)); /* key is "n" */
    setivalue(value, n);                 /* value is n */
    /* No need to anchor the key: Due to the resize, the next operation
       cannot trigger a garbage collection */
    t.luaH_set(L, key, value); /* t.n = n */
    for (i = 0; i < n; i++)
    {
      TValue v = getslot(L, f + i);
      t.luaH_setint(L, i + 1, v);
    }
    luaC_checkGC(L);
  }

  /*
  ** initial stack:  func arg1 ... argn extra1 ...
  **                 ^ ci->func                    ^ L->top
  ** final stack: func nil ... nil extra1 ... func arg1 ... argn
  **                                          ^ ci->func
  */
  private static void buildhiddenargs(lua_State L, CallInfo ci, Proto p,
                                      int totalargs, int nfixparams, int nextra)
  {
    int i;
    ci.nextraargs = nextra;
    luaD_checkstack(L, p.maxstacksize + 1);
    /* copy function to the top of the stack, after extra arguments */
    copyslot(L, L.top++, ci.func);
    /* move fixed parameters to after the copied function */
    for (i = 1; i <= nfixparams; i++)
    {
      copyslot(L, L.top++, ci.func + i);
      setslottag(L, ci.func + i, LUA_VNIL); /* erase original parameter (for GC) */
    }
    ci.func += totalargs + 1; /* 'func' now lives after hidden arguments */
    ci.top += totalargs + 1;
  }

  public static void luaT_adjustvarargs(lua_State L, CallInfo ci, Proto p)
  {
    int totalargs = (int) (L.top - ci.func) - 1;
    int nfixparams = p.numparams;
    int nextra = totalargs - nfixparams; /* number of extra arguments */
    if ((p.flag & PF_VATAB) != 0)
    { /* does it need a vararg table? */
      assert (p.flag & PF_VAHID) == 0;
      createvarargtab(L, ci.func + nfixparams + 1, nextra);
      /* move table to proper place (last parameter) */
      copyslot(L, ci.func + nfixparams + 1, L.top - 1);
    }
    else
    { /* no table */
      assert (p.flag & PF_VAHID) != 0;
      buildhiddenargs(L, ci, p, totalargs, nfixparams, nextra);
      /* set vararg parameter to nil */
      setslottag(L, ci.func + nfixparams + 1, LUA_VNIL);
      assert L.top <= ci.top && ci.top <= L.stack_last;
    }
  }

  public static void luaT_getvararg(lua_State L, CallInfo ci, int ra, TValue rc)
  {
    int nextra = ci.nextraargs;
    IntResult n = new IntResult();
    if (tointegerns(rc, n) != 0)
    { /* integral value? */
      if (n.v >= 1 && n.v <= nextra)
      {
        int slot = ci.func - nextra + (int) n.v - 1;
        copyslot(L, ra, slot);
        return;
      }
    }
    else if (ttisstring(rc))
    { /* string value? */
      TString ts = tsvalue(rc);
      if (ts.data.size() == 1 && ts.data.byteAt(0) == 'n')
      { /* key is "n"? */
        setsloti(L, ra, nextra);
        return;
      }
    }
    setslottag(L, ra, LUA_VNIL); /* else produce nil */
  }

  public static void luaT_getvararg(lua_State L, CallInfo ci, int ra, int rc)
  {
    TValue v = getslot(L, rc);
    luaT_getvararg(L, ci, ra, v);
  }

  /*
  ** Get the number of extra arguments in a vararg function. If vararg
  ** table has been optimized away, that number is in the call info.
  ** Otherwise, get the field 'n' from the vararg table and check that it
  ** has a proper value (non-negative integer not larger than the stack
  ** limit).
  */
  private static int getnumargs(lua_State L, CallInfo ci, Table h)
  {
    if (h == null) /* no vararg table? */
      return ci.nextraargs;
    else
    {
      TValue res = new TValue();
      ByteView bytes = Lt.n_char;
      if (h.luaH_getshortstr(TString.luaS_createstr(L, bytes), res) != LUA_VNUMINT ||
          (ivalue(res) < 0 || ivalue(res) > Integer.MAX_VALUE / 2))
        luaG_runerror(L, Lt.vararg_table_has_no_proper_n);
      assert ttisinteger(res);
      return (int) ivalue(res);
    }
  }

  /*
  ** Get 'wanted' vararg arguments and put them in 'where'. 'vatab' is
  ** the register of the vararg table or -1 if there is no vararg table.
  */
  public static void luaT_getvarargs(lua_State L, CallInfo ci, int where, int wanted,
                                     int vatab)
  {
    Table h;
    if (vatab < 0)
      h = null;
    else
    {
      int vt = ci.func + vatab + 1;
      h = slothval(L, vt);
    }
    int nargs = getnumargs(L, ci, h); /* number of available vararg args. */
    int i, touse;                     /* 'touse' is minimum between 'wanted' and 'nargs' */
    if (wanted < 0)
    {
      touse = wanted = nargs;       /* get all extra arguments available */
      checkstackp(L, nargs, where); /* ensure stack space */
      L.top = where + nargs;     /* next instruction will need top */
    }
    else
      touse = (nargs > wanted) ? wanted : nargs;
    if (h == null)
    {                             /* no vararg table? */
      for (i = 0; i < touse; i++) /* get vararg values from the stack */
        copyslot(L, where + i, ci.func - nargs + i);
    }
    else
    { /* get vararg values from vararg table */
      for (i = 0; i < touse; i++)
      {
        TValue res = new TValue();
        byte tag = h.luaH_getint(i + 1, res);
        if (tagisempty(tag))
          setslottag(L, where + i, LUA_VNIL);
        else
          setslot(L, where + i, res);
      }
    }
    for (; i < wanted; i++) /* complete required results with nil */
      setslottag(L, where + i, LUA_VNIL);
  }
}
