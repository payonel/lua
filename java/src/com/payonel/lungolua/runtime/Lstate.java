package com.payonel.lungolua.runtime;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.compiler.Llex.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Lfunc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.object.Lstring.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Ltm.*;
import static com.payonel.lungolua.vm.Ldebug.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.lua_Alloc;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_WarnFunction;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.object.Closure;
import com.payonel.lungolua.object.GCObject;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.object.Udata;

public final class Lstate
{
  private Lstate()
  {
  }

  /*
  ** Moreover, there is another set of lists that control gray objects.
  ** These lists are linked by fields 'gclist'. (All objects that
  ** can become gray have such a field. The field is not the same
  ** in all objects, but it always has this name.)  Any gray object
  ** must belong to one of these lists, and all objects in these lists
  ** must be gray (with two exceptions explained below):
  **
  ** 'gray': regular gray objects, still waiting to be visited.
  ** 'grayagain': objects that must be revisited at the atomic phase.
  **   That includes
  **   - black objects got in a write barrier;
  **   - all kinds of weak tables during propagation phase;
  **   - all threads.
  ** 'weak': tables with weak values to be cleared;
  ** 'ephemeron': ephemeron tables with white->white entries;
  ** 'allweak': tables with weak keys and/or weak values to be cleared.
  **
  ** The exceptions to that "gray rule" are:
  ** - TOUCHED2 objects in generational mode stay in a gray list (because
  ** they must be visited again at the end of the cycle), but they are
  ** marked black because assignments to them must activate barriers (to
  ** move them back to TOUCHED1).
  ** - Open upvalues are kept gray to avoid barriers, but they stay out
  ** of gray lists. (They don't even have a 'gclist' field.)
  */

  /*
  ** About 'nCcalls':  This count has two parts: the lower 16 bits counts
  ** the number of recursive invocations in the C stack; the higher
  ** 16 bits counts the number of non-yieldable calls in the stack.
  ** (They are together so that we can change and save both with one
  ** instruction.)
  */

  /* true if this thread does not have non-yieldable calls in the stack */

  /* real number of C calls */

  /* Increment the number of non-yieldable calls */

  /* Decrement the number of non-yieldable calls */

  /* Non-yieldable call increment */
  public static final int nyci = 0x10000 | 1;

  /*
  ** Extra stack space to handle TM calls and some other extras. This
  ** space is not included in 'stack_last'. It is used only to avoid stack
  ** checks, either because the element will be promptly popped or because
  ** there will be a stack check soon after the push. Function frames
  ** never use this extra space, so it does not need to be kept clean.
  */
  public static final int EXTRA_STACK = 5;

  public static final int BASIC_STACK_SIZE = 2 * LUA_MINSTACK;

  /* kinds of Garbage Collection */
  public static final int KGC_INC = 0; /* incremental gc */
  public static final int KGC_GENMINOR = 1; /* generational gc in minor (regular) mode */
  public static final int KGC_GENMAJOR = 2; /* generational in major mode */

  /* the events a hook runs for ('hookev'): what is left to do when it ends after a thaw */
  public static final byte HE_INSTR = 1;  /* count or line hook before an instruction: it runs again */
  public static final byte HE_CALL = 2;   /* call hook at the start of a Lua function: it starts */
  public static final byte HE_CALLVA = 3; /* call hook of a vararg function, in its VARARGPREP */
  public static final byte HE_CALLC = 4;  /* call hook of a C function: it runs */
  public static final byte HE_RET = 5;    /* return hook: the return goes on */
  public static final byte HE_RETTBC = 6; /* return hook after closing variables */

  /* the hooks called already for an instruction ('hookdone') */
  public static final byte HD_COUNT = 1;
  public static final byte HD_LINE = 2;

  /*
  ** Maximum expected number of results from a function
  ** (must fit in CIST_NRESULTS).
  */
  public static final int MAXRESULTS = 250;

  /*
  ** Bits in CallInfo status
  */
  /* bits 0-7 are the expected number of results from this function + 1 */
  public static final int CIST_NRESULTS = 0xff;

  /* bits 8-11 count call metamethods (and their extra arguments) */
  public static final int CIST_CCMT = 8; /* the offset, not the mask */
  public static final int MAX_CCMT = 0xf << CIST_CCMT;

  /* Bits 12-14 are used for CIST_RECST (see below) */
  public static final int CIST_RECST = 12; /* the offset, not the mask */

  /* call is running a C function (still in first 16 bits) */
  public static final int CIST_C = 1 << (CIST_RECST + 3);
  /* call is on a fresh "luaV_execute" frame */
  public static final int CIST_FRESH = CIST_C << 1;
  /* function is closing tbc variables */
  public static final int CIST_CLSRET = CIST_FRESH << 1;
  /* function has tbc variables to close */
  public static final int CIST_TBC = CIST_CLSRET << 1;
  /* original value of 'allowhook' */
  public static final int CIST_OAH = CIST_TBC << 1;
  /* call is running a debug hook */
  public static final int CIST_HOOKED = CIST_OAH << 1;
  /* doing a yieldable protected call */
  public static final int CIST_YPCALL = CIST_HOOKED << 1;
  /* call was tail called */
  public static final int CIST_TAIL = CIST_YPCALL << 1;
  /* last hook called yielded */
  public static final int CIST_HOOKYIELD = CIST_TAIL << 1;
  /* function "called" a finalizer */
  public static final int CIST_FIN = CIST_HOOKYIELD << 1;
  /* C function is in an API call with a continuation ('lua_getik'...):
     a metamethod the call runs may yield */
  public static final int CIST_APIK = CIST_FIN << 1;
  /* Lua function frozen at a budget point: the instruction before 'savedpc'
     has not run yet (see 'lua_freeze') */
  public static final int CIST_FROZE = CIST_APIK << 1;
  /* C function is in a call that cannot yield ('lua_callk' or 'lua_pcallk'
     in a thread that cannot yield): after a thaw, its end counts down the
     non-yieldable calls */
  public static final int CIST_NYCALL = CIST_FROZE << 1;
  /* C function whose calls with continuations can freeze but not yield, as
     in a thread that cannot yield (a library function that keeps the
     yieldability of stock Lua: see 'lua_freezeonly') */
  public static final int CIST_FZONLY = CIST_NYCALL << 1;
  /* function raised an error and is calling the message handler (a call
     that cannot yield): when the handler ends after a thaw, the error goes
     on (see 'unroll') */
  public static final int CIST_ERRH = CIST_FZONLY << 1;
  /* frame entered through C ('ccall'): 'nccalls' has the count it runs
     with (frames entered from Lua code run with their caller's) */
  public static final int CIST_CCOUNT = CIST_ERRH << 1;

  /* the budget, in 'hookmask' (not a hook: the hook API never shows it) */
  public static final int LUA_MASKBUDGET = 1 << 6;

  /* how a frozen thread was entered (field 'frozen') */
  public static final byte FZ_RESUME = 1; /* by 'lua_resume' or 'lua_thaw' */
  public static final byte FZ_CALL = 2;   /* by a 'lua_call' of the host */
  public static final byte FZ_PCALL = 3;  /* by a 'lua_pcall' of the host */
  public static final byte FZ_CLOSE = 4;  /* by 'lua_closethreadk' (closing its variables) */
  public static final byte FZ_HOSTCLOSE = 5; /* by a 'lua_pcall' of the host that failed (closing its variables) */

  /*
  ** How 'finishCcall' finishes an API call with a continuation whose
  ** metamethod yielded, so that the stack is as the call leaves it.
  */
  public static final byte APIK_PUSH = 1;    /* the result is on the top: done */
  public static final byte APIK_OVERKEY = 2; /* the result replaces the key below it */
  public static final byte APIK_POP1 = 3;    /* pop the value set */
  public static final byte APIK_POP2 = 4;    /* pop the value set and its key */
  public static final byte APIK_BOOL = 5;    /* the result, as a boolean, is on the top */
  public static final byte APIK_ARITH = 6;   /* the result replaces both operands */
  public static final byte APIK_CONCAT = 7;  /* the result replaces two values; go on */
  public static final byte APIK_SETTOP = 8;  /* closing down to 'apifirst'; then it is the top */
  public static final byte APIK_CLOSESLOT = 9; /* closing 'apifirst'; then its slot is nil */

  public static int get_nresults(int cs)
  {
    return (cs & CIST_NRESULTS) - 1;
  }

  /*
  ** Field CIST_RECST stores the "recover status", used to keep the error
  ** status while closing to-be-closed variables in coroutines, so that
  ** Lua can correctly resume after an yield from a __close method called
  ** because of an error.  (Three bits are enough for error status.)
  */
  public static byte getcistrecst(CallInfo ci)
  {
    return (byte) ((ci.callstatus >>> CIST_RECST) & 7);
  }

  public static void setcistrecst(CallInfo ci, int st)
  {
    assert (st & 7) == st; /* status must fit in three bits */
    ci.callstatus = (ci.callstatus & ~(7 << CIST_RECST)) | (st << CIST_RECST);
  }

  /* active function is a Lua function */
  public static int isLua(CallInfo ci)
  {
    return ((ci.callstatus & CIST_C) == 0) ? 1 : 0;
  }

  /* call is running Lua code (not a hook) */
  public static int isLuacode(CallInfo ci)
  {
    return ((ci.callstatus & (CIST_C | CIST_HOOKED)) == 0) ? 1 : 0;
  }

  public static void setoah(CallInfo ci, int v)
  {
    ci.callstatus = (v != 0) ? (ci.callstatus | CIST_OAH) : (ci.callstatus & ~CIST_OAH);
  }

  public static byte getoah(CallInfo ci)
  {
    return (byte) (((ci.callstatus & CIST_OAH) != 0) ? 1 : 0);
  }

  /* 'L' is frozen, entered as 'kind' (FZ_*): its stack is as it is now */
  public static void luaE_setfrozen(lua_State L, byte kind)
  {
    L.frozen = kind;
    L.frozentop = L.top;
  }

  public static void setthvalue(lua_State L, TValue obj, lua_State x)
  {
    assert x.tt == LUA_VTHREAD;
    obj.ref = x;
    obj.tt_ = ctb(LUA_VTHREAD);
    checkliveness(L, obj);
  }

  public static lua_State thvalue(TValue o)
  {
    assert ttisthread(o);
    assert ((GCObject) o.ref).tt == LUA_VTHREAD;
    return (lua_State) o.ref;
  }

  public static byte slottag(lua_State L, int idx)
  {
    return L.stack_tt[idx];
  }

  public static void setslottag(lua_State L, int idx, byte tt)
  {
    L.stack_tt[idx] = tt;
  }

  public static TValue getslot(lua_State L, int idx)
  {
    return new TValue(L.stack_prim[idx], L.stack_ref[idx], L.stack_tt[idx]);
  }

  public static void setslot(lua_State L, int idx, TValue v)
  {
    L.stack_prim[idx] = v.prim;
    L.stack_ref[idx] = v.ref;
    L.stack_tt[idx] = v.tt_;
    checkliveness(L, v);
  }

  public static void copyslot(lua_State L, int dst, int src)
  {
    L.stack_prim[dst] = L.stack_prim[src];
    L.stack_ref[dst] = L.stack_ref[src];
    L.stack_tt[dst] = L.stack_tt[src];
  }

  public static void copyslot(lua_State to, int dst, lua_State from, int src)
  {
    to.stack_prim[dst] = from.stack_prim[src];
    to.stack_ref[dst] = from.stack_ref[src];
    to.stack_tt[dst] = from.stack_tt[src];
  }

  public static void slot2obj(lua_State L, TValue dst, int src)
  {
    dst.prim = L.stack_prim[src];
    dst.ref = L.stack_ref[src];
    dst.tt_ = L.stack_tt[src];
    checkliveness(L, dst);
  }

  public static long slotival(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == LUA_VNUMINT;
    return L.stack_prim[idx];
  }

  public static double slotfltval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == LUA_VNUMFLT;
    return Double.longBitsToDouble(L.stack_prim[idx]);
  }

  public static double slotnval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == LUA_VNUMINT || L.stack_tt[idx] == LUA_VNUMFLT;
    return (L.stack_tt[idx] == LUA_VNUMINT) ? (double) L.stack_prim[idx]
                                             : Double.longBitsToDouble(L.stack_prim[idx]);
  }

  public static Object slotpval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == LUA_VLIGHTUSERDATA;
    return L.stack_ref[idx];
  }

  public static lua_CFunction slotfval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VLCF);
    return ((CFuncInfo) L.stack_ref[idx]).fptr;
  }

  public static GCObject slotgcval(lua_State L, int idx)
  {
    assert (L.stack_tt[idx] & BIT_ISCOLLECTABLE) != 0;
    return (GCObject) L.stack_ref[idx];
  }

  public static TString slottsval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VSHRSTR);
    assert novariant(((GCObject) L.stack_ref[idx]).tt) == LUA_TSTRING;
    return (TString) L.stack_ref[idx];
  }

  public static Table slothval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VTABLE);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VTABLE;
    return (Table) L.stack_ref[idx];
  }

  public static Udata slotuval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VUSERDATA);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VUSERDATA;
    return (Udata) L.stack_ref[idx];
  }

  public static lua_State slotthval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VTHREAD);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VTHREAD;
    return (lua_State) L.stack_ref[idx];
  }

  public static Closure slotclval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VLCL) || L.stack_tt[idx] == ctb(LUA_VCCL);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VLCL || ((GCObject) L.stack_ref[idx]).tt == LUA_VCCL;
    return (Closure) L.stack_ref[idx];
  }

  public static LClosure slotclLval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VLCL);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VLCL;
    return (LClosure) L.stack_ref[idx];
  }

  public static CClosure slotclCval(lua_State L, int idx)
  {
    assert L.stack_tt[idx] == ctb(LUA_VCCL);
    assert ((GCObject) L.stack_ref[idx]).tt == LUA_VCCL;
    return (CClosure) L.stack_ref[idx];
  }

  public static void setslotnil(lua_State L, int idx)
  {
    L.stack_tt[idx] = LUA_VNIL;
  }

  public static void setslotempty(lua_State L, int idx)
  {
    L.stack_tt[idx] = LUA_VEMPTY;
  }

  public static void setslotbt(lua_State L, int idx)
  {
    L.stack_tt[idx] = LUA_VTRUE;
  }

  public static void setslotbf(lua_State L, int idx)
  {
    L.stack_tt[idx] = LUA_VFALSE;
  }

  public static void setsloti(lua_State L, int idx, long x)
  {
    L.stack_prim[idx] = x;
    L.stack_tt[idx] = LUA_VNUMINT;
  }

  public static void setslotflt(lua_State L, int idx, double x)
  {
    L.stack_prim[idx] = Double.doubleToRawLongBits(x);
    L.stack_tt[idx] = LUA_VNUMFLT;
  }

  public static void chgsloti(lua_State L, int idx, long x)
  {
    assert L.stack_tt[idx] == LUA_VNUMINT;
    L.stack_prim[idx] = x;
  }

  public static void chgslotflt(lua_State L, int idx, double x)
  {
    assert L.stack_tt[idx] == LUA_VNUMFLT;
    L.stack_prim[idx] = Double.doubleToRawLongBits(x);
  }

  public static void setslotp(lua_State L, int idx, Object x)
  {
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = LUA_VLIGHTUSERDATA;
  }

  public static void setslotf(lua_State L, int idx, CFuncInfo cfi)
  {
    L.stack_ref[idx] = cfi;
    L.stack_tt[idx] = ctb(LUA_VLCF);
  }

  public static void setslots(lua_State L, int idx, TString x)
  {
    assert novariant(x.tt) == LUA_TSTRING;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    checkliveness(L, getslot(L, idx));
  }

  public static void setsloth(lua_State L, int idx, Table x)
  {
    assert x.tt == LUA_VTABLE;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    checkliveness(L, getslot(L, idx));
  }

  public static void setslotu(lua_State L, int idx, Udata x)
  {
    assert x.tt == LUA_VUSERDATA;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    checkliveness(L, getslot(L, idx));
  }

  public static void setslotclL(lua_State L, int idx, LClosure x)
  {
    assert x.tt == LUA_VLCL;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    checkliveness(L, getslot(L, idx));
  }

  public static void setslotclC(lua_State L, int idx, CClosure x)
  {
    assert x.tt == LUA_VCCL;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    checkliveness(L, getslot(L, idx));
  }

  public static void setslotth(lua_State L, int idx, lua_State x)
  {
    assert x.tt == LUA_VTHREAD;
    L.stack_ref[idx] = x;
    L.stack_tt[idx] = ctb(x.tt);
    TValue v = getslot(L, idx);
    checkliveness(L, v);
  }

  /* }================================================================== */

  /* true if this thread does not have non-yieldable calls in the stack */
  public static int yieldable(lua_State L)
  {
    return ((L.nCcalls & 0xffff0000) == 0) ? 1 : 0;
  }

  /* real number of C calls */
  public static int getCcalls(lua_State L)
  {
    return L.nCcalls & 0xffff;
  }

  public static void incnny(lua_State L)
  {
    L.nCcalls += 0x10000;
  }

  public static void decnny(lua_State L)
  {
    L.nCcalls -= 0x10000;
  }

  /* number of non-yieldable calls */
  public static int getnny(lua_State L)
  {
    return L.nCcalls >>> 16;
  }

  public static int stacksize(lua_State th)
  {
    return th.stack_last;
  }

  /*
  ** 'g->nilvalue' being a nil value flags that the state was completely
  ** build.
  */

  /* actual number of total memory allocated */

  public static lua_State mainthread(global_State g)
  {
    return g.mainth;
  }

  public static int completestate(global_State g)
  {
    return (ttype(g.nilvalue) == LUA_TNIL) ? 1 : 0;
  }

  /*
  ** At a resume of 'L' by 'from' (maybe none): the resumer's budget group is
  ** in effect if it has one, otherwise the thread's own.
  */
  public static void luaE_enterbudget(lua_State L, lua_State from)
  {
    BudgetGroup bg = (from != null && from.beff != null) ? from.beff : L.bown;
    if (L.beff != bg)
      luaE_setbeff(L, bg);
  }

  /* when the resume ends (not in a freeze): back to the thread's own */
  public static void luaE_leavebudget(lua_State L)
  {
    if (L.beff != L.bown)
      luaE_setbeff(L, L.bown);
  }

  /* spend 'n' steps of the budget, if there is one */
  public static void luaE_spend(lua_State L, long n)
  {
    if ((L.hookmask & LUA_MASKBUDGET) != 0)
      L.beff.left -= n;
  }

  /* C work of 'n' steps: true when the budget is spent (see 'lua_spend') */
  public static boolean luaE_spent(lua_State L, long n)
  {
    if ((L.hookmask & LUA_MASKBUDGET) == 0)
      return false;
    BudgetGroup bg = L.beff;
    bg.left -= n;
    return bg.left <= 0;
  }

  /* is the budget spent? */
  public static boolean luaE_budgetout(lua_State L)
  {
    return (L.hookmask & LUA_MASKBUDGET) != 0 && L.beff.left <= 0;
  }

  /* see 'lua_freezeonly' */
  public static void luaE_freezeonly(lua_State L)
  {
    L.ci.callstatus |= CIST_FZONLY;
  }

  /* actual number of total memory allocated */
  public static long gettotalbytes(global_State g)
  {
    return g.GCtotalbytes - g.GCdebt;
  }

  public static TValue gfasttm(global_State g, Table mt, int e)
  {
    return (checknoTM(mt, e) != 0) ? null : luaT_gettm(mt, e, g.tmname[e]);
  }

  /*
  ** set GCdebt to a new value keeping the real number of allocated
  ** objects (GCtotalobjs - GCdebt) invariant and avoiding overflows in
  ** 'GCtotalobjs'.
  */
  public static void luaE_setdebt(global_State g, long debt)
  {
    long tb = gettotalbytes(g);
    assert tb > 0;
    if (debt > MAX_LMEM - tb)
      debt = MAX_LMEM - tb; /* will make GCtotalbytes == MAX_LMEM */
    g.GCtotalbytes = tb + debt;
    g.GCdebt = debt;
  }

  public static CallInfo luaE_extendCI(lua_State L, int err)
  {
    CallInfo ci;
    ci = luaM_alloc_CallInfo(L);
    if (ci == null)
    { /* allocation failed? */
      if (err != 0)
        luaD_throw(L, (byte) LUA_ERRMEM); /* raise the error */
      return null;     /* else only report it */
    }
    ci.next = L.ci.next;
    ci.previous = L.ci;
    L.ci.next = ci;
    if (ci.next != null)
      ci.next.previous = ci;
    L.nci++;
    return ci;
  }

  /*
  ** free all CallInfo structures not in use by a thread
  */
  private static void freeCI(lua_State L)
  {
    CallInfo ci = L.ci;
    CallInfo next = ci.next;
    ci.next = null;
    while ((ci = next) != null)
    {
      next = ci.next;
      luaM_free_CallInfo(L, ci);
      L.nci--;
    }
  }

  /*
  ** free half of the CallInfo structures not in use by a thread,
  ** keeping the first one.
  */
  public static void luaE_shrinkCI(lua_State L)
  {
    CallInfo ci = L.ci.next; /* first free CallInfo */
    CallInfo next;
    if (ci == null)
      return; /* no extra elements */
    while ((next = ci.next) != null)
    {                              /* two extra elements? */
      CallInfo next2 = next.next; /* next's next */
      ci.next = next2;            /* remove next from the list */
      L.nci--;
      luaM_free_CallInfo(L, next); /* free next */
      if (next2 == null)
        break; /* no more elements */
      else
      {
        next2.previous = ci;
        ci = next2; /* continue */
      }
    }
  }

  /*
  ** Called when 'getCcalls(L)' larger or equal to LUAI_MAXCCALLS.
  ** If equal, raises an overflow error. If value is larger than
  ** LUAI_MAXCCALLS (which means it is handling an overflow) but
  ** not much larger, does not report an error (to allow overflow
  ** handling to work).
  */
  public static void luaE_checkcstack(lua_State L)
  {
    if (getCcalls(L) == LUAI_MAXCCALLS)
      luaG_runerror(L, Lt.C_stack_overflow);
    else if (getCcalls(L) >= (LUAI_MAXCCALLS / 10 * 11))
      luaD_errerr(L); /* error while handling stack error */
  }

  public static void luaE_incCstack(lua_State L)
  {
    L.nCcalls++;
    if (getCcalls(L) >= LUAI_MAXCCALLS)
      luaE_checkcstack(L);
  }

  private static void resetCI(lua_State L)
  {
    CallInfo ci = L.ci = L.base_ci;
    ci.func = 0;
    setslottag(L, ci.func, LUA_VNIL);                 /* 'function' entry for basic 'ci' */
    ci.top = ci.func + 1 + LUA_MINSTACK; /* +1 for 'function' entry */
    ci.k = null;
    ci.callstatus = CIST_C;
    L.status = LUA_OK;
    L.errfunc = 0; /* stack unwind can "throw away" the error function */
  }

  private static void stack_init(lua_State L1, lua_State L)
  {
    int i;
    int nslots = BASIC_STACK_SIZE + EXTRA_STACK;
    long[] stk = luaM_alloc_TValue_span(L.l_G, L, new long[0], nslots, false);
    if (stk == null)
    {
      luaD_throw(L, (byte) LUA_ERRMEM);
    }
    L1.stack_prim = stk;
    L1.stack_ref = luaM_alloc_TValue_span(new Object[0], nslots);
    L1.stack_tt = luaM_alloc_TValue_span(new byte[0], nslots);
    L1.tbclist = 0;
    for (i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
      setslottag(L1, i, LUA_VNIL); /* erase new stack */
    L1.stack_last = BASIC_STACK_SIZE;
    /* initialize first ci */
    resetCI(L1);
    L1.top = 1; /* +1 for 'function' entry */
  }

  private static void freestack(lua_State L)
  {
    luaM_alloc_StkId_span(L.l_G, L, L.tbc, 0, false);
    L.tbc = new int[0];
    L.ntbc = 0;
    if (L.stack_prim.length == 0)
      return;            /* stack not completely built yet */
    L.ci = L.base_ci; /* free the entire 'ci' list */
    freeCI(L);
    assert L.nci == 0;
    {
      luaM_alloc_TValue_span(L.l_G, L, L.stack_prim, 0, false);
    }
  }

  /*
  ** Create registry table and its predefined values
  */
  private static void init_registry(lua_State L, global_State g)
  {
    /* create registry */
    TValue aux = new TValue();
    Table registry = Table.luaH_new(L);
    sethvalue(L, g.l_registry, registry);
    registry.luaH_resize(L, LUA_RIDX_LAST, 0);
    /* registry[1] = false */
    aux.tt_ = LUA_VFALSE;
    registry.luaH_setint(L, 1, aux);
    /* registry[LUA_RIDX_MAINTHREAD] = L */
    setthvalue(L, aux, L);
    registry.luaH_setint(L, LUA_RIDX_MAINTHREAD, aux);
    /* registry[LUA_RIDX_GLOBALS] = new table (table of globals) */
    sethvalue(L, aux, Table.luaH_new(L));
    registry.luaH_setint(L, LUA_RIDX_GLOBALS, aux);
  }

  /*
  ** open parts of the state that may cause memory-allocation errors.
  */
  private static void f_luaopen(lua_State L)
  {
    global_State g = L.l_G;
    stack_init(L, L); /* init stack */
    init_registry(L, g);
    luaS_init(L);
    luaT_init(L);
    luaX_init(L);
    g.gcstp = 0;              /* allow gc */
    setnilvalue(g.nilvalue); /* now state is complete */
    luai_userstateopen(L);
  }

  private static final class LuaOpen implements ProtectedCall
  {
    @Override
    public void run(lua_State L)
    {
      f_luaopen(L);
    }
  }

  /*
  ** preinitialize a thread with consistent values without allocating
  ** any memory (to avoid errors)
  */
  private static void preinit_thread(lua_State L, global_State g)
  {
    L.l_G = g;
  }

  public static long luaE_threadsize(lua_State L)
  {
    long sz = Cost.Thread + L.nci * Cost.CallInfo;
    if (L.stack_prim.length != 0)
      sz += (stacksize(L) + EXTRA_STACK) * Cost.TValue;
    sz += (long) L.tbc.length * Cost.Ref;
    return sz;
  }

  private static void close_state(lua_State L)
  {
    global_State g = L.l_G;
    if (L.beff != null)
      luaE_setbeff(L, null); /* closing: no budget points */
    if (completestate(g) == 0)    /* closing a partially built state? */
      luaC_freeallobjects(L); /* just collect its objects */
    else
    { /* closing a fully built state */
      resetCI(L);
      if (L.frozen != 0)
      { /* (a frozen thread runs none of its '__close': they are dropped) */
        luaF_closeupval(L, 1);
        L.tbclist = 0;
        L.ntbc = 0;
      }
      else
        luaD_closeprotected(L, 1, (byte) LUA_OK, 0); /* close all upvalues */
      L.top = 1;         /* empty the stack to run finalizers */
      luaC_freeallobjects(L);            /* collect all objects */
      luai_userstateclose(L);
    }
    g.groupbyid = luaM_alloc_BudgetGroup_ptr_span(g, L, g.groupbyid, 0, false);
    freestack(L);
    assert gettotalbytes(g) == Cost.Global;
    g.frealloc.permit(Cost.Global, 0, LUA_TTHREAD); /* free main block */
  }

  public static lua_State lua_newthread(lua_State L)
  {
    global_State g = L.l_G;
    lua_State L1;
    lua_lock(L);
    luaC_checkGC(L);
    /* create new thread */
    L1 = luaM_malloc_lua_State(L);
    luaC_link(L, L1, (byte) LUA_TTHREAD);
    /* anchor it on L stack */
    setslotth(L, L.top, L1);
    api_incr_top(L);
    preinit_thread(L1, g);
    L1.hookmask = (L.hookmask & ~LUA_MASKBUDGET) & 0xFF; /* (its budget is set at a resume) */
    L1.bown = L.beff; /* it belongs to the budget group in effect */
    L1.basehookcount = L.basehookcount;
    L1.hook = L.hook;
    resethookcount(L1);
    /* initialize L1 extra space */
    L1.extra = mainthread(g).extra;
    luai_userstatethread(L, L1);
    stack_init(L1, L); /* init stack */
    lua_unlock(L);
    return L1;
  }

  /* put 'bg' (or nothing) in effect for 'th', with the budget bit of its mask */
  public static void luaE_setbeff(lua_State th, BudgetGroup bg)
  {
    th.beff = bg;
    if (bg != null)
    {
      th.hookmask |= LUA_MASKBUDGET;
      for (CallInfo ci = th.ci; ci != null; ci = ci.previous)
        if (isLua(ci) != 0)
          ci.trap = 1; /* to count inside 'luaV_execute' */
    }
    else
      th.hookmask &= ~LUA_MASKBUDGET;
  }

  public static void luaE_freethread(lua_State L, lua_State L1)
  {
    /* (a group's reference to the thread it was set on is weak: the collector
       clears it when the thread is dead, see 'atomic') */
    luaF_closeupval(L1, 0); /* close all upvalues */
    assert L1.openupval == null;
    luai_userstatefree(L, L1);
    freestack(L1);
    luaM_free_lua_State(L, L1);
  }

  /*
  ** The start of a close of the variables of a thread: it goes back to the
  ** base, with no status, so that a '__close' can run. Returns the status
  ** to close with.
  */
  public static byte luaE_beginclose(lua_State L)
  {
    byte status = L.status;
    resetCI(L);
    return (status == LUA_YIELD) ? LUA_OK : status;
  }

  /* its end: the error is at 1 (or there is none) */
  public static void luaE_endclose(lua_State L, byte status)
  {
    if (status != LUA_OK) /* errors? */
      luaD_seterrorobj(L, status, 1);
    else
      L.top = 1;
    if (L != L.l_G.budgetth) /* (the thread of callbacks keeps its room) */
      luaD_reallocstack(L, (int) L.ci.top, 0);
  }

  public static byte luaE_resetthread(lua_State L, byte status)
  {
    resetCI(L);
    if (status == LUA_YIELD)
      status = LUA_OK;
    status = luaD_closeprotected(L, 1, status, 0);
    luaE_endclose(L, status);
    return status;
  }


  /*
  ** After the budget callback left, with a pending exit, the thread it ran
  ** on: close its pending variables (with no error) and go back to the base,
  ** with no change to its stack, which is reserved. The error of a
  ** '__close', if any, is at 1.
  */
  public static byte luaE_resetcallbackthread(lua_State L)
  {
    L.errorJmp = null;
    L.nfz = 0;
    resetCI(L);
    byte status = luaD_closeprotected(L, 1, (byte) LUA_OK, 0);
    if (status != LUA_OK)
      luaD_seterrorobj(L, status, 1);
    else
      L.top = 1;
    return status;
  }

  /*
  ** Close the variables of 'L' as a plain call (see 'lua_closethread'): for a
  ** C function with no continuation, a thread closing itself, or a frozen
  ** thread (refused).
  */
  public static int luaE_closethreadplain(lua_State L, lua_State from)
  {
    byte status;
    lua_lock(L);
    if (L.frozen != 0)
    { /* a frozen thread is read-only: it only thaws (the message is above its frames) */
      luaD_checkstack(L, 1);
      setslots(L, L.top, TString.luaS_createstr(L, Lt.cannot_close_a_frozen_coroutine));
      L.top++;
      lua_unlock(L);
      return LUA_ERRRUN;
    }
    L.nCcalls = (from != null) ? getCcalls(from) : 0;
    /* the '__close' of the thread run as a call that cannot freeze, in the
       chain of the thread that closes it (see 'luaD_closeprotected') */
    boolean plain_on = L != from;
    try
    {
      if (plain_on)
      {
        L.resumer = from;
        L.resumedk = 0;
        L.nfz++;
        luaE_enterbudget(L, from);
      }
      status = luaE_resetthread(L, L.status);
      if (L == from) /* closing itself? */
        luaD_throwbaselevel(L, status);
      lua_unlock(L);
      return status;
    }
    finally
    {
      if (plain_on)
      {
        L.nfz--;
        L.resumer = null;
        luaE_leavebudget(L);
      }
    }
  }

  /*
  ** The host's close of a thread (no 'from'): its '__close' run under its
  ** budget, and a freeze there lands here, as at a resume ('lua_isfrozen'
  ** tells; 'lua_thaw' finishes the close). From a C function, with no
  ** continuation, they run as a plain call.
  */
  public static int lua_closethread(lua_State L, lua_State from)
  {
    if (from == null && L.frozen == 0)
      return lua_closethreadk(L, null, 0, null).status;
    return luaE_closethreadplain(L, from);
  }


  public static lua_State lua_newstate(lua_Alloc f, int seed)
  {
    lua_State L;
    if (!f.permit(0, Cost.Global, LUA_TTHREAD))
      return null;
    global_State g = new global_State();
    if (g == null)
      return null;
    L = g.mainth;
    L.tt = LUA_VTHREAD;
    g.currentwhite = (byte) bitmask(WHITE0BIT);
    L.marked = luaC_white(g);
    preinit_thread(L, g);
    assert novariant(L.tt) >= LUA_TSTRING;
    g.allgc.next = L; /* by now, only object is the main thread */
    L.next = null;
    incnny(L); /* main thread is always non yieldable */
    g.frealloc = f;
    g.seed = seed;
    g.nexthash = seed;
    L.hash = luaC_newhash(g);
    g.gcstp = (byte) GCSTPGC; /* no GC while building state */
    g.gcstate = (byte) GCSpause;
    g.gckind = KGC_INC;
    setivalue(g.nilvalue, 0); /* to signal that state is not yet built */
    g.gcparams[LUA_GCPPAUSE] = luaO_codeparam(LUAI_GCPAUSE);
    g.gcparams[LUA_GCPSTEPMUL] = luaO_codeparam(LUAI_GCMUL);
    g.gcparams[LUA_GCPSTEPSIZE] = luaO_codeparam(LUAI_GCSTEPSIZE);
    g.gcparams[LUA_GCPMINORMUL] = luaO_codeparam(LUAI_GENMINORMUL);
    g.gcparams[LUA_GCPMINORMAJOR] = luaO_codeparam(LUAI_MINORMAJOR);
    g.gcparams[LUA_GCPMAJORMINOR] = luaO_codeparam(LUAI_MAJORMINOR);
    LuaOpen open = new LuaOpen();
    if (luaD_rawrunprotected(L, open, false) != LUA_OK)
    {
      /* memory allocation error: free partial state */
      close_state(L);
      L = null;
    }
    return L;
  }


  public static void lua_close(lua_State L)
  {
    lua_lock(L);
    L = mainthread(L.l_G); /* only the main thread can be closed */
    close_state(L);
  }

  public static void luaE_warning(lua_State L, ByteView msg, int tocont)
  {
    lua_WarnFunction wf = L.l_G.warnf;
    if (wf != null)
    {
      lua_unlock(L);
      wf.warn(L, new HostString(msg), tocont);
      lua_lock(L);
    }
  }

  /*
  ** Generate a warning from an error message
  */
  public static void luaE_warnerror(lua_State L, ByteView where)
  {
    int errobj = L.top - 1; /* error object */
    ByteView msg = (novariant(slottag(L, errobj)) == LUA_TSTRING)
                               ? slottsval(L, errobj).data.view()
                               : Lt.error_object_is_not_a_string;
    /* produce warning "error in %s (%s)" (where, msg) */
    luaE_warning(L, Lt.error_in, 1);
    luaE_warning(L, where, 1);
    luaE_warning(L, Lt.sp_lparen, 1);
    luaE_warning(L, msg, 1);
    luaE_warning(L, Lt.rparen, 0);
  }
}
