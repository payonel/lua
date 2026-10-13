package com.payonel.lungolua.tests;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.bytes.InternalString.sv_byteat;
import static com.payonel.lungolua.bytes.Lctype.lisdigit;
import static com.payonel.lungolua.bytes.UtilCast.as_integer;
import static com.payonel.lungolua.bytes.UtilCast.as_userdata;
import static com.payonel.lungolua.bytes.UtilCast.func_to_string;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.lib.Linit.luaL_openlibs;
import static com.payonel.lungolua.lib.Linit.luaL_openselectedlibs;
import static com.payonel.lungolua.object.Lfunc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.object.Lstring.*;
import static com.payonel.lungolua.object.Ltable.*;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.runtime.Ltm.*;
import static com.payonel.lungolua.vm.Ldebug.*;
import static com.payonel.lungolua.vm.Lopcodes.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.luaL_Regi;
import com.payonel.lungolua.api.luaL_Stream;
import com.payonel.lungolua.api.lua_Alloc;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_Debug;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_ResumeResult;
import com.payonel.lungolua.api.lua_WarnFunction;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.GCObject;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.Llimits.IntResult;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.object.Udata;
import com.payonel.lungolua.object.UpVal;
import com.payonel.lungolua.platform.OsFile;
import com.payonel.lungolua.runtime.CallInfo;
import com.payonel.lungolua.runtime.Cost;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

/*
** The whole module only makes sense with LUA_DEBUG on
*/
public final class Ltests
{
  private Ltests()
  {
  }

  private static String to_string(ByteView sv)
  {
    return sv.toString();
  }

  private static ByteView to_view(String str)
  {
    return ByteView.of(str);
  }

  private static HostString to_hstring(String str)
  {
    return new HostString(ByteView.of(str));
  }

  private static Object l_Trick = null;

  private static TValue obj_at(lua_State L, int k)
  {
    return getslot(L, L.ci.func + k);
  }

  private static void setnameval(lua_State L, ByteView name, int val)
  {
    lua_pushinteger(L, val);
    lua_setfield(L, -2, new HostString(name));
  }

  private static void pushobject(lua_State L, TValue o)
  {
    setslot(L, L.top, o);
    api_incr_top(L);
  }

  private static void badexit(String fmt, String s1, String s2)
  {
    os_print_stderr(to_view(String.format(fmt, s1)));
    if (s2 != null)
      os_print_stderr(to_view(String.format("extra info: %s\n", s2)));
    /* avoid assertion failures when exiting */
    l_memcontrol.numblocks = l_memcontrol.total = 0;
    System.exit(1);
  }

  private static int tpanic(lua_State L)
  {
    String msg = "error object is not a string";
    if (lua_type(L, -1) == LUA_TSTRING)
    {
      msg = to_string(lua_toistring(L, -1).view());
    }
    badexit("PANIC: unprotected error in call to Lua API (%s)\n", msg, null);
    return 0; /* do not return to Lua */
  }

  private static final lua_CFunction tpanic = Ltests::tpanic;

  private static final ByteOwner buffsp = new ByteOwner(200); /* should be enough for tests... */

  private static int onoff = 0;
  private static int mode = 0; /* start in normal mode */
  private static int lasttocont = 0;

  /*
  ** Warning function for tests. First, it concatenates all parts of
  ** a warning in buffer 'buff'. Then, it has three modes:
  ** - 0.normal: messages starting with '#' are shown on standard output;
  ** - other messages abort the tests (they represent real warning
  ** conditions; the standard tests should not generate these conditions
  ** unexpectedly);
  ** - 1.allow: all messages are shown;
  ** - 2.store: all warnings go to the global '_WARN';
  */
  private static void warnf(lua_State L, HostString msg, int tocont)
  {
    String mstr = to_string(msg.to_utf8());
    ByteView m = to_view(mstr);

    if (lasttocont == 0 && tocont == 0 && !m.empty() && m.at(0) == '@')
    { /* control message? */
      if (buffsp.get(0) != '\0')
        badexit("Control warning during warning: %s\naborting...\n", mstr, to_string(buffsp.str_view()));
      if (m.equals(Lt.off_1))
        onoff = 0;
      else if (m.equals(Lt.on_1))
        onoff = 1;
      else if (m.equals(Lt.normal_1))
        mode = 0;
      else if (m.equals(Lt.allow))
        mode = 1;
      else if (m.equals(Lt.store))
        mode = 2;
      else
        badexit("Invalid control warning in test mode: %s\naborting...\n", mstr, null);
      return;
    }
    lasttocont = tocont;
    int used = buffsp.str_view().size();
    if (m.size() >= buffsp.size() - used)
      badexit("warnf-buffer overflow (%s)\n", mstr, to_string(buffsp.str_view()));
    buffsp.copy_from(m, used); /* add new message to current warning */
    buffsp.set(used + m.size(), '\0');
    if (tocont == 0)
    { /* message finished? */
      luaL_checkstack(L, 1, new HostString(Lt.warn_stack_space));
      lua_getglobal(L, new HostString(Lt._WARN));
      if (lua_toboolean(L, -1) == 0)
        lua_pop(L, 1); /* ok, no previous unexpected warning */
      else
      {
        String warning = to_string(lua_toistring(L, -1).view());
        badexit("Unhandled warning in store mode: %s\naborting...\n", warning, to_string(buffsp.str_view()));
      }
      switch (mode)
      {
      case 0:
      {                                         /* normal */
        if (buffsp.get(0) != '#' && onoff != 0) /* unexpected warning? */
          badexit("Unexpected warning in test mode: %s\naborting...\n",
                  to_string(buffsp.str_view()), null);
      } /* FALLTHROUGH */
      case 1:
      { /* allow */
        if (onoff != 0)
          os_print_stderr(to_view(String.format("Lua warning: %s\n", to_string(buffsp.str_view())))); /* print warning */
        break;
      }
      case 2:
      { /* store */
        luaL_checkstack(L, 1, new HostString(Lt.warn_stack_space));
        lua_pushstring(L, HostString.from_bytes(buffsp.str_view()));
        lua_setglobal(L, new HostString(Lt._WARN)); /* assign message to global '_WARN' */
        break;
      }
      }
      buffsp.set(0, '\0'); /* prepare buffer for next warning */
    }
  }

  private static final class TestWarn implements lua_WarnFunction
  {
    @Override
    public void warn(lua_State L, HostString msg, int tocont)
    {
      warnf(L, msg, tocont);
    }
  }

  private static final TestWarn test_warnf = new TestWarn();

  /*
  ** {======================================================================
  ** Controlled version for realloc.
  ** =======================================================================
  */

  private static final class Memcontrol
  {
    int failnext;
    long numblocks;
    long total;
    long maxmem;
    long memlimit;
    long countlimit;
    long[] objcount;

    Memcontrol(int failnext, long numblocks, long total, long maxmem, long memlimit, long countlimit, long[] objcount)
    {
      this.failnext = failnext;
      this.numblocks = numblocks;
      this.total = total;
      this.maxmem = maxmem;
      this.memlimit = memlimit;
      this.countlimit = countlimit;
      this.objcount = objcount;
    }
  }

  private static final Memcontrol l_memcontrol =
      new Memcontrol(0, 0L, 0L, 0L, 0L, (~0L), new long[] {0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L});

  private static boolean debug_realloc(Memcontrol mc, long oldsize, long nsize, int tag)
  {
    int type = (Long.compareUnsigned(tag, LUA_NUMTYPES) < 0) ? tag : 0;
    if (mc.memlimit == 0)
    {                                             /* first time? */
      String limit = System.getenv("MEMLIMIT"); /* initialize memory limit */
      mc.memlimit = (limit != null) ? Long.parseUnsignedLong(limit, 10) : ~0L;
    }
    if (nsize == 0)
    {
      mc.objcount[type]--;
      mc.numblocks--;
      mc.total -= oldsize;
      return true;
    }
    if (mc.failnext != 0)
    {
      mc.failnext = 0;
      return false; /* fake a single memory allocation error */
    }
    if (mc.countlimit != ~0L && nsize != oldsize)
    { /* count limit in use? */
      if (mc.countlimit == 0)
        return false; /* fake a memory allocation error */
      mc.countlimit--;
    }
    if (Long.compareUnsigned(nsize, oldsize) > 0 && Long.compareUnsigned(mc.total + nsize - oldsize, mc.memlimit) > 0)
      return false; /* fake a memory allocation error */
    if (oldsize == 0)
    {
      mc.numblocks++;
      mc.objcount[type]++;
    }
    mc.total -= oldsize;
    mc.total += nsize;
    if (Long.compareUnsigned(mc.total, mc.maxmem) > 0)
      mc.maxmem = mc.total;
    return true;
  }

  private static final class DebugAlloc implements lua_Alloc
  {
    @Override
    public boolean permit(long oldsize, long nsize, int tag)
    {
      return debug_realloc(l_memcontrol, oldsize, nsize, tag);
    }
  }

  private static final DebugAlloc debug_alloc = new DebugAlloc();

  /* }====================================================================== */

  /*
  ** Check GC invariants. For incremental mode, a black object cannot
  ** point to a white one. For generational mode, really old objects
  ** cannot point to young objects. Both old1 and touched2 objects
  ** cannot point to new objects (but can point to survivals).
  ** (Threads and open upvalues, despite being marked "really old",
  ** continue to be visited in all collections, and therefore can point to
  ** new objects. They, and only they, are old but gray.)
  */
  private static int testobjref1(global_State g, GCObject f, GCObject t)
  {
    if (isdead(g, t) != 0)
      return 0;
    if (issweepphase(g) != 0)
      return 1; /* no invariants */
    else if (g.gckind != KGC_GENMINOR)
      return !(isblack(f) != 0 && iswhite(t) != 0) ? 1 : 0; /* basic incremental invariant */
    else
    { /* generational mode */
      if ((getage(f) == G_OLD && isblack(f) != 0) && isold(t) == 0)
        return 0;
      if ((getage(f) == G_OLD1 || getage(f) == G_TOUCHED2) &&
          getage(t) == G_NEW)
        return 0;
      return 1;
    }
  }

  private static void printobj(global_State g, GCObject o)
  {
    String tn = to_string(ttypename(o.tt & NOVARIANT_MASK));
    os_print_stdout(to_view(String.format("||%s(%s)-%c%c(%02X)||",
                                          tn, to_string(func_to_string(o).view()),
                                          isdead(g, o) != 0 ? 'd' : isblack(o) != 0 ? 'b'
                                                              : iswhite(o) != 0   ? 'w'
                                                                                  : 'g',
                                          "ns01oTt".charAt(getage(o)), o.marked & 0xFF)));
    if (o.tt == LUA_VSTRING)
    {
      String msg = to_string(((TString) o).data.view());
      os_print_stdout(to_view(String.format(" '%s'", msg)));
    }
  }

  private static void lua_printvalue(TValue v)
  {
    switch (((v).tt_ & WITHVARIANT_MASK))
    {
    case LUA_VNUMINT:
    case LUA_VNUMFLT:
    {
      ByteOwner buff = new ByteOwner(LUA_N2SBUFFSZ);
      int len = luaO_tostringbuff(v, buff.span());
      buff.set(len, '\0');
      os_print_stdout(to_view(String.format("%s", to_string(buff.str_view()))));
      break;
    }
    case LUA_VSHRSTR:
      assert (v.tt_ & NOVARIANT_MASK) == LUA_TSTRING;
      ByteView sv = tsvalue(v).data.view();
      os_print_stdout(to_view(String.format("'%s'", to_string(sv.substr(0, sv.find(0))))));
      break;
    case LUA_VFALSE:
      os_print_stdout(to_view(String.format("%s", "false")));
      break;
    case LUA_VTRUE:
      os_print_stdout(to_view(String.format("%s", "true")));
      break;
    case LUA_VLIGHTUSERDATA:
      os_print_stdout(to_view(String.format("light udata: %s", to_string(func_to_string(pvalue(v)).view()))));
      break;
    case LUA_VUSERDATA:
      os_print_stdout(to_view(String.format("full udata: %s", to_string(func_to_string(uvalue(v)).view()))));
      break;
    case LUA_VNIL:
      os_print_stdout(to_view(String.format("nil")));
      break;
    case LUA_VLCF:
      os_print_stdout(to_view(String.format("light C function: %s", to_string(func_to_string(fvalue(v)).view()))));
      break;
    case LUA_VCCL:
      os_print_stdout(to_view(String.format("C closure: %s", to_string(func_to_string(clCvalue(v)).view()))));
      break;
    case LUA_VLCL:
      os_print_stdout(to_view(String.format("Lua function: %s", to_string(func_to_string(clLvalue(v)).view()))));
      break;
    case LUA_VTHREAD:
      os_print_stdout(to_view(String.format("thread: %s", to_string(func_to_string(thvalue(v)).view()))));
      break;
    case LUA_VTABLE:
      os_print_stdout(to_view(String.format("table: %s", to_string(func_to_string(hvalue(v)).view()))));
      break;
    default:
      assert false;
    }
  }

  private static int testobjref(global_State g, GCObject f, GCObject t)
  {
    int r1 = testobjref1(g, f, t);
    if (r1 == 0)
    {
      os_print_stdout(to_view(String.format("%d(%02X) - ", g.gcstate, g.currentwhite & 0xFF)));
      printobj(g, f);
      os_print_stdout(to_view(String.format("  ->  ")));
      printobj(g, t);
      os_print_stdout(to_view(String.format("\n")));
    }
    return r1;
  }

  private static void checkobjref(global_State g, GCObject f, GCObject t)
  {
    assert testobjref(g, f, t) != 0;
  }

  private static void checkobjrefN(global_State g, GCObject f, GCObject t)
  {
    if (t != null)
      checkobjref(g, f, t);
  }

  private static void checkvalref(global_State g, GCObject f, TValue t)
  {
    assert ((t).tt_ & BIT_ISCOLLECTABLE) == 0 || ((((t).tt_ & WITHVARIANT_MASK) == gcvalue(t).tt) && testobjref(g, f, gcvalue(t)) != 0);
  }

  private static void checktable(global_State g, Table h)
  {
    int i;
    int asize = h.asize();
    int n, limit = sizenode(h);
    assert (h.tt & NOVARIANT_MASK) >= LUA_TSTRING;
    GCObject hgc = h;
    checkobjrefN(g, hgc, h.metatable);
    for (i = 0; i < asize; i++)
    {
      TValue aux = new TValue();
      arr2obj(h, i, aux);
      checkvalref(g, hgc, aux);
    }
    for (n = 0; n < limit; n++)
    {
      if (!((gnode(h, n).tt_ & NOVARIANT_MASK) == LUA_TNIL))
      {
        TValue k = new TValue();
        getnodekey(mainthread(g), k, gnode(h, n));
        assert !((gnode(h, n)).key_tt == LUA_TNIL);
        checkvalref(g, hgc, k);
        checkvalref(g, hgc, gnode(h, n));
      }
    }
  }

  private static void checkudata(global_State g, Udata u)
  {
    int i;
    assert (u.tt & NOVARIANT_MASK) >= LUA_TSTRING;
    GCObject hgc = u;
    checkobjrefN(g, hgc, u.metatable);
    for (i = 0; i < u.uv.length; i++)
      checkvalref(g, hgc, u.uv[i]);
  }

  private static void checkproto(global_State g, Proto f)
  {
    int i;
    assert (f.tt & NOVARIANT_MASK) >= LUA_TSTRING;
    GCObject fgc = f;
    checkobjrefN(g, fgc, f.source);
    for (i = 0; i < f.k.length; i++)
    {
      if ((f.k[i].tt_ & BIT_ISCOLLECTABLE) != 0)
        checkobjref(g, fgc, gcvalue(f.k[i]));
    }
    for (i = 0; i < f.upvalues.length; i++)
      checkobjrefN(g, fgc, f.upvalues[i].name);
    for (i = 0; i < f.p.length; i++)
      checkobjrefN(g, fgc, f.p[i]);
    for (i = 0; i < f.locvars.length; i++)
      checkobjrefN(g, fgc, f.locvars[i].varname);
  }

  private static void checkCclosure(global_State g, CClosure cl)
  {
    assert (cl.tt & NOVARIANT_MASK) >= LUA_TSTRING;
    GCObject clgc = cl;
    int i;
    for (i = 0; i < cl.upvalue.length; i++)
      checkvalref(g, clgc, cl.upvalue[i]);
  }

  private static void checkLclosure(global_State g, LClosure cl)
  {
    assert (cl.tt & NOVARIANT_MASK) >= LUA_TSTRING;
    GCObject clgc = cl;
    int i;
    checkobjrefN(g, clgc, cl.p);
    for (i = 0; i < cl.upvals.length; i++)
    {
      UpVal uv = cl.upvals[i];
      if (uv != null)
      {
        checkobjrefN(g, clgc, uv);
        if (upisopen(uv) == 0)
        {
          assert (uv.tt & NOVARIANT_MASK) >= LUA_TSTRING;
          TValue uvv = upvalgetval(uv);
          checkvalref(g, uv, uvv);
        }
      }
    }
  }

  private static int lua_checkpc(lua_State L, CallInfo ci)
  {
    if (isLua(ci) == 0)
      return 1;
    else
    {
      int f = ci.func;
      Proto p = slotclLval(L, f).p;
      return (0 <= ci.savedpc &&
              ci.savedpc <= p.code.length) ? 1 : 0;
    }
  }

  private static void check_stack(global_State g, lua_State L1)
  {
    int o;
    CallInfo ci;
    UpVal uv;
    assert isdead(g, L1) == 0;
    if (L1.stack_prim.length == 0)
    { /* incomplete thread? */
      assert L1.openupval == null && L1.ci == null;
      return;
    }
    for (uv = L1.openupval; uv != null; uv = uv.opennext)
      assert upisopen(uv) != 0; /* must be open */
    assert L1.top <= L1.stack_last;
    assert L1.tbclist <= L1.top;
    for (ci = L1.ci; ci != null; ci = ci.previous)
    {
      assert ci.top <= L1.stack_last;
      assert lua_checkpc(L1, ci) != 0;
    }
    for (o = 0; o < L1.stack_last; o++)
    { /* entire stack must have valid values */
      TValue v = getslot(L1, o);
      checkliveness(L1, v);
    }
  }

  private static void checkrefs(global_State g, GCObject o)
  {
    switch (o.tt)
    {
    case LUA_VUSERDATA:
    {
      checkudata(g, (Udata) o);
      break;
    }
    case LUA_VUPVAL:
    {
      TValue uvv = upvalgetval((UpVal) o);
      checkvalref(g, o, uvv);
      break;
    }
    case LUA_VBUDGET:
    {
      checkvalref(g, o, ((BudgetGroup) o).cb);
      break;
    }
    case LUA_VTABLE:
    {
      checktable(g, (Table) o);
      break;
    }
    case LUA_VTHREAD:
    {
      check_stack(g, (lua_State) o);
      break;
    }
    case LUA_VLCL:
    {
      checkLclosure(g, (LClosure) o);
      break;
    }
    case LUA_VCCL:
    {
      checkCclosure(g, (CClosure) o);
      break;
    }
    case LUA_VPROTO:
    {
      checkproto(g, (Proto) o);
      break;
    }
    case LUA_VSHRSTR:
    {
      assert isgray(o) == 0; /* strings are never gray */
      break;
    }
    case LUA_VLCF:
    {
      assert isgray(o) == 0; /* CFuncInfo has no refs, never gray */
      break;
    }
    default:
      assert false;
    }
  }

  /*
  ** Check consistency of an object:
  ** - Dead objects can only happen in the 'allgc' list during a sweep
  ** phase (controlled by the caller through 'maybedead').
  ** - During pause, all objects must be white.
  ** - In generational mode:
  **   * objects must be old enough for their lists ('listage').
  **   * old objects cannot be white.
  **   * old objects must be black, except for 'touched1', 'old0',
  **     threads, and open upvalues.
  **   * 'touched1' objects must be gray.
  */
  private static void checkobject(global_State g, GCObject o, int maybedead,
                                  int listage)
  {
    if (isdead(g, o) != 0)
      assert maybedead != 0;
    else
    {
      assert g.gcstate != GCSpause || iswhite(o) != 0;
      if (g.gckind == KGC_GENMINOR)
      { /* generational mode? */
        assert getage(o) >= listage;
        if (isold(o) != 0)
        {
          assert iswhite(o) == 0;
          assert isblack(o) != 0 ||
                 getage(o) == G_TOUCHED1 ||
                 getage(o) == G_OLD0 ||
                 o.tt == LUA_VTHREAD ||
                 (o.tt == LUA_VUPVAL && upisopen((UpVal) o) != 0);
        }
        assert getage(o) != G_TOUCHED1 || isgray(o) != 0;
      }
      checkrefs(g, o);
    }
  }

  private static long checkgraylist(global_State g, GCObject o)
  {
    int total = 0; /* count number of elements in the list */
    /* better to keep it if we need to print an object */
    while (o != null)
    {
      assert (isgray(o) != 0) ^ (getage(o) == G_TOUCHED2);
      assert ((o.marked) & bitmask(TESTBIT)) == 0;
      if (keepinvariant(g) != 0)
        o.marked = (byte) (o.marked | bitmask(TESTBIT)); /* mark that object is in a gray list */
      total++;
      switch (o.tt)
      {
      case LUA_VTABLE:
        o = ((Table) o).gclist;
        break;
      case LUA_VLCL:
        o = ((LClosure) o).gclist;
        break;
      case LUA_VCCL:
        o = ((CClosure) o).gclist;
        break;
      case LUA_VTHREAD:
        o = ((lua_State) o).gclist;
        break;
      case LUA_VPROTO:
        o = ((Proto) o).gclist;
        break;
      case LUA_VUSERDATA:
        assert ((Udata) o).uv.length > 0;
        assert o.tt == LUA_VUSERDATA;
        o = ((Udata) o).gclist;
        break;
      default:
        assert false; /* other objects cannot be in a gray list */
      }
    }
    return total;
  }

  /*
  ** Check objects in gray lists.
  */
  private static long checkgrays(global_State g)
  {
    long total = 0; /* count number of elements in all lists */
    if (keepinvariant(g) == 0)
      return total;
    total += checkgraylist(g, g.gray.gclist);
    total += checkgraylist(g, g.grayagain.gclist);
    total += checkgraylist(g, g.weak.gclist);
    total += checkgraylist(g, g.allweak.gclist);
    total += checkgraylist(g, g.ephemeron.gclist);
    return total;
  }

  /*
  ** Check whether 'o' should be in a gray list. If so, increment
  ** 'count' and check its TESTBIT. (It must have been previously set by
  ** 'checkgraylist'.)
  */
  private static void incifingray(global_State g, GCObject o, IntResult count)
  {
    if (keepinvariant(g) == 0)
      return; /* gray lists not being kept in these phases */
    if (o.tt == LUA_VUPVAL)
    {
      /* only open upvalues can be gray */
      assert isgray(o) == 0 || upisopen((UpVal) o) != 0;
      return; /* upvalues are never in gray lists */
    }
    /* these are the ones that must be in gray lists */
    if (isgray(o) != 0 || getage(o) == G_TOUCHED2)
    {
      count.v++;
      assert ((o.marked) & bitmask(TESTBIT)) != 0;
      o.marked &= (byte) ~bitmask(TESTBIT); /* prepare for next cycle */
    }
  }

  private static long checklist(global_State g, int maybedead, int tof,
                                GCObject newl, GCObject survival, GCObject old, GCObject reallyold)
  {
    GCObject o;
    IntResult total = new IntResult(); /* number of object that should be in  gray lists */
    for (o = newl; o != survival; o = o.next)
    {
      checkobject(g, o, maybedead, G_NEW);
      incifingray(g, o, total);
      assert (tof == 0) == (tofinalize(o) == 0);
    }
    for (o = survival; o != old; o = o.next)
    {
      checkobject(g, o, 0, G_SURVIVAL);
      incifingray(g, o, total);
      assert (tof == 0) == (tofinalize(o) == 0);
    }
    for (o = old; o != reallyold; o = o.next)
    {
      checkobject(g, o, 0, G_OLD1);
      incifingray(g, o, total);
      assert (tof == 0) == (tofinalize(o) == 0);
    }
    for (o = reallyold; o != null; o = o.next)
    {
      checkobject(g, o, 0, G_OLD);
      incifingray(g, o, total);
      assert (tof == 0) == (tofinalize(o) == 0);
    }
    return total.v;
  }

  private static int lua_checkmemory(lua_State L)
  {
    global_State g = L.l_G;
    GCObject o;
    int maybedead;
    long totalin;                              /* total of objects that are in gray lists */
    IntResult totalshould = new IntResult(); /* total of objects that should be in gray lists */
    if (keepinvariant(g) != 0)
    {
      assert iswhite(mainthread(g)) == 0;
      assert (g.l_registry.tt_ & BIT_ISCOLLECTABLE) != 0;
      assert iswhite(gcvalue(g.l_registry)) == 0;
    }
    assert (g.l_registry.tt_ & BIT_ISCOLLECTABLE) != 0;
    assert isdead(g, gcvalue(g.l_registry)) == 0;
    assert g.sweepgc == null || issweepphase(g) != 0;
    totalin = checkgrays(g);

    /* check 'fixedgc' list */
    for (o = g.fixedgc; o != null; o = o.next)
    {
      assert o.tt == LUA_VSHRSTR && isgray(o) != 0 && getage(o) == G_OLD;
    }

    /* check 'allgc' list */
    maybedead = (GCSatomic < g.gcstate && g.gcstate <= GCSswpallgc) ? 1 : 0;
    totalshould.v = checklist(g, maybedead, 0, g.allgc.next,
                              g.survival, g.old1, g.reallyold);

    /* check 'finobj' list */
    totalshould.v += checklist(g, 0, 1, g.finobj.next,
                               g.finobjsur, g.finobjold1, g.finobjrold);

    /* check 'tobefnz' list */
    for (o = g.tobefnz.next; o != null; o = o.next)
    {
      checkobject(g, o, 0, G_NEW);
      incifingray(g, o, totalshould);
      assert tofinalize(o) != 0;
      assert o.tt == LUA_VUSERDATA || o.tt == LUA_VTABLE;
    }

    /* check the queues of the budget groups */
    for (BudgetGroup bg : g.groupbyid)
    {
      if (bg == null)
        continue;
      for (o = bg.fnz.next; o != null; o = o.next)
      {
        checkobject(g, o, 0, G_NEW);
        incifingray(g, o, totalshould);
        assert tofinalize(o) != 0;
        assert o.tt == LUA_VUSERDATA || o.tt == LUA_VTABLE;
      }
    }
    if (keepinvariant(g) != 0)
      assert totalin == totalshould.v;
    return 0;
  }

  private static final lua_CFunction lua_checkmemory = Ltests::lua_checkmemory;

  /* }====================================================== */

  /*
  ** {======================================================
  ** Disassembler
  ** =======================================================
  */

  private static String buildop(Proto p, int pc)
  {
    StringBuilder buff = new StringBuilder();
    StringBuilder out = buff;
    int i = p.code[pc];
    int o = GET_OPCODE(i);
    String namebuf = to_string(opnames[o]);
    String name = namebuf;
    int line = luaG_getfuncline(p, pc);
    int lineinfo = (p.lineinfo.length != 0) ? p.lineinfo[pc] : 0;
    if (lineinfo == ABSLINEINFO)
      out.append(String.format("(__"));
    else
      out.append(String.format("(%2d", lineinfo));
    out.append(String.format(" - %4d) %4d - ", line, pc));
    switch (getOpMode(o))
    {
    case iABC:
      out.append(String.format("%-12s%4d %4d %4d%s", name,
                               GETARG_A(i), GETARG_B(i), GETARG_C(i),
                               GETARG_k(i) != 0 ? " (k)" : ""));
      break;
    case ivABC:
      out.append(String.format("%-12s%4d %4d %4d%s", name,
                               GETARG_A(i), GETARG_vB(i), GETARG_vC(i),
                               GETARG_k(i) != 0 ? " (k)" : ""));
      break;
    case iABx:
      out.append(String.format("%-12s%4d %4d", name, GETARG_A(i), GETARG_Bx(i)));
      break;
    case iAsBx:
      out.append(String.format("%-12s%4d %4d", name, GETARG_A(i), GETARG_sBx(i)));
      break;
    case iAx:
      out.append(String.format("%-12s%4d", name, GETARG_Ax(i)));
      break;
    case isJ:
      out.append(String.format("%-12s%4d", name, GETARG_sJ(i)));
      break;
    }
    return buff.toString();
  }

  private static int listcode(lua_State L)
  {
    int pc;
    Proto p;
    luaL_argcheck(L, lua_isfunction(L, 1) != 0 && lua_iscfunction(L, 1) == 0,
                  1, new HostString(Lt.Lua_function_expected));
    TValue o = obj_at(L, 1);
    p = getproto(o);
    lua_newtable(L);
    setnameval(L, Lt.maxstack, p.maxstacksize);
    setnameval(L, Lt.numparams, p.numparams);
    for (pc = 0; pc < p.code.length; pc++)
    {
      lua_pushinteger(L, pc + 1);
      String op = buildop(p, pc);
      ByteView sv = to_view(op);
      lua_pushstring(L, new HostString(sv));
      lua_settable(L, -3);
    }
    return 1;
  }

  private static final lua_CFunction listcode = Ltests::listcode;

  private static int printcode(lua_State L)
  {
    int pc;
    Proto p;
    luaL_argcheck(L, lua_isfunction(L, 1) != 0 && lua_iscfunction(L, 1) == 0,
                  1, new HostString(Lt.Lua_function_expected));
    TValue o = obj_at(L, 1);
    p = getproto(o);
    os_print_stdout(to_view(String.format("maxstack: %d\n", p.maxstacksize)));
    os_print_stdout(to_view(String.format("numparams: %d\n", p.numparams)));
    for (pc = 0; pc < p.code.length; pc++)
    {
      os_print_stdout(to_view(String.format("%s\n", buildop(p, pc))));
    }
    return 0;
  }

  private static final lua_CFunction printcode = Ltests::printcode;

  private static int listk(lua_State L)
  {
    Proto p;
    int i;
    luaL_argcheck(L, lua_isfunction(L, 1) != 0 && lua_iscfunction(L, 1) == 0,
                  1, new HostString(Lt.Lua_function_expected));
    TValue o = obj_at(L, 1);
    p = getproto(o);
    lua_createtable(L, p.k.length, 0);
    for (i = 0; i < p.k.length; i++)
    {
      pushobject(L, p.k[i]);
      lua_rawseti(L, -2, i + 1);
    }
    return 1;
  }

  private static final lua_CFunction listk = Ltests::listk;

  private static int listabslineinfo(lua_State L)
  {
    Proto p;
    int i;
    luaL_argcheck(L, lua_isfunction(L, 1) != 0 && lua_iscfunction(L, 1) == 0,
                  1, new HostString(Lt.Lua_function_expected));
    TValue o = obj_at(L, 1);
    p = getproto(o);
    luaL_argcheck(L, p.abslineinfo.length != 0, 1, new HostString(Lt.function_has_no_debug_info));
    lua_createtable(L, 2 * p.abslineinfo.length, 0);
    for (i = 0; i < p.abslineinfo.length; i++)
    {
      lua_pushinteger(L, p.abslineinfo[i].pc);
      lua_rawseti(L, -2, 2 * i + 1);
      lua_pushinteger(L, p.abslineinfo[i].line);
      lua_rawseti(L, -2, 2 * i + 2);
    }
    return 1;
  }

  private static final lua_CFunction listabslineinfo = Ltests::listabslineinfo;

  private static int listlocals(lua_State L)
  {
    Proto p;
    int pc = ((int) (luaL_checkinteger(L, 2))) - 1;
    int i = 0;
    InternalString name;
    luaL_argcheck(L, lua_isfunction(L, 1) != 0 && lua_iscfunction(L, 1) == 0,
                  1, new HostString(Lt.Lua_function_expected));
    TValue o = obj_at(L, 1);
    p = getproto(o);
    while ((name = p.luaF_getlocalname(++i, pc)) != null)
      lua_pushlstring(L, name.view(), 0, name.size());
    return i - 1;
  }

  private static final lua_CFunction listlocals = Ltests::listlocals;

  /* }====================================================== */

  private static void lua_printstack(lua_State L)
  {
    int i;
    int n = lua_gettop(L);
    os_print_stdout(to_view(String.format("stack: >>\n")));
    for (i = 1; i <= n; i++)
    {
      os_print_stdout(to_view(String.format("%3d: ", i)));
      TValue v = getslot(L, L.ci.func + i);
      lua_printvalue(v);
      os_print_stdout(to_view(String.format("\n")));
    }
    os_print_stdout(to_view(String.format("<<\n")));
  }

  private static int lua_printallstack(lua_State L)
  {
    int p;
    int i = 1;
    CallInfo ci = L.base_ci;
    os_print_stdout(to_view(String.format("stack: >>\n")));
    for (p = 0; p < L.top; p++)
    {
      if (ci != null && p == ci.func)
      {
        os_print_stdout(to_view(String.format("  ---\n")));
        if (ci == L.ci)
          ci = null; /* printed last frame */
        else
          ci = ci.next;
      }
      os_print_stdout(to_view(String.format("%3d: ", i++)));
      TValue v = getslot(L, p);
      lua_printvalue(v);
      os_print_stdout(to_view(String.format("\n")));
    }
    os_print_stdout(to_view(String.format("<<\n")));
    return 0;
  }

  private static final lua_CFunction lua_printallstack = Ltests::lua_printallstack;

  private static int get_limits(lua_State L)
  {
    lua_createtable(L, 0, 5);
    setnameval(L, Lt.IS32INT, 1);
    setnameval(L, Lt.MAXARG_Ax, MAXARG_Ax);
    setnameval(L, Lt.MAXARG_Bx, MAXARG_Bx);
    setnameval(L, Lt.OFFSET_sBx, OFFSET_sBx);
    setnameval(L, Lt.NUM_OPCODES, NUM_OPCODES);
    return 1;
  }

  private static final lua_CFunction get_limits = Ltests::get_limits;

  private static int get_sizes(lua_State L)
  {
    lua_newtable(L);
    setnameval(L, Lt.Lua_state, Cost.Thread);
    setnameval(L, Lt.global_state, Cost.Global);
    setnameval(L, Lt.TValue, Cost.TValue);
    setnameval(L, Lt.Node, Cost.Node);
    setnameval(L, Lt.stack_Value, Long.BYTES + Cost.Ref + Byte.BYTES);
    return 1;
  }

  private static final lua_CFunction get_sizes = Ltests::get_sizes;

  private static int mem_query(lua_State L)
  {
    if (lua_isnone(L, 1) != 0)
    {
      lua_pushinteger(L, ((long) (l_memcontrol.total)));
      lua_pushinteger(L, ((long) (l_memcontrol.numblocks)));
      lua_pushinteger(L, ((long) (l_memcontrol.maxmem)));
      return 3;
    }
    else if (lua_isnumber(L, 1) != 0)
    {
      long limit = ((long) (luaL_checkinteger(L, 1)));
      l_memcontrol.memlimit = (limit == 0) ? ~0L : limit;
      return 0;
    }
    else
    {
      ByteView t = luaL_checklstring(L, 1);
      int i;
      for (i = LUA_NUMTYPES - 1; i >= 0; i--)
      {
        if (ttypename(i).equals(t))
        {
          lua_pushinteger(L, ((long) (l_memcontrol.objcount[i])));
          return 1;
        }
      }
      lua_pushstring(L, new HostString(t));
      return luaL_error(L, new HostString(Lt.unknown_type_s));
    }
  }

  private static final lua_CFunction mem_query = Ltests::mem_query;

  private static int alloc_count(lua_State L)
  {
    if (lua_isnone(L, 1) != 0)
      l_memcontrol.countlimit = ((long) (~0L));
    else
      l_memcontrol.countlimit = ((long) (luaL_checkinteger(L, 1)));
    return 0;
  }

  private static final lua_CFunction alloc_count = Ltests::alloc_count;

  private static int alloc_failnext(lua_State L)
  {
    l_memcontrol.failnext = 1;
    return 0;
  }

  private static final lua_CFunction alloc_failnext = Ltests::alloc_failnext;

  private static int settrick(lua_State L)
  {
    TValue o = obj_at(L, 1);
    if (((o.tt_ & NOVARIANT_MASK) == LUA_TNIL))
      l_Trick = null;
    else
    {
      assert (o.tt_ & BIT_ISCOLLECTABLE) != 0;
      l_Trick = gcvalue(o);
    }
    return 0;
  }

  private static final lua_CFunction settrick = Ltests::settrick;

  private static int gc_color(lua_State L)
  {
    luaL_checkany(L, 1);
    TValue o = obj_at(L, 1);
    if ((o.tt_ & BIT_ISCOLLECTABLE) == 0)
      lua_pushstring(L, new HostString(Lt.no_collectable));
    else
    {
      assert (o.tt_ & BIT_ISCOLLECTABLE) != 0;
      GCObject obj = gcvalue(o);
      ByteView color = isdead(L.l_G, obj) != 0 ? Lt.dead : iswhite(obj) != 0 ? Lt.white : isblack(obj) != 0 ? Lt.black : Lt.gray;
      lua_pushstring(L, new HostString(color));
    }
    return 1;
  }

  private static final lua_CFunction gc_color = Ltests::gc_color;

  private static int gc_age(lua_State L)
  {
    luaL_checkany(L, 1);
    TValue o = obj_at(L, 1);
    if ((o.tt_ & BIT_ISCOLLECTABLE) == 0)
      lua_pushstring(L, new HostString(Lt.no_collectable));
    else
    {
      ByteView[] gennames = {Lt.new_kw, Lt.survival, Lt.old0, Lt.old1, Lt.old, Lt.touched1, Lt.touched2};
      assert (o.tt_ & BIT_ISCOLLECTABLE) != 0;
      GCObject obj = gcvalue(o);
      lua_pushstring(L, new HostString(gennames[getage(obj)]));
    }
    return 1;
  }

  private static final lua_CFunction gc_age = Ltests::gc_age;

  private static int gc_printobj(lua_State L)
  {
    luaL_checkany(L, 1);
    TValue o = obj_at(L, 1);
    if ((o.tt_ & BIT_ISCOLLECTABLE) == 0)
      os_print_stdout(to_view(String.format("no collectable\n")));
    else
    {
      assert (o.tt_ & BIT_ISCOLLECTABLE) != 0;
      GCObject obj = gcvalue(o);
      printobj(L.l_G, obj);
      os_print_stdout(to_view(String.format("\n")));
    }
    return 0;
  }

  private static final lua_CFunction gc_printobj = Ltests::gc_printobj;

  private static final ByteView[] statenames = {
      Lt.propagate, Lt.enteratomic,
      Lt.atomic, Lt.sweepallgc,
      Lt.sweepfinobj, Lt.sweeptobefnz,
      Lt.sweepend, Lt.callfin,
      Lt.pause, Lt.empty_str};

  private static int gc_state(lua_State L)
  {
    final int[] states = {
        GCSpropagate, GCSenteratomic, GCSatomic, GCSswpallgc, GCSswpfinobj,
        GCSswptobefnz, GCSswpend, GCScallfin, GCSpause, -1};
    final ByteView defstate = Lt.empty_str;
    int option = states[luaL_checkoptioni(L, 1, defstate, statenames)];
    global_State g = L.l_G;
    if (option == -1)
    {
      lua_pushlstring(L, statenames[g.gcstate], 0, statenames[g.gcstate].size());
      return 1;
    }
    else
    {
      if (g.gckind != KGC_INC)
        luaL_error(L, new HostString(Lt.cannot_change_states_in_generational_mode));
      lua_lock(L);
      if (option < g.gcstate)
      {                                   /* must cross 'pause'? */
        luaC_runtilstate(L, GCSpause, 1); /* run until pause */
      }
      luaC_runtilstate(L, option, 0); /* do not skip propagation state */
      assert g.gcstate == option;
      lua_unlock(L);
      return 0;
    }
  }

  private static final lua_CFunction gc_state = Ltests::gc_state;

  private static int tracinggc = 0;

  public static void luai_tracegctest(lua_State L, int first)
  {
    if (tracinggc == 0)
      return;
    else
    {
      global_State g = L.l_G;
      lua_unlock(L);
      g.gcstp = (byte) GCSTPGC;
      lua_checkstack(L, 10);
      lua_getfield(L, LUA_REGISTRYINDEX, new HostString(Lt.tracegc));
      lua_pushboolean(L, first);
      lua_call(L, 1, 0);
      g.gcstp = 0;
      lua_lock(L);
    }
  }

  private static int tracegc(lua_State L)
  {
    if (lua_isnil(L, 1) != 0)
      tracinggc = 0;
    else
    {
      tracinggc = 1;
      lua_setfield(L, LUA_REGISTRYINDEX, new HostString(Lt.tracegc));
    }
    return 0;
  }

  private static final lua_CFunction tracegc = Ltests::tracegc;

  private static int hash_query(lua_State L)
  {
    if (lua_isnone(L, 2) != 0)
    {
      TString ts;
      luaL_argcheck(L, lua_type(L, 1) == LUA_TSTRING, 1, new HostString(Lt.string_expected));
      TValue o = obj_at(L, 1);
      assert (o.tt_ & NOVARIANT_MASK) == LUA_TSTRING;
      ts = tsvalue(o);
      lua_pushinteger(L, ((int) (luaS_gethash(ts))));
    }
    else
    {
      TValue o = obj_at(L, 1);
      TValue o2 = obj_at(L, 2);
      Table t;
      luaL_checktype(L, 2, LUA_TTABLE);
      t = hvalue(o2);
      lua_pushinteger(L, ((long) (t.luaH_mainposition(o))));
    }
    return 1;
  }

  private static final lua_CFunction hash_query = Ltests::hash_query;

  private static int stacklevel(lua_State L)
  {
    lua_pushinteger(L, ((long) (L.top)));
    lua_pushinteger(L, stacksize(L));
    lua_pushinteger(L, Integer.toUnsignedLong(L.nCcalls));
    lua_pushinteger(L, L.nci);
    lua_pushinteger(L, StackWalker.getInstance().walk(s -> s.count()));
    return 5;
  }

  private static final lua_CFunction stacklevel = Ltests::stacklevel;

  private static int resetCI(lua_State L)
  {
    CallInfo ci = L.ci;
    while (ci.next != null)
    {
      CallInfo tofree = ci.next;
      ci.next = ci.next.next;
      luaM_free_CallInfo(L, tofree);
      L.nci--;
    }
    return 0;
  }

  private static final lua_CFunction resetCI = Ltests::resetCI;

  private static int reallocstack(lua_State L)
  {
    int n = ((int) (luaL_checkinteger(L, 1)));
    lua_lock(L);
    luaD_reallocstack(L, ((int) (L.top)) + n, 1);
    lua_unlock(L);
    return 0;
  }

  private static final lua_CFunction reallocstack = Ltests::reallocstack;

  private static int table_query(lua_State L)
  {
    Table t;
    int i = ((int) (luaL_optinteger(L, 2, -1)));
    int asize;
    luaL_checktype(L, 1, LUA_TTABLE);
    TValue o = obj_at(L, 1);
    t = hvalue(o);
    asize = t.asize();
    if (i == -1)
    {
      lua_pushinteger(L, Integer.toUnsignedLong(asize));
      lua_pushinteger(L, ((long) (allocsizenode(t))));
      lua_pushinteger(L, ((long) (asize > 0 ? t.lenhint : 0)));
      return 3;
    }
    else if (Integer.compareUnsigned(i, asize) < 0)
    {
      lua_pushinteger(L, i);
      if (!(((getArrTag(t, i)) & NOVARIANT_MASK) == LUA_TNIL))
      {
        TValue v = new TValue();
        arr2obj(t, Integer.toUnsignedLong(i), v);
        setslot(L, L.top, v);
      }
      else
        setslottag(L, L.top, LUA_VNIL);
      api_incr_top(L);
      lua_pushnil(L);
    }
    else if (Integer.compareUnsigned((i -= asize), sizenode(t)) < 0)
    {
      TValue k = new TValue();
      getnodekey(L, k, gnode(t, i));
      if (!((gnode(t, i).tt_ & NOVARIANT_MASK) == LUA_TNIL) ||
          ((k.tt_ & NOVARIANT_MASK) == LUA_TNIL) ||
          ((k.tt_ & NOVARIANT_MASK) == LUA_TNUMBER))
      {
        pushobject(L, k);
      }
      else
        lua_pushliteral(L, new HostString(Lt.undef));
      if (!((gnode(t, i).tt_ & NOVARIANT_MASK) == LUA_TNIL))
        pushobject(L, gnode(t, i));
      else
        lua_pushnil(L);
      lua_pushinteger(L, (t.node[i].next));
    }
    return 3;
  }

  private static final lua_CFunction table_query = Ltests::table_query;

  private static int gc_query(lua_State L)
  {
    global_State g = L.l_G;
    ByteView kind = g.gckind == KGC_INC ? Lt.inc : g.gckind == KGC_GENMAJOR ? Lt.genmajor : Lt.genminor;
    lua_pushstring(L, new HostString(kind));
    lua_pushlstring(L, statenames[g.gcstate], 0, statenames[g.gcstate].size());
    lua_pushinteger(L, ((long) (gettotalbytes(g))));
    lua_pushinteger(L, ((long) (g.GCdebt)));
    lua_pushinteger(L, ((long) (g.GCmarked)));
    lua_pushinteger(L, ((long) (g.GCmajorminor)));
    return 6;
  }

  private static final lua_CFunction gc_query = Ltests::gc_query;

  private static int test_codeparam(lua_State L)
  {
    long p = luaL_checkinteger(L, 1);
    lua_pushinteger(L, luaO_codeparam(Integer.toUnsignedLong((int) (p))));
    return 1;
  }

  private static final lua_CFunction test_codeparam = Ltests::test_codeparam;

  private static int test_applyparam(lua_State L)
  {
    long p = luaL_checkinteger(L, 1);
    long x = luaL_checkinteger(L, 2);
    lua_pushinteger(L, ((long) (luaO_applyparam(((int) (p)), x))));
    return 1;
  }

  private static final lua_CFunction test_applyparam = Ltests::test_applyparam;

  private static int string_query(lua_State L)
  {
    global_State.StringTable strt = L.l_G.strt;
    int s = ((int) (luaL_optinteger(L, 1, 0))) - 1;
    if (s == -1)
    {
      lua_pushinteger(L, ((int) (strt.buckets.length)));
      lua_pushinteger(L, ((int) (strt.count)));
      return 2;
    }
    else if (s < ((int) (strt.buckets.length)))
    {
      int n = 0;
      for (TString ts = strt.buckets[s]; ts != null; ts = ts.hnext)
      {
        setslots(L, L.top, ts);
        api_incr_top(L);
        n++;
      }
      return n;
    }
    else
      return 0;
  }

  private static final lua_CFunction string_query = Ltests::string_query;

  private static int getreftable(lua_State L)
  {
    if (lua_istable(L, 2) != 0) /* is there a table as second argument? */
      return 2;                 /* use it as the table */
    else
      return LUA_REGISTRYINDEX; /* default is to use the register */
  }

  private static int tref(lua_State L)
  {
    int t = getreftable(L);
    int level = lua_gettop(L);
    luaL_checkany(L, 1);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, luaL_ref(L, t));
    /* to avoid warnings */
    assert lua_gettop(L) == level + 1; /* +1 for result */
    return 1;
  }

  private static final lua_CFunction tref = Ltests::tref;

  private static int getref(lua_State L)
  {
    int t = getreftable(L);
    int level = lua_gettop(L);
    lua_rawgeti(L, t, luaL_checkinteger(L, 1));
    /* to avoid warnings */
    assert lua_gettop(L) == level + 1;
    return 1;
  }

  private static final lua_CFunction getref = Ltests::getref;

  private static int unref(lua_State L)
  {
    int t = getreftable(L);
    int level = lua_gettop(L);
    luaL_unref(L, t, ((int) (luaL_checkinteger(L, 1))));
    /* to avoid warnings */
    assert lua_gettop(L) == level;
    return 0;
  }

  private static final lua_CFunction unref = Ltests::unref;

  private static int upvalue(lua_State L)
  {
    int n = ((int) (luaL_checkinteger(L, 2)));
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (lua_isnone(L, 3) != 0)
    {
      HostString name = lua_getupvalue(L, 1, n);
      if (name == null)
        return 0;
      lua_pushstring(L, name);
      return 2;
    }
    else
    {
      HostString name = lua_setupvalue(L, 1, n);
      if (name != null)
        lua_pushstring(L, name);
      else
        lua_pushnil(L);
      return 1;
    }
  }

  private static final lua_CFunction upvalue = Ltests::upvalue;

  private static int newuserdata(lua_State L)
  {
    long size = luaL_optinteger(L, 1, 0);
    int nuv = ((int) (luaL_optinteger(L, 2, 0)));
    byte[] p = (byte[]) lua_newuserdatauv(L, size, nuv);
    int pi = 0;
    while (size-- != 0)
      p[pi++] = '\0';
    return 1;
  }

  private static final lua_CFunction newuserdata = Ltests::newuserdata;

  private static int pushuserdata(lua_State L)
  {
    long u = luaL_checkinteger(L, 1);
    lua_pushlightuserdata(L, as_userdata(u));
    return 1;
  }

  private static final lua_CFunction pushuserdata = Ltests::pushuserdata;

  private static int udataval(lua_State L)
  {
    lua_pushinteger(L, as_integer(lua_touserdata(L, 1)));
    return 1;
  }

  private static final lua_CFunction udataval = Ltests::udataval;

  private static int doonnewstack(lua_State L)
  {
    lua_State L1 = lua_newthread(L);
    ByteView sv1 = luaL_checklstring(L, 1);
    int status = luaL_loadbufferx(L1, sv1, HostString.from_bytes(sv1), new HostString(Lt.t_char));
    if (status == LUA_OK)
      status = lua_pcall(L1, 0, 0, 0);
    lua_pushinteger(L, status);
    return 1;
  }

  private static final lua_CFunction doonnewstack = Ltests::doonnewstack;

  private static int s2d(lua_State L)
  {
    lua_pushnumber(L, ((double) (luaL_checklstring(L, 1).toDouble(0))));
    return 1;
  }

  private static final lua_CFunction s2d = Ltests::s2d;

  private static int d2s(lua_State L)
  {
    double d = ((double) (luaL_checknumber(L, 1)));
    ByteOwner db = new ByteOwner(Double.BYTES);
    db.write_double(d, 0);
    lua_pushlstring(L, db.view(), 0, Double.BYTES);
    return 1;
  }

  private static final lua_CFunction d2s = Ltests::d2s;

  private static int num2int(lua_State L)
  {
    lua_pushinteger(L, lua_tointeger(L, 1));
    return 1;
  }

  private static final lua_CFunction num2int = Ltests::num2int;

  private static int makeseed(lua_State L)
  {
    lua_pushinteger(L, Integer.toUnsignedLong(luaL_makeseed(L)));
    return 1;
  }

  private static final lua_CFunction makeseed = Ltests::makeseed;

  private static int newstate(lua_State L)
  {
    lua_State L1 = lua_newstate(lua_getallocf(L), 0);
    if (L1 != null)
    {
      lua_atpanic(L1, tpanic);
      lua_pushlightuserdata(L, L1);
    }
    else
      lua_pushnil(L);
    return 1;
  }

  private static final lua_CFunction newstate = Ltests::newstate;

  private static lua_State getstate(lua_State L)
  {
    lua_State L1 = (lua_State) lua_touserdata(L, 1);
    luaL_argcheck(L, L1 != null, 1, new HostString(Lt.state_expected));
    return L1;
  }

  private static int loadlib(lua_State L)
  {
    lua_State L1 = getstate(L);
    int load = ((int) (luaL_checkinteger(L, 2)));
    int preload = ((int) (luaL_checkinteger(L, 3)));
    luaL_openselectedlibs(L1, load, preload);
    luaL_requiref(L1, new HostString(Lt.T_char), luaB_opentests, 0);
    assert lua_type(L1, -1) == LUA_TTABLE;
    /* 'requiref' should not reload module already loaded... */
    luaL_requiref(L1, new HostString(Lt.T_char), null, 1); /* seg. fault if it reloads */
    /* ...but should return the same module */
    assert lua_compare(L1, -1, -2, LUA_OPEQ) != 0;
    return 0;
  }

  private static final lua_CFunction loadlib = Ltests::loadlib;

  private static int closestate(lua_State L)
  {
    lua_State L1 = getstate(L);
    lua_close(L1);
    return 0;
  }

  private static final lua_CFunction closestate = Ltests::closestate;

  private static int doremote(lua_State L)
  {
    lua_State L1 = getstate(L);
    ByteView codev = luaL_checklstring(L, 2);
    int status;
    lua_settop(L1, 0);
    status = luaL_loadbufferx(L1, codev, HostString.from_bytes(codev), new HostString(Lt.t_char));
    if (status == LUA_OK)
      status = lua_pcall(L1, 0, LUA_MULTRET, 0);
    if (status != LUA_OK)
    {
      lua_pushnil(L);
      lua_pushstring(L, lua_tostring(L1, -1));
      lua_pushinteger(L, status);
      return 3;
    }
    else
    {
      int i = 0;
      while (lua_isnone(L1, ++i) == 0)
        lua_pushstring(L, lua_tostring(L1, i));
      lua_pop(L1, i - 1);
      return i - 1;
    }
  }

  private static final lua_CFunction doremote = Ltests::doremote;

  private static int log2_aux(lua_State L)
  {
    int x = (int) luaL_checkinteger(L, 1);
    lua_pushinteger(L, luaO_ceillog2(x));
    return 1;
  }

  private static final lua_CFunction log2_aux = Ltests::log2_aux;

  private static final class jmp_buf extends RuntimeException
  {
    jmp_buf()
    {
      super(null, null, false, false);
    }
  }

  private static final class Aux
  {
    final jmp_buf jb = new jmp_buf();
    String paniccode;
    lua_State L;
  }

  /*
  ** does a long-jump back to "main program".
  */
  private static int panicback(lua_State L)
  {
    Aux b;
    lua_checkstack(L, 1);                                         /* open space for 'Aux' struct */
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString(Lt._jmpbuf)); /* get 'Aux' struct */
    b = (Aux) lua_touserdata(L, -1);
    lua_pop(L, 1); /* remove 'Aux' struct */
    ByteView panicsv = to_view(b.paniccode);
    runC(b.L, L, panicsv); /* run optional panic code */
    throw b.jb;
  }

  private static final lua_CFunction panicback = Ltests::panicback;

  private static int checkpanic(lua_State L)
  {
    Aux b = new Aux();
    lua_State L1;
    ByteView code = luaL_checklstring(L, 1);
    lua_Alloc f = lua_getallocf(L);
    ByteView pc = luaL_optlstring(L, 2, Lt.empty_str);
    b.paniccode = to_string(pc);
    b.L = L;
    L1 = lua_newstate(f, 0); /* create new state */
    if (L1 == null)
    { /* error? */
      lua_pushstring(L, new HostString(Lt.not_enough_memory));
      return 1;
    }
    lua_atpanic(L1, panicback); /* set its panic function */
    lua_pushlightuserdata(L1, b);
    lua_setfield(L1, LUA_REGISTRYINDEX, new HostString(Lt._jmpbuf)); /* store 'Aux' struct */
    try
    {                    /* set jump buffer */
      runC(L, L1, code); /* run code unprotected */
      lua_pushliteral(L, new HostString(Lt.no_errors));
    }
    catch (jmp_buf e)
    { /* error handling */
      if (e != b.jb)
        throw e;
      /* move error message to original state */
      lua_pushstring(L, lua_tostring(L1, -1));
    }
    lua_close(L1);
    return 1;
  }

  private static final lua_CFunction checkpanic = Ltests::checkpanic;

  private static int externKstr(lua_State L)
  {
    int len;
    ByteView sv = luaL_checklstring(L, 1);
    ByteView s = sv;
    len = sv.size();
    lua_pushlstring(L, new ByteView(s.array(), s.offset(), len), 0, len);
    return 1;
  }

  private static final lua_CFunction externKstr = Ltests::externKstr;

  /*
  ** Create a buffer with the content of a given string and then
  ** create an external string using that buffer. Use the allocation
  ** function from Lua to create and free the buffer.
  */
  private static int externstr(lua_State L)
  {
    int len;
    ByteView sv = luaL_checklstring(L, 1);
    ByteView s = sv;
    len = sv.size();
    lua_pushlstring(L, new ByteView(s.array(), s.offset(), len), 0, len);
    return 1;
  }

  private static final lua_CFunction externstr = Ltests::externstr;

  /*
  ** {====================================================================
  ** function to test the API with C. It interprets a kind of assembler
  ** language with calls to the API, so the test can be driven by Lua code
  ** =====================================================================
  */

  private static final class CScript
  {
    ByteView code;
    int pos;

    CScript(ByteView code, int pos)
    {
      this.code = code;
      this.pos = pos;
    }
  }

  private static int curbyte(CScript sc)
  {
    return sv_byteat(sc.code, sc.pos);
  }

  private static final ByteView delimits = Lt.ws_delims;

  private static void skip(CScript sc)
  {
    for (;;)
    {
      int c = curbyte(sc);
      if (c != -1 && delimits.find((byte) c) != ByteView.npos)
        sc.pos++;
      else if (c == '#')
      { /* comment? */
        while (curbyte(sc) != '\n' && curbyte(sc) != -1)
          sc.pos++; /* until end-of-line */
      }
      else
        break;
    }
  }

  private static int getnum_aux(lua_State L, lua_State L1, CScript sc)
  {
    int res = 0;
    int sig = 1;
    skip(sc);
    if (curbyte(sc) == '.')
    {
      res = ((int) (lua_tointeger(L1, -1)));
      lua_pop(L1, 1);
      sc.pos++;
      return res;
    }
    else if (curbyte(sc) == '*')
    {
      res = lua_gettop(L1);
      sc.pos++;
      return res;
    }
    else if (curbyte(sc) == '!')
    {
      sc.pos++;
      if (curbyte(sc) == 'G')
        res = LUA_RIDX_GLOBALS;
      else if (curbyte(sc) == 'M')
        res = LUA_RIDX_MAINTHREAD;
      else
        assert false;
      sc.pos++;
      return res;
    }
    else if (curbyte(sc) == '-')
    {
      sig = -1;
      sc.pos++;
    }
    if (lisdigit(curbyte(sc)) == 0)
    {
      lua_pushstring(L, new HostString(sc.code.substr(sc.pos)));
      luaL_error(L, new HostString(Lt.number_expected_s));
    }
    while (lisdigit(curbyte(sc)) != 0)
    {
      res = res * 10 + (curbyte(sc) - '0');
      sc.pos++;
    }
    return sig * res;
  }

  private static String getstring_aux(lua_State L, CScript sc)
  {
    StringBuilder buff = new StringBuilder();
    skip(sc);
    int c = curbyte(sc);
    if (c == '"' || c == '\'')
    { /* quoted string? */
      int quote = c;
      sc.pos++;
      while ((c = curbyte(sc)) != quote)
      {
        if (c == -1)
          luaL_error(L, new HostString(Lt.unfinished_string_in_C_script));
        buff.append((char) c);
        sc.pos++;
      }
      sc.pos++;
    }
    else
    {
      while ((c = curbyte(sc)) != -1 && delimits.find((byte) c) == ByteView.npos)
      {
        buff.append((char) c);
        sc.pos++;
      }
    }
    return buff.toString();
  }

  private static int getindex_aux(lua_State L, lua_State L1, CScript sc)
  {
    skip(sc);
    int c = curbyte(sc);
    sc.pos++;
    switch (c)
    {
    case 'R':
      return LUA_REGISTRYINDEX;
    case 'U':
      return lua_upvalueindex(getnum_aux(L, L1, sc));
    default:
    {
      int n;
      sc.pos--; /* to read again */
      n = getnum_aux(L, L1, sc);
      if (n == 0)
        return 0;
      else
        return lua_absindex(L1, n);
    }
    }
  }

  private static final ByteView[] statcodes = {Lt.OK, Lt.YIELD, Lt.ERRRUN, Lt.ERRSYNTAX, Lt.not_enough_memory, Lt.ERRERR};

  /*
  ** Avoid these stat codes from being collected, to avoid possible
  ** memory error when pushing them.
  */
  private static void regcodes(lua_State L)
  {
    int i;
    for (i = 0; i < statcodes.length; i++)
    {
      lua_pushboolean(L, 1);
      lua_setfield(L, LUA_REGISTRYINDEX, new HostString(statcodes[i]));
    }
  }

  private static final ByteView ops = Lt.special_chars;

  private static final class X extends RuntimeException
  {
    int x;
  }

  private static final X x = new X();

  private static int runC(lua_State L, lua_State L1, ByteView code)
  {
    int status = 0;
    CScript sc = new CScript(code, 0);
    for (;;)
    {
      String instr = getstring_aux(L, sc);
      ByteView inst = to_view(instr);
      if (inst.empty())
        return 0;
      else if (inst.equals(Lt.absindex))
      {
        lua_pushinteger(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.append))
      {
        int t = getindex_aux(L, L1, sc);
        int i = ((int) (lua_rawlen(L1, t)));
        lua_rawseti(L1, t, i + 1);
      }
      else if (inst.equals(Lt.arith))
      {
        int op;
        skip(sc);
        op = ((int) (ops.find((byte) curbyte(sc))));
        sc.pos++;
        lua_arith(L1, op);
      }
      else if (inst.equals(Lt.call))
      {
        int narg = getnum_aux(L, L1, sc);
        int nres = getnum_aux(L, L1, sc);
        lua_call(L1, narg, nres);
      }
      else if (inst.equals(Lt.callk))
      {
        int narg = getnum_aux(L, L1, sc);
        int nres = getnum_aux(L, L1, sc);
        int i = getindex_aux(L, L1, sc);
        lua_callk(L1, narg, nres, i, Cfunck);
      }
      else if (inst.equals(Lt.checkstack))
      {
        int sz = getnum_aux(L, L1, sc);
        String msgstr = getstring_aux(L, sc);
        ByteView msg = to_view(msgstr);
        luaL_checkstack(L1, sz,
                        msg.empty() ? null
                                    : new HostString(msg));
      }
      else if (inst.equals(Lt.rawcheckstack))
      {
        int sz = getnum_aux(L, L1, sc);
        lua_pushboolean(L1, lua_checkstack(L1, sz));
      }
      else if (inst.equals(Lt.compare))
      {
        String opt = getstring_aux(L, sc); /* EQ, LT, or LE */
        int op = (opt.charAt(0) == 'E')   ? LUA_OPEQ
                 : (opt.charAt(1) == 'T') ? LUA_OPLT
                                          : LUA_OPLE;
        int a = getindex_aux(L, L1, sc);
        int b = getindex_aux(L, L1, sc);
        lua_pushboolean(L1, lua_compare(L1, a, b, op));
      }
      else if (inst.equals(Lt.concat))
      {
        lua_concat(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.copy))
      {
        int f = getindex_aux(L, L1, sc);
        lua_copy(L1, f, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.func2num))
      {
        lua_CFunction func = lua_tocfunction(L1, getindex_aux(L, L1, sc));
        lua_pushinteger(L1, as_integer(func));
      }
      else if (inst.equals(Lt.getfield))
      {
        int t = getindex_aux(L, L1, sc);
        int tp = lua_getfield(L1, t, to_hstring(getstring_aux(L, sc)));
        assert tp == lua_type(L1, -1);
      }
      else if (inst.equals(Lt.getglobal))
      {
        lua_getglobal(L1, to_hstring(getstring_aux(L, sc)));
      }
      else if (inst.equals(Lt.getmetatable))
      {
        if (lua_getmetatable(L1, getindex_aux(L, L1, sc)) == 0)
          lua_pushnil(L1);
      }
      else if (inst.equals(Lt.gettable))
      {
        int tp = lua_gettable(L1, getindex_aux(L, L1, sc));
        assert tp == lua_type(L1, -1);
      }
      else if (inst.equals(Lt.gettop))
      {
        lua_pushinteger(L1, lua_gettop(L1));
      }
      else if (inst.equals(Lt.gsub))
      {
        int a = getnum_aux(L, L1, sc);
        int b = getnum_aux(L, L1, sc);
        int c = getnum_aux(L, L1, sc);
        luaL_gsub(L1, lua_tostring(L1, a),
                  lua_tostring(L1, b),
                  lua_tostring(L1, c));
      }
      else if (inst.equals(Lt.insert))
      {
        lua_insert(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.iscfunction))
      {
        lua_pushboolean(L1, lua_iscfunction(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isfunction))
      {
        lua_pushboolean(L1, lua_isfunction(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isnil))
      {
        lua_pushboolean(L1, lua_isnil(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isnull))
      {
        lua_pushboolean(L1, lua_isnone(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isnumber))
      {
        lua_pushboolean(L1, lua_isnumber(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isstring))
      {
        lua_pushboolean(L1, lua_isstring(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.istable))
      {
        lua_pushboolean(L1, lua_istable(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isudataval))
      {
        lua_pushboolean(L1, lua_islightuserdata(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.isuserdata))
      {
        lua_pushboolean(L1, lua_isuserdata(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.len))
      {
        lua_len(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.Llen))
      {
        lua_pushinteger(L1, luaL_len(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.loadfile))
      {
        HostString fn = luaL_checkstring(L1, getnum_aux(L, L1, sc));
        luaL_loadfilex(L1, fn, new HostString(Lt.t_char));
      }
      else if (inst.equals(Lt.loadstring))
      {
        ByteView sv2 = luaL_checklstring(L1, getnum_aux(L, L1, sc));
        HostString name = to_hstring(getstring_aux(L, sc));
        HostString mode = to_hstring(getstring_aux(L, sc));
        luaL_loadbufferx(L1, sv2, name, mode);
      }
      else if (inst.equals(Lt.newmetatable))
      {
        lua_pushboolean(L1, luaL_newmetatable(L1, to_hstring(getstring_aux(L, sc))));
      }
      else if (inst.equals(Lt.newtable))
      {
        lua_newtable(L1);
      }
      else if (inst.equals(Lt.newthread))
      {
        lua_newthread(L1);
      }
      else if (inst.equals(Lt.resetthread))
      {
        lua_pushinteger(L1, lua_resetthread(L1)); /* deprecated */
      }
      else if (inst.equals(Lt.newuserdata))
      {
        lua_newuserdata(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.next_kw))
      {
        lua_next(L1, -2);
      }
      else if (inst.equals(Lt.objsize))
      {
        lua_pushinteger(L1, ((long) (lua_rawlen(L1, getindex_aux(L, L1, sc)))));
      }
      else if (inst.equals(Lt.pcall))
      {
        int narg = getnum_aux(L, L1, sc);
        int nres = getnum_aux(L, L1, sc);
        status = lua_pcall(L1, narg, nres, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pcallk))
      {
        int narg = getnum_aux(L, L1, sc);
        int nres = getnum_aux(L, L1, sc);
        int i = getindex_aux(L, L1, sc);
        status = lua_pcallk(L1, narg, nres, 0, i, Cfunck);
      }
      else if (inst.equals(Lt.pop))
      {
        lua_pop(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.printstack))
      {
        int n = getnum_aux(L, L1, sc);
        if (n != 0)
        {
          TValue v = getslot(L, L.ci.func + n);
          lua_printvalue(v);
          os_print_stdout(to_view(String.format("\n")));
        }
        else
          lua_printstack(L1);
      }
      else if (inst.equals(Lt.print))
      {
        String msg = getstring_aux(L, sc);
        os_print_stdout(to_view(String.format("%s\n", msg)));
      }
      else if (inst.equals(Lt.warningC))
      {
        lua_warning(L1, to_hstring(getstring_aux(L, sc)), 1);
      }
      else if (inst.equals(Lt.warning))
      {
        lua_warning(L1, to_hstring(getstring_aux(L, sc)), 0);
      }
      else if (inst.equals(Lt.pushbool))
      {
        lua_pushboolean(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pushcclosure))
      {
        lua_pushcclosure(L1, testC, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pushint))
      {
        lua_pushinteger(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pushnil))
      {
        lua_pushnil(L1);
      }
      else if (inst.equals(Lt.pushnum))
      {
        lua_pushnumber(L1, (double) getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pushstatus))
      {
        lua_pushistringref(L1, statcodes[status]);
      }
      else if (inst.equals(Lt.pushstring))
      {
        lua_pushstring(L1, to_hstring(getstring_aux(L, sc)));
      }
      else if (inst.equals(Lt.pushupvalueindex))
      {
        lua_pushinteger(L1, lua_upvalueindex(getnum_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.pushvalue))
      {
        lua_pushvalue(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.pushfstringI))
      {
        HostString fmt = lua_tostring(L, -2);
        if (luaO_fmtargcount(fmt.to_utf8()) > 0)
          lua_pushinteger(L1, lua_tointeger(L, -1));
        lua_pushfstring(L1, fmt);
      }
      else if (inst.equals(Lt.pushfstringS))
      {
        HostString fmt = lua_tostring(L, -2);
        if (luaO_fmtargcount(fmt.to_utf8()) > 0)
          lua_pushstring(L1, lua_tostring(L, -1));
        lua_pushfstring(L1, fmt);
      }
      else if (inst.equals(Lt.pushfstringP))
      {
        HostString fmt = lua_tostring(L, -2);
        if (luaO_fmtargcount(fmt.to_utf8()) > 0)
        {
          if (lua_islightcfunction(L, -1) != 0)
          {
            InternalString fa = lua_funcaddressi(L, -1);
            lua_pushistring(L1, fa);
          }
          else
          {
            lua_pushlightuserdata(L1, lua_topointer(L, -1));
          }
        }
        lua_pushfstring(L1, fmt);
      }
      else if (inst.equals(Lt.rawget))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawget(L1, t);
      }
      else if (inst.equals(Lt.rawgeti))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawgeti(L1, t, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.rawgetp))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawgetp(L1, t, as_userdata(getnum_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.rawset))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawset(L1, t);
      }
      else if (inst.equals(Lt.rawseti))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawseti(L1, t, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.rawsetp))
      {
        int t = getindex_aux(L, L1, sc);
        lua_rawsetp(L1, t, as_userdata(getnum_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.remove_kw))
      {
        lua_remove(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.replace))
      {
        lua_replace(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.resume))
      {
        int i = getindex_aux(L, L1, sc);
        status = lua_resume(lua_tothread(L1, i), L, getnum_aux(L, L1, sc)).status;
      }
      else if (inst.equals(Lt.traceback))
      {
        HostString msg = to_hstring(getstring_aux(L, sc));
        int level = getnum_aux(L, L1, sc);
        luaL_traceback(L1, L1, msg, level);
      }
      else if (inst.equals(Lt.threadstatus))
      {
        lua_pushistringref(L1, statcodes[lua_status(L1)]);
      }
      else if (inst.equals(Lt.alloccount))
      {
        l_memcontrol.countlimit = Integer.toUnsignedLong(getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.return_kw))
      {
        int n = getnum_aux(L, L1, sc);
        if (L1 != L)
        {
          int i;
          for (i = 0; i < n; i++)
          {
            int idx = -(n - i);
            switch (lua_type(L1, idx))
            {
            case LUA_TBOOLEAN:
              lua_pushboolean(L, lua_toboolean(L1, idx));
              break;
            default:
              lua_pushstring(L, lua_tostring(L1, idx));
              break;
            }
          }
        }
        return n;
      }
      else if (inst.equals(Lt.rotate))
      {
        int i = getindex_aux(L, L1, sc);
        lua_rotate(L1, i, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.setfield))
      {
        int t = getindex_aux(L, L1, sc);
        lua_setfield(L1, t, to_hstring(getstring_aux(L, sc)));
      }
      else if (inst.equals(Lt.seti))
      {
        int t = getindex_aux(L, L1, sc);
        lua_seti(L1, t, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.setglobal))
      {
        lua_setglobal(L1, to_hstring(getstring_aux(L, sc)));
      }
      else if (inst.equals(Lt.sethook))
      {
        int mask = getnum_aux(L, L1, sc);
        int count = getnum_aux(L, L1, sc);
        String s = getstring_aux(L, sc);
        sethookaux(L1, mask, count, to_view(s));
      }
      else if (inst.equals(Lt.setmetatable))
      {
        int idx = getindex_aux(L, L1, sc);
        lua_setmetatable(L1, idx);
      }
      else if (inst.equals(Lt.settable))
      {
        lua_settable(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.settop))
      {
        lua_settop(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.testudata))
      {
        int i = getindex_aux(L, L1, sc);
        lua_pushboolean(L1, (luaL_testudata(L1, i, to_hstring(getstring_aux(L, sc))) != null) ? 1 : 0);
      }
      else if (inst.equals(Lt.error_kw))
      {
        lua_error(L1);
      }
      else if (inst.equals(Lt.abort_kw))
      {
        Runtime.getRuntime().halt(134);
      }
      else if (inst.equals(Lt.throw_kw))
      {
        throw x;
      }
      else if (inst.equals(Lt.tobool))
      {
        lua_pushboolean(L1, lua_toboolean(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.tocfunction))
      {
        /* copy the function value directly to preserve identity */
        lua_pushvalue(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.tointeger))
      {
        lua_pushinteger(L1, lua_tointeger(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.tonumber))
      {
        lua_pushnumber(L1, lua_tonumber(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.topointer))
      {
        int idx = getindex_aux(L, L1, sc);
        if (lua_islightcfunction(L1, idx) != 0)
        {
          InternalString fa = lua_funcaddressi(L1, idx);
          lua_pushistring(L1, fa);
        }
        else
        {
          Object p = lua_topointer(L1, idx);
          lua_pushlightuserdata(L1, p);
        }
      }
      else if (inst.equals(Lt.touserdata))
      {
        lua_pushlightuserdata(L1, lua_touserdata(L1, getindex_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.tostring))
      {
        int i = getindex_aux(L, L1, sc);
        if (lua_tolstring(L1, i) == null) /* not convertible? */
          lua_pushnil(L1);
        else
        {
          HostString s = lua_tostring(L1, i);
          lua_pushstring(L1, s);
          assert s.equals(lua_tostring(L1, -1));
        }
      }
      else if (inst.equals(Lt.Ltolstring))
      {
        luaL_tolstring(L1, getindex_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.type))
      {
        lua_pushstring(L1, luaL_typename(L1, getnum_aux(L, L1, sc)));
      }
      else if (inst.equals(Lt.xmove))
      {
        int f = getindex_aux(L, L1, sc);
        int t = getindex_aux(L, L1, sc);
        lua_State fs = (f == 0) ? L1 : lua_tothread(L1, f);
        lua_State ts = (t == 0) ? L1 : lua_tothread(L1, t);
        int n = getnum_aux(L, L1, sc);
        if (n == 0)
          n = lua_gettop(fs);
        lua_xmove(fs, ts, n);
      }
      else if (inst.equals(Lt.isyieldable))
      {
        lua_pushboolean(L1, lua_isyieldable(lua_tothread(L1, getindex_aux(L, L1, sc))));
      }
      else if (inst.equals(Lt.yield_kw))
      {
        return lua_yield(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.yieldk))
      {
        int nres = getnum_aux(L, L1, sc);
        int i = getindex_aux(L, L1, sc);
        return lua_yieldk(L1, nres, i, Cfunck);
      }
      else if (inst.equals(Lt.toclose))
      {
        lua_toclose(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.closeslot))
      {
        lua_closeslot(L1, getnum_aux(L, L1, sc));
      }
      else if (inst.equals(Lt.argerror))
      {
        int arg = getnum_aux(L, L1, sc);
        luaL_argerror(L1, arg, to_hstring(getstring_aux(L, sc)));
      }
      else
      {
        lua_pushstring(L, new HostString(inst));
        luaL_error(L, new HostString(Lt.unknown_instruction_s));
      }
    }
  }

  private static int testC(lua_State L)
  {
    lua_State L1;
    ByteView pc;
    if (lua_isuserdata(L, 1) != 0)
    {
      L1 = getstate(L);
      pc = luaL_checklstring(L, 2);
    }
    else if (lua_isthread(L, 1) != 0)
    {
      L1 = lua_tothread(L, 1);
      pc = luaL_checklstring(L, 2);
    }
    else
    {
      L1 = L;
      pc = luaL_checklstring(L, 1);
    }
    return runC(L, L1, pc);
  }

  private static final lua_CFunction testC = Ltests::testC;

  private static int Cfunc(lua_State L)
  {
    ByteView script = lua_tolstring(L, lua_upvalueindex(1));
    return runC(L, L, script);
  }

  private static final lua_CFunction Cfunc = Ltests::Cfunc;

  private static int Cfunck(lua_State L, int status, long ctx)
  {
    lua_pushistringref(L, statcodes[status]);
    lua_setglobal(L, new HostString(Lt.status));
    lua_pushinteger(L, ((long) (ctx)));
    lua_setglobal(L, new HostString(Lt.ctx));
    ByteView script = lua_tolstring(L, ((int) (ctx)));
    return runC(L, L, script);
  }

  private static final lua_KFunction Cfunck = Ltests::Cfunck;

  private static int makeCfunc(lua_State L)
  {
    luaL_checkstring(L, 1);
    lua_pushcclosure(L, Cfunc, lua_gettop(L));
    return 1;
  }

  private static final lua_CFunction makeCfunc = Ltests::makeCfunc;

  /* }====================================================== */

  /*
  ** {======================================================
  ** tests for C hooks
  ** =======================================================
  */

  /*
  ** C hook that runs the C script stored in registry.C_HOOK[L]
  */
  private static void Chook(lua_State L, lua_Debug ar)
  {
    ByteView[] events = {Lt.call, Lt.ret, Lt.line, Lt.count, Lt.tailcall};
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString(Lt.C_HOOK));
    lua_pushlightuserdata(L, L);
    lua_gettable(L, -2); /* get C_HOOK[L] (script saved by sethookaux) */
    ByteView scptv = lua_tostring(L, -1).to_utf8();
    String scpt = to_string(scptv.substr(0, scptv.find(0)));
    lua_pop(L, 2);                            /* remove C_HOOK and script */
    lua_pushistringref(L, events[ar.event]); /* may be used by script */
    lua_pushinteger(L, ar.currentline);      /* may be used by script */
    runC(L, L, to_view(scpt));               /* run script from C_HOOK[L] */
  }

  private static final lua_Hook Chook = Ltests::Chook;

  /*
  ** sets 'registry.C_HOOK[L] = scpt' and sets 'Chook' as a hook
  */
  private static void sethookaux(lua_State L, int mask, int count, ByteView scpt)
  {
    if (scpt.empty())
    {                               /* no script? */
      lua_sethook(L, null, 0, 0); /* turn off hooks */
      return;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString(Lt.C_HOOK)); /* get C_HOOK table */
    if (lua_istable(L, -1) == 0)
    {                  /* no hook table? */
      lua_pop(L, 1);   /* remove previous value */
      lua_newtable(L); /* create new C_HOOK table */
      lua_pushvalue(L, -1);
      lua_setfield(L, LUA_REGISTRYINDEX, new HostString(Lt.C_HOOK)); /* register it */
    }
    lua_pushlightuserdata(L, L);
    lua_pushlstring(L, scpt, 0, scpt.size());
    lua_settable(L, -3); /* C_HOOK[L] = script */
    lua_sethook(L, Chook, mask, count);
  }

  private static int sethook(lua_State L)
  {
    if (lua_isnoneornil(L, 1) != 0)
      lua_sethook(L, null, 0, 0); /* turn off hooks */
    else
    {
      ByteView scpt = luaL_checklstring(L, 1);
      ByteView smask = luaL_checklstring(L, 2);
      int count = ((int) (luaL_optinteger(L, 3, 0)));
      int mask = 0;
      if (smask.find('c') != ByteView.npos)
        mask |= LUA_MASKCALL;
      if (smask.find('r') != ByteView.npos)
        mask |= LUA_MASKRET;
      if (smask.find('l') != ByteView.npos)
        mask |= LUA_MASKLINE;
      if (count > 0)
        mask |= LUA_MASKCOUNT;
      sethookaux(L, mask, count, scpt);
    }
    return 0;
  }

  private static final lua_CFunction sethook = Ltests::sethook;

  private static int coresume(lua_State L)
  {
    lua_State co = lua_tothread(L, 1);
    luaL_argcheck(L, co != null, 1, new HostString(Lt.coroutine_expected));
    lua_ResumeResult rr = lua_resume(co, L, 0);
    int status = rr.status;
    if (status != LUA_OK && status != LUA_YIELD)
    {
      lua_pushboolean(L, 0);
      lua_insert(L, -2);
      return 2; /* return false + error message */
    }
    else
    {
      lua_pushboolean(L, 1);
      return 1;
    }
  }

  private static final lua_CFunction coresume = Ltests::coresume;

  private static int nonblock(lua_State L)
  {
    OsFile f = ((luaL_Stream) luaL_checkuserobject(L, 1, new HostString(LUA_FILEHANDLE))).f;
    os_setnonblock(f);
    return 0;
  }

  private static final lua_CFunction nonblock = Ltests::nonblock;

  /* }====================================================== */

  private static final luaL_Regi[] tests_funcs = {
      new luaL_Regi(Lt.checkmemory, lua_checkmemory),
      new luaL_Regi(Lt.closestate, closestate),
      new luaL_Regi(Lt.d2s, d2s),
      new luaL_Regi(Lt.doonnewstack, doonnewstack),
      new luaL_Regi(Lt.doremote, doremote),
      new luaL_Regi(Lt.gccolor, gc_color),
      new luaL_Regi(Lt.gcage, gc_age),
      new luaL_Regi(Lt.gcstate, gc_state),
      new luaL_Regi(Lt.tracegc, tracegc),
      new luaL_Regi(Lt.pobj, gc_printobj),
      new luaL_Regi(Lt.getref, getref),
      new luaL_Regi(Lt.hash, hash_query),
      new luaL_Regi(Lt.log2, log2_aux),
      new luaL_Regi(Lt.limits, get_limits),
      new luaL_Regi(Lt.listcode, listcode),
      new luaL_Regi(Lt.printcode, printcode),
      new luaL_Regi(Lt.printallstack, lua_printallstack),
      new luaL_Regi(Lt.listk, listk),
      new luaL_Regi(Lt.listabslineinfo, listabslineinfo),
      new luaL_Regi(Lt.listlocals, listlocals),
      new luaL_Regi(Lt.loadlib, loadlib),
      new luaL_Regi(Lt.checkpanic, checkpanic),
      new luaL_Regi(Lt.newstate, newstate),
      new luaL_Regi(Lt.newuserdata, newuserdata),
      new luaL_Regi(Lt.num2int, num2int),
      new luaL_Regi(Lt.makeseed, makeseed),
      new luaL_Regi(Lt.pushuserdata, pushuserdata),
      new luaL_Regi(Lt.gcquery, gc_query),
      new luaL_Regi(Lt.querystr, string_query),
      new luaL_Regi(Lt.querytab, table_query),
      new luaL_Regi(Lt.codeparam, test_codeparam),
      new luaL_Regi(Lt.applyparam, test_applyparam),
      new luaL_Regi(Lt.ref, tref),
      new luaL_Regi(Lt.resume, coresume),
      new luaL_Regi(Lt.s2d, s2d),
      new luaL_Regi(Lt.sethook, sethook),
      new luaL_Regi(Lt.stacklevel, stacklevel),
      new luaL_Regi(Lt.resetCI, resetCI),
      new luaL_Regi(Lt.reallocstack, reallocstack),
      new luaL_Regi(Lt.sizes, get_sizes),
      new luaL_Regi(Lt.testC, testC),
      new luaL_Regi(Lt.makeCfunc, makeCfunc),
      new luaL_Regi(Lt.totalmem, mem_query),
      new luaL_Regi(Lt.alloccount, alloc_count),
      new luaL_Regi(Lt.allocfailnext, alloc_failnext),
      new luaL_Regi(Lt.trick, settrick),
      new luaL_Regi(Lt.udataval, udataval),
      new luaL_Regi(Lt.unref, unref),
      new luaL_Regi(Lt.upvalue, upvalue),
      new luaL_Regi(Lt.externKstr, externKstr),
      new luaL_Regi(Lt.externstr, externstr),
      new luaL_Regi(Lt.nonblock, nonblock)};

  private static void checkfinalmem()
  {
    if (l_memcontrol.numblocks != 0) {
      os_print_stderr(to_view(String.format("[checkfinalmem] numblocks=%s total=%s\n",
                                            Long.toUnsignedString(l_memcontrol.numblocks), Long.toUnsignedString(l_memcontrol.total))));
    }
    assert l_memcontrol.numblocks == 0;
    assert l_memcontrol.total == 0;
  }

  public static int luaB_opentests(lua_State L)
  {
    lua_Alloc f = lua_getallocf(L);
    lua_atpanic(L, tpanic);
    lua_setwarnf(L, test_warnf);
    lua_pushboolean(L, 0);
    lua_setglobal(L, new HostString(Lt._WARN)); /* _WARN = false */
    regcodes(L);
    Runtime.getRuntime().addShutdownHook(new Thread(Ltests::checkfinalmem));
    assert f == debug_alloc;
    lua_setallocf(L, f); /* exercise this function */
    luaL_newlibi(L, tests_funcs);
    return 1;
  }

  public static final lua_CFunction luaB_opentests = Ltests::luaB_opentests;

  public static lua_State luai_newstate()
  {
    return lua_newstate(debug_alloc, luaL_makeseed(null));
  }

  public static void luai_openlibstest(lua_State L)
  {
    luaL_openlibs(L);
    luaL_requiref(L, new HostString(Lt.T_char), luaB_opentests, 1);
    lua_pop(L, 1);
  }
}
