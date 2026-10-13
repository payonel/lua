package com.payonel.lungolua.vm;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaConf.*;
import static com.payonel.lungolua.bytes.InternalString.sv_byteat;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.runtime.Ltm.*;
import static com.payonel.lungolua.vm.Lopcodes.*;
import static com.payonel.lungolua.vm.Lvm.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.InternalDebug;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_Debug;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.Closure;
import com.payonel.lungolua.object.FmtArgs;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.runtime.CallInfo;
import com.payonel.lungolua.runtime.ProtectedCall;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

public final class Ldebug
{
  private Ldebug()
  {
  }

  /* Active Lua function (given call info) */
  public static LClosure ci_func(lua_State L, CallInfo ci)
  {
    return slotclLval(L, ci.func);
  }

  public static void resethookcount(lua_State L)
  {
    L.hookcount = L.basehookcount;
  }

  /*
  ** mark for entries in 'lineinfo' array that has absolute information in
  ** 'abslineinfo' array
  */
  public static final int ABSLINEINFO = -0x80;

  public static final class LocalSlot
  {
    public InternalString name;
    public int pos;

    public LocalSlot(InternalString name, int pos)
    {
      this.name = name;
      this.pos = pos;
    }
  }

  public static final int UPVALORIGIN = NOSLOT - 1;

  public static int upvalorigin(int uvidx)
  {
    return UPVALORIGIN - uvidx;
  }

  public static RuntimeException luaG_forerror(lua_State L, int idx, ByteView what)
  {
    TValue v = getslot(L, idx);
    throw luaG_forerror(L, v, what);
  }

  public static RuntimeException luaG_callerror(lua_State L, int idx)
  {
    TValue v = getslot(L, idx);
    throw luaG_callerror(L, v, idx);
  }

  public static RuntimeException luaG_concaterror(lua_State L, int p1, int p2)
  {
    TValue v1 = getslot(L, p1);
    TValue v2 = getslot(L, p2);
    throw luaG_concaterror(L, v1, v2, p1, p2);
  }

  private static int LuaClosure(Closure f)
  {
    return (f != null && f.tt == LUA_VLCL) ? 1 : 0;
  }

  private static final InternalString[] tmplainname = {
      new InternalString(Lt.index_kw), new InternalString(Lt.newindex),
      new InternalString(Lt.gc), new InternalString(Lt.mode), new InternalString(Lt.len), new InternalString(Lt.eq),
      new InternalString(Lt.add), new InternalString(Lt.sub), new InternalString(Lt.mul), new InternalString(Lt.mod), new InternalString(Lt.pow),
      new InternalString(Lt.div), new InternalString(Lt.idiv),
      new InternalString(Lt.band), new InternalString(Lt.bor), new InternalString(Lt.bxor), new InternalString(Lt.shl), new InternalString(Lt.shr),
      new InternalString(Lt.unm), new InternalString(Lt.bnot), new InternalString(Lt.lt), new InternalString(Lt.le),
      new InternalString(Lt.concat), new InternalString(Lt.call), new InternalString(Lt.close_kw)};

  static
  {
    assert tmplainname.length == TM_N : "tmplainname must cover every metamethod (ORDER TM)";
  }

  private static final class VarName
  {
    ByteView kind;
    InternalString name;

    VarName(ByteView kind, InternalString name)
    {
      this.kind = kind;
      this.name = name;
    }
  }

  private static int currentpc(lua_State L, CallInfo ci)
  {
    assert isLua(ci) != 0;
    return ci.savedpc - 1;
  }

  /*
  ** Get a "base line" to find the line corresponding to an instruction.
  ** Base lines are regularly placed at MAXIWTHABS intervals, so usually
  ** an integer division gets the right place. When the source file has
  ** large sequences of empty/comment lines, it may need extra entries,
  ** so the original estimate needs a correction.
  ** If the original estimate is -1, the initial 'if' ensures that the
  ** 'while' will run at least once.
  ** The assertion that the estimate is a lower bound for the correct base
  ** is valid as long as the debug info has been generated with the same
  ** value for MAXIWTHABS or smaller. (Previous releases use a little
  ** smaller value.)
  */
  private static final class BaseLine
  {
    int pc;
    int line;

    BaseLine(int pc, int line)
    {
      this.pc = pc;
      this.line = line;
    }
  }

  private static BaseLine getbaseline(Proto f, int pc)
  {
    if (f.abslineinfo.length == 0 || pc < f.abslineinfo[0].pc)
    {
      return new BaseLine(-1, f.linedefined); /* start from the beginning */
    }
    else
    {
      int i = pc / MAXIWTHABS - 1; /* get an estimate */
      /* estimate must be a lower bound of the correct base */
      assert i < 0 ||
                 (i < (int) f.abslineinfo.length && f.abslineinfo[i].pc <= pc);
      while (i + 1 < (int) f.abslineinfo.length && pc >= f.abslineinfo[i + 1].pc)
        i++; /* low estimate; adjust it */
      return new BaseLine(f.abslineinfo[i].pc, f.abslineinfo[i].line);
    }
  }

  /*
  ** Get the line corresponding to instruction 'pc' in function 'f';
  ** first gets a base line and from there does the increments until
  ** the desired instruction.
  */
  public static int luaG_getfuncline(Proto f, int pc)
  {
    if (f.lineinfo.length == 0) /* no debug information? */
      return -1;
    else
    {
      BaseLine base = getbaseline(f, pc);
      int basepc = base.pc;
      int baseline = base.line;
      while (basepc++ < pc)
      { /* walk until given instruction */
        assert f.lineinfo[basepc] != ABSLINEINFO;
        baseline += f.lineinfo[basepc]; /* correct line */
      }
      return baseline;
    }
  }

  private static int getcurrentline(lua_State L, CallInfo ci)
  {
    return luaG_getfuncline(ci_func(L, ci).p, currentpc(L, ci));
  }

  /*
  ** Set 'trap' for all active Lua frames.
  ** This function can be called during a signal, under "reasonable"
  ** assumptions. A new 'ci' is completely linked in the list before it
  ** becomes part of the "active" list, and we assume that pointers are
  ** atomic; see comment in next function.
  ** (A compiler doing interprocedural optimizations could, theoretically,
  ** reorder memory writes in such a way that the list could be
  ** temporarily broken while inserting a new element. We simply assume it
  ** has no good reasons to do that.)
  */
  private static void settraps(CallInfo ci)
  {
    for (; ci != null; ci = ci.previous)
      if (isLua(ci) != 0)
        ci.trap = 1;
  }

  /*
  ** This function can be called during a signal, under "reasonable"
  ** assumptions.
  ** Fields 'basehookcount' and 'hookcount' (set by 'resethookcount')
  ** are for debug only, and it is no problem if they get arbitrary
  ** values (causes at most one wrong hook call). 'hookmask' is an atomic
  ** value. We assume that pointers are atomic too (e.g., gcc ensures that
  ** for all platforms where it runs). Moreover, 'hook' is always checked
  ** before being called (see 'luaD_hook').
  */
  public static void lua_sethook(lua_State L, lua_Hook func, int mask, int count)
  {
    if (func == null || mask == 0)
    { /* turn off hooks? */
      mask = 0;
      func = null;
    }
    L.hook = func;
    L.basehookcount = count;
    resethookcount(L);
    L.hookmask = (mask & 0xFF) | (L.hookmask & LUA_MASKBUDGET);
    if (mask != 0)
      settraps(L.ci); /* to trace inside 'luaV_execute' */
  }

  /*
  ** A new budget group for 'co', made in protected mode on 'co', so that a
  ** failure raises nothing on it (see 'lua_setbudget').
  */
  private static final class NewGroup implements ProtectedCall
  {
    long n = 0;
    lua_Budget f = null;
    BudgetGroup bg = null;

    @Override
    public void run(lua_State co)
    {
      BudgetGroup g1 = luaM_malloc_BudgetGroup(co);
      luaC_link(co, g1, LUA_VBUDGET);
      g1.budget = (long) n;
      g1.left = (long) n;
      g1.f = f;
      g1.root = co;
      if (luaC_newowner(co, g1))
        bg = g1; /* (otherwise too many groups: the new one is garbage) */
    }
  }

  /*
  ** Set the budget of thread 'co'. A thread with no budget group gets a new
  ** one, which it belongs to and passes to the threads it creates (and,
  ** while they run, to the ones it resumes); a thread that has one updates
  ** it in place: the limit and the callback are those of every thread of the
  ** group, and its steps start anew (in the group's own callback, when the
  ** callback ends). 'n' 0 removes the thread from its group. The group is in
  ** effect at once, by the rule of a resume ('luaE_enterbudget'), unless the
  ** thread is suspended: then at its next resume. So a thread never runs
  ** without its budget once it has been set. Returns LUA_OK, or LUA_ERRMEM
  ** (no memory for a new group) or LUA_ERRRUN (too many groups at once), and
  ** then nothing changed: no error is raised, on 'co' or elsewhere.
  */
  public static int lua_setbudget(lua_State co, long n, lua_Budget f)
  {
    lua_lock(co);
    global_State g = co.l_G;
    if (n <= 0 || f == null)
    { /* no budget */
      co.bown = null;
    }
    else if (co.bown != null)
    { /* the group of the thread, updated */
      BudgetGroup bg = co.bown;
      bg.budget = (long) n;
      bg.f = f;
      if (!(g.budgetL != null && g.budgetg == bg)) /* (its callback refills it as it ends) */
        bg.left = (long) n;
    }
    else
    {
      NewGroup ng = new NewGroup();
      ng.n = n;
      ng.f = f;
      byte status = luaD_rawrunprotected(co, ng);
      if (status != LUA_OK || ng.bg == null)
      {
        lua_unlock(co);
        return (status != LUA_OK) ? LUA_ERRMEM : LUA_ERRRUN;
      }
      co.bown = ng.bg;
    }
    if (co.status != LUA_YIELD) /* (a suspended thread gets it at its next resume) */
      luaE_enterbudget(co, co.resumer); /* as at a resume: no thread runs without its budget */
    lua_unlock(co);
    return LUA_OK;
  }

  /* the thread the budget group of 'co' was set on (nothing once freed) */
  public static lua_State lua_budgetroot(lua_State co)
  {
    return (co.bown != null) ? co.bown.root : null;
  }

  /*
  ** The value for the callback of the group of 'co' (its own group, or the
  ** one in effect): set it by popping a value from 'L', or push it onto 'L'.
  */
  public static void lua_setbudgetvalue(lua_State L, lua_State co)
  {
    lua_lock(L);
    BudgetGroup bg = co.bown;
    assert bg != null : "no budget";
    bg.cb = getslot(L, L.top - 1);
    luaC_barrier(L, bg, bg.cb);
    L.top--;
    lua_unlock(L);
  }

  public static void lua_pushbudgetvalue(lua_State L, lua_State co, int effective)
  {
    lua_lock(L);
    BudgetGroup bg = (effective != 0) ? co.beff : co.bown;
    if (bg != null)
      setslot(L, L.top, bg.cb);
    else
      setslottag(L, L.top, LUA_VNIL);
    api_incr_top(L);
    lua_unlock(L);
  }

  /*
  ** A thread to run the budget callback's Lua code on (kept by its owner,
  ** such as the debug library: anchored there), so that the thread that
  ** spent the budget keeps its stack and C calls as they are.
  */
  public static lua_State lua_budgetthread(lua_State L)
  {
    return L.l_G.budgetth;
  }

  /*
  ** The room a callback needs is reserved in its thread (the collector does
  ** not trim it): the stack and a number of frames, so that the callback
  ** allocates neither, up to a depth that no sane callback reaches.
  */
  private static final int BUDGET_STACK = 1000;
  private static final int BUDGET_FRAMES = 40;

  public static void lua_setbudgetthread(lua_State L, lua_State B)
  {
    boolean ok = luaD_growstack(B, BUDGET_STACK, 0) != 0;
    while (ok && B.nci < BUDGET_FRAMES)
      ok = luaE_extendCI(B, 0) != null;
    if (!ok)
    { /* (not the state's thread of callbacks yet: the next budget set tries again) */
      lua_lock(L); /* (as for any throw) */
      luaD_throw(L, (byte) LUA_ERRMEM);
    }
    L.l_G.budgetth = B;
  }

  /* push onto 'L' the function of the frame 'ar' of thread 'L1' */
  public static void lua_pushframefunction(lua_State L, lua_State L1, InternalDebug ar)
  {
    lua_lock(L);
    TValue f = getslot(L1, ar.i_ci.func);
    setslot(L, L.top, f);
    api_incr_top(L);
    lua_unlock(L);
  }

  /* do '__gc' of the budget group of 'L' wait to be called? */
  public static int lua_hasdrain(lua_State L)
  { /* (and can they be called here: where the chain can freeze, outside hooks
       and the '__gc' being called (hooks off), outside the budget callback,
       with room for the call; the same as at an instruction boundary, see
       'luaG_traceexec' and 'drainpoint') */
    lua_lock(L);
    int has = (luaC_hasdrain(L) && L.allowhook != 0 && L.l_G.budgetL == null && luaD_nofreeze(L) == 0 &&
               luaD_checkminstack(L) != 0) ? 1 : 0;
    lua_unlock(L);
    return has;
  }

  /* push the function that calls them (see 'luaC_pushdrain') */
  public static void lua_pushdrain(lua_State L)
  {
    lua_lock(L);
    luaC_pushdrain(L);
    lua_unlock(L);
  }

  /* is 'L' running budgeted code, or the budget callback? */
  public static int lua_budgeted(lua_State L)
  {
    return (L.beff != null || L.l_G.budgetL != null) ? 1 : 0;
  }

  public static long lua_getbudget(lua_State co)
  {
    BudgetGroup bg = co.bown;
    return bg == null ? 0 : bg.budget;
  }

  public static long lua_budgetleft(lua_State co)
  {
    global_State g = co.l_G;
    BudgetGroup bg = co.bown;
    if (bg == null)
      return 0;
    /* (in the callback, it refills when it ends) */
    return (g.budgetL != null && g.budgetg == bg) ? bg.budget : bg.left;
  }

  public static lua_Hook lua_gethook(lua_State L)
  {
    return L.hook;
  }

  public static int lua_gethookmask(lua_State L)
  {
    return L.hookmask & ~LUA_MASKBUDGET;
  }

  public static int lua_gethookcount(lua_State L)
  {
    return L.basehookcount;
  }

  public static InternalDebug lua_getstacki(lua_State L, int level)
  {
    CallInfo ci;
    if (level < 0)
      return null; /* invalid (negative) level */
    lua_lock(L);
    for (ci = L.ci; level > 0 && ci != L.base_ci; ci = ci.previous)
      level--;
    if (level == 0 && ci != L.base_ci)
    { /* level found? */
      lua_unlock(L);
      InternalDebug ar = new InternalDebug();
      ar.i_ci = ci;
      return ar;
    }
    lua_unlock(L);
    return null; /* no such level */
  }

  public static lua_Debug lua_getstack(lua_State L, int level)
  {
    InternalDebug iar = lua_getstacki(L, level);
    if (iar == null)
      return null;
    lua_Debug ar = new lua_Debug();
    ar.i_ci = iar.i_ci;
    return ar;
  }

  private static InternalString upvalname(Proto p, int uv)
  {
    assert uv < (int) p.upvalues.length;
    TString s = p.upvalues[uv].name;
    if (s == null)
      return Lt.qmark_i;
    else
      return s.data;
  }

  private static LocalSlot findvararg(lua_State L, CallInfo ci, int n)
  {
    int fv = ci.func;
    LClosure cl = slotclLval(L, fv);
    if ((cl.p.flag & PF_VAHID) != 0)
    {
      int nextra = ci.nextraargs;
      if (n >= -nextra)
      { /* 'n' is negative */
        return new LocalSlot(Lt.vararg_i, ci.func - nextra - (n + 1)); /* generic name for any vararg */
      }
    }
    return new LocalSlot(null, 0); /* no such vararg */
  }

  public static LocalSlot luaG_findlocal(lua_State L, CallInfo ci, int n)
  {
    int base = ci.func + 1;
    InternalString name = null;
    if (isLua(ci) != 0)
    {
      if (n < 0) /* access to vararg values? */
        return findvararg(L, ci, n);
      else
        name = ci_func(L, ci).p.luaF_getlocalname(n, currentpc(L, ci));
    }
    if (name == null)
    { /* no 'standard' name? */
      int limit = (ci == L.ci) ? L.top : ci.next.func;
      if (limit - base >= n && n > 0)
      { /* is 'n' inside 'ci' stack? */
        /* generic name for any valid slot */
        name = (isLua(ci) != 0) ? Lt.temporary_i : Lt.C_temporary_i;
      }
      else
        return new LocalSlot(null, 0); /* no name */
    }
    return new LocalSlot(name, base + (n - 1));
  }

  public static InternalString lua_getlocali(lua_State L, InternalDebug ar, int n)
  {
    InternalString name;
    lua_lock(L);
    if (ar == null)
    {                                      /* information about non-active function? */
      if (slottag(L, L.top - 1) != ctb(LUA_VLCL)) /* not a Lua function? */
        name = null;
      else /* consider live variables at function start (parameters) */
      {
        int fv = L.top - 1;
        name = slotclLval(L, fv).p.luaF_getlocalname(n, 0);
      }
    }
    else
    {                   /* active function; get information through 'ar' */
      LocalSlot local = luaG_findlocal(L, ar.i_ci, n);
      name = local.name;
      if (name != null)
      {
        copyslot(L, L.top, local.pos);
        api_incr_top(L);
      }
    }
    lua_unlock(L);
    return name;
  }

  public static HostString lua_getlocal(lua_State L, lua_Debug ar, int n)
  {
    InternalDebug iar = new InternalDebug();
    if (ar != null)
      iar.i_ci = ar.i_ci;
    InternalString name = lua_getlocali(L, (ar != null) ? iar : null, n);
    if (name == null)
      return null;
    return new HostString(name.view());
  }

  public static InternalString lua_setlocali(lua_State L, InternalDebug ar, int n)
  {
    InternalString name;
    lua_lock(L);
    LocalSlot local = luaG_findlocal(L, ar.i_ci, n);
    name = local.name;
    if (name != null)
    {
      api_checkpop(L, 1);
      copyslot(L, local.pos, L.top - 1);
      L.top--; /* pop value */
    }
    lua_unlock(L);
    return name;
  }

  public static HostString lua_setlocal(lua_State L, lua_Debug ar, int n)
  {
    InternalDebug iar = new InternalDebug();
    iar.i_ci = ar.i_ci;
    InternalString name = lua_setlocali(L, iar, n);
    if (name == null)
      return null;
    return new HostString(name.view());
  }

  private static void funcinfo(InternalDebug ar, Closure cl)
  {
    if (LuaClosure(cl) == 0)
    {
      ar.source = Lt.C;
      ar.linedefined = -1;
      ar.lastlinedefined = -1;
      ar.what = Lt.C_char;
    }
    else
    {
      Proto p = ((LClosure) cl).p;
      if (p.source != null)
        ar.source = p.source.data.view();
      else
        ar.source = Lt.literal_1;
      ar.linedefined = p.linedefined;
      ar.lastlinedefined = p.lastlinedefined;
      ar.what = (ar.linedefined == 0) ? Lt.main : Lt.Lua;
    }
    luaO_chunkid(ar.short_src_out.span(), ar.source);
  }

  private static int nextline(Proto p, int currentline, int pc)
  {
    if (p.lineinfo[pc] != ABSLINEINFO)
      return currentline + p.lineinfo[pc];
    else
      return luaG_getfuncline(p, pc);
  }

  private static void collectvalidlines(lua_State L, Closure f)
  {
    if (LuaClosure(f) == 0)
    {
      setslottag(L, L.top, LUA_VNIL);
      api_incr_top(L);
    }
    else
    {
      Proto p = ((LClosure) f).p;
      int currentline = p.linedefined;
      Table t = Table.luaH_new(L);      /* new table to store active lines */
      setsloth(L, L.top, t); /* push it on stack */
      api_incr_top(L);
      if (p.lineinfo.length != 0)
      { /* proto with debug information? */
        int i;
        TValue v = new TValue();
        v.tt_ = LUA_VTRUE;     /* boolean 'true' to be the value of all indices */
        if (isvararg(p) == 0) /* regular function? */
          i = 0;            /* consider all instructions */
        else
        { /* vararg function */
          assert GET_OPCODE(p.code[0]) == OP_VARARGPREP;
          currentline = nextline(p, currentline, 0);
          i = 1; /* skip first instruction (OP_VARARGPREP) */
        }
        for (; i < (int) p.lineinfo.length; i++)
        {                                            /* for each instruction */
          currentline = nextline(p, currentline, i); /* get its line */
          t.luaH_setint(L, currentline, v);        /* table[line] = true */
        }
      }
    }
  }

  private static VarName getfuncname(lua_State L, CallInfo ci)
  {
    /* calling function is a known function? */
    if (ci != null && (ci.callstatus & CIST_TAIL) == 0)
      return funcnamefromcall(L, ci.previous);
    else
      return new VarName(null, null); /* no way to find a name */
  }

  private static int auxgetinfo(lua_State L, ByteView what, int wpos,
                                InternalDebug ar, Closure f, CallInfo ci)
  {
    int status = 1;
    for (; wpos < what.size(); wpos++)
    {
      switch (what.at(wpos))
      {
      case 'S':
      {
        funcinfo(ar, f);
        break;
      }
      case 'l':
      {
        ar.currentline = (ci != null && isLua(ci) != 0) ? getcurrentline(L, ci) : -1;
        break;
      }
      case 'u':
      {
        ar.nups = (int) ((f == null) ? 0 : ((LuaClosure(f) != 0) ? ((LClosure) f).upvals.length : ((CClosure) f).upvalue.length));
        if (LuaClosure(f) == 0)
        {
          ar.isvararg = 1;
          ar.nparams = 0;
        }
        else
        {
          ar.isvararg = (byte) ((isvararg(((LClosure) f).p) != 0) ? 1 : 0);
          ar.nparams = ((LClosure) f).p.numparams;
        }
        break;
      }
      case 't':
      {
        if (ci != null)
        {
          ar.istailcall = (byte) (((ci.callstatus & CIST_TAIL) != 0) ? 1 : 0);
          ar.extraargs = (ci.callstatus & MAX_CCMT) >>> CIST_CCMT;
        }
        else
        {
          ar.istailcall = 0;
          ar.extraargs = 0;
        }
        break;
      }
      case 'n':
      {
        VarName fn = getfuncname(L, ci);
        ar.namewhat = fn.kind;
        ar.name = (fn.kind != null && fn.name != null) ? fn.name.view() : null;
        break;
      }
      case 'r':
      {
        if (ci == null || (ci.callstatus & CIST_HOOKED) == 0)
          ar.ftransfer = ar.ntransfer = 0;
        else
        {
          ar.ftransfer = L.transferinfo.ftransfer;
          ar.ntransfer = L.transferinfo.ntransfer;
        }
        break;
      }
      case 'L':
      case 'f': /* handled by lua_getinfo */
        break;
      default:
        status = 0; /* invalid option */
      }
    }
    return status;
  }

  public static InternalDebug lua_getinfoi(lua_State L, ByteView what, InternalDebug ar)
  {
    int status;
    Closure cl;
    CallInfo ci;
    int func;
    int wpos = 0;
    lua_lock(L);
    if (sv_byteat(what, 0) == '>')
    {
      ci = null;
      func = L.top - 1;
      assert novariant(slottag(L, func)) == LUA_TFUNCTION : "function expected";
      wpos = 1;   /* skip the '>' */
      L.top--;
    }
    else
    {
      ci = ar.i_ci;
      func = ci.func;
      assert novariant(slottag(L, func)) == LUA_TFUNCTION;
    }
    if (slottag(L, func) == ctb(LUA_VLCL) || slottag(L, func) == ctb(LUA_VCCL))
    {
      cl = slotclval(L, func);
      assert novariant(cl.tt) == LUA_TFUNCTION;
    }
    else
      cl = null;
    InternalDebug moredetail = ar.copy();
    status = auxgetinfo(L, what, wpos, moredetail, cl, ci);
    if (what.find('f', wpos) != ByteView.npos)
    {
      copyslot(L, L.top, func);
      api_incr_top(L);
    }
    if (what.find('L', wpos) != ByteView.npos)
      collectvalidlines(L, cl);
    lua_unlock(L);
    if (status == 0)
      return null; /* invalid option */
    return moredetail;
  }

  public static lua_Debug lua_getinfo(lua_State L, HostString options, lua_Debug arin)
  {
    ByteView what = options.to_utf8();
    InternalDebug iar = new InternalDebug();
    iar.event = arin.event;
    iar.i_ci = arin.i_ci;
    InternalDebug res = lua_getinfoi(L, what, iar);
    if (res == null)
      return null; /* invalid option */
    lua_Debug ar = arin.copy();
    ar.i_ci = res.i_ci;
    if (what.find('S') != ByteView.npos)
    {
      ar.source = res.source;
      ar.short_src = HostString.from_bytes(res.short_src_out.str_view());
      ar.what = new HostString(res.what);
      ar.linedefined = res.linedefined;
      ar.lastlinedefined = res.lastlinedefined;
    }
    if (what.find('l') != ByteView.npos)
      ar.currentline = res.currentline;
    if (what.find('u') != ByteView.npos)
    {
      ar.nups = res.nups;
      ar.nparams = res.nparams;
      ar.isvararg = res.isvararg;
    }
    if (what.find('t') != ByteView.npos)
    {
      ar.istailcall = res.istailcall;
      ar.extraargs = res.extraargs;
    }
    if (what.find('n') != ByteView.npos)
    {
      ar.namewhat = (res.namewhat != null) ? HostString.from_bytes(res.namewhat) : new HostString();
      ar.name = (res.name != null) ? HostString.from_bytes(res.name) : new HostString();
    }
    if (what.find('r') != ByteView.npos)
    {
      ar.ftransfer = res.ftransfer;
      ar.ntransfer = res.ntransfer;
    }
    return ar;
  }

  /*
  ** {======================================================
  ** Symbolic Execution
  ** =======================================================
  */

  private static int filterpc(int pc, int jmptarget)
  {
    if (pc < jmptarget) /* is code conditional (inside a jump)? */
      return -1;        /* cannot know who sets that register */
    else
      return pc; /* current position sets that register */
  }

  /*
  ** Try to find last instruction before 'lastpc' that modified register 'reg'.
  */
  private static int findsetreg(Proto p, int lastpc, int reg)
  {
    int pc;
    int setreg = -1;   /* keep last instruction that changed 'reg' */
    int jmptarget = 0; /* any code before this address is conditional */
    if (testMMMode(GET_OPCODE(p.code[lastpc])) != 0)
      lastpc--; /* previous instruction was not actually executed */
    for (pc = 0; pc < lastpc; pc++)
    {
      int i = p.code[pc];
      int op = GET_OPCODE(i);
      int a = GETARG_A(i);
      int change; /* true if current instruction changed 'reg' */
      switch (op)
      {
      case OP_LOADNIL:
      { /* set registers from 'a' to 'a+b' */
        int b = GETARG_B(i);
        change = (a <= reg && reg <= a + b) ? 1 : 0;
        break;
      }
      case OP_TFORCALL:
      { /* affect all regs above its base */
        change = (reg >= a + 2) ? 1 : 0;
        break;
      }
      case OP_CALL:
      case OP_TAILCALL:
      { /* affect all registers above base */
        change = (reg >= a) ? 1 : 0;
        break;
      }
      case OP_JMP:
      { /* doesn't change registers, but changes 'jmptarget' */
        int b = GETARG_sJ(i);
        int dest = pc + 1 + b;
        /* jump does not skip 'lastpc' and is larger than current one? */
        if (dest <= lastpc && dest > jmptarget)
          jmptarget = dest; /* update 'jmptarget' */
        change = 0;
        break;
      }
      default: /* any instruction that sets A */
        change = (testAMode(op) != 0 && reg == a) ? 1 : 0;
        break;
      }
      if (change != 0)
        setreg = filterpc(pc, jmptarget);
    }
    return setreg;
  }

  /*
  ** Find a "name" for the constant 'c'.
  */
  private static VarName kname(Proto p, int index)
  {
    TValue kvalue = p.k[index];
    if (ttisstring(kvalue))
      return new VarName(Lt.constant, tsvalue(kvalue).data);
    else
      return new VarName(null, Lt.qmark_i);
  }

  private static final class RegName
  {
    VarName var;
    int pc;

    RegName(VarName var, int pc)
    {
      this.var = var;
      this.pc = pc;
    }
  }

  private static RegName basicgetobjname(Proto p, int pc, int reg)
  {
    InternalString name = p.luaF_getlocalname(reg + 1, pc);
    if (name != null) /* is a local? */
      return new RegName(new VarName(Lt.local, name), pc);
    /* else try symbolic execution */
    pc = findsetreg(p, pc, reg);
    if (pc != -1)
    { /* could find instruction? */
      int i = p.code[pc];
      int op = GET_OPCODE(i);
      switch (op)
      {
      case OP_MOVE:
      {
        int b = GETARG_B(i); /* move from 'b' to 'a' */
        if (b < GETARG_A(i))
          return basicgetobjname(p, pc, b); /* get name for 'b' */
        break;
      }
      case OP_GETUPVAL:
      {
        return new RegName(new VarName(Lt.upvalue, upvalname(p, GETARG_B(i))), pc);
      }
      case OP_LOADK:
        return new RegName(kname(p, GETARG_Bx(i)), pc);
      case OP_LOADKX:
        return new RegName(kname(p, GETARG_Ax(p.code[pc + 1])), pc);
      default:
        break;
      }
    }
    return new RegName(new VarName(null, name), pc); /* could not find reasonable name */
  }

  /*
  ** Find a "name" for the register 'c'.
  */
  private static InternalString rname(Proto p, int pc, int c)
  {
    VarName what = basicgetobjname(p, pc, c).var; /* search for 'c' */
    if (what.kind == null || !what.kind.equals(Lt.constant)) /* did not find a constant name? */
      return Lt.qmark_i;
    return what.name;
  }

  /*
  ** Check whether table being indexed by instruction 'i' is the
  ** environment '_ENV'
  */
  private static ByteView isEnv(Proto p, int pc, int i, int isup)
  {
    int t = GETARG_B(i);       /* table index */
    InternalString name = null; /* name of indexed variable */
    if (isup != 0)                  /* is 't' an upvalue? */
      name = upvalname(p, t);
    else
    { /* 't' is a register */
      VarName what = basicgetobjname(p, pc, t).var;
      name = what.name;
      /* 'name' must be the name of a local variable (at the current
         level or an upvalue) */
      if (what.kind == null || (!what.kind.equals(Lt.local) && !what.kind.equals(Lt.upvalue)))
        name = null; /* cannot be the variable _ENV */
    }
    return (name != null && name.view().equals(Lt._ENV)) ? Lt.global : Lt.field;
  }

  /*
  ** Extend 'basicgetobjname' to handle table accesses
  */
  private static VarName getobjname(Proto p, int lastpc, int reg)
  {
    RegName basic = basicgetobjname(p, lastpc, reg);
    lastpc = basic.pc;
    if (basic.var.kind != null)
      return basic.var;
    else if (lastpc != -1)
    { /* could find instruction? */
      int i = p.code[lastpc];
      int op = GET_OPCODE(i);
      switch (op)
      {
      case OP_GETTABUP:
      {
        int k = GETARG_C(i); /* key index */
        return new VarName(isEnv(p, lastpc, i, 1), kname(p, k).name);
      }
      case OP_GETTABLE:
      case OP_GETVARG:
      {
        int k = GETARG_C(i); /* key index */
        return new VarName(isEnv(p, lastpc, i, 0), rname(p, lastpc, k));
      }
      case OP_GETI:
      {
        return new VarName(Lt.field, Lt.integer_index_i);
      }
      case OP_GETFIELD:
      {
        int k = GETARG_C(i); /* key index */
        return new VarName(isEnv(p, lastpc, i, 0), kname(p, k).name);
      }
      case OP_SELF:
      {
        int k = GETARG_C(i); /* key index */
        return new VarName(Lt.method, kname(p, k).name);
      }
      default:
        break;
      }
    }
    return new VarName(null, basic.var.name); /* could not find reasonable name */
  }

  /*
  ** Try to find a name for a function based on the code that called it.
  ** (Only works when function was called by a Lua function.)
  ** Returns what the name is (e.g., "for iterator", "method",
  ** "metamethod") and sets '*name' to point to the name.
  */
  private static VarName funcnamefromcode(Proto p, int pc)
  {
    int tm = 0;            /* (initial value avoids warnings) */
    int i = p.code[pc]; /* calling instruction */
    switch (GET_OPCODE(i))
    {
    case OP_CALL:
    case OP_TAILCALL:
      return getobjname(p, pc, GETARG_A(i)); /* get function name */
    case OP_TFORCALL:
    { /* for iterator */
      return new VarName(Lt.for_iterator_i.view(), Lt.for_iterator_i);
    }
    /* other instructions can do calls through metamethods */
    case OP_SELF:
    case OP_GETTABUP:
    case OP_GETTABLE:
    case OP_GETI:
    case OP_GETFIELD:
      tm = TM_INDEX;
      break;
    case OP_SETTABUP:
    case OP_SETTABLE:
    case OP_SETI:
    case OP_SETFIELD:
      tm = TM_NEWINDEX;
      break;
    case OP_MMBIN:
    case OP_MMBINI:
    case OP_MMBINK:
    {
      tm = GETARG_C(i);
      break;
    }
    case OP_UNM:
      tm = TM_UNM;
      break;
    case OP_BNOT:
      tm = TM_BNOT;
      break;
    case OP_LEN:
      tm = TM_LEN;
      break;
    case OP_CONCAT:
      tm = TM_CONCAT;
      break;
    case OP_EQ:
      tm = TM_EQ;
      break;
    /* no cases for OP_EQI and OP_EQK, as they don't call metamethods */
    case OP_LT:
    case OP_LTI:
    case OP_GTI:
      tm = TM_LT;
      break;
    case OP_LE:
    case OP_LEI:
    case OP_GEI:
      tm = TM_LE;
      break;
    case OP_CLOSE:
    case OP_RETURN:
      tm = TM_CLOSE;
      break;
    default:
      return new VarName(null, null); /* cannot find a reasonable name */
    }
    return new VarName(Lt.metamethod, tmplainname[tm]);
  }

  /*
  ** Try to find a name for a function based on how it was called.
  */
  private static VarName funcnamefromcall(lua_State L, CallInfo ci)
  {
    if ((ci.callstatus & CIST_HOOKED) != 0)
    { /* was it called inside a hook? */
      return new VarName(Lt.hook, Lt.qmark_i);
    }
    else if ((ci.callstatus & CIST_FIN) != 0)
    { /* was it called as a finalizer? */
      return new VarName(Lt.metamethod, Lt.mm_gc_i); /* report it as such */
    }
    else if (isLua(ci) != 0)
      return funcnamefromcode(ci_func(L, ci).p, currentpc(L, ci));
    else
      return new VarName(null, null);
  }

  /* }====================================================== */

  private static ByteView formatvarinfo(lua_State L, ByteView kind, InternalString name)
  {
    if (kind == null)
      return Lt.empty_str; /* no information */
    else
      return luaO_pushfstring(L, Lt.fmt_paren_s_quoted_s, new FmtArgs().str(kind).str(name.view())).view();
  }

  private static ByteView varinfo(lua_State L, int src)
  {
    CallInfo ci = L.ci;
    InternalString name = null; /* to avoid warnings */
    ByteView kind = null;
    if (isLua(ci) != 0)
    {
      if (src <= UPVALORIGIN)
      {
        name = upvalname(ci_func(L, ci).p, (int) (UPVALORIGIN - src));
        kind = Lt.upvalue;
      }
      else if (src != NOSLOT)
      {
        int base = ci.func + 1;
        if (src >= base && src < ci.top)
        {

          VarName obj = getobjname(ci_func(L, ci).p, currentpc(L, ci), (int) (src - base));
          kind = obj.kind;
          name = obj.name;
        }
      }
    }
    return formatvarinfo(L, kind, name);
  }

  /*
  ** Raise a type error
  */
  private static RuntimeException typeerror(lua_State L, TValue o, ByteView op, ByteView extra)
  {
    ByteView t = luaT_objtypename(L, o);
    throw luaG_runerror(L, Lt.attempt_to_s_a_s_value_s, new FmtArgs().str(op).str(t).str(extra));
  }

  /*
  ** Raise a type error with "standard" information about the faulty
  ** object 'o' (using 'varinfo').
  */
  public static RuntimeException luaG_typeerror(lua_State L, TValue o, ByteView op, int src)
  {
    throw typeerror(L, o, op, varinfo(L, src));
  }

  /*
  ** Raise an error for calling a non-callable object. Try to find a name
  ** for the object based on how it was called ('funcnamefromcall'); if it
  ** cannot get a name there, try 'varinfo'.
  */
  public static RuntimeException luaG_callerror(lua_State L, TValue o, int src)
  {
    CallInfo ci = L.ci;
    VarName fn = funcnamefromcall(L, ci);
    ByteView extra = (fn.kind != null) ? formatvarinfo(L, fn.kind, fn.name) : varinfo(L, src);
    throw typeerror(L, o, Lt.call, extra);
  }

  public static RuntimeException luaG_forerror(lua_State L, TValue o, ByteView what)
  {
    throw luaG_runerror(L, Lt.bad_for_s_number_expected_got_s, new FmtArgs().str(what).str(luaT_objtypename(L, o)));
  }

  public static RuntimeException luaG_concaterror(lua_State L, TValue p1, TValue p2, int src1, int src2)
  {
    if (ttisstring(p1) || cvt2str(p1) != 0)
    {
      p1 = p2;
      src1 = src2;
    }
    throw luaG_typeerror(L, p1, Lt.concatenate, src1);
  }

  public static RuntimeException luaG_opinterror(lua_State L, TValue p1, TValue p2, ByteView msg, int src1, int src2)
  {
    if (!ttisnumber(p1)) /* first operand is wrong? */
    {
      p2 = p1;           /* now second is wrong */
      src2 = src1;
    }
    throw luaG_typeerror(L, p2, msg, src2);
  }

  /*
  ** Error when both values are convertible to numbers, but not to integers
  */
  public static RuntimeException luaG_tointerror(lua_State L, TValue p1, TValue p2, int src1, int src2)
  {
    IntResult temp = new IntResult();
    if (luaV_tointegerns(p1, temp, LUA_FLOORN2I) == 0)
      src2 = src1;
    throw luaG_runerror(L, Lt.number_s_has_no_integer_representation, new FmtArgs().str(varinfo(L, src2)));
  }

  public static RuntimeException luaG_ordererror(lua_State L, TValue p1, TValue p2)
  {
    ByteView t1 = luaT_objtypename(L, p1);
    ByteView t2 = luaT_objtypename(L, p2);
    if (t1.equals(t2))
      throw luaG_runerror(L, Lt.attempt_to_compare_two_s_values, new FmtArgs().str(t1));
    else
      throw luaG_runerror(L, Lt.attempt_to_compare_s_with_s, new FmtArgs().str(t1).str(t2));
  }

  public static RuntimeException luaG_errnnil(lua_State L, LClosure cl, int k)
  {
    InternalString globalname = Lt.qmark_i; /* default name if k == 0 */
    if (k > 0)
      globalname = kname(cl.p, k - 1).name;
    throw luaG_runerror(L, Lt.global_s_already_defined, new FmtArgs().str(globalname.view()));
  }

  /* add src:line information to 'msg' */
  public static ByteView luaG_addinfo(lua_State L, ByteView msg, TString src, int line)
  {
    if (src == null) /* no debug information? */
      return luaO_pushfstring(L, Lt.fmt_unknown_loc_s, new FmtArgs().str(msg)).view();

    ByteOwner bytes = new ByteOwner(LUA_IDSIZE);
    luaO_chunkid(bytes.span(), src.data.view());
    ByteView sv = bytes.str_view();
    return luaO_pushfstring(L, Lt.fmt_s_d_s, new FmtArgs().str(sv).num(line).str(msg)).view();
  }

  public static RuntimeException luaG_errormsg(lua_State L)
  {
    if (L.errfunc != 0)
    { /* is there an error handling function? */
      int errfunc = (int) (L.errfunc);
      assert novariant(slottag(L, errfunc)) == LUA_TFUNCTION;
      copyslot(L, L.top, L.top - 1); /* move argument */
      copyslot(L, L.top - 1, errfunc);  /* push function */
      L.top++;                           /* assume EXTRA_STACK */
      luaD_callerrh(L);                   /* call it */
    }
    throw luaG_throwerror(L);
  }

  /* raise the error object at the top */
  public static RuntimeException luaG_throwerror(lua_State L)
  {
    if (tagisempty(slottag(L, L.top - 1)))
    { /* error object is nil? */
      /* change it to a proper message */
      ByteView errstr = Lt.no_error_object;
      setslots(L, L.top - 1, TString.luaS_createstr(L, errstr));
    }
    throw luaD_throw(L, (byte) LUA_ERRRUN);
  }

  public static RuntimeException luaG_runerror(lua_State L, ByteView fmt,
                                               FmtArgs args)
  {
    CallInfo ci = L.ci;
    InternalString msg;
    luaC_checkGC(L); /* error message uses memory */
    msg = luaO_pushvfstring(L, fmt, args);
    if (msg == null)
      luaD_throw(L, (byte) LUA_ERRMEM);
    if (isLua(ci) != 0)
    { /* Lua function? */
      /* add source:line information */
      luaG_addinfo(L, msg.view(), ci_func(L, ci).p.source, getcurrentline(L, ci));
      copyslot(L, L.top - 2, L.top - 1); /* remove 'msg' */
      L.top--;
    }
    throw luaG_errormsg(L);
  }

  public static RuntimeException luaG_runerror(lua_State L, ByteView fmt)
  {
    throw luaG_runerror(L, fmt, new FmtArgs());
  }

  /*
  ** Check whether new instruction 'newpc' is in a different line from
  ** previous instruction 'oldpc'. More often than not, 'newpc' is only
  ** one or a few instructions after 'oldpc' (it must be after, see
  ** caller), so try to avoid calling 'luaG_getfuncline'. If they are
  ** too far apart, there is a good chance of a ABSLINEINFO in the way,
  ** so it goes directly to 'luaG_getfuncline'.
  */
  private static int changedline(Proto p, int oldpc, int newpc)
  {
    if (p.lineinfo.length == 0) /* no debug information? */
      return 0;
    if (newpc - oldpc < MAXIWTHABS / 2)
    {                /* not too far apart? */
      int delta = 0; /* line difference */
      int pc = oldpc;
      for (;;)
      {
        int lineinfo = p.lineinfo[++pc];
        if (lineinfo == ABSLINEINFO)
          break; /* cannot compute delta; fall through */
        delta += lineinfo;
        if (pc == newpc)
          return (delta != 0) ? 1 : 0; /* delta computed successfully */
      }
    }
    /* either instructions are too far apart or there is an absolute line
       info in the way; compute line difference explicitly */
    return (luaG_getfuncline(p, oldpc) != luaG_getfuncline(p, newpc)) ? 1 : 0;
  }

  /*
  ** Traces Lua calls. If code is running the first instruction of a function,
  ** and function is not vararg, and it is not coming from an yield,
  ** calls 'luaD_hookcall'. (Vararg functions will call 'luaD_hookcall'
  ** after adjusting its variable arguments; otherwise, they could call
  ** a line/count hook before the call hook. Functions coming from
  ** an yield already called 'luaD_hookcall' before yielding.)
  */
  public static int luaG_tracecall(lua_State L)
  {
    CallInfo ci = L.ci;
    Proto p = ci_func(L, ci).p;
    ci.trap = 1; /* ensure hooks will be checked */
    if (ci.savedpc == 0)
    { /* first instruction (not resuming)? */
      if (isvararg(p) != 0)
      {
        luaE_spend(L, 1); /* (VARARGPREP runs untraced) */
        return 0;         /* hooks will start at VARARGPREP instruction */
      }
      else if ((ci.callstatus & (CIST_HOOKYIELD | CIST_FROZE)) == 0) /* not yielded or frozen? */
        luaD_hookcall(L, ci);                      /* check 'call' hook */
    }
    return 1; /* keep 'trap' on */
  }

  /*
  ** The budget is spent before the instruction at 'savedpc - 1': call the
  ** budget callback, and freeze there if it asks to (the instruction runs
  ** when it thaws).
  */
  private static void budgetpoint(lua_State L, CallInfo ci, Proto p)
  {
    if (luaP_isIT(p.code[ci.savedpc - 1]) == 0) /* top not being used? */
      L.top = ci.top;                 /* correct top */
    lua_State co = luaD_budget(L);
    if (co != null)
    {
      ci.callstatus |= CIST_FROZE;
      luaD_freeze(L, co);
    }
  }

  /*
  ** At this instruction boundary of a budgeted thread, call the '__gc' that
  ** wait for its group (a restartable point: a freeze inside them goes on
  ** here). The thread's hooks are off while they run.
  */
  private static void drainpoint(lua_State L, CallInfo ci, Proto p)
  {
    if (luaP_isIT(p.code[ci.savedpc - 1]) == 0) /* top not being used? */
      L.top = ci.top;                 /* correct top */
    int top = L.top;
    int ci_top = ci.top;
    assert ci_top == ci.func + 1 + p.maxstacksize; /* (what 'unroll' gives back if a '__gc' freezes) */
    /* (the call of the '__gc' needs room, as a finalizer in stock: the frames
       of the function that calls them and of a '__gc', the stack, C calls; if
       it cannot be had here without an error, they wait for another point;
       the program sees no error) */
    if (luaD_checkminstack(L) == 0)
      return;
    if (ci.top < L.top + LUA_MINSTACK + 1)
      ci.top = L.top + LUA_MINSTACK + 1;
    ci.callstatus |= CIST_FROZE; /* (if it freezes, it goes on at this instruction) */
    luaC_pushdrain(L);
    luaD_call(L, L.top - 1, 0);
    ci.callstatus &= ~CIST_FROZE;
    ci.top = ci_top;
    L.top = top;
  }

  public static int luaG_traceexec(lua_State L, int pc)
  {
    CallInfo ci = L.ci;
    byte mask = (byte) L.hookmask;
    Proto p = ci_func(L, ci).p;
    int counthook;
    if ((mask & (LUA_MASKLINE | LUA_MASKCOUNT | LUA_MASKBUDGET)) == 0)
    {                   /* no hooks? */
      ci.trap = 0; /* don't need to stop again */
      return 0;         /* turn off 'trap' */
    }
    pc++;                 /* reference is always next instruction */
    ci.savedpc = pc; /* save 'pc' */
    if ((mask & LUA_MASKBUDGET) != 0 && (ci.callstatus & CIST_HOOKYIELD) == 0)
    { /* a step of the budget (after its callback, if it is spent) */
      global_State g = L.l_G;
      BudgetGroup bg = L.beff;
      /* (the first point after a thaw calls no '__gc': if no step was taken
         since, the VM passed no other point; else that one was the first) */
      boolean first = bg.nodrain && bg.left == bg.budget;
      bg.nodrain = false;
      if (!first && bg.fnz.next != null && L.allowhook != 0 && g.budgetL == null && luaD_nofreeze(L) == 0)
        drainpoint(L, ci, p);
      if (L.beff != null && L.beff.left <= 0 && g.budgetL == null)
        budgetpoint(L, ci, p);
      if (L.beff != null) /* (the callback may have removed it) */
        L.beff.left--;
    }
    if ((mask & (LUA_MASKLINE | LUA_MASKCOUNT)) == 0)
      return 1; /* only the budget */
    /* A hook function that freezes is finished after a thaw ('unroll'), and
       this instruction is traced again: 'hookdone' says what of it has been
       done already (the count, which also stays as it is, and the line
       hook), and the hooks left run now. */
    boolean refrozen = (ci.hookdone != 0); /* (not a hook that yielded: that is CIST_HOOKYIELD alone) */
    if (refrozen)
      ci.callstatus &= ~CIST_HOOKYIELD; /* (it kept the budget from counting the instruction again) */
    counthook = 0;
    if ((mask & LUA_MASKCOUNT) != 0 && (ci.hookdone & HD_COUNT) == 0)
    {
      counthook = (--L.hookcount == 0) ? 1 : 0;
      ci.hookdone |= HD_COUNT;
    }
    if (counthook != 0)
      resethookcount(L); /* reset count */
    else if ((mask & LUA_MASKLINE) == 0)
    { /* no line hook and count != 0; nothing to be done now */
      ci.hookdone = 0;
      return 1;
    }
    if ((ci.callstatus & CIST_HOOKYIELD) != 0 && !refrozen)
    {                                    /* hook yielded last time? */
      ci.callstatus &= ~CIST_HOOKYIELD; /* erase mark */
      ci.hookdone = 0;
      return 1;                          /* do not call hook again (VM yielded, so it did not move) */
    }
    ci.callstatus &= ~CIST_HOOKYIELD;
    if (luaP_isIT(p.code[ci.savedpc - 1]) == 0) /* top not being used? */
      L.top = ci.top;                 /* correct top */
    ci.hookev = HE_INSTR;
    if (counthook != 0)
    {
      ci.callstatus |= CIST_FROZE | CIST_HOOKYIELD; /* (if the hook function freezes) */
      luaD_hook(L, LUA_HOOKCOUNT, -1, 0, 0); /* call count hook */
      ci.callstatus &= ~(CIST_FROZE | CIST_HOOKYIELD);
    }
    if ((mask & LUA_MASKLINE) != 0)
    {
      /* 'L->oldpc' may be invalid; use zero in this case */
      int oldpc = (L.oldpc < (int) p.code.length) ? L.oldpc : 0;
      int npci = pc - 1;
      if ((ci.hookdone & HD_LINE) == 0 &&
          (npci <= oldpc || /* call hook when jump back (loop), */
           changedline(p, oldpc, npci) != 0))
      { /* or when enter new line */
        int newline = luaG_getfuncline(p, npci);
        ci.hookdone |= HD_LINE;
        ci.callstatus |= CIST_FROZE | CIST_HOOKYIELD;
        luaD_hook(L, LUA_HOOKLINE, newline, 0, 0); /* call line hook */
        ci.callstatus &= ~(CIST_FROZE | CIST_HOOKYIELD);
      }
      L.oldpc = npci; /* 'pc' of last call to line hook */
    }
    ci.hookdone = 0;
    if (L.status == LUA_YIELD)
    { /* did hook yield? */
      if (counthook != 0)
        L.hookcount = 1;               /* undo decrement to zero */
      ci.callstatus |= CIST_HOOKYIELD; /* mark that it yielded */
      luaD_throw(L, (byte) LUA_YIELD);
    }
    return 1; /* keep 'trap' on */
  }
}
