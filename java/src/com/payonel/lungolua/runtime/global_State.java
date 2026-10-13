package com.payonel.lungolua.runtime;

import java.util.BitSet;

import com.payonel.lungolua.api.lua_Alloc;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_WarnFunction;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.gc.EphIndex;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.GCListed;
import com.payonel.lungolua.object.GCObject;
import com.payonel.lungolua.object.Lobject;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;

public final class global_State
{
  public lua_Alloc frealloc = null; /* function to reallocate memory */
  public int apilock = 0;
  public long GCtotalbytes = Cost.Global; /* number of bytes currently allocated + debt */
  public long GCdebt = 0;       /* bytes counted but not yet allocated */
  public long GCmarked = 0;     /* number of objects marked in a GC cycle */
  public long GCmajorminor = 0; /* auxiliary counter to control major-minor shifts */

  public static final class StringTable
  {
    public TString[] buckets = new TString[0];
    public int count = 0;
  }

  public final StringTable strt = new StringTable();
  public final TValue l_registry = new TValue(0, null, Lobject.LUA_VNIL);
  public final TValue nilvalue = new TValue(0, null, Lobject.LUA_VNIL); /* a nil value */
  public int seed = 0;     /* randomized seed for hashes */
  public int nexthash = 0; /* the hash of the next object created */
  public final int[] gcparams = new int[6]; /* LUA_GCPN */
  public byte currentwhite = 0;
  public byte gcstate = 0;            /* state of garbage collector */
  public byte gckind = 0;             /* kind of GC running */
  public boolean gcstopem = false;    /* stops emergency collections */
  public byte gcstp = 0;              /* control whether GC is running */
  public boolean gcemergency = false; /* true if this is an emergency collection */
  public final GCObject allgc = new GCObject(); /* list of all collectable objects */
  public GCObject sweepgc = null; /* current position of sweep in list */
  public int sweepq = 0;          /* the next group whose queue the sweep takes */
  public final GCObject finobj = new GCObject();      /* list of collectable objects with finalizers */
  public final GCListed gray = new GCListed();        /* list of gray objects */
  public final GCListed grayagain = new GCListed();   /* list of objects to be traversed atomically */
  public final GCListed weak = new GCListed();        /* list of tables with weak values */
  public final GCListed ephemeron = new GCListed();   /* list of ephemeron tables (weak keys) */
  public EphIndex ephidx = null; /* (while ephemerons converge: their entries that wait) */
  public final GCListed allweak = new GCListed();     /* list of all-weak tables */
  public final GCObject tobefnz = new GCObject();     /* list of userdata to be GC */
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
  public final TString[] tmname = new TString[Ltm.TM_N]; /* array with tag-method names */
  public final Table[] mt = new Table[9];                /* metatables for basic types (LUA_NUMTYPES) */
  public lua_WarnFunction warnf = null;                  /* warning function */
  /* the budget callback ('lua_setbudget'); the budget is in each group */
  public lua_State budgetL = null;     /* thread running it, while it runs */
  public BudgetGroup budgetg = null;   /* the group whose callback runs (kept by the collector) */
  public lua_State budgetth = null;    /* a thread for the callback (see 'lua_budgetthread') */
  public int budgetnfz = 0;            /* its 'nfz' when the callback began */
  public boolean budgetexit = false;   /* the callback left: its pending '__close' run */
  public BudgetGroup[] groupbyid = new BudgetGroup[0]; /* the budget groups by owner id (weak; 0 is not an id) */
  public int nextownerid = 1;          /* the next owner id to give */
  public final BitSet ownerskip = new BitSet(65536); /* ids not to give in this round (see 'luaC_newowner') */
  public lua_State freezetarget = null; /* coroutine being frozen (unwinding) */
  public boolean freezekill = false;    /* ... or whose chain is being destroyed, */
  public final TValue killerr = new TValue(); /* with this error (no collection in between) */
  public InternalString locale_name = new InternalString();
  public final lua_State mainth = new lua_State(); /* main thread of this state */
}
