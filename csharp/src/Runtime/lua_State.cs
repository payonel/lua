using System;
using Payonel.LungoLua.Compat;
using lua_Hook = Payonel.LungoLua.Api.lua_Hook;
using BudgetGroup = Payonel.LungoLua.Object.BudgetGroup;
using GCListed = Payonel.LungoLua.Object.GCListed;
using UpVal = Payonel.LungoLua.Object.UpVal;

namespace Payonel.LungoLua.Runtime;

/*
** 'per thread' state
*/
public sealed class lua_State : GCListed
{
  public lua_State()
  {
    twups = this;
  }

  public bool allowhook = true;
  public sbyte status = 0; /* LUA_OK */
  public int top = 0; /* first free slot in the stack */
  public global_State l_G = null;
  public CallInfo ci = null;    /* call info for current function */
  public int stack_last = 0;    /* end of stack (last element + 1) */
  /* stack base: its slots as columns (see the slot accessors in Lstate) */
  public long[] stack_prim = new long[0];
  public object[] stack_ref = new object[0];
  public sbyte[] stack_tt = new sbyte[0];
  public UpVal openupval = null; /* list of open upvalues in this stack */
  public int tbclist = 0;
  public int[] tbc = new int[0];
  public int ntbc = 0;
  public lua_State twups;       /* list of threads with open upvalues */
  public lua_longjmp errorJmp = null;  /* current error recover point */
  public readonly CallInfo base_ci = new CallInfo(); /* CallInfo for first level (C host) */
  public volatile lua_Hook hook = null;
  public int errfunc = 0;  /* current error handling function (stack index) */
  public int nCcalls = 0;  /* number of nested non-yieldable or C calls */
  public int oldpc = 0;    /* last pc traced */
  public int nci = 0;      /* number of items in 'ci' list */
  public int basehookcount = 0;
  public int hookcount = 0;
  public volatile int hookmask = 0;

  public sealed class TransferInfo
  {                          /* info about transferred values (for call/return hooks) */
    public int ftransfer = 0; /* offset of first value transferred */
    public int ntransfer = 0; /* number of values transferred */
  }

  public readonly TransferInfo transferinfo = new TransferInfo();
  public object extra = null;
  /* freezing ('lua_freeze') */
  public lua_State resumer = null;      /* thread resuming this one, while it runs */
  public lua_State frozenchild = null;  /* frozen while resuming this thread */
  public lua_State frozenparent = null; /* the thread that resumed it, in a frozen chain */
  public int nfz = 0;                   /* number of calls in the stack that cannot freeze */
  public bool resumedk = false;      /* resumed by a call with a continuation */
  public BudgetGroup bown = null;       /* the budget group it belongs to, if any */
  public BudgetGroup beff = null;       /* the budget group in effect while it runs */
  public sbyte frozen = 0;               /* frozen: how it was entered (FZ_*); 0: not */
  public bool closeself = false;     /* closing its own variables ('lua_closeself'): a thaw goes on with that */
  public sbyte closestatus = 0;          /* closing its variables ('lua_closethreadk'): the status it closes with */
  public int frozenfunc = 0;            /* frozen in a host call: the function called, */
  public int frozenerrfunc = 0;         /* its message handler, */
  public int frozennres = 0;            /* its number of results, */
  public int frozencc = 0;              /* its 'nCcalls' where it froze */
  public int frozentop = 0;             /* its top where it froze (what a refusal pushes on it after is not its) */
  public int ccbase = 0;                /* its 'nCcalls' when resumed (see 'forgetcalls') */
}
