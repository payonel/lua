package com.payonel.lungolua.runtime;

import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.GCListed;
import com.payonel.lungolua.object.UpVal;

/*
** 'per thread' state
*/
public final class lua_State extends GCListed
{
  public boolean allowhook = true;
  public byte status = 0; /* LUA_OK */
  public int top = 0; /* first free slot in the stack */
  public global_State l_G = null;
  public CallInfo ci = null;    /* call info for current function */
  public int stack_last = 0;    /* end of stack (last element + 1) */
  /* stack base: its slots as columns (see the slot accessors in Lstate) */
  public long[] stack_prim = new long[0];
  public Object[] stack_ref = new Object[0];
  public byte[] stack_tt = new byte[0];
  public UpVal openupval = null; /* list of open upvalues in this stack */
  public int tbclist = 0;
  public int[] tbc = new int[0];
  public int ntbc = 0;
  public lua_State twups = this;       /* list of threads with open upvalues */
  public lua_longjmp errorJmp = null;  /* current error recover point */
  public final CallInfo base_ci = new CallInfo(); /* CallInfo for first level (C host) */
  public volatile lua_Hook hook = null;
  public int errfunc = 0;  /* current error handling function (stack index) */
  public int nCcalls = 0;  /* number of nested non-yieldable or C calls */
  public int oldpc = 0;    /* last pc traced */
  public int nci = 0;      /* number of items in 'ci' list */
  public int basehookcount = 0;
  public int hookcount = 0;
  public volatile int hookmask = 0;

  public static final class TransferInfo
  {                          /* info about transferred values (for call/return hooks) */
    public int ftransfer = 0; /* offset of first value transferred */
    public int ntransfer = 0; /* number of values transferred */
  }

  public final TransferInfo transferinfo = new TransferInfo();
  public Object extra = null;
  /* freezing ('lua_freeze') */
  public lua_State resumer = null;      /* thread resuming this one, while it runs */
  public lua_State frozenchild = null;  /* frozen while resuming this thread */
  public lua_State frozenparent = null; /* the thread that resumed it, in a frozen chain */
  public int nfz = 0;                   /* number of calls in the stack that cannot freeze */
  public boolean resumedk = false;      /* resumed by a call with a continuation */
  public BudgetGroup bown = null;       /* the budget group it belongs to, if any */
  public BudgetGroup beff = null;       /* the budget group in effect while it runs */
  public byte frozen = 0;               /* frozen: how it was entered (FZ_*); 0: not */
  public boolean closeself = false;     /* closing its own variables ('lua_closeself'): a thaw goes on with that */
  public byte closestatus = 0;          /* closing its variables ('lua_closethreadk'): the status it closes with */
  public int frozenfunc = 0;            /* frozen in a host call: the function called, */
  public int frozenerrfunc = 0;         /* its message handler, */
  public int frozennres = 0;            /* its number of results, */
  public int frozencc = 0;              /* its 'nCcalls' where it froze */
  public int frozentop = 0;             /* its top where it froze (what a refusal pushes on it after is not its) */
  public int ccbase = 0;                /* its 'nCcalls' when resumed (see 'forgetcalls') */
}
