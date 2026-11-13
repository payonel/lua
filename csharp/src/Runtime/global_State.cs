using System;
using Payonel.LungoLua.Compat;
using lua_Alloc = Payonel.LungoLua.Api.lua_Alloc;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_WarnFunction = Payonel.LungoLua.Api.lua_WarnFunction;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using EphIndex = Payonel.LungoLua.Gc.EphIndex;
using BudgetGroup = Payonel.LungoLua.Object.BudgetGroup;
using GCListed = Payonel.LungoLua.Object.GCListed;
using GCObject = Payonel.LungoLua.Object.GCObject;
using Lobject = Payonel.LungoLua.Object.Lobject;
using TString = Payonel.LungoLua.Object.TString;
using TValue = Payonel.LungoLua.Object.TValue;
using Table = Payonel.LungoLua.Object.Table;

namespace Payonel.LungoLua.Runtime;

public sealed class global_State
{
  public lua_Alloc frealloc = null; /* function to reallocate memory */
  public int apilock = 0;
  public long GCtotalbytes = Cost.Global; /* number of bytes currently allocated + debt */
  public long GCdebt = 0;       /* bytes counted but not yet allocated */
  public long GCmarked = 0;     /* number of objects marked in a GC cycle */
  public long GCmajorminor = 0; /* auxiliary counter to control major-minor shifts */

  public sealed class StringTable
  {
    public TString[] buckets = new TString[0];
    public int count = 0;
  }

  public readonly StringTable strt = new StringTable();
  public readonly TValue l_registry = new TValue(0, null, Lobject.LUA_VNIL);
  public readonly TValue nilvalue = new TValue(0, null, Lobject.LUA_VNIL); /* a nil value */
  public int seed = 0;     /* randomized seed for hashes */
  public int nexthash = 0; /* the hash of the next object created */
  public readonly int[] gcparams = new int[6]; /* LUA_GCPN */
  public sbyte currentwhite = 0;
  public sbyte gcstate = 0;            /* state of garbage collector */
  public sbyte gckind = 0;             /* kind of GC running */
  public bool gcstopem = false;    /* stops emergency collections */
  public sbyte gcstp = 0;              /* control whether GC is running */
  public bool gcemergency = false; /* true if this is an emergency collection */
  public readonly GCObject allgc = new GCObject(); /* list of all collectable objects */
  public GCObject sweepgc = null; /* current position of sweep in list */
  public int sweepq = 0;          /* the next group whose queue the sweep takes */
  public readonly GCObject finobj = new GCObject();      /* list of collectable objects with finalizers */
  public readonly GCListed gray = new GCListed();        /* list of gray objects */
  public readonly GCListed grayagain = new GCListed();   /* list of objects to be traversed atomically */
  public readonly GCListed weak = new GCListed();        /* list of tables with weak values */
  public readonly GCListed ephemeron = new GCListed();   /* list of ephemeron tables (weak keys) */
  public EphIndex ephidx = null; /* (while ephemerons converge: their entries that wait) */
  public readonly GCListed allweak = new GCListed();     /* list of all-weak tables */
  public readonly GCObject tobefnz = new GCObject();     /* list of userdata to be GC */
  public GCObject fixedgc = null; /* list of objects not to be collected */
  /* fields for generational collector */
  public GCObject survival = null;   /* start of objects that survived one GC cycle */
  public GCObject old1 = null;       /* start of old1 objects */
  public GCObject reallyold = null;  /* objects more than one cycle old ("really old") */
  public GCObject firstold1 = null;  /* first OLD1 object in the list (if any) */
  public GCObject finobjsur = null;  /* list of survival objects with finalizers */
  public GCObject finobjold1 = null; /* list of old1 objects with finalizers */
  public GCObject finobjrold = null; /* list of really old objects with finalizers */
  public lua_State twups = null;     /* list of threads with open upvalues */
  public lua_CFunction panic = null; /* to be called in unprotected errors */
  public TString memerrmsg = null;   /* message for memory-allocation errors */
  public TString nofreezemsg = null; /* message of a chain destroyed because it cannot freeze */
  public readonly TString[] tmname = new TString[Ltm.TM_N]; /* array with tag-method names */
  public readonly Table[] mt = new Table[9];                /* metatables for basic types (LUA_NUMTYPES) */
  public lua_WarnFunction warnf = null;                  /* warning function */
  /* the budget callback ('lua_setbudget'); the budget is in each group */
  public lua_State budgetL = null;     /* thread running it, while it runs */
  public BudgetGroup budgetg = null;   /* the group whose callback runs (kept by the collector) */
  public lua_State budgetth = null;    /* a thread for the callback (see 'lua_budgetthread') */
  public int budgetnfz = 0;            /* its 'nfz' when the callback began */
  public bool budgetexit = false;   /* the callback left: its pending '__close' run */
  public BudgetGroup[] groupbyid = new BudgetGroup[0]; /* the budget groups by owner id (weak; 0 is not an id) */
  public int nextownerid = 1;          /* the next owner id to give */
  public readonly BitSet ownerskip = new BitSet(65536); /* ids not to give in this round (see 'luaC_newowner') */
  public lua_State freezetarget = null; /* coroutine being frozen (unwinding) */
  public bool freezekill = false;    /* ... or whose chain is being destroyed, */
  public readonly TValue killerr = new TValue(); /* with this error (no collection in between) */
  public InternalString locale_name = new InternalString();
  public readonly lua_State mainth = new lua_State(); /* main thread of this state */
}
