package lua.runtime;

import static lua.api.LuaApi.*;
import static lua.bytes.Lload.*;
import static lua.bytes.Lzio.*;
import static lua.compiler.Lparser.*;
import static lua.compiler.Lundump.*;
import static lua.gc.Lgc.*;
import static lua.object.Lfunc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Ldebug.*;
import static lua.vm.Lvm.*;

import lua.api.lua_Budget;
import lua.api.lua_CFunction;
import lua.api.lua_Debug;
import lua.api.lua_Hook;
import lua.api.lua_KFunction;
import lua.api.lua_ResumeResult;
import lua.bytes.ByteView;
import lua.bytes.LoadFrame;
import lua.bytes.LoadResume;
import lua.bytes.Lt;
import lua.bytes.MBuffer;
import lua.bytes.ZIO;
import lua.compiler.Dyndata;
import lua.compiler.Labeldesc;
import lua.compiler.Vardesc;
import lua.object.BudgetGroup;
import lua.object.CClosure;
import lua.object.FmtArgs;
import lua.object.GCObject;
import lua.object.LClosure;
import lua.object.Proto;
import lua.object.TString;
import lua.object.TValue;
import lua.object.Table;

public final class Ldo
{
  private Ldo()
  {
  }

  public static void luaD_checkstack(lua_State L, int n)
  {
    if (L.stack_last - L.top <= n)
      luaD_growstack(L, n, 1);
  }

  public static void checkstackp(lua_State L, int n, int p)
  {
    luaD_checkstack(L, n);
  }

  /* some stack space for error handling */
  public static final int STACKERRSPACE = 200;

  /* maximum stack size that respects size_t */
  public static final long MAXSTACK_BYSIZET = Long.divideUnsigned(MAX_SIZET, Cost.TValue) - STACKERRSPACE;

  /*
  ** Minimum between LUAI_MAXSTACK and MAXSTACK_BYSIZET
  ** (Maximum size for the stack must respect size_t.)
  */
  public static final int MAXSTACK = (Long.compareUnsigned(LUAI_MAXSTACK, MAXSTACK_BYSIZET) < 0) ? LUAI_MAXSTACK : (int) MAXSTACK_BYSIZET;

  /* stack size with extra space for error handling (a thread handling a
     stack overflow has a stack of this size) */
  public static final int ERRORSTACKSIZE = MAXSTACK + STACKERRSPACE;

  /* status of a freeze unwinding the C stack: not an error, and never a
     result of the API */
  public static final byte LUA_FREEZE = 100;

  /* status of the exit of the budget callback (freeze): it unwinds the C
     stack to the callback's caller, and only that catches it */
  public static final byte LUA_CBEXIT = 101;

  private static int errorstatus(byte s)
  {
    return (s > LUA_YIELD && s != LUA_FREEZE && s != LUA_CBEXIT) ? 1 : 0;
  }

  /*
  ** {======================================================
  ** Error-recovery functions
  ** =======================================================
  */

  private static void LUAI_TRY(lua_State L, lua_longjmp c, ProtectedCall f)
  {
    try
    {
      f.run(L); /* call function protected */
    }
    catch (LuaError e)
    {              /* Lua error */
      if (e.status == LUA_CBEXIT)
      { /* only the call of the callback catches its exit */
        if (!c.exitpoint)
          throw e;
      }
      else if (e.level != c.level) /* not the correct level? */
        throw e;     /* rethrow to upper level */
      c.status = e.status;
    }
    catch (AssertionError e)
    {
      throw e;
    }
    catch (OutOfMemoryError e)
    {
      c.status = LUA_ERRMEM;
    }
    catch (Throwable e)
    {                 /* non-Lua exception */
      c.status = -1; /* create some error code */
    }
  }

  public static void luaD_seterrorobj(lua_State L, byte errcode, int oldtop)
  {
    if (errcode == LUA_ERRMEM)
    {                                          /* memory error? */
      setslots(L, oldtop, L.l_G.memerrmsg); /* reuse preregistered msg. */
    }
    else
    {
      assert errorstatus(errcode) != 0;        /* must be a real error */
      assert !tagisempty(slottag(L, L.top - 1)); /* with a non-nil object */
      copyslot(L, oldtop, L.top - 1);      /* move it to 'oldtop' */
    }
    L.top = oldtop + 1; /* top goes back to old top plus error object */
  }

  public static RuntimeException luaD_throw(lua_State L, byte errcode)
  {
    if (L.errorJmp != null)
    {                       /* thread has an error handler? */
      throw new LuaError(errcode, L.errorJmp.level); /* jump to it */
    }
    else
    { /* thread has no error handler */
      global_State g = L.l_G;
      lua_State mainth = mainthread(g);
      errcode = luaE_resetthread(L, errcode); /* close all upvalues */
      L.status = errcode;
      if (mainth.errorJmp != null)
      {                                              /* main thread has a handler? */
        TValue v = getslot(L, L.top - 1);
        setslot(mainth, mainth.top++, v);
        throw luaD_throw(mainth, errcode);                 /* re-throw in main thread */
      }
      else
      { /* no handler at all; abort */
        if (g.panic != null)
        { /* panic function? */
          lua_unlock(L);
          g.panic.call(L); /* call panic function (last chance to jump out) */
        }
        Runtime.getRuntime().halt(134);
        throw new AssertionError();
      }
    }
  }

  public static RuntimeException luaD_throwbaselevel(lua_State L, byte errcode)
  {
    if (L.errorJmp != null)
    {
      /* unroll error entries up to the first level */
      while (L.errorJmp.previous != null)
        L.errorJmp = L.errorJmp.previous;
    }
    throw luaD_throw(L, errcode);
  }

  public static byte luaD_rawrunprotected(lua_State L, ProtectedCall f, boolean exitpoint)
  {
    int oldnCcalls = L.nCcalls;
    int oldnfz = L.nfz;
    lua_longjmp lj = new lua_longjmp();
    lj.status = LUA_OK;
    lj.exitpoint = exitpoint;
    lj.previous = L.errorJmp; /* chain new error handler */
    lj.level = (lj.previous != null) ? lj.previous.level + 1 : 1;
    L.errorJmp = lj;
    LUAI_TRY(L, lj, f);   /* call 'f' catching errors */
    L.errorJmp = lj.previous; /* restore old error handler */
    L.nCcalls = oldnCcalls;
    L.nfz = oldnfz;
    return lj.status;
  }

  public static byte luaD_rawrunprotected(lua_State L, ProtectedCall f)
  {
    return luaD_rawrunprotected(L, f, false);
  }

  /* }====================================================== */

  /*
  ** {==================================================================
  ** Stack reallocation
  ** ===================================================================
  */

  /* (the stack's limits, MAXSTACK and ERRORSTACKSIZE, are in ldo.h) */

  /* raise a stack error while running the message handler */
  public static RuntimeException luaD_errerr(lua_State L)
  {
    ByteView errstr = Lt.error_in_error_handling;
    TString msg = TString.luaS_createstr(L, errstr);
    setslots(L, L.top, msg);
    L.top++; /* assume EXTRA_STACK */
    throw luaD_throw(L, (byte) LUA_ERRERR);
  }

  /*
  ** Check whether stacks have enough space to run a simple function (such
  ** as a finalizer): At least BASIC_STACK_SIZE in the Lua stack, two
  ** available CallInfos, and two "slots" in the C stack.
  */
  public static int luaD_checkminstack(lua_State L)
  {
    if (getCcalls(L) >= LUAI_MAXCCALLS - 2)
      return 0; /* not enough C-stack slots */
    if (L.ci.next == null && luaE_extendCI(L, 0) == null)
      return 0; /* unable to allocate first ci */
    if (L.ci.next.next == null && luaE_extendCI(L, 0) == null)
      return 0; /* unable to allocate second ci */
    if (L.top + BASIC_STACK_SIZE <= L.stack_last)
      return 1; /* enough (BASIC_STACK_SIZE) free slots in the Lua stack */
    else        /* try to grow stack to a size with enough free slots */
      return luaD_growstack(L, BASIC_STACK_SIZE, 0);
  }

  private static void correctstack(lua_State L)
  {
    for (CallInfo ci = L.ci; ci != null; ci = ci.previous)
    {
      if (isLua(ci) != 0)
        ci.trap = 1; /* signal to update 'trap' in 'luaV_execute' */
    }
  }

  public static int luaD_reallocstack(lua_State L, int newsize, int raiseerror)
  {
    int oldsize = stacksize(L);
    int i;
    int oldn = oldsize + EXTRA_STACK;
    int newn = newsize + EXTRA_STACK;
    int keep = (oldn < newn) ? oldn : newn;
    byte oldgcstop = L.l_G.gcstopem;
    assert newsize <= MAXSTACK || newsize == ERRORSTACKSIZE;
    L.l_G.gcstopem = 1; /* stop emergency collection */
    long[] newstk = luaM_alloc_TValue_span(L.l_G, L, new long[0], newn, false);
    L.l_G.gcstopem = oldgcstop; /* restore emergency collection */
    if (newstk == null)
    {
      correctstack(L);
      if (raiseerror != 0)
        luaD_throw(L, (byte) LUA_ERRMEM);
      else
        return 0; /* do not raise an error */
    }
    Object[] newstk_ref = luaM_alloc_TValue_span(new Object[0], newn);
    byte[] newstk_tt = luaM_alloc_TValue_span(new byte[0], newn);
    for (int si = 0; si < keep; si++)
    {
      newstk[si] = L.stack_prim[si];
      newstk_ref[si] = L.stack_ref[si];
      newstk_tt[si] = L.stack_tt[si];
    }
    luaM_alloc_TValue_span(L.l_G, L, L.stack_prim, 0, false);
    L.stack_prim = newstk;
    L.stack_ref = newstk_ref;
    L.stack_tt = newstk_tt;
    correctstack(L);
    L.stack_last = newsize;
    for (i = oldsize + EXTRA_STACK; i < newsize + EXTRA_STACK; i++)
      setslottag(L, i, LUA_VNIL); /* erase new segment */
    return 1;
  }

  /*
  ** Try to grow the stack by at least 'n' elements. When 'raiseerror'
  ** is true, raises any error; otherwise, return 0 in case of errors.
  */
  public static int luaD_growstack(lua_State L, int n, int raiseerror)
  {
    int size = stacksize(L);
    if (size > MAXSTACK)
    {
      /* if stack is larger than maximum, thread is already using the
         extra space reserved for errors, that is, thread is handling
         a stack error; cannot grow further than that. */
      assert stacksize(L) == ERRORSTACKSIZE;
      if (raiseerror != 0)
        luaD_errerr(L); /* stack error inside message handler */
      return 0;         /* if not 'raiseerror', just signal it */
    }
    else if (n < MAXSTACK)
    {                                   /* avoids arithmetic overflows */
      int newsize = size + (size >> 1); /* tentative new size (size * 1.5) */
      int needed = (int) L.top + n;
      if (newsize > MAXSTACK) /* cannot cross the limit */
        newsize = MAXSTACK;
      if (newsize < needed) /* but must respect what was asked for */
        newsize = needed;
      if (newsize <= MAXSTACK)
        return luaD_reallocstack(L, newsize, raiseerror);
    }
    /* else stack overflow */
    /* add extra size to be able to handle the error message */
    luaD_reallocstack(L, ERRORSTACKSIZE, raiseerror);
    if (raiseerror != 0)
      luaG_runerror(L, Lt.stack_overflow);
    return 0;
  }

  /*
  ** Compute how much of the stack is being used, by computing the
  ** maximum top of all call frames in the stack and the current top.
  */
  private static int stackinuse(lua_State L)
  {
    CallInfo ci;
    int res;
    int lim = L.top;
    for (ci = L.ci; ci != null; ci = ci.previous)
    {
      if (lim < ci.top)
        lim = ci.top;
    }
    assert lim <= L.stack_last + EXTRA_STACK;
    res = (int) lim + 1; /* part of stack in use */
    if (res < LUA_MINSTACK)
      res = LUA_MINSTACK; /* ensure a minimum size */
    return res;
  }

  /*
  ** If stack size is more than 3 times the current use, reduce that size
  ** to twice the current use. (So, the final stack size is at most 2/3 the
  ** previous size, and half of its entries are empty.)
  ** As a particular case, if stack was handling a stack overflow and now
  ** it is not, 'max' (limited by MAXSTACK) will be smaller than
  ** stacksize (equal to ERRORSTACKSIZE in this case), and so the stack
  ** will be reduced to a "regular" size.
  */
  public static void luaD_shrinkstack(lua_State L)
  {
    if (L == L.l_G.budgetth)
      return; /* its stack and frames are reserved: a callback never allocates them */
    int inuse = stackinuse(L);
    int max = (inuse > MAXSTACK / 3) ? MAXSTACK : inuse * 3;
    /* if thread is currently not handling a stack overflow and its
       size is larger than maximum "reasonable" size, shrink it */
    if (inuse <= MAXSTACK && stacksize(L) > max)
    {
      int nsize = (inuse > MAXSTACK / 2) ? MAXSTACK : inuse * 2;
      luaD_reallocstack(L, nsize, 0); /* ok if that fails */
    }
    luaE_shrinkCI(L); /* shrink CI list */
  }

  /* }================================================================== */

  /*
  ** Call a hook for the given event. Make sure there is a hook to be
  ** called. (Both 'L->hook' and 'L->hookmask', which trigger this
  ** function, can be changed asynchronously by signals.)
  */
  public static void luaD_hook(lua_State L, int event, int line,
                               int ftransfer, int ntransfer)
  {
    lua_Hook hook = L.hook;
    if (hook != null && L.allowhook != 0 && L.l_G.budgetL == null)
    { /* make sure there is a hook (and no budget callback is running) */
      CallInfo ci = L.ci;
      int top = L.top;     /* preserve original 'top' */
      int ci_top = ci.top; /* idem for 'ci->top' */
      lua_Debug ar = new lua_Debug();
      ar.event = event;
      ar.currentline = line;
      ar.i_ci = ci;
      L.transferinfo.ftransfer = ftransfer;
      L.transferinfo.ntransfer = ntransfer;
      if (isLua(ci) != 0 && L.top < ci.top)
        L.top = ci.top;           /* protect entire activation register */
      luaD_checkstack(L, LUA_MINSTACK); /* ensure minimum stack size */
      if (ci.top < L.top + LUA_MINSTACK)
        ci.top = L.top + LUA_MINSTACK;
      L.allowhook = 0; /* cannot call hooks inside a hook */
      ci.callstatus |= CIST_HOOKED;
      ci.hooktop = top;     /* (a hook function that calls with 'lua_callhook' */
      ci.hookcitop = ci_top; /* can freeze: when it ends after a thaw, this is */
      lua_unlock(L);          /* how it is finished: see 'luaD_finishhook') */
      hook.call(L, ar);
      lua_lock(L);
      luaD_finishhook(L, ci);
    }
  }

  /*
  ** The end of a hook: hooks and the frame go back as they were. (After a
  ** thaw, 'unroll' does this when the hook function ends.)
  */
  public static void luaD_finishhook(lua_State L, CallInfo ci)
  {
    assert L.allowhook == 0;
    L.allowhook = 1;
    ci.top = ci.hookcitop;
    L.top = ci.hooktop;
    ci.callstatus &= ~CIST_HOOKED;
  }


  /*
  ** {==================================================================
  ** Budget and freezing
  ** ===================================================================
  */

  private static final class BudgetCall implements ProtectedCall
  {
    lua_Budget f;
    lua_State user;

    @Override
    public void run(lua_State L)
    {
      lua_unlock(L);
      f.call(user, L);
      lua_lock(L);
    }
  }

  /*
  ** The topmost budgeted thread of the chain 'L' (the thread that spent the
  ** budget) runs in, the one a freeze freezes; and what keeps the freeze from
  ** passing (the innermost of a call that cannot freeze and a resume without
  ** a continuation), or 0.
  */
  private static final class ChainTop
  {
    lua_State T;
    int why;

    ChainTop(lua_State T, int why)
    {
      this.T = T;
      this.why = why;
    }
  }

  private static ChainTop chaintop(lua_State L)
  {
    int nfz = L.l_G.budgetnfz; /* (the callback's own calls do not count) */
    lua_State T = L;
    int why = 0;
    for (;;)
    {
      if (nfz > 0 && why == 0)
        why = LUA_NOFREEZE_CALL;
      lua_State R = T.resumer;
      if (R == null || R.beff == null)
        return new ChainTop(T, why);
      if (T.resumedk == 0 && why == 0)
        why = LUA_NOFREEZE_RESUME;
      T = R;
      nfz = T.nfz;
    }
  }

  /*
  ** Call the message handler at 'L->top - 2' for the error being raised. A
  ** freeze may pass the handler: the mark on the frame of the error says
  ** that, when the handler ends (after a thaw), the error goes on ('unroll').
  ** An error in the handler, or any other way out, leaves no mark.
  */
  public static void luaD_callerrh(lua_State L)
  {
    CallInfo ci = L.ci;
    ci.callstatus |= CIST_ERRH;
    try
    {
      luaD_callnoyield(L, L.top - 2, 1);
    }
    catch (LuaError e)
    {
      if (e.status != LUA_FREEZE)
        ci.callstatus &= ~CIST_ERRH;
      throw e;
    }
    ci.callstatus &= ~CIST_ERRH;
  }

  /*
  ** Could the chain of the running thread 'L' freeze now? No call that
  ** cannot freeze in the way, and every resume with a continuation. 0 if it
  ** could, else what is in the way (LUA_NOFREEZE_*).
  */
  public static int luaD_nofreeze(lua_State L)
  {
    lua_State T = L;
    for (;;)
    {
      if (T.nfz > 0)
        return LUA_NOFREEZE_CALL;
      lua_State R = T.resumer;
      if (R == null || R.beff == null)
        return 0;
      if (T.resumedk == 0)
        return LUA_NOFREEZE_RESUME;
      T = R;
    }
  }

  /*
  ** Destroy the chain of 'L', which ran out of budget, with error 'err':
  ** the C stack unwinds to the call that resumed or called 'user', the top
  ** of the chain, as in a freeze, but its threads die (see 'luaD_killed').
  */
  private static RuntimeException killchain(lua_State L, lua_State user, TValue err)
  {
    global_State g = L.l_G;
    setobj(L, g.killerr, err);
    g.freezekill = true;
    g.freezetarget = user;
    throw luaD_throw(L, LUA_FREEZE);
  }

  /*
  ** Call the budget callback: the budget is spent, at a budget point of 'L'
  ** (the running thread: an instruction, or a check point of C work). The
  ** callback runs with no hooks and no budget (and hooks do not see it); the
  ** budget refills after it. It leaves 'L' as it is (see 'lua_Budget').
  ** Returns the coroutine to freeze, if the callback asked to ('lua_freeze'
  ** exits it). When that cannot be, or the callback raises an error, the
  ** chain is destroyed (see 'killchain'): no user code catches or sees it.
  */
  public static lua_State luaD_budget(lua_State L)
  {
    global_State g = L.l_G;
    assert g.budgetL == null; /* (a callback never runs inside another) */
    CallInfo ci = L.ci;
    int top = L.top;     /* preserve original 'top' */
    int ci_top = ci.top; /* idem for 'ci->top' */
    byte allowhook = L.allowhook;
    int oldpc = L.oldpc;       /* hooks do not see the callback: their */
    int hookcount = L.hookcount; /* state stays as it is */
    int errfunc = L.errfunc;
    BudgetGroup bg = L.beff;
    lua_State B = g.budgetth;
    int bncalls = (B != null) ? B.nCcalls : 0;
    /* the budget goes on also if the callback raises an error */
    try
    {
      if (isLua(ci) != 0 && L.top < ci.top)
        L.top = ci.top; /* protect entire activation register */
      /* (no room is made in the stack of 'L': the callback leaves it, and its
         C calls, as they are, so that where a stack overflow happens does not
         depend on where the budget runs out; see 'lua_budgetthread') */
      L.allowhook = 0; /* no hooks inside */
      L.errfunc = 0;   /* and no message handler: its errors are not for user code */
      g.budgetL = L;
      g.budgetg = bg; /* (kept by the collector while the callback runs) */
      g.budgetnfz = L.nfz;
      ChainTop chain = chaintop(L);
      lua_State user = chain.T;
      bg.left = MAX_LMEM; /* no budget inside */
      BudgetCall bc = new BudgetCall();
      bc.f = bg.f;
      bc.user = user;
      byte status = luaD_rawrunprotected(L, bc, true);
      TValue err = new TValue(); /* nil */
      if (status != LUA_OK)
      { /* the callback left: an exit, or an error */
        if (status == LUA_ERRMEM)
          setsvalue(L, err, g.memerrmsg);
        else if (status != LUA_CBEXIT)
        {
          TValue v = getslot(L, L.top - 1);
          setobj(L, err, v);
        }
        if (B != null && B != L)
        { /* its thread goes back to the base, closing what is pending */
          B.nCcalls = bncalls;
          g.budgetexit = true; /* (a 'freeze' in them is outside the callback) */
          byte cs = luaE_resetcallbackthread(B);
          g.budgetexit = false;
          if (cs != LUA_OK)
          { /* a '__close' failed */
            status = cs;
            TValue v = getslot(B, 1);
            setobj(L, err, v);
          }
        }
        L.ci = ci;
      }
      L.errfunc = errfunc;
      L.allowhook = allowhook;
      L.oldpc = oldpc;
      L.hookcount = hookcount;
      ci.top = ci_top;
      L.top = top;
      if (status == LUA_OK)
        return null;
      if (status == LUA_CBEXIT)
      {
        if (chain.why == 0)
          return user;
        setsvalue(L, err, g.nofreezemsg);
      }
      throw killchain(L, user, err);
    }
    finally
    {
      g.budgetL = null;
      g.budgetg = null;
      g.budgetexit = false;
      bg.left = bg.budget;
    }
  }

  /*
  ** Is 'X' a thread of the chain whose budget callback runs (from the thread
  ** that spent the budget up to the topmost budgeted one), or the thread the
  ** callback runs on? A freeze from the callback freezes the whole chain.
  */
  private static boolean incallbackchain(global_State g, lua_State X)
  {
    if (X == g.budgetth)
      return true;
    for (lua_State T = g.budgetL; T != null;)
    {
      if (T == X)
        return true;
      lua_State R = T.resumer;
      if (R == null || R.beff == null)
        break;
      T = R;
    }
    return false;
  }

  /* leave the budget callback: 'lua_freeze' */
  public static RuntimeException luaD_exitcallback(lua_State L)
  {
    global_State g = L.l_G;
    if (g.budgetL == null)
      luaG_runerror(L, Lt.freeze_outside_the_budget_callback);
    if (g.budgetexit || !incallbackchain(g, L))
    { /* (raised on the thread that runs: a Lua callback's or a C callback's, given another) */
      lua_State R = (L == g.budgetth || L == g.budgetL) ? L : g.budgetL;
      luaG_runerror(R, Lt.freeze_outside_the_budget_callback);
    }
    throw new LuaError(LUA_CBEXIT, 0);
  }

  /*
  ** Freeze 'co': unwind the C stack of 'L', the running thread, up to the
  ** call that resumed or called 'co' ('lua_resumek', 'lua_thawk', or a host
  ** 'lua_pcallk' or 'lua_callk'), which finishes the freeze. Nothing else
  ** changes on the way.
  */
  public static RuntimeException luaD_freeze(lua_State L, lua_State co)
  {
    assert L.errorJmp != null;
    L.l_G.freezetarget = co;
    L.frozencc = L.nCcalls; /* (see 'forgetcalls') */
    throw luaD_throw(L, LUA_FREEZE);
  }

  /*
  ** The chain of thread 'L' is being destroyed (see 'killchain'): 'L' dies
  ** with the error, as a coroutine does that ended with an error, and
  ** leaves the budget it ran with. Returns true if the unwinding ends here.
  */
  public static boolean luaD_killed(lua_State L)
  {
    global_State g = L.l_G;
    L.resumer = null;
    luaE_leavebudget(L);
    L.status = LUA_ERRRUN; /* mark thread as 'dead' */
    L.closeself = false;
    setslot(L, L.top, g.killerr); /* (twice, as an error leaves it: the second */
    setslot(L, L.top + 1, g.killerr); /* stays after a resume moves the first) */
    L.top += 2;
    L.ci.top = L.top;
    if (g.freezetarget != L)
      return false;
    g.freezetarget = null;
    g.freezekill = false;
    setnilvalue(g.killerr);
    return true;
  }

  /* }================================================================== */

  /*
  ** Executes a call hook for Lua functions. This function is called
  ** whenever 'hookmask' is not zero, so it checks whether call hooks are
  ** active.
  */
  public static void luaD_hookcall(lua_State L, CallInfo ci)
  {
    L.oldpc = 0; /* set 'oldpc' for new function */
    if ((L.hookmask & LUA_MASKCALL) != 0)
    { /* is call hook on? */
      int event = ((ci.callstatus & CIST_TAIL) != 0) ? LUA_HOOKTAILCALL
                                                     : LUA_HOOKCALL;
      Proto p = ci_func(L, ci).p;
      boolean entry = (ci.savedpc == 0); /* (otherwise, in its 'VARARGPREP') */
      ci.hookev = entry ? HE_CALL : HE_CALLVA;
      if (entry)
        ci.callstatus |= CIST_FROZE; /* (if it freezes, the function starts again, without the hook) */
      ci.savedpc++; /* hooks assume 'pc' is already incremented */
      luaD_hook(L, event, -1, 1, p.numparams);
      ci.savedpc--; /* correct 'pc' */
      ci.callstatus &= ~CIST_FROZE;
    }
  }

  /*
  ** Executes a return hook for Lua and C functions and sets/corrects
  ** 'oldpc'. (Note that this correction is needed by the line hook, so it
  ** is done even when return hooks are off.)
  */
  private static void rethook(lua_State L, CallInfo ci, int nres, byte ev)
  {
    if ((L.hookmask & LUA_MASKRET) != 0 &&
        !(isLua(ci) == 0 && withvariant(slottag(L, ci.func)) == LUA_VLCF && slotfval(L, ci.func) == luaC_drainfunction()))
    {                                   /* is return hook on (and not for the call of the waiting '__gc')? */
      int firstres = L.top - nres; /* index of first result */
      int delta = 0;                    /* correction for vararg functions */
      int ftransfer;
      if (isLua(ci) != 0)
      {
        Proto p = ci_func(L, ci).p;
        if ((p.flag & PF_VAHID) != 0)
          delta = ci.nextraargs + p.numparams + 1;
      }
      ci.func += delta; /* if vararg, back to virtual 'func' */
      ftransfer = (int) (firstres - ci.func);
      ci.nres = nres; /* (what the return needs, if the hook freezes) */
      ci.hookdelta = delta;
      ci.hookev = ev;
      luaD_hook(L, LUA_HOOKRET, -1, ftransfer, nres); /* call it */
      ci.func -= delta;
    }
    if (isLua(ci = ci.previous) != 0)
      L.oldpc = ci.savedpc - 1; /* set 'oldpc' */
  }

  /*
  ** Check whether 'func' has a '__call' metafield. If so, put it in the
  ** stack, below original 'func', so that 'luaD_precall' can call it.
  ** Raise an error if there is no '__call' metafield.
  ** Bits CIST_CCMT in status count how many _call metamethods were
  ** invoked and how many corresponding extra arguments were pushed.
  ** (This count will be saved in the 'callstatus' of the call).
  **  Raise an error if this counter overflows.
  */
  private static int tryfuncTM(lua_State L, int func, int status)
  {
    TValue tm;
    int p;
    tm = luaT_gettmbyobj(L, func, TM_CALL);
    if (ttisnil(tm)) /* no metamethod? */
      luaG_callerror(L, func);
    for (p = L.top; p > func; p--) /* open space for metamethod */
      copyslot(L, p, p - 1);
    L.top++;                          /* stack space pre-allocated by the caller */
    setslot(L, func, tm);               /* metamethod is the new function to be called */
    if ((status & MAX_CCMT) == MAX_CCMT) /* is counter full? */
      luaG_runerror(L, Lt.call_chain_too_long);
    return status + (1 << CIST_CCMT); /* increment counter */
  }

  /* Generic case for 'moveresult' */
  private static void genmoveresults(lua_State L, int res, int nres,
                                     int wanted)
  {
    int firstresult = L.top - nres; /* index of first result */
    int i;
    if (nres > wanted)         /* extra results? */
      nres = wanted;           /* don't need them */
    for (i = 0; i < nres; i++) /* move all results to correct place */
      copyslot(L, res + i, firstresult + i);
    for (; i < wanted; i++) /* complete wanted number of results */
      setslottag(L, res + i, LUA_VNIL);
    L.top = res + wanted; /* top points after the last result */
  }

  /*
  ** Given 'nres' results at 'firstResult', move 'fwanted-1' of them
  ** to 'res'.  Handle most typical cases (zero results for commands,
  ** one result for expressions, multiple results for tail calls/single
  ** parameters) separated. The flag CIST_TBC in 'fwanted', if set,
  ** forces the switch to go to the default case.
  */
  private static void moveresults(lua_State L, int res, int nres,
                                  int fwanted)
  {
    switch (fwanted)
    {           /* handle typical cases separately */
    case 0 + 1: /* no values needed */
      L.top = res;
      return;
    case 1 + 1:                             /* one value needed */
      if (nres == 0)                        /* no results? */
        setslottag(L, res, LUA_VNIL);             /* adjust with nil */
      else                                  /* at least one result */
        copyslot(L, res, L.top - nres); /* move it to proper place */
      L.top = res + 1;
      return;
    case LUA_MULTRET + 1:
      genmoveresults(L, res, nres, nres); /* we want all results */
      break;
    default:
    { /* two/more results and/or to-be-closed variables */
      int wanted = get_nresults(fwanted);
      if ((fwanted & CIST_TBC) != 0)
      { /* to-be-closed variables? */
        L.ci.nres = nres;
        L.ci.callstatus |= CIST_CLSRET; /* in case of yields */
        res = luaF_close(L, res, (byte) CLOSEKTOP, 1);
        L.ci.callstatus &= ~CIST_CLSRET;
        if ((L.hookmask & ~LUA_MASKBUDGET) != 0)
        { /* if needed, call hook after '__close's */
          int savedres = res;
          L.ci.hookres = res;
          rethook(L, L.ci, nres, HE_RETTBC);
          res = savedres;
        }
        if (wanted == LUA_MULTRET)
          wanted = nres; /* we want all results */
      }
      genmoveresults(L, res, nres, wanted);
      break;
    }
    }
  }

  /*
  ** Finishes a function call: calls hook if necessary, moves current
  ** number of results to proper place, and returns to previous call
  ** info. If function has to close variables, hook must be called after
  ** that.
  */
  public static void luaD_poscall(lua_State L, CallInfo ci, int nres)
  {
    int fwanted = ci.callstatus & (CIST_TBC | CIST_NRESULTS);
    if ((L.hookmask & ~LUA_MASKBUDGET) != 0 && (fwanted & CIST_TBC) == 0)
      rethook(L, ci, nres, HE_RET);
    /* move results to proper place */
    moveresults(L, ci.func, nres, fwanted);
    /* function cannot be in any of these cases when returning */
    assert (ci.callstatus &
            (CIST_HOOKED | CIST_YPCALL | CIST_FIN | CIST_CLSRET)) == 0;
    L.ci = ci.previous; /* back to caller (after closing variables) */
  }

  private static CallInfo next_ci(lua_State L)
  {
    return L.ci.next != null ? L.ci.next : luaE_extendCI(L, 1);
  }

  /*
  ** Allocate and initialize CallInfo structure. At this point, the
  ** only valid fields in the call status are number of results,
  ** CIST_C (if it's a C function), and number of extra arguments.
  ** (All these bit-fields fit in 16-bit values.)
  */
  private static CallInfo prepCallInfo(lua_State L, int func, int status,
                                       int top)
  {
    CallInfo ci = L.ci = next_ci(L); /* new frame */
    ci.func = func;
    assert (status & ~(CIST_NRESULTS | CIST_C | MAX_CCMT | CIST_CCOUNT)) == 0;
    ci.callstatus = status;
    if ((status & CIST_CCOUNT) != 0)   /* entered through C? */
      ci.nccalls = L.nCcalls; /* (see 'forgetcalls') */
    ci.top = top;
    return ci;
  }

  /*
  ** precall for C functions
  */
  private static int precallC(lua_State L, int func, int status,
                              lua_CFunction f)
  {
    int n; /* number of returns */
    CallInfo ci;
    checkstackp(L, LUA_MINSTACK, func); /* ensure minimum stack size */
    L.ci = ci = prepCallInfo(L, func, status | CIST_C,
                             L.top + LUA_MINSTACK);
    assert ci.top <= L.stack_last;
    luaE_spend(L, 1); /* a C function call */
    if ((L.hookmask & LUA_MASKCALL) != 0 && f != luaC_drainfunction())
    {
      int narg = (int) (L.top - func) - 1;
      ci.hookev = HE_CALLC;
      luaD_hook(L, LUA_HOOKCALL, -1, 1, narg);
    }
    lua_unlock(L);
    n = f.call(L); /* do the actual call */
    lua_lock(L);
    api_checknelems(L, n);
    luaD_poscall(L, ci, n);
    return n;
  }

  /*
  ** Prepare a function for a tail call, building its call info on top
  ** of the current call info. 'narg1' is the number of arguments plus 1
  ** (so that it includes the function itself). Return the number of
  ** results, if it was a C function, or -1 for a Lua function.
  */
  public static int luaD_pretailcall(lua_State L, CallInfo ci, int func,
                                     int narg1, int delta)
  {
    int status = LUA_MULTRET + 1;
    for (;;)
    {
      switch (withvariant(slottag(L, func)))
      {
      case LUA_VCCL: /* C closure */
      {
        CClosure ccl = slotclCval(L, func);
        return precallC(L, func, status, ccl.f);
      }
      case LUA_VLCF: /* light C function */
        return precallC(L, func, status, slotfval(L, func));
      case LUA_VLCL:
      { /* Lua function */
        Proto p = slotclLval(L, func).p;
        int fsize = p.maxstacksize; /* frame size */
        int nfixparams = p.numparams;
        int i;
        checkstackp(L, fsize - delta, func);
        ci.func -= delta;        /* restore 'func' (if vararg) */
        for (i = 0; i < narg1; i++) /* move down function and arguments */
          copyslot(L, ci.func + i, func + i);
        func = ci.func; /* moved-down function */
        for (; narg1 <= nfixparams; narg1++)
          setslottag(L, func + narg1, LUA_VNIL); /* complete missing arguments */
        ci.top = func + 1 + fsize;  /* top for new function */
        assert ci.top <= L.stack_last;
        ci.savedpc = 0; /* starting point */
        ci.callstatus |= CIST_TAIL;
        L.top = func + narg1; /* set top */
        return -1;
      }
      default:
      {                                      /* not a function */
        checkstackp(L, 1, func);             /* space for metamethod */
        status = tryfuncTM(L, func, status); /* try '__call' metamethod */
        narg1++;
        continue; /* try again */
      }
      }
    }
  }

  private static CallInfo precall(lua_State L, int func, int nresults, int ccount)
  {
    assert nresults + 1 <= MAXRESULTS + 1;
    int status = (nresults + 1) | ccount;
    for (;;)
    {
      switch (withvariant(slottag(L, func)))
      {
      case LUA_VCCL: /* C closure */
      {
        CClosure ccl = slotclCval(L, func);
        precallC(L, func, status, ccl.f);
        return null;
      }
      case LUA_VLCF: /* light C function */
        precallC(L, func, status, slotfval(L, func));
        return null;
      case LUA_VLCL:
      { /* Lua function */
        CallInfo ci;
        Proto p = slotclLval(L, func).p;
        int narg = (int) (L.top - func) - 1; /* number of real arguments */
        int nfixparams = p.numparams;
        int fsize = p.maxstacksize; /* frame size */
        checkstackp(L, fsize, func);
        L.ci = ci = prepCallInfo(L, func, status, func + 1 + fsize);
        ci.savedpc = 0; /* starting point */
        for (; narg < nfixparams; narg++)
          setslottag(L, L.top++, LUA_VNIL); /* complete missing arguments */
        assert ci.top <= L.stack_last;
        return ci;
      }
      default:
      {                                      /* not a function */
        checkstackp(L, 1, func);             /* space for metamethod */
        status = tryfuncTM(L, func, status); /* try '__call' metamethod */
        continue;                            /* try again with metamethod */
      }
      }
    }
  }

  public static CallInfo luaD_precall(lua_State L, int func, int nresults)
  {
    return precall(L, func, nresults, 0);
  }

  /*
  ** Call a function (C or Lua) through C. 'inc' can be 1 (increment
  ** number of recursive invocations in the C stack) or nyci (the same
  ** plus increment number of non-yieldable calls).
  ** This function can be called with some use of EXTRA_STACK, so it should
  ** check the stack before doing anything else. 'luaD_precall' already
  ** does that.
  */
  private static void ccall(lua_State L, int func, int nResults, int inc)
  {
    CallInfo ci;
    L.nCcalls += inc;
    if (getCcalls(L) >= LUAI_MAXCCALLS)
    {
      checkstackp(L, 0, func); /* free any use of EXTRA_STACK */
      luaE_checkcstack(L);
    }
    if ((ci = precall(L, func, nResults, CIST_CCOUNT)) != null)
    {                               /* Lua function? */
      ci.callstatus |= CIST_FRESH; /* mark that it is a "fresh" execute */
      luaV_execute(L, ci);          /* call it */
    }
    L.nCcalls -= inc;
  }

  /*
  ** External interface for 'ccall'
  */
  public static void luaD_call(lua_State L, int func, int nResults)
  {
    ccall(L, func, nResults, 1);
  }

  /*
  ** Similar to 'luaD_call', but does not allow yields during the call.
  */
  public static void luaD_callnoyield(lua_State L, int func, int nResults)
  {
    ccall(L, func, nResults, nyci);
  }

  /*
  ** Finish the job of 'lua_pcallk' after it was interrupted by an yield.
  ** (The caller, 'finishCcall', does the final call to 'adjustresults'.)
  ** The main job is to complete the 'luaD_pcall' called by 'lua_pcallk'.
  ** If a '__close' method yields here, eventually control will be back
  ** to 'finishCcall' (when that '__close' method finally returns) and
  ** 'finishpcallk' will run again and close any still pending '__close'
  ** methods. Similarly, if a '__close' method errs, 'precover' calls
  ** 'unroll' which calls ''finishCcall' and we are back here again, to
  ** close any pending '__close' methods.
  ** Note that, up to the call to 'luaF_close', the corresponding
  ** 'CallInfo' is not modified, so that this repeated run works like the
  ** first one (except that it has at least one less '__close' to do). In
  ** particular, field CIST_RECST preserves the error status across these
  ** multiple runs, changing only if there is a new error.
  */
  private static byte finishpcallk(lua_State L, CallInfo ci)
  {
    byte status = getcistrecst(ci); /* get original status */
    if (status == LUA_OK)    /* no error? */
      status = LUA_YIELD;              /* was interrupted by an yield */
    else
    { /* error */
      int func = ci.funcidx;
      L.allowhook = getoah(ci);             /* restore 'allowhook' */
      func = luaF_close(L, func, status, ((ci.callstatus & CIST_NYCALL) != 0) ? 2 : 1); /* can yield (where the call could) or raise an error */
      luaD_seterrorobj(L, status, func);
      luaD_shrinkstack(L);      /* restore stack size in case of overflow */
      setcistrecst(ci, LUA_OK); /* clear original status */
    }
    ci.callstatus &= ~CIST_YPCALL;
    L.errfunc = ci.old_errfunc;
    /* if it is here, there were errors or yields; unlike 'lua_pcallk',
       do not change status */
    return status;
  }

  /*
  ** Finishes an API call with a continuation ('lua_getik' and the others)
  ** whose metamethod yielded and has now returned (its result is on the
  ** top): the stack is left as the call leaves it. A concatenation goes on
  ** with the values left, and its metamethods may yield again.
  */
  private static void finishapik(lua_State L, CallInfo ci)
  {
    switch (ci.apiop)
    {
    case APIK_PUSH:
      break;
    case APIK_OVERKEY:
      copyslot(L, L.top - 2, L.top - 1);
      L.top--;
      break;
    case APIK_POP1:
      L.top--;
      break;
    case APIK_POP2:
      L.top -= 2;
      break;
    case APIK_BOOL:
      setslottag(L, L.top - 1, tagisfalse(slottag(L, L.top - 1)) ? LUA_VFALSE : LUA_VTRUE);
      break;
    case APIK_ARITH:
      copyslot(L, L.top - 3, L.top - 1);
      L.top -= 2;
      break;
    case APIK_SETTOP:
    case APIK_CLOSESLOT:
    { /* the '__close' that was interrupted has returned: close what is left */
      boolean nny = yieldable(L) == 0 || (ci.callstatus & CIST_FZONLY) != 0;
      if (nny)
        ci.callstatus |= CIST_NYCALL; /* (its next '__close', as in 'armk') */
      int level = luaF_close(L, ci.apifirst, (byte) CLOSEKTOP, nny ? 2 : 1); /* may yield again */
      ci.callstatus &= ~CIST_NYCALL;
      if (ci.apiop == APIK_SETTOP)
        L.top = level;
      else
        setslottag(L, level, LUA_VNIL);
      break;
    }
    default:
    {
      assert ci.apiop == APIK_CONCAT;
      int top = L.top - 1;        /* top when the metamethod was called */
      copyslot(L, top - 2, top);     /* its result, over its first operand */
      L.top = top - 1;
      if (yieldable(L) == 0 || (ci.callstatus & CIST_FZONLY) != 0)
        ci.callstatus |= CIST_NYCALL; /* (its next metamethods, as in 'armk') */
      luaV_concat(L, (int) (L.top - ci.apifirst)); /* may yield again */
      ci.callstatus &= ~CIST_NYCALL;
      luaC_checkGC(L);
      break;
    }
    }
    ci.callstatus &= ~CIST_APIK;
  }

  /*
  ** The C calls and non-yieldable calls of a thread ('nCcalls') are where a
  ** C stack overflow happens and whether it can yield: a freeze must not
  ** change them. Each frame keeps the count it runs with ('nccalls'); a
  ** thaw restores the count where the thread froze ('frozencc'), and the
  ** frames going on after it take theirs. Where stock Lua resets the count
  ** of a thread (a resume after a yield, an error down to a protected call
  ** that the resume recovers), the frames left take the reset count too:
  ** they now run with no C calls under them.
  */
  private static void forgetcalls(lua_State L, CallInfo ci)
  {
    for (; ci != L.base_ci; ci = ci.previous)
      ci.nccalls = L.ccbase;
    L.nCcalls = L.ccbase;
  }

  /* the count frame 'ci' runs with: that of the frame that entered through
     C under it (or the thread's base) */
  private static int framecalls(lua_State L, CallInfo ci)
  {
    CallInfo start = ci;
    while (ci != L.base_ci && (ci.callstatus & CIST_CCOUNT) == 0)
      ci = ci.previous;
    int n = (ci == L.base_ci) ? L.ccbase : ci.nccalls;
    if (ci != start)
    { /* the frame keeps what it found, so that a deep chain of Lua calls is not
         walked again and again (a thaw in a deep recursion) */
      start.nccalls = n;
      start.callstatus |= CIST_CCOUNT;
    }
    return n;
  }

  /*
  ** Completes the execution of a C function interrupted by an yield.
  ** The interruption must have happened while the function was either
  ** closing its tbc variables in 'moveresults' or executing
  ** 'lua_callk'/'lua_pcallk'. In the first case, it just redoes
  ** 'luaD_poscall'. In the second case, the call to 'finishpcallk'
  ** finishes the interrupted execution of 'lua_pcallk'.  After that, it
  ** calls the continuation of the interrupted function and finally it
  ** completes the job of the 'luaD_call' that called the function.  In
  ** the call to 'adjustresults', we do not know the number of results
  ** of the function called by 'lua_callk'/'lua_pcallk', so we are
  ** conservative and use LUA_MULTRET (always adjust).
  */
  private static void finishCcall(lua_State L, CallInfo ci)
  {
    int n; /* actual number of results from C function */
    L.nCcalls = framecalls(L, ci); /* (see 'forgetcalls') */
    if ((ci.callstatus & CIST_CLSRET) != 0)
    { /* was closing TBC variable? */
      assert (ci.callstatus & CIST_TBC) != 0;
      n = ci.nres; /* just redo 'luaD_poscall' */
      /* don't need to reset CIST_CLSRET, as it will be set again anyway */
    }
    else
    {
      byte status = LUA_YIELD;   /* default if there were no errors */
      lua_KFunction kf = ci.k; /* continuation function */
      assert kf != null; /* must have a continuation */
      luaE_spend(L, 1); /* a hand-off back to C */
      if ((ci.callstatus & CIST_YPCALL) != 0) /* was inside a 'lua_pcallk'? */
        status = finishpcallk(L, ci);   /* finish it (a '__close' that fails here leaves the mark of the call */
      else if ((ci.callstatus & CIST_APIK) != 0) /* inside an API call ('lua_getik'...)? */
        finishapik(L, ci);              /* that cannot yield: see 'precover') */
      ci.callstatus &= ~CIST_NYCALL; /* (its call that cannot yield ended) */
      adjustresults(L, LUA_MULTRET);    /* finish 'lua_callk' */
      lua_unlock(L);
      n = kf.call(L, status, ci.ctx); /* call continuation */
      lua_lock(L);
      api_checknelems(L, n);
    }
    luaD_poscall(L, ci, n); /* finish 'luaD_call' */
  }

  /*
  ** The function of a hook that froze ended, after a thaw: finish the hook,
  ** and then what it interrupted. A Lua function interrupted at an
  ** instruction or at its start (CIST_FROZE) goes on there; the rest of a
  ** call or return is done here.
  */
  private static void finishhooked(lua_State L, CallInfo ci)
  {
    byte ev = ci.hookev;
    ci.hookev = 0;
    luaD_finishhook(L, ci);
    switch (ev)
    {
    case HE_CALLVA:
      ci.savedpc--; /* ('luaD_hookcall' did not get to correct it) */
      L.oldpc = 1;  /* next opcode will be seen as a "new" line */
      break;
    case HE_CALLC:
    { /* the hook was before the function runs */
      lua_CFunction f = (withvariant(slottag(L, ci.func)) == LUA_VCCL) ? slotclCval(L, ci.func).f
                                                                       : slotfval(L, ci.func);
      lua_unlock(L);
      int n = f.call(L);
      lua_lock(L);
      api_checknelems(L, n);
      luaD_poscall(L, ci, n);
      break;
    }
    case HE_RET:
    case HE_RETTBC:
    { /* the hook was before the results move */
      int nres = ci.nres;
      ci.func -= ci.hookdelta; /* (back from the virtual 'func' of a vararg function) */
      if (isLua(ci.previous) != 0)
        L.oldpc = ci.previous.savedpc - 1;
      int fwanted = ci.callstatus & (CIST_TBC | CIST_NRESULTS);
      if (ev == HE_RET)
        moveresults(L, ci.func, nres, fwanted);
      else
      { /* (the variables are closed already) */
        int wanted = get_nresults(fwanted);
        if (wanted == LUA_MULTRET)
          wanted = nres;
        genmoveresults(L, ci.hookres, nres, wanted);
      }
      L.ci = ci.previous;
      break;
    }
    default: /* HE_INSTR, HE_CALL: the function goes on (see 'CIST_FROZE') */
      break;
    }
  }

  /*
  ** Executes "full continuation" (everything in the stack) of a
  ** previously interrupted coroutine until the stack is empty (or another
  ** interruption long-jumps out of the loop).
  */
  private static void unroll(lua_State L)
  {
    CallInfo ci;
    while ((ci = L.ci) != L.base_ci)
    {                       /* something in the stack */
      if ((ci.callstatus & CIST_HOOKED) != 0)
      { /* its hook function ended (after a thaw) */
        L.nCcalls = framecalls(L, ci); /* (see 'forgetcalls') */
        finishhooked(L, ci);
        continue;
      }
      if ((ci.callstatus & CIST_ERRH) != 0)
      { /* its message handler ended (after a thaw): the error goes on */
        ci.callstatus &= ~CIST_ERRH;
        luaG_throwerror(L);
      }
      if (isLua(ci) == 0)       /* C function? */
        finishCcall(L, ci); /* complete its execution */
      else
      {                      /* Lua function */
        if ((ci.callstatus & CIST_FROZE) != 0)
        {                    /* frozen at a budget point? */
          ci.savedpc--;     /* its instruction was not executed yet */
          ci.top = ci.func + 1 + ci_func(L, ci).p.maxstacksize; /* its frame as before (a drain point raises it) */
        }
        else
          luaV_finishOp(L);  /* finish interrupted instruction */
        L.nCcalls = framecalls(L, ci); /* (see 'forgetcalls') */
        luaV_execute(L, ci); /* execute down to higher C 'boundary' */
      }
    }
    if ((L.base_ci.callstatus & CIST_ERRH) != 0)
    { /* the handler of an error at base level ended (after a thaw): the error goes on */
      L.base_ci.callstatus &= ~CIST_ERRH;
      luaG_throwerror(L);
    }
  }

  /*
  ** Try to find a suspended protected call (a "recover point") for the
  ** given thread.
  */
  private static CallInfo findpcall(lua_State L)
  {
    CallInfo ci;
    for (ci = L.ci; ci != null; ci = ci.previous)
    { /* search for a pcall */
      if ((ci.callstatus & CIST_YPCALL) != 0)
        return ci;
    }
    return null; /* no pending pcall */
  }

  /*
  ** Signal an error in the call to 'lua_resume', not in the execution
  ** of the coroutine itself. (Such errors should not be handled by any
  ** coroutine error handler and should not kill the coroutine.)
  */
  private static int resume_error(lua_State L, ByteView msg, int narg)
  {
    api_checkpop(L, narg);
    L.top -= narg;                           /* remove args from the stack */
    luaD_checkstack(L, 1); /* (a frozen thread has no room in its frame) */
    setslots(L, L.top, TString.luaS_createstr(L, msg)); /* push error message */
    L.top++;
    lua_unlock(L);
    return LUA_ERRRUN;
  }

  /*
  ** Do the work for 'lua_resume' in protected mode. Most of the work
  ** depends on the status of the coroutine: initial state, suspended
  ** inside a hook, or regularly suspended (optionally with a continuation
  ** function), plus erroneous cases: non-suspended coroutine or dead
  ** coroutine.
  */
  private static void resume(lua_State L, int n)
  {
    int firstArg = L.top - n; /* first argument */
    CallInfo ci = L.ci;
    if (L.status == LUA_OK)                  /* starting a coroutine? */
      ccall(L, firstArg - 1, LUA_MULTRET, 0); /* just call its body */
    else
    { /* resuming from previous yield */
      assert L.status == LUA_YIELD;
      L.status = LUA_OK; /* mark that it is running (again) */
      if (isLua(ci) != 0)
      { /* yielded inside a hook? */
        /* undo increment made by 'luaG_traceexec': instruction was not
           executed yet */
        assert (ci.callstatus & CIST_HOOKYIELD) != 0;
        ci.savedpc--;
        L.top = firstArg; /* discard arguments */
        luaV_execute(L, ci); /* just continue running Lua code */
      }
      else
      { /* 'common' yield */
        if (ci.k != null)
        { /* does it have a continuation function? */
          lua_unlock(L);
          n = ci.k.call(L, LUA_YIELD, ci.ctx); /* call continuation */
          lua_lock(L);
          api_checknelems(L, n);
        }
        luaD_poscall(L, ci, n); /* finish 'luaD_call' */
      }
      unroll(L); /* run continuation */
    }
  }

  private static final class Unroll implements ProtectedCall
  {
    @Override
    public void run(lua_State L)
    {
      unroll(L);
    }
  }

  private static final class Resume implements ProtectedCall
  {
    int nargs;

    @Override
    public void run(lua_State L)
    {
      resume(L, nargs);
    }
  }

  /*
  ** Unrolls a coroutine in protected mode while there are recoverable
  ** errors, that is, errors inside a protected call. (Any error
  ** interrupts 'unroll', and this loop protects it again so it can
  ** continue.) Stops with a normal end (status == LUA_OK), an yield
  ** (status == LUA_YIELD), or an unprotected error ('findpcall' doesn't
  ** find a recover point).
  */
  private static byte precover(lua_State L, byte status)
  {
    CallInfo ci;
    while (errorstatus(status) != 0 && (ci = findpcall(L)) != null)
    {
      L.ci = ci;               /* go down to recovery functions */
      if ((ci.callstatus & CIST_NYCALL) != 0) /* a protected call that cannot yield? */
        L.nCcalls = framecalls(L, ci); /* (it recovers in C in stock Lua) */
      else
        forgetcalls(L, ci); /* (as the resume does in stock Lua) */
      setcistrecst(ci, status); /* status to finish 'pcall' */
      Unroll u = new Unroll();
      status = luaD_rawrunprotected(L, u);
    }
    return status;
  }

  /*
  ** End 'lua_resumek' or 'lua_thawk' of coroutine 'L' (status 'status'). A
  ** freeze of 'L' ends here, with 'L' as if it returned nothing; a freeze of
  ** a coroutine below goes on in 'from', which resumed 'L' with continuation
  ** 'k' (see 'thaw').
  */
  private static lua_ResumeResult endresume(lua_State L, lua_State from, byte status, lua_KFunction k)
  {
    L.resumer = null;
    if (status != LUA_FREEZE)
      luaE_leavebudget(L);
    if (status == LUA_FREEZE && L.l_G.freezekill)
    { /* the chain is destroyed: 'L' dies */
      if (!luaD_killed(L))
        luaD_throw(from, LUA_FREEZE); /* ('from' dies on its way) */
      int nres = (int) (L.top - (L.ci.func + 1));
      lua_unlock(L);
      return new lua_ResumeResult(LUA_ERRRUN, nres);
    }
    if (status == LUA_FREEZE)
    {
      global_State g = L.l_G;
      luaE_setfrozen(L, FZ_RESUME);
      if (g.freezetarget == L)
      { /* the freeze of 'L' ends here */
        g.freezetarget = null;
        lua_unlock(L);
        return new lua_ResumeResult(LUA_OK, 0);
      }
      assert from != null && k != null;
      from.frozenchild = L;
      L.frozenparent = from;
      from.frozencc = from.nCcalls; /* (see 'forgetcalls') */
      luaD_throw(from, LUA_FREEZE); /* go on freezing 'from' (still locked, as for any throw) */
    }
    if (errorstatus(status) == 0)
      assert status == L.status; /* normal end or yield */
    else
    {                                        /* unrecoverable error */
      L.status = status;                    /* mark thread as 'dead' */
      luaD_seterrorobj(L, status, L.top); /* push error message */
      L.ci.top = L.top;
    }
    int nresults = (status == LUA_YIELD) ? L.ci.nyield
                                         : (int) (L.top - (L.ci.func + 1));
    lua_unlock(L);
    return new lua_ResumeResult(status, nresults);
  }

  /*
  ** Go on with frozen thread 'L' (in protected mode). If it was frozen while
  ** resuming coroutine 'co', resume 'co' first, then the C function that
  ** resumed it goes on through its continuation, with the result of 'co'.
  */
  private static void thaw(lua_State L)
  {
    if (L.frozenchild != null)
    {
      lua_State co = L.frozenchild;
      CallInfo ci = L.ci; /* the C function resuming 'co' */
      L.frozenchild = null;
      co.frozenparent = null;
      lua_unlock(L);
      lua_ResumeResult rr = thawcore(co, L, ci.ctx, ci.k);
      int n = ci.k.call(L, rr.status, ci.ctx);
      lua_lock(L);
      api_checknelems(L, n);
      luaD_poscall(L, ci, n);
    }
    unroll(L); /* go on with the rest */
  }

  private static final class Thaw implements ProtectedCall
  {
    @Override
    public void run(lua_State L)
    {
      thaw(L);
    }
  }

  /*
  ** The close of the variables of thread 'L' for 'from' (see
  ** 'lua_closethreadk'). It starts (first), or goes on after a thaw: the
  ** hook function of the '__close' that froze ends first. A '__close' that
  ** freezes makes 'L' frozen in the close, like a coroutine resumed by
  ** 'from' that froze; the close ends with the status and error it has.
  */
  private static final class CloseRun implements ProtectedCall
  {
    byte status;

    @Override
    public void run(lua_State L)
    {
      status = luaD_closeprotected(L, 1, status, 2);
    }
  }

  private static lua_ResumeResult closerun(lua_State L, lua_State from, byte status, boolean first)
  {
    global_State g = L.l_G;
    CloseRun cr = new CloseRun();
    cr.status = status;
    byte st = LUA_OK;
    if (!first)
    { /* the '__close' that froze goes on */
      Thaw t = new Thaw();
      st = luaD_rawrunprotected(L, t);
      if (st != LUA_OK && st != LUA_FREEZE)
      { /* it failed: the close goes on with that error */
        L.ci = L.base_ci;
        L.allowhook = 1;
        cr.status = st;
        st = LUA_OK;
      }
      else if (st == LUA_OK)
        cr.status = L.closestatus;
    }
    if (st == LUA_OK)
    {
      st = luaD_rawrunprotected(L, cr);
      if (st == LUA_OK)
        status = cr.status;
    }
    if (st == LUA_FREEZE)
    {
      if (g.freezekill)
      { /* the chain is destroyed: 'L' dies with it */
        L.resumer = null;
        if (!luaD_killed(L))
          luaD_throw(from, LUA_FREEZE);
        lua_unlock(L);
        return new lua_ResumeResult(LUA_ERRRUN, 1);
      }
      luaE_setfrozen(L, FZ_CLOSE);
      if (g.freezetarget == L)
      { /* the freeze of 'L' ends here: the close is where it lands, as a
           resume is ('from' has no budget: the kernel closed 'L') */
        g.freezetarget = null;
        L.resumer = null;
        lua_unlock(L);
        return new lua_ResumeResult(LUA_OK, 0);
      }
      from.frozenchild = L;
      L.frozenparent = from;
      from.frozencc = from.nCcalls;
      luaD_throw(from, LUA_FREEZE); /* go on freezing 'from' */
    }
    L.resumer = null;
    luaE_leavebudget(L);
    luaE_endclose(L, status);
    lua_unlock(L);
    return new lua_ResumeResult(status, (status != LUA_OK) ? 1 : 0);
  }

  /*
  ** A coroutine that closes its own variables ('lua_closeself') froze in a
  ** '__close': after a thaw its handler ends (or fails), and the close goes
  ** on with the variables left. Returns how the coroutine ends (the status
  ** it would have), or LUA_FREEZE if it froze again.
  */
  private static byte selfcloseend(lua_State L, byte status)
  {
    if (status == LUA_FREEZE)
      return status; /* (frozen again, in the handler) */
    byte cs = (status == LUA_OK) ? (byte) L.closestatus : status;
    if (status != LUA_OK)
    { /* the handler failed: the close goes on with its error */
      L.ci = L.base_ci;
      L.allowhook = 1;
    }
    CloseRun cr = new CloseRun();
    cr.status = cs;
    byte st = luaD_rawrunprotected(L, cr);
    if (st == LUA_FREEZE)
      return st;
    L.closeself = false;
    luaE_endclose(L, cr.status);
    return cr.status;
  }

  /*
  ** A host call failed ('lua_pcall' at base level, also after a thaw): the
  ** variables of its function close here, where a '__close' can freeze (it
  ** runs at the thread's base: a thaw finishes it, see FZ_HOSTCLOSE).
  ** Returns the status the close ends with, or LUA_FREEZE when a freeze
  ** passes (it lands at the host call), or the chain is destroyed.
  */
  private static final class HostClose implements ProtectedCall
  {
    int level;
    byte status;

    @Override
    public void run(lua_State L)
    {
      status = luaD_closeprotected(L, level, status, 2);
    }
  }

  public static byte luaD_hostclose(lua_State L, int level, byte status)
  {
    HostClose hc = new HostClose();
    hc.level = level;
    hc.status = status;
    byte st = luaD_rawrunprotected(L, hc);
    if (st == LUA_OK)
      return hc.status;
    assert st == LUA_FREEZE;
    return st;
  }

  /*
  ** The close of a failed host call, frozen in a '__close' (FZ_HOSTCLOSE),
  ** goes on: that '__close' finishes, the others run, and the host call
  ** ends with its error, as 'lua_pcall' gives it (at 'frozenfunc'); or it
  ** freezes again.
  */
  private static lua_ResumeResult hostcloserun(lua_State L, lua_State from, int hosterrfunc,
                                               int hostnCcalls)
  {
    global_State g = L.l_G;
    Thaw t = new Thaw();
    byte st = luaD_rawrunprotected(L, t);
    byte status = (byte) L.closestatus;
    if (st != LUA_OK && st != LUA_FREEZE)
    { /* it failed: the close goes on with that error */
      L.ci = L.base_ci;
      L.allowhook = 1;
      status = st;
      st = LUA_OK;
    }
    if (st == LUA_OK)
    {
      st = luaD_hostclose(L, L.frozenfunc, status);
      if (st != LUA_FREEZE)
      {
        status = st;
        st = LUA_OK;
      }
    }
    L.resumer = null;
    L.errfunc = hosterrfunc;
    L.nCcalls = hostnCcalls;
    if (st == LUA_FREEZE)
    {
      if (g.freezekill)
      { /* the chain is destroyed: 'L' dies */
        if (!luaD_killed(L))
          luaD_throw(from, LUA_FREEZE);
        lua_unlock(L);
        return new lua_ResumeResult(LUA_ERRRUN, 1);
      }
      luaE_setfrozen(L, FZ_HOSTCLOSE); /* (frozen again in the close: it lands here) */
      assert g.freezetarget == L;
      g.freezetarget = null;
      lua_unlock(L);
      return new lua_ResumeResult(LUA_OK, 0);
    }
    luaE_leavebudget(L);
    L.ci = L.base_ci;
    L.allowhook = 1;
    luaD_seterrorobj(L, status, L.frozenfunc);
    luaD_shrinkstack(L);
    lua_unlock(L);
    return new lua_ResumeResult(status, 1);
  }

  /*
  ** End a host call that a thaw went on with: it finished (results from
  ** 'frozenfunc' on), failed (the error at 'frozenfunc', as 'luaD_pcall'
  ** leaves it), or froze again.
  */
  private static lua_ResumeResult endhostcall(lua_State L, lua_State from, byte status, lua_KFunction k,
                                              byte kind, int hosterrfunc, int hostnCcalls)
  {
    L.resumer = null;
    if (status != LUA_FREEZE)
      luaE_leavebudget(L);
    int atfreeze = L.errfunc; /* (the handler in effect where it froze) */
    L.errfunc = hosterrfunc;
    L.nCcalls = hostnCcalls;
    if (status == LUA_FREEZE && L.l_G.freezekill)
    { /* the chain is destroyed: 'L' dies */
      if (!luaD_killed(L))
        luaD_throw(from, LUA_FREEZE);
      lua_unlock(L);
      return new lua_ResumeResult(LUA_ERRRUN, 1);
    }
    if (status == LUA_FREEZE)
    {
      global_State g = L.l_G;
      luaE_setfrozen(L, kind);
      L.frozenerrfunc = atfreeze;
      if (g.freezetarget == L)
      {
        g.freezetarget = null;
        lua_unlock(L);
        return new lua_ResumeResult(LUA_OK, 0);
      }
      assert from != null && k != null;
      from.frozenchild = L;
      L.frozenparent = from;
      from.frozencc = from.nCcalls;
      luaD_throw(from, LUA_FREEZE);
    }
    int nres;
    if (errorstatus(status) == 0)
      nres = (int) (L.top - L.frozenfunc); /* the results */
    else
    { /* an error that nothing inside recovered: the call's variables close
         (a '__close' can freeze, and lands here) */
      L.ci = L.base_ci;
      L.allowhook = 1;
      if (kind == FZ_PCALL)
      {
        luaE_enterbudget(L, null); /* (as the host call runs) */
        byte cs = luaD_hostclose(L, L.frozenfunc, status);
        luaE_leavebudget(L);
        if (cs == LUA_FREEZE)
        {
          global_State g = L.l_G;
          if (g.freezekill)
          {
            boolean landed = luaD_killed(L);
            assert landed;
            lua_unlock(L);
            return new lua_ResumeResult(LUA_ERRRUN, 1);
          }
          luaE_setfrozen(L, FZ_HOSTCLOSE);
          L.frozenerrfunc = atfreeze;
          assert g.freezetarget == L;
          g.freezetarget = null;
          lua_unlock(L);
          return new lua_ResumeResult(LUA_OK, 0);
        }
        status = cs;
      }
      else
        status = luaD_closeprotected(L, L.frozenfunc, status);
      luaD_seterrorobj(L, status, L.frozenfunc);
      luaD_shrinkstack(L);
      nres = 1;
    }
    lua_unlock(L);
    return new lua_ResumeResult(status, nres);
  }

  /*
  ** Close the variables of thread 'L' for the C function of thread 'from'
  ** that calls with continuation 'k': a '__close' can freeze, the freeze
  ** passes through 'from', and when it thaws, 'k' gets the status of the
  ** close (as 'lua_closethread' gives it, with the error at the top of 'L').
  ** Without a continuation, or for a thread that closes itself, it is
  ** 'lua_closethread'.
  */
  public static lua_ResumeResult lua_closethreadk(lua_State L, lua_State from, long ctx, lua_KFunction k)
  {
    if (L == from || L.frozen != 0 || (from != null && k == null))
    {
      int s = luaE_closethreadplain(L, from);
      return new lua_ResumeResult(s, (s != LUA_OK) ? 1 : 0);
    }
    lua_lock(L);
    L.nCcalls = (from != null) ? getCcalls(from) : 0;
    L.nfz = 0;
    L.resumer = from;
    L.resumedk = (byte) ((from != null) ? 1 : 0);
    if (from != null)
    { /* where a freeze passing through goes on (the host's close, with no
         'from', is where a freeze of 'L' lands) */
      from.ci.k = k;
      from.ci.ctx = ctx;
    }
    luaE_enterbudget(L, from);
    byte status = luaE_beginclose(L);
    return closerun(L, from, status, true);
  }

  /*
  ** A coroutine closes its own variables and ends ('coroutine.close' of the
  ** running coroutine): its frames are gone, the '__close' of all its
  ** variables run, and it ends with the status of the close, as by
  ** 'luaD_throwbaselevel'. A '__close' can freeze: the coroutine is frozen as
  ** by any freeze, and a thaw goes on with the close ('selfcloseend').
  */
  public static RuntimeException lua_closeself(lua_State L)
  {
    lua_lock(L);
    global_State g = L.l_G;
    CloseRun cr = new CloseRun();
    cr.status = luaE_beginclose(L);
    L.closeself = true;
    byte st = luaD_rawrunprotected(L, cr);
    if (st == LUA_FREEZE)
      luaD_throw(L, LUA_FREEZE); /* (a kill is the same: the chain goes on dying) */
    L.closeself = false;
    luaE_endclose(L, cr.status);
    throw luaD_throwbaselevel(L, cr.status);
  }

  public static lua_ResumeResult lua_resume(lua_State L, lua_State from, int nargs)
  {
    return lua_resumek(L, from, nargs, 0, null);
  }

  /*
  ** 'lua_resume' by a C function in 'from' with continuation 'k': a freeze
  ** of a coroutine below 'from' passes through, and when it thaws, 'k' gets
  ** the result of 'L' (see 'lua_resumenres').
  */
  public static lua_ResumeResult lua_resumek(lua_State L, lua_State from, int nargs, long ctx, lua_KFunction k)
  {
    byte status;
    lua_lock(L);
    if (from == null)
      k = null; /* (a continuation goes on in the caller, 'from': none, no continuation) */
    if (L.status == LUA_OK)
    {                           /* may be starting a coroutine */
      if (L.ci != L.base_ci) /* not in base level? */
        return new lua_ResumeResult(resume_error(L, Lt.cannot_resume_non_suspended_coroutine, nargs), 1);
      else if ((int) (L.top - (L.ci.func + 1)) == nargs) /* no function? */
        return new lua_ResumeResult(resume_error(L, Lt.cannot_resume_dead_coroutine, nargs), 1);
    }
    else if (L.status != LUA_YIELD) /* ended with errors? */
      return new lua_ResumeResult(resume_error(L, Lt.cannot_resume_dead_coroutine, nargs), 1);
    L.nCcalls = (from != null) ? getCcalls(from) : 0;
    if (getCcalls(L) >= LUAI_MAXCCALLS)
      return new lua_ResumeResult(resume_error(L, Lt.C_stack_overflow, nargs), 1);
    L.nCcalls++;
    L.ccbase = L.nCcalls;
    if (L.status == LUA_YIELD)
      forgetcalls(L, L.ci); /* (its frames run with no C calls under them) */
    L.nfz = 0;
    L.resumer = from;
    L.resumedk = (byte) ((k != null) ? 1 : 0);
    if (k != null)
    { /* where a freeze passing through goes on */
      from.ci.k = k;
      from.ci.ctx = ctx;
    }
    luaE_enterbudget(L, from);
    luaE_spend(L, 1); /* a hand-off */
    api_checkpop(L, (L.status == LUA_OK) ? nargs + 1 : nargs);
    Resume r = new Resume();
    r.nargs = nargs;
    status = luaD_rawrunprotected(L, r);
    /* continue running after recoverable errors */
    status = precover(L, status);
    return endresume(L, from, status, k);
  }

  private static lua_ResumeResult thawcore(lua_State L, lua_State from, long ctx, lua_KFunction k)
  {
    byte status;
    lua_lock(L);
    byte kind = L.frozen;
    if (kind == 0)
      return new lua_ResumeResult(resume_error(L, Lt.cannot_thaw_a_coroutine_that_is_not_frozen, 0), 1);
    int hostnCcalls = L.nCcalls;
    if (from != null && getCcalls(from) >= LUAI_MAXCCALLS)
      return new lua_ResumeResult(resume_error(L, Lt.C_stack_overflow, 0), 1);
    L.frozen = 0;
    L.top = L.frozentop; /* (what a refused resume or close left on it is not its) */
    global_State g = L.l_G;
    L.nCcalls = L.frozencc; /* its calls, as when it froze */
    L.nfz = 0;
    L.resumer = from;
    L.resumedk = (byte) ((k != null) ? 1 : 0);
    if (k != null)
    { /* where a freeze passing through goes on */
      from.ci.k = k;
      from.ci.ctx = ctx;
    }
    luaE_enterbudget(L, from);
    if (kind == FZ_CLOSE)
    {
      if (g.budgetL == null && L.beff != null)
      { /* (a thaw starts with a whole budget, and no '__gc' at its first point) */
        L.beff.left = L.beff.budget;
        L.beff.nodrain = true;
      }
      return closerun(L, from, L.closestatus, false);
    }
    int hosterrfunc = L.errfunc;
    if (kind == FZ_HOSTCLOSE)
    {
      L.errfunc = L.frozenerrfunc;
      if (g.budgetL == null && L.beff != null)
      { /* (a thaw starts with a whole budget, and no '__gc' at its first point) */
        L.beff.left = L.beff.budget;
        L.beff.nodrain = true;
      }
      return hostcloserun(L, from, hosterrfunc, hostnCcalls);
    }
    if (kind == FZ_PCALL)
      L.errfunc = L.frozenerrfunc;
    if (g.budgetL == null && L.beff != null) /* a thaw starts with a whole budget, so that */
    {                                                /* it makes progress before it can freeze, */
      L.beff.left = L.beff.budget;
      L.beff.nodrain = true;                       /* and it starts no '__gc' */
    }
    Thaw t = new Thaw();
    status = luaD_rawrunprotected(L, t);
    status = precover(L, status);
    if (L.closeself)
      status = selfcloseend(L, status);
    if (kind == FZ_RESUME)
      return endresume(L, from, status, k);
    return endhostcall(L, from, status, k, kind, hosterrfunc, hostnCcalls);
  }

  /*
  ** Go on with frozen coroutine 'L' (see 'lua_resumek' for 'k'). Only
  ** unbudgeted code can thaw: not the budget callback, nor a thread with a
  ** budget.
  */
  public static lua_ResumeResult lua_thawk(lua_State L, lua_State from, long ctx, lua_KFunction k)
  {
    lua_lock(L);
    if (from == null)
      k = null; /* (as in 'lua_resumek') */
    if (L.l_G.budgetL != null || (from != null && from.beff != null))
      return new lua_ResumeResult(resume_error(L, Lt.cannot_thaw_from_budgeted_code, 0), 1);
    if (L.frozenparent != null) /* only the freeze target thaws its chain */
      return new lua_ResumeResult(resume_error(L, Lt.cannot_thaw_a_coroutine_that_is_not_the_freeze_target, 0), 1);
    lua_unlock(L);
    return thawcore(L, from, ctx, k);
  }

  public static lua_ResumeResult lua_thaw(lua_State L, lua_State from)
  {
    return lua_thawk(L, from, 0, null);
  }

  /* number of results of coroutine 'L' after a resume or thaw ended with 'status' */
  public static int lua_resumenres(lua_State L, int status)
  {
    if (L.frozen != 0)
      return 0;
    else if (status == LUA_YIELD)
      return L.ci.nyield;
    else
      return (int) (L.top - (L.ci.func + 1));
  }

  public static lua_State lua_frozenchild(lua_State L)
  {
    return L.frozenchild;
  }

  public static int lua_isfrozen(lua_State L)
  {
    return (L.frozen != 0) ? 1 : 0;
  }

  /*
  ** Freeze, from the budget callback: leave it (see 'luaD_budget').
  */
  public static RuntimeException lua_freeze(lua_State L)
  {
    lua_lock(L); /* (as for any throw: it is caught with the lock held) */
    throw luaD_exitcallback(L);
  }

  /*
  ** Would a freeze here freeze the chain of the running thread 'L'? In the
  ** budget callback, whether 'lua_freeze' will (instead of destroying the
  ** chain); elsewhere, whether the thread runs under a budget and no call
  ** that cannot freeze lies in the way. 0 if it would, else which of the
  ** things in the way it is (LUA_NOFREEZE_*).
  */
  public static int lua_nofreeze(lua_State L)
  {
    global_State g = L.l_G;
    if (g.budgetL != null)
    {
      if (!incallbackchain(g, L))
        return LUA_NOFREEZE_BUDGET; /* (nothing freezes from there) */
      return chaintop(g.budgetL).why;
    }
    if (L.beff == null)
      return LUA_NOFREEZE_BUDGET;
    return luaD_nofreeze(L);
  }

  /*
  ** Call the Lua function of a hook (below its arguments), as a hook
  ** function that can freeze: it cannot yield, and when it ends after a
  ** thaw the hook is finished by 'luaD_finishhook'.
  */
  public static void lua_callhook(lua_State L, int nargs)
  {
    lua_lock(L);
    assert (L.ci.callstatus & CIST_HOOKED) != 0;
    luaE_spend(L, 1); /* a hand-off */
    luaD_callnoyield(L, L.top - (nargs + 1), 0);
    lua_unlock(L);
  }

  public static int lua_isyieldable(lua_State L)
  {
    return yieldable(L);
  }


  public static int lua_yieldk(lua_State L, int nresults, long ctx,
                               lua_KFunction k)
  {
    CallInfo ci;
    lua_lock(L);
    ci = L.ci;
    api_checkpop(L, nresults);
    if (yieldable(L) == 0)
    {
      if (L != mainthread(L.l_G))
        luaG_runerror(L, Lt.attempt_to_yield_across_a_C_call_boundary);
      else
        luaG_runerror(L, Lt.attempt_to_yield_from_outside_a_coroutine);
    }
    L.status = LUA_YIELD;
    ci.nyield = nresults; /* save number of results */
    if (isLua(ci) != 0)
    { /* inside a hook? */
      assert isLuacode(ci) == 0;
      assert nresults == 0 : "hooks cannot yield values";
      assert k == null : "hooks cannot continue after yielding";
    }
    else
    {
      if ((ci.k = k) != null) /* is there a continuation? */
        ci.ctx = ctx;         /* save context */
      luaD_throw(L, (byte) LUA_YIELD);
    }
    assert (ci.callstatus & CIST_HOOKED) != 0; /* must be inside a hook */
    lua_unlock(L);
    return 0; /* return to 'luaD_hook' */
  }

  /*
  ** Auxiliary structure to call 'luaF_close' in protected mode.
  */
  private static final class CloseP implements ProtectedCall
  {
    int level;
    byte status;
    int yy; /* can a '__close' freeze? */

    @Override
    public void run(lua_State L)
    {
      closepaux(L, this);
    }
  }

  /*
  ** Auxiliary function to call 'luaF_close' in protected mode.
  */
  private static void closepaux(lua_State L, CloseP pcl)
  {
    luaF_close(L, pcl.level, pcl.status, pcl.yy);
  }

  /*
  ** Calls 'luaF_close' in protected mode. Return the original status
  ** or, in case of errors, the new status. With 'yy', the '__close' can
  ** freeze, when the caller made its frame one that recovers (as the one of
  ** a protected call with a continuation: see 'lua_pcallk').
  */
  public static byte luaD_closeprotected(lua_State L, int level, byte status, int yy)
  {
    CallInfo old_ci = L.ci;
    byte old_allowhooks = L.allowhook;
    for (;;)
    { /* keep closing upvalues until no more errors */
      CloseP pcl = new CloseP();
      pcl.level = level;
      pcl.status = status;
      pcl.yy = yy;
      L.closestatus = status; /* (if a '__close' freezes, this is how it goes on) */
      status = luaD_rawrunprotected(L, pcl);
      if (status == LUA_OK) /* no more errors? */
        return pcl.status;
      else if (status == LUA_FREEZE)
      { /* a '__close' spent the budget */
        global_State g = L.l_G;
        if (!g.freezekill)
          luaD_throw(L, LUA_FREEZE); /* a freeze: the frame recovers (see 'yy') */
        /* the chain is destroyed (a call that cannot freeze is in the way):
           the unwinding goes on to where the chain's top thread was resumed
           or called; for a close that nothing resumed or called, here, it fails */
        if (g.freezetarget != L || L.errorJmp != null)
        {
          if (L.errorJmp != null)
            luaD_throw(L, LUA_FREEZE);
          if (L.resumer != null)
            luaD_throw(L.resumer, LUA_FREEZE);
        }
        g.freezekill = false;
        g.freezetarget = null;
        L.ci = old_ci;
        L.allowhook = old_allowhooks;
        setslot(L, L.top, g.killerr);
        L.top++;
        setnilvalue(g.killerr);
        return LUA_ERRRUN;
      }
      else
      { /* an error occurred; restore saved state and repeat */
        L.ci = old_ci;
        L.allowhook = old_allowhooks;
      }
    }
  }

  public static byte luaD_closeprotected(lua_State L, int level, byte status)
  {
    return luaD_closeprotected(L, level, status, 0);
  }

  /*
  ** Call the C function 'func' in protected mode, restoring basic
  ** thread information ('allowhook', etc.) and in particular
  ** its stack level in case of errors.
  */
  public static byte luaD_pcall(lua_State L, ProtectedCall func, int old_top,
                                int ef, boolean close)
  {
    byte status;
    CallInfo old_ci = L.ci;
    byte old_allowhooks = L.allowhook;
    int old_errfunc = L.errfunc;
    L.errfunc = ef;
    status = luaD_rawrunprotected(L, func);
    if (status == LUA_FREEZE)
      return status; /* nothing changes; the caller finishes it */
    if (status != LUA_OK)
    { /* an error occurred? */
      L.ci = old_ci;
      L.allowhook = old_allowhooks;
      if (!close)
        return status; /* the caller closes the variables (and restores 'errfunc') */
      status = luaD_closeprotected(L, old_top, status);
      luaD_seterrorobj(L, status, old_top);
      luaD_shrinkstack(L); /* restore stack size in case of overflow */
    }
    L.errfunc = old_errfunc;
    return status;
  }

  public static byte luaD_pcall(lua_State L, ProtectedCall func, int old_top, int ef)
  {
    return luaD_pcall(L, func, old_top, ef, true);
  }

  /*
  ** Execute a protected parser.
  */
  private static final class SParser implements ProtectedCall
  { /* data to 'f_parser' */
    ZIO z;
    MBuffer buff = new MBuffer();
    Dyndata dyd = new Dyndata();  /* dynamic structures used by the parser */
    ByteView mode;
    ByteView name;

    @Override
    public void run(lua_State L)
    {
      f_parser(L, this);
    }
  }

  private static void checkmode(lua_State L, ByteView mode, ByteView x)
  {
    if (mode.find(x.at(0)) == ByteView.npos)
    {
      luaO_pushfstring(L, Lt.attempt_to_load_a_s_chunk_mode_is_s,
                       new FmtArgs().str(x).str(mode));
      luaD_throw(L, (byte) LUA_ERRSYNTAX);
    }
  }

  /* the frame of 'f_parser' (see lload.h) */
  private static final class FParserFrame extends LoadFrame
  {
    int otop = 0; /* original top */
    Table anchor = null;
    int c = 0;
    int fixed = 0;

    FParserFrame()
    {
    }

    FParserFrame(FParserFrame o)
    {
      super(o);
      otop = o.otop;
      anchor = o.anchor;
      c = o.c;
      fixed = o.fixed;
    }

    @Override
    public LoadFrame copy()
    {
      return new FParserFrame(this);
    }

    @Override
    public int size()
    {
      return 48;
    }
  }

  /*
  ** Before the first call to the reader function, Lua reserves a slot
  ** with a table for anchoring stuff. (A load that a freeze stopped keeps
  ** its frame, see lload.h.)
  */
  private static void f_parser(lua_State L, SParser p)
  {
    FParserFrame l = new FParserFrame();
    LoadResume rz = p.z.rz;
    LClosure cl = null;
    ByteView mode = p.mode;
    if (p.z.resuming)
    {
      luaZ_unhold(rz, L.ci); /* (the stopped load's arrays and buffer) */
      l = new FParserFrame((FParserFrame) luaZ_restore(rz, LF_FPARSER));
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.otop = L.top;
          luaD_checkstack(L, 2);
          l.anchor = Table.luaH_new(L);       /* create the anchor table */
          luaC_givebackhash(L.l_G, l.anchor); /* (the parser's own) */
          setsloth(L, L.top++, l.anchor);     /* anchor the anchor table */
          l.site = 1;
        case 1:
          l.c = (p.z.pos < p.z.chunk.size())
                    ? p.z.chunk.uat(p.z.pos++)
                    : luaZ_fill(p.z); /* read first character */
          if (l.c == LUA_SIGNATURE.at(0))
          {
            l.fixed = 0;
            if (mode.find('B') != ByteView.npos)
              l.fixed = 1;
            else
              checkmode(L, mode, Lt.binary);
            l.site = 2;
          }
          else
          {
            checkmode(L, mode, Lt.text);
            l.site = 3;
          }
          continue;
        case 2:
          cl = luaU_undump(L, p.z, l.anchor, p.buff, p.name, l.fixed);
          break;
        case 3:
          cl = luaY_parser(L, p.z, l.anchor, p.buff, p.dyd, p.name, l.c);
          break;
        default:
          luaZ_badstate(rz);
        }
        break; /* (the function is made) */
      }
    }
    catch (Throwable e)
    {
      luaZ_record(rz, LF_FPARSER, l);
      throw e;
    }
    L.top = l.otop; /* restore stack */
    setslotclL(L, L.top++, cl); /* push closure */
    assert cl.upvals.length == cl.p.upvalues.length;
    cl.luaF_initupvals(L);
  }

  /*
  ** Anchor an object in a table in the stack.  First, anchor the object
  ** temporarily in the stack, as luaH_set may call an emergency GC.
  ** Then, add it in the table with itself as its key.
  */
  public static void luaD_anchorobj(lua_State L, Table anchor, GCObject obj)
  {
    TValue v = new TValue();
    setgcovalue(L, v, obj);
    setslot(L, L.top, v); /* temporary anchor in the stack */
    L.top++;
    anchor.luaH_set(L, v, v);
    /* Because this is a new key, luaH_set will call the GC barrier, so
       we don't need to call the barrier again here */
    L.top--;
  }

  public static byte luaD_protectedparser(lua_State L, ZIO z, ByteView name, ByteView mode,
                                          LoadResume rz)
  {
    SParser p = new SParser();
    try
    {
      byte status;
      incnny(L); /* cannot yield during parsing */
      CallInfo oldci = L.ci;
      int oldtop = L.top;
      p.z = z;
      p.buff.attach(L);
      p.name = (name != null) ? name : Lt.qmark;
      p.mode = (mode != null) ? mode : Lt.bt;
      p.dyd.actvar.arr = new Vardesc[0];
      p.dyd.actvar.size = 0;
      p.dyd.gt.arr = new Labeldesc[0];
      p.dyd.gt.size = 0;
      p.dyd.label.arr = new Labeldesc[0];
      p.dyd.label.size = 0;
      if (rz != null)
      {
        rz.dyd = p.dyd;
        rz.buff = p.buff;
      }
      status = luaD_pcall(L, p, L.top, L.errfunc);
      if (status == LUA_FREEZE && rz != null && rz.box != null && luaZ_recorded(rz))
      { /* a freeze stopped the load: its box keeps it, to go on after the thaw */
        luaZ_hold(rz, oldci, (int) (oldtop - oldci.func));
        decnny(L);
        luaD_throw(L, LUA_FREEZE); /* (the chain is destroyed, or goes on freezing) */
      }
      luaM_alloc_Vardesc_span(L.l_G, L, p.dyd.actvar.arr, 0, false);
      luaM_alloc_Labeldesc_span(L.l_G, L, p.dyd.gt.arr, 0, false);
      luaM_alloc_Labeldesc_span(L.l_G, L, p.dyd.label.arr, 0, false);
      p.buff.growTo(0); /* (luaZ_freebuffer) */
      decnny(L);
      if (status == LUA_FREEZE)
      {
        /* A freeze in the reader function abandons the parse (it starts over
           after a thaw): what it left in the stack, below the frames of the
           reader function, is dead, and a closure or prototype half made must
           not linger there. */
        if (L.ci != oldci && oldci.next != null)
          for (int s = oldtop; s < oldci.next.func; s++)
            setslotnil(L, s);
        luaD_throw(L, LUA_FREEZE); /* (the chain is destroyed, or goes on freezing) */
      }
      return status;
    }
    finally
    {
      p.buff.destroy();
    }
  }

  public static byte luaD_protectedparser(lua_State L, ZIO z, ByteView name, ByteView mode)
  {
    return luaD_protectedparser(L, z, name, mode, null);
  }
}
