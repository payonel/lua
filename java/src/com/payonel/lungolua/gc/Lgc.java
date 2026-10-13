package com.payonel.lungolua.gc;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.object.Lfunc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lnames.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.object.Ltable.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.runtime.Ltm.*;

import java.util.Arrays;
import java.util.IdentityHashMap;

import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.object.GCListed;
import com.payonel.lungolua.object.GCObject;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.Node;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.object.Udata;
import com.payonel.lungolua.object.UpVal;
import com.payonel.lungolua.runtime.Cost;
import com.payonel.lungolua.runtime.ProtectedCall;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

public final class Lgc
{
  private Lgc()
  {
  }

  /*
  ** Collectable objects may have one of three colors: white, which means
  ** the object is not marked; gray, which means the object is marked, but
  ** its references may be not marked; and black, which means that the
  ** object and all its references are marked.  The main invariant of the
  ** garbage collector, while marking objects, is that a black object can
  ** never point to a white one. Moreover, any gray object must be in a
  ** "gray list" (gray, grayagain, weak, allweak, ephemeron) so that it
  ** can be visited again before finishing the collection cycle. (Open
  ** upvalues are an exception to this rule, as they are attached to
  ** a corresponding thread.)  These lists have no meaning when the
  ** invariant is not being enforced (e.g., sweep phase).
  */

  /*
  ** Possible states of the Garbage Collector
  */
  public static final int GCSpropagate = 0;
  public static final int GCSenteratomic = 1;
  public static final int GCSatomic = 2;
  public static final int GCSswpallgc = 3;
  public static final int GCSswpfinobj = 4;
  public static final int GCSswptobefnz = 5;
  public static final int GCSswpend = 6;
  public static final int GCScallfin = 7;
  public static final int GCSpause = 8;

  public static boolean issweepphase(global_State g)
  {
    return GCSswpallgc <= g.gcstate && g.gcstate <= GCSswpend;
  }

  /*
  ** macro to tell when main invariant (white objects cannot point to black
  ** ones) must be kept. During a collection, the sweep phase may break
  ** the invariant, as objects turned white may point to still-black
  ** objects. The invariant is restored when sweep ends and all objects
  ** are white again.
  */

  public static boolean keepinvariant(global_State g)
  {
    return g.gcstate <= GCSatomic;
  }

  /*
  ** some useful bit tricks
  */
  public static int bitmask(int b)
  {
    return 1 << b;
  }

  public static int bit2mask(int b1, int b2)
  {
    return bitmask(b1) | bitmask(b2);
  }

  /*
  ** Layout for bit use in 'marked' field. First three bits are
  ** used for object "age" in generational mode. Last bit is used
  ** by tests.
  */
  public static final int WHITE0BIT = 3; /* object is white (type 0) */
  public static final int WHITE1BIT = 4; /* object is white (type 1) */
  public static final int BLACKBIT = 5; /* object is black */
  public static final int FINALIZEDBIT = 6; /* object has been marked for finalization */

  public static final int TESTBIT = 7;

  public static final int WHITEBITS = bit2mask(WHITE0BIT, WHITE1BIT);

  public static boolean iswhite(GCObject x)
  {
    return (x.marked & WHITEBITS) != 0;
  }

  public static boolean isblack(GCObject x)
  {
    return (x.marked & bitmask(BLACKBIT)) != 0;
  }

  /* neither white nor black */
  public static boolean isgray(GCObject x)
  {
    return (x.marked & (WHITEBITS | bitmask(BLACKBIT))) == 0;
  }

  public static boolean tofinalize(GCObject x)
  {
    return (x.marked & bitmask(FINALIZEDBIT)) != 0;
  }

  public static byte otherwhite(global_State g)
  {
    return (byte) (g.currentwhite ^ WHITEBITS);
  }

  public static boolean isdeadm(byte ow, byte m)
  {
    return ((m & 0xFF) & (ow & 0xFF)) != 0;
  }

  public static boolean isdead(global_State g, GCObject v)
  {
    return isdeadm(otherwhite(g), v.marked);
  }

  public static void changewhite(GCObject x)
  {
    x.marked = (byte) (x.marked ^ WHITEBITS);
  }

  public static void nw2black(GCObject x)
  {
    assert !iswhite(x);
    x.marked = (byte) (x.marked | bitmask(BLACKBIT));
  }

  public static byte luaC_white(global_State g)
  {
    return (byte) (g.currentwhite & WHITEBITS);
  }

  /* object age in generational mode */
  public static final int G_NEW = 0; /* created in current cycle */
  public static final int G_SURVIVAL = 1; /* created in previous cycle */
  public static final int G_OLD0 = 2; /* marked old by frw. barrier in this cycle */
  public static final int G_OLD1 = 3; /* first full cycle as old */
  public static final int G_OLD = 4; /* really old object (not to be visited) */
  public static final int G_TOUCHED1 = 5; /* old object touched this cycle */
  public static final int G_TOUCHED2 = 6; /* old object touched in previous cycle */

  public static final int AGEBITS = 7; /* all age bits (111) */

  public static int getage(GCObject o)
  {
    return o.marked & AGEBITS;
  }

  public static void setage(GCObject o, int a)
  {
    o.marked = (byte) ((o.marked & ~AGEBITS) | a);
  }

  public static boolean isold(GCObject o)
  {
    return getage(o) > G_SURVIVAL;
  }

  /*
  ** In generational mode, objects are created 'new'. After surviving one
  ** cycle, they become 'survival'. Both 'new' and 'survival' can point
  ** to any other object, as they are traversed at the end of the cycle.
  ** We call them both 'young' objects.
  ** If a survival object survives another cycle, it becomes 'old1'.
  ** 'old1' objects can still point to survival objects (but not to
  ** new objects), so they still must be traversed. After another cycle
  ** (that, being old, 'old1' objects will "survive" no matter what)
  ** finally the 'old1' object becomes really 'old', and then they
  ** are no more traversed.
  **
  ** To keep its invariants, the generational mode uses the same barriers
  ** also used by the incremental mode. If a young object is caught in a
  ** forward barrier, it cannot become old immediately, because it can
  ** still point to other young objects. Instead, it becomes 'old0',
  ** which in the next cycle becomes 'old1'. So, 'old0' objects is
  ** old but can point to new and survival objects; 'old1' is old
  ** but cannot point to new objects; and 'old' cannot point to any
  ** young object.
  **
  ** If any old object ('old0', 'old1', 'old') is caught in a back
  ** barrier, it becomes 'touched1' and goes into a gray list, to be
  ** visited at the end of the cycle.  There it evolves to 'touched2',
  ** which can point to survivals but not to new objects. In yet another
  ** cycle then it becomes 'old' again.
  **
  ** The generational mode must also control the colors of objects,
  ** because of the barriers.  While the mutator is running, young objects
  ** are kept white. 'old', 'old1', and 'touched2' objects are kept black,
  ** as they cannot point to new objects; exceptions are threads and open
  ** upvalues, which age to 'old1' and 'old' but are kept gray. 'old0'
  ** objects may be gray or black, as in the incremental mode. 'touched1'
  ** objects are kept gray, as they must be visited again at the end of
  ** the cycle.
  */

  /*
  ** {======================================================
  ** Default Values for GC parameters
  ** =======================================================
  */

  /*
  ** Minor collections will shift to major ones after LUAI_MINORMAJOR%
  ** bytes become old.
  */
  public static final int LUAI_MINORMAJOR = 70;

  /*
  ** Major collections will shift to minor ones after a collection
  ** collects at least LUAI_MAJORMINOR% of the new bytes.
  */
  public static final int LUAI_MAJORMINOR = 50;

  /*
  ** A young (minor) collection will run after creating LUAI_GENMINORMUL%
  ** new bytes.
  */
  public static final int LUAI_GENMINORMUL = 20;

  /* incremental */

  /* Number of bytes must be LUAI_GCPAUSE% before starting new cycle */
  public static final int LUAI_GCPAUSE = 250;

  /*
  ** Step multiplier: The collector handles LUAI_GCMUL% work units for
  ** each new allocated word. (Each "work unit" corresponds roughly to
  ** sweeping one object or traversing one slot.)
  */
  public static final int LUAI_GCMUL = 200;

  /* How many bytes to allocate before next GC step */
  public static final int LUAI_GCSTEPSIZE = 200 * Cost.Table;

  /* }====================================================== */

  /*
  ** Control when GC is running:
  */
  public static final int GCSTPUSR = 1; /* bit true when GC stopped by user */
  public static final int GCSTPGC = 2; /* bit true when GC stopped by itself */
  public static final int GCSTPCLS = 4; /* bit true when closing Lua state */

  public static boolean gcrunning(global_State g)
  {
    return g.gcstp == 0;
  }

  public static void luaC_checkGC(lua_State L)
  {
    if (L.l_G.GCdebt <= 0)
      luaC_step(L);
  }

  /*
  ** The hash of a new object as a table key: a sequence that starts at the
  ** seed and steps by an odd constant, so it visits every value once (the
  ** k-th object made gets seed + k * HASHSTEP).
  */
  public static final int HASHSTEP = 0x9E3779B9;

  public static int luaC_newhash(global_State g)
  {
    int h = g.nexthash;
    g.nexthash = h + HASHSTEP;
    return h;
  }

  /*
  ** An object made just now that is never a program's key (a table of the
  ** parser's own) gives its hash back: the sequence depends only on what the
  ** program makes.
  */
  public static void luaC_givebackhash(global_State g, GCObject o)
  {
    assert o.hash + HASHSTEP == g.nexthash;
    g.nexthash = o.hash;
    o.hash = 0;
  }

  public static void luaC_objbarrier(lua_State L, GCObject p, GCObject o)
  {
    if (isblack(p) && iswhite(o))
      luaC_barrier_(L, p, o);
  }

  public static void luaC_barrier(lua_State L, GCObject p, TValue v)
  {
    if (iscollectable(v))
      luaC_objbarrier(L, p, gcvalue(v));
  }

  public static void luaC_objbarrierback(lua_State L, GCObject p, GCObject o)
  {
    if (isblack(p) && iswhite(o))
      luaC_barrierback_(L, p);
  }

  public static void luaC_barrierback(lua_State L, GCObject p, TValue v)
  {
    if (iscollectable(v))
      luaC_objbarrierback(L, p, gcvalue(v));
  }

  /*
  ** Maximum number of elements to sweep in each single step.
  ** (Large enough to dissipate fixed overheads but small enough
  ** to allow small steps for the collector.)
  */
  private static final int GCSWEEPMAX = 20;

  /*
  ** Cost (in work units) of running one finalizer.
  */
  private static final int CWUFIN = 10;

  /* mask with all color bits */
  private static final int maskcolors = bitmask(BLACKBIT) | WHITEBITS;

  /* mask with all GC bits */
  private static final int maskgcbits = maskcolors | AGEBITS;

  private static void makewhite(global_State g, GCObject x)
  {
    x.marked = (byte) ((x.marked & ~maskcolors) | luaC_white(g));
  }

  /* make an object gray (neither white nor black) */
  private static void set2gray(GCObject x)
  {
    x.marked = (byte) (x.marked & ~maskcolors);
  }

  /* make an object black (coming from any color) */
  private static void set2black(GCObject x)
  {
    x.marked = (byte) ((x.marked & ~WHITEBITS) | bitmask(BLACKBIT));
  }

  private static boolean keyiswhite(Node n)
  {
    return keyiscollectable(n) && iswhite(gckey(n));
  }

  private static GCObject gcvalueN(TValue o)
  {
    return iscollectable(o) ? gcvalue(o) : null;
  }

  private static GCObject gcvalarr(Table t, int i)
  {
    return ((getArrTag(t, i) & BIT_ISCOLLECTABLE) != 0) ? (GCObject) getArrVal(t, i).ref : null;
  }

  private static void markvalue(global_State g, TValue o)
  {
    checkliveness(mainthread(g), o);
    if (iscollectable(o) && iswhite(gcvalue(o)))
      reallymarkobject(g, gcvalue(o));
  }

  private static void markkey(global_State g, Node n)
  {
    if (keyiswhite(n))
      reallymarkobject(g, gckey(n));
  }

  private static void markobject(global_State g, GCObject t)
  {
    if (iswhite(t))
      reallymarkobject(g, t);
  }

  private static void markobjectN(global_State g, GCObject t)
  {
    if (t != null)
      markobject(g, t);
  }

  /*
  ** {======================================================
  ** Generic functions
  ** =======================================================
  */

  private static long objsize(GCObject o)
  {
    long res;
    switch (o.tt)
    {
    case LUA_VTABLE:
    {
      res = ((Table) o).luaH_size();
      break;
    }
    case LUA_VLCL:
    {
      LClosure cl = (LClosure) o;
      res = sizeLclosure(cl.upvals.length);
      break;
    }
    case LUA_VCCL:
    {
      CClosure cl = (CClosure) o;
      res = sizeCclosure(cl.upvalue.length);
      break;
    }
    case LUA_VUSERDATA:
    {
      Udata u = (Udata) o;
      res = Cost.Udata + (long) u.uv.length * Cost.TValue + u.data.size() + u.objsize;
      break;
    }
    case LUA_VPROTO:
    {
      res = ((Proto) o).luaF_protosize();
      break;
    }
    case LUA_VTHREAD:
    {
      res = luaE_threadsize((lua_State) o);
      break;
    }
    case LUA_VSHRSTR:
    {
      assert novariant(o.tt) == LUA_TSTRING;
      TString ts = (TString) o;
      res = Cost.TString + (strisexternal(ts) ? 0 : ts.data.size());
      break;
    }
    case LUA_VUPVAL:
    {
      res = Cost.UpVal;
      break;
    }
    case LUA_VBUDGET:
    {
      res = Cost.BudgetGroup;
      break;
    }
    case LUA_VLCF:
    {
      res = Cost.CFuncInfo;
      break;
    }
    default:
      res = 0;
      assert false;
    }
    return res;
  }

  private static GCListed getgclist(GCObject o)
  {
    assert o.tt == LUA_VTABLE || o.tt == LUA_VLCL || o.tt == LUA_VCCL ||
           o.tt == LUA_VTHREAD || o.tt == LUA_VPROTO ||
           (o.tt == LUA_VUSERDATA && ((Udata) o).uv.length > 0);
    return (GCListed) o;
  }

  /*
  ** Link a collectable object 'o' with a known type into the list 'p'.
  ** (Must be a macro to access the 'gclist' field in different types.)
  */

  private static void linkgclist_(GCListed o, GCListed list)
  {
    assert !isgray(o); /* cannot be in a gray list */
    o.gclist = list.gclist;
    list.gclist = o;
    set2gray(o); /* now it is */
  }

  /*
  ** Link a generic collectable object 'o' into the list 'p'.
  */

  /*
  ** Clear keys for empty entries in tables. If entry is empty, mark its
  ** entry as dead. This allows the collection of the key, but keeps its
  ** entry in the table: its removal could break a chain and could break
  ** a table traversal.  Other places never manipulate dead keys, because
  ** its associated empty value is enough to signal that the entry is
  ** logically empty.
  */
  private static void clearkey(Node n)
  {
    assert isempty(n);
    if (keyiscollectable(n))
      setdeadkey(n); /* unused key; remove it */
  }

  /*
  ** tells whether a key or value can be cleared from a weak
  ** table. Non-collectable objects are never removed from weak
  ** tables. Strings behave as 'values', so are never removed too. for
  ** other objects: if really collected, cannot keep them; for objects
  ** being finalized, keep them in keys, but not in values
  */
  private static boolean iscleared(global_State g, GCObject o)
  {
    if (o == null)
      return false; /* non-collectable value */
    else if (novariant(o.tt) == LUA_TSTRING)
    {
      markobject(g, o);
      return false;
    }
    else
      return iswhite(o);
  }

  /*
  ** Barrier that moves collector forward, that is, marks the white object
  ** 'v' being pointed by the black object 'o'.  In the generational
  ** mode, 'v' must also become old, if 'o' is old; however, it cannot
  ** be changed directly to OLD, because it may still point to non-old
  ** objects. So, it is marked as OLD0. In the next cycle it will become
  ** OLD1, and in the next it will finally become OLD (regular old). By
  ** then, any object it points to will also be old.  If called in the
  ** incremental sweep phase, it clears the black object to white (sweep
  ** it) to avoid other barrier calls for this same object. (That cannot
  ** be done is generational mode, as its sweep does not distinguish
  ** white from dead.)
  */
  public static void luaC_barrier_(lua_State L, GCObject o, GCObject v)
  {
    global_State g = L.l_G;
    assert isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o);
    if (keepinvariant(g))
    {                         /* must keep invariant? */
      reallymarkobject(g, v); /* restore invariant */
      if (isold(o))
      {
        assert !isold(v); /* white object could not be old */
        setage(v, G_OLD0);     /* restore generational invariant */
      }
    }
    else
    { /* sweep phase */
      assert issweepphase(g);
      if (g.gckind != KGC_GENMINOR) /* incremental mode? */
        makewhite(g, o);             /* mark 'o' as white to avoid other barriers */
    }
  }

  /*
  ** barrier that moves collector backward, that is, mark the black object
  ** pointing to a white object as gray again.
  */
  public static void luaC_barrierback_(lua_State L, GCObject o)
  {
    global_State g = L.l_G;
    assert isblack(o) && !isdead(g, o);
    assert (g.gckind != KGC_GENMINOR) || (isold(o) && getage(o) != G_TOUCHED1);
    if (getage(o) == G_TOUCHED2) /* already in gray list? */
      set2gray(o);               /* make it gray to become touched1 */
    else                         /* link it in 'grayagain' and paint it gray */
      linkgclist_(getgclist(o), g.grayagain);
    if (isold(o))       /* generational mode? */
      setage(o, G_TOUCHED1); /* touched in current cycle */
  }

  public static void luaC_fix(lua_State L, GCObject o)
  {
    global_State g = L.l_G;
    assert g.allgc.next == o; /* object must be 1st in 'allgc' list! */
    set2gray(o);               /* they will be gray forever */
    setage(o, G_OLD);          /* and old forever */
    g.allgc.next = o.next;        /* remove object from 'allgc' list */
    o.next = g.fixedgc;      /* link it to 'fixedgc' list */
    g.fixedgc = o;
  }

  /*
  ** link an already-allocated collectable object (of given type) into
  ** the 'allgc' list.
  */
  public static GCObject luaC_link(lua_State L, GCObject o, byte tt)
  {
    global_State g = L.l_G;
    o.marked = luaC_white(g);
    o.owner = 0;
    o.tt = tt;
    /* only objects that hash by identity as table keys take a hash: whether
       a string is new or reused depends on the collector, and the sequence
       must depend only on what the program makes */
    switch (tt)
    {
    case LUA_VTABLE:
    case LUA_VUSERDATA:
    case LUA_VLCL:
    case LUA_VCCL:
    case LUA_VTHREAD:
      o.hash = luaC_newhash(g);
      break;
    default:
      o.hash = 0;
    }
    o.next = g.allgc.next;
    g.allgc.next = o;
    return o;
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Mark functions
  ** =======================================================
  */

  /*
  ** Mark an object.  Userdata with no user values, strings, and closed
  ** upvalues are visited and turned black here.  Open upvalues are
  ** already indirectly linked through their respective threads in the
  ** 'twups' list, so they don't go to the gray list; nevertheless, they
  ** are kept gray to avoid barriers, as their values will be revisited
  ** by the thread or by 'remarkupvals'.  Other objects are added to the
  ** gray list to be visited (and turned black) later.  Both userdata and
  ** upvalues can call this function recursively, but this recursion goes
  ** for at most two levels: An upvalue cannot refer to another upvalue
  ** (only closures can), and a userdata's metatable must be a table.
  */
  private static void reallymarkobject(global_State g, GCObject o)
  {
    if (g.ephidx != null)
      ephmarked(g, o);
    g.GCmarked += objsize(o);
    switch (o.tt)
    {
    case LUA_VSHRSTR:
    {
      set2black(o); /* nothing to visit */
      break;
    }
    case LUA_VLCF:
    {
      set2black(o); /* CFuncInfo has no internal GC refs */
      break;
    }
    case LUA_VBUDGET:
    {
      BudgetGroup bg = (BudgetGroup) o;
      set2black(bg); /* its only reference is the value for the callback */
      markvalue(g, bg.cb);
      break;
    }
    case LUA_VUPVAL:
    {
      UpVal uv = (UpVal) o;
      if (upisopen(uv))
        set2gray(uv); /* open upvalues are kept gray */
      else
        set2black(uv);        /* closed upvalues are visited here */
      TValue uvv = upvalgetval(uv);
      markvalue(g, uvv); /* mark its content */
      break;
    }
    case LUA_VUSERDATA:
    {
      Udata u = (Udata) o;
      if (u.uv.length == 0)
      {                               /* no user values? */
        markobjectN(g, u.metatable); /* mark its metatable */
        set2black(u);                 /* nothing else to mark */
        break;
      }
      /* else... */
    } /* FALLTHROUGH */
    case LUA_VLCL:
    case LUA_VCCL:
    case LUA_VTABLE:
    case LUA_VTHREAD:
    case LUA_VPROTO:
    {
      linkgclist_(getgclist(o), g.gray); /* to be visited later */
      break;
    }
    default:
      assert false;
      break;
    }
  }

  /*
  ** mark metamethods for basic types
  */
  private static void markmt(global_State g)
  {
    int i;
    for (i = 0; i < LUA_NUMTYPES; i++)
      markobjectN(g, g.mt[i]);
  }

  /*
  ** mark all objects in list of being-finalized
  */
  private static void markbeingfnz(global_State g)
  {
    GCObject o;
    for (o = g.tobefnz.next; o != null; o = o.next)
      markobject(g, o);
  }

  /*
  ** For each non-marked thread, simulates a barrier between each open
  ** upvalue and its value. (If the thread is collected, the value will be
  ** assigned to the upvalue, but then it can be too late for the barrier
  ** to act. The "barrier" does not need to check colors: A non-marked
  ** thread must be young; upvalues cannot be older than their threads; so
  ** any visited upvalue must be young too.) Also removes the thread from
  ** the list, as it was already visited. Removes also threads with no
  ** upvalues, as they have nothing to be checked. (If the thread gets an
  ** upvalue later, it will be linked in the list again.)
  */
  private static void remarkupvals(global_State g)
  {
    lua_State thread;
    lua_State prev = null;
    while ((thread = (prev != null) ? prev.twups : g.twups) != null)
    {
      if (!iswhite(thread) && thread.openupval != null)
        prev = thread; /* keep marked thread with upvalues in the list */
      else
      { /* thread is not marked or without upvalues */
        UpVal uv;
        assert !isold(thread) || thread.openupval == null;
        if (prev != null)
          prev.twups = thread.twups; /* remove thread from the list */
        else
          g.twups = thread.twups;
        thread.twups = thread; /* mark that it is out of list */
        for (uv = thread.openupval; uv != null; uv = uv.opennext)
        {
          assert getage(uv) <= getage(thread);
          if (!iswhite(uv))
          { /* upvalue already visited? */
            assert upisopen(uv) && isgray(uv);
            TValue uvv = upvalgetval(uv);
            markvalue(g, uvv); /* mark its value */
          }
        }
      }
    }
  }

  private static void cleargraylists(global_State g)
  {
    g.gray.gclist = g.grayagain.gclist = null;
    g.weak.gclist = g.allweak.gclist = g.ephemeron.gclist = null;
  }

  /*
  ** mark root set and reset all gray lists, to start a new collection.
  ** 'GCmarked' is initialized to count the total number of live bytes
  ** during a cycle.
  */
  private static void restartcollection(global_State g)
  {
    cleargraylists(g);
    g.GCmarked = 0;
    markobject(g, mainthread(g));
    markvalue(g, g.l_registry);
    markmt(g);
    if (g.budgetg != null)
      markobject(g, g.budgetg);
    if (g.budgetth != null)
      markobject(g, g.budgetth); /* (the thread of the callbacks stays while the state points to it) */
    markbeingfnz(g); /* mark any finalizing object left from previous cycle */
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Traverse functions
  ** =======================================================
  */

  /*
  ** Check whether object 'o' should be kept in the 'grayagain' list for
  ** post-processing by 'correctgraylist'. (It could put all old objects
  ** in the list and leave all the work to 'correctgraylist', but it is
  ** more efficient to avoid adding elements that will be removed.) Only
  ** TOUCHED1 objects need to be in the list. TOUCHED2 doesn't need to go
  ** back to a gray list, but then it must become OLD. (That is what
  ** 'correctgraylist' does when it finds a TOUCHED2 object.)
  ** This function is a no-op in incremental mode, as objects cannot be
  ** marked as touched in that mode.
  */
  private static void genlink(global_State g, GCObject o)
  {
    assert isblack(o);
    if (getage(o) == G_TOUCHED1)
    {                                 /* touched in this cycle? */
      linkgclist_(getgclist(o), g.grayagain); /* link it back in 'grayagain' */
    } /* everything else do not need to be linked back */
    else if (getage(o) == G_TOUCHED2)
      setage(o, G_OLD); /* advance age */
  }

  /*
  ** Traverse a table with weak values and link it to proper list. During
  ** propagate phase, keep it in 'grayagain' list, to be revisited in the
  ** atomic phase. In the atomic phase, if table has any white value,
  ** put it in 'weak' list, to be cleared; otherwise, call 'genlink'
  ** to check table age in generational mode.
  */
  private static void traverseweakvalue(global_State g, Table h)
  {
    int i, nsize = sizenode(h);
    /* if there is array part, assume it may have white values (it is not
       worth traversing it now just to check) */
    boolean hasclears = h.array_prim.length > 0;
    for (i = 0; i < nsize; i++)
    {                       /* traverse hash part */
      Node n = gnode(h, i);
      if (isempty(n)) /* entry is empty? */
        clearkey(n);        /* clear its key */
      else
      {
        assert !keyisnil(n);
        markkey(g, n);
        if (!hasclears && iscleared(g, gcvalueN(n))) /* a white value? */
          hasclears = true;                                   /* table will have to be cleared */
      }
    }
    if (g.gcstate == GCSpropagate)
      linkgclist_(h, g.grayagain); /* must retraverse it in atomic phase */
    else if (hasclears)
      linkgclist_(h, g.weak); /* has to be cleared later */
    else
    {
      assert novariant(h.tt) >= LUA_TSTRING;
      genlink(g, h);
    }
  }

  /*
  ** Traverse the array part of a table.
  */
  private static boolean traversearray(global_State g, Table h)
  {
    int asize = h.asize();
    boolean marked = false; /* true if some object is marked in this traversal */
    int i;
    for (i = 0; i < asize; i++)
    {
      GCObject o = gcvalarr(h, i);
      if (o != null && iswhite(o))
      {
        marked = true;
        reallymarkobject(g, o);
      }
    }
    return marked;
  }

  /*
  ** Traverse an ephemeron table and link it to proper list. Returns true
  ** iff any object was marked during this traversal (which implies that
  ** convergence has to continue). During propagation phase, keep table
  ** in 'grayagain' list, to be visited again in the atomic phase. In
  ** the atomic phase, if table has any white->white entry, it has to
  ** be revisited during ephemeron convergence (as that key may turn
  ** black). Otherwise, if it has any white key, table has to be cleared
  ** (in the atomic phase). In generational mode, some tables
  ** must be kept in some gray list for post-processing; this is done
  ** by 'genlink'.
  */
  private static boolean traverseephemeron(global_State g, Table h, boolean inv)
  {
    boolean hasclears = false; /* true if table has white keys */
    boolean hasww = false;     /* true if table has entry "white-key -> white-value" */
    int i;
    int nsize = sizenode(h);
    boolean marked = traversearray(g, h); /* traverse array part */
    /* traverse hash part; if 'inv', traverse descending
       (see 'convergeephemerons') */
    for (i = 0; i < nsize; i++)
    {
      Node n = inv ? gnode(h, nsize - 1 - i) : gnode(h, i);
      if (isempty(n)) /* entry is empty? */
        clearkey(n);        /* clear its key */
      else if (iscleared(g, gckeyN(n)))
      {                          /* key is not marked (yet)? */
        hasclears = true;           /* table must be cleared */
        if (iscollectable(n) && iswhite(gcvalue(n)))
        {           /* value not marked yet? */
          hasww = true; /* white-white entry */
          if (g.ephidx != null)
            ephwait(g, gckeyN(n), gcvalue(n)); /* (converging: it waits for its key) */
        }
      }
      else if (iscollectable(n) && iswhite(gcvalue(n)))
      { /* value not marked yet? */
        marked = true;
        reallymarkobject(g, gcvalue(n)); /* mark it now */
      }
    }
    /* link table into proper list */
    if (g.ephidx != null)
      linkgclist_(h, g.ephemeron); /* (converging: sorted out at its end) */
    else if (g.gcstate == GCSpropagate)
      linkgclist_(h, g.grayagain); /* must retraverse it in atomic phase */
    else if (hasww)                /* table has white->white entries? */
      linkgclist_(h, g.ephemeron); /* have to propagate again */
    else if (hasclears)            /* table has white keys? */
      linkgclist_(h, g.allweak);   /* may have to clean white keys */
    else
    {
      assert novariant(h.tt) >= LUA_TSTRING;
      genlink(g, h); /* check whether collector still needs to see it */
    }
    return marked;
  }

  private static void traversestrongtable(global_State g, Table h)
  {
    int i, nsize = sizenode(h);
    traversearray(g, h);
    for (i = 0; i < nsize; i++)
    {                       /* traverse hash part */
      Node n = gnode(h, i);
      if (isempty(n)) /* entry is empty? */
        clearkey(n);        /* clear its key */
      else
      {
        assert !keyisnil(n);
        markkey(g, n);
        markvalue(g, n);
      }
    }
    assert novariant(h.tt) >= LUA_TSTRING;
    genlink(g, h);
  }

  /*
  ** (result & 1) iff weak values; (result & 2) iff weak keys.
  */
  private static int getmode(global_State g, Table h)
  {
    TValue mode = gfasttm(g, h.metatable, TM_MODE);
    if (mode == null || !ttisstring(mode))
      return 0; /* ignore non-string modes */
    else
    {
      ByteView smode = tsvalue(mode).data.view();
      boolean weakkey = smode.find('k') != ByteView.npos;
      boolean weakvalue = smode.find('v') != ByteView.npos;
      return ((weakkey ? 1 : 0) << 1) | (weakvalue ? 1 : 0);
    }
  }

  private static long traversetable(global_State g, Table h)
  {
    markobjectN(g, h.metatable);
    switch (getmode(g, h))
    {
    case 0: /* not weak */
      traversestrongtable(g, h);
      break;
    case 1: /* weak values */
      traverseweakvalue(g, h);
      break;
    case 2: /* weak keys */
      traverseephemeron(g, h, false);
      break;
    case 3: /* all weak; nothing to traverse */
      if (g.gcstate == GCSpropagate)
        linkgclist_(h, g.grayagain); /* must visit again its metatable */
      else
        linkgclist_(h, g.allweak); /* must clear collected entries */
      break;
    }
    return 1 + 2 * sizenode(h) + h.asize();
  }

  private static long traverseudata(global_State g, Udata u)
  {
    int i;
    markobjectN(g, u.metatable); /* mark its metatable */
    for (i = 0; i < u.uv.length; i++)
      markvalue(g, u.uv[i]);
    assert novariant(u.tt) >= LUA_TSTRING;
    genlink(g, u);
    return 1 + u.uv.length;
  }

  private static long traverseproto(global_State g, Proto f)
  {
    int i;
    markobjectN(g, f.source);
    for (i = 0; i < f.k.length; i++) /* mark literals */
      markvalue(g, f.k[i]);
    for (i = 0; i < f.upvalues.length; i++) /* mark upvalue names */
      markobjectN(g, f.upvalues[i].name);
    for (i = 0; i < f.p.length; i++) /* mark nested protos */
      markobjectN(g, f.p[i]);
    for (i = 0; i < f.locvars.length; i++) /* mark local-variable names */
      markobjectN(g, f.locvars[i].varname);
    return 1 + f.k.length + f.upvalues.length + f.p.length + f.locvars.length;
  }

  private static long traverseCclosure(global_State g, CClosure cl)
  {
    int i;
    for (i = 0; i < cl.upvalue.length; i++) /* mark its upvalues */
      markvalue(g, cl.upvalue[i]);
    return 1 + cl.upvalue.length;
  }

  private static long traverseLclosure(global_State g, LClosure cl)
  {
    int i;
    markobjectN(g, cl.p); /* mark its prototype */
    for (i = 0; i < cl.upvals.length; i++)
    { /* visit its upvalues */
      UpVal uv = cl.upvals[i];
      markobjectN(g, uv); /* mark upvalue */
    }
    return 1 + cl.upvals.length;
  }

  /*
  ** Traverse a thread, marking the elements in the stack up to its top
  ** and cleaning the rest of the stack in the final traversal. That
  ** ensures that the entire stack have valid (non-dead) objects.
  ** Threads have no barriers. In gen. mode, old threads must be visited
  ** at every cycle, because they might point to young objects.  In inc.
  ** mode, the thread can still be modified before the end of the cycle,
  ** and therefore it must be visited again in the atomic phase. To ensure
  ** these visits, threads must return to a gray list if they are not new
  ** (which can only happen in generational mode) or if the traverse is in
  ** the propagate phase (which can only happen in incremental mode).
  */
  private static long traversethread(global_State g, lua_State th)
  {
    UpVal uv;
    int o = 0;
    if (isold(th) || g.gcstate == GCSpropagate)
      linkgclist_(th, g.grayagain); /* insert into 'grayagain' list */
    if (th.stack_prim.length == 0)
      return 0; /* stack not completely built yet */
    assert g.gcstate == GCSatomic ||
               th.openupval == null || isintwups(th);
    for (; o < th.top; o++) /* mark live elements in the stack */
    {
      TValue v = getslot(th, o);
      markvalue(g, v);
    }
    for (uv = th.openupval; uv != null; uv = uv.opennext)
      markobject(g, uv); /* open upvalues cannot be collected */
    if (th.frozenchild != null)
      markobject(g, th.frozenchild); /* the coroutine a frozen thread resumed */
    if (th.frozenparent != null)
      markobject(g, th.frozenparent); /* a frozen chain lives and dies as one */
    if (th.bown != null)
      markobject(g, th.bown); /* its budget groups */
    if (th.beff != null)
      markobject(g, th.beff);
    if (g.gcstate == GCSatomic)
    { /* final traversal? */
      if (!g.gcemergency)
        luaD_shrinkstack(th); /* do not change stack in emergency cycle */
      for (o = th.top; o < th.stack_last + EXTRA_STACK; o++)
        setslottag(th, o, LUA_VNIL); /* clear dead stack slice */
      /* 'remarkupvals' may have removed thread from 'twups' list */
      if (!isintwups(th) && th.openupval != null)
      {
        th.twups = g.twups; /* link it back to the list */
        g.twups = th;
      }
    }
    return 1 + (long) th.top;
  }

  /*
  ** traverse one gray object, turning it to black. Return an estimate
  ** of the number of slots traversed.
  */
  private static long propagatemark(global_State g)
  {
    GCObject o = g.gray.gclist;
    nw2black(o);
    g.gray.gclist = getgclist(o).gclist; /* remove from 'gray' list */
    switch (o.tt)
    {
    case LUA_VTABLE:
      return traversetable(g, (Table) o);
    case LUA_VUSERDATA:
      return traverseudata(g, (Udata) o);
    case LUA_VLCL:
      return traverseLclosure(g, (LClosure) o);
    case LUA_VCCL:
      return traverseCclosure(g, (CClosure) o);
    case LUA_VPROTO:
      return traverseproto(g, (Proto) o);
    case LUA_VTHREAD:
      return traversethread(g, (lua_State) o);
    default:
      assert false;
      return 0;
    }
  }

  private static void propagateall(global_State g)
  {
    while (g.gray.gclist != null)
      propagatemark(g);
  }

  /*
  ** The marks of the ephemeron tables converge in one propagation, in time
  ** linear in their entries and in what they mark: an entry whose key is
  ** not marked yet, and whose value is not either, waits in an index by its
  ** key's hash (the one each object is given when made, not its address),
  ** and when its key is marked, its value is. Stock goes over every table
  ** again until nothing changes, which a chain of entries makes quadratic:
  ** a program could hold off a freeze for long in the atomic phase. The
  ** index is memory counted by the host; if the host does not allow more,
  ** the entries that do not fit have their values marked: what those hold
  ** lives one cycle more (an entry is cleared as ever when its key is not
  ** marked).
  */
  private static final int EPH_SLOT = 16; /* (what a slot counts: a key and a value) */
  private static final int EPH_READY = 8;

  private static long ephroom(int cap)
  {
    return (long) cap * EPH_SLOT + (long) (cap / 2) * EPH_READY;
  }

  /* the objects that can be keys of entries that wait (strings are never) */
  private static boolean ephkey(GCObject o)
  {
    switch (o.tt)
    {
    case LUA_VTABLE:
    case LUA_VUSERDATA:
    case LUA_VLCL:
    case LUA_VCCL:
    case LUA_VTHREAD:
    case LUA_VLCF:
      return true;
    default:
      return false;
    }
  }

  private static void ephput(EphIndex x, GCObject key, GCObject value)
  {
    int mask = x.cap - 1;
    int i = key.hash & mask;
    while (x.keys[i] != null)
      i = (i + 1) & mask;
    x.keys[i] = key;
    x.values[i] = value;
  }

  /* the index grows to 'cap' slots, if the host allows it */
  private static boolean ephgrow(global_State g, EphIndex x, int cap)
  {
    if (!g.frealloc.permit(x.cap != 0 ? ephroom(x.cap) : 0, ephroom(cap), 0))
      return false;
    GCObject[] oldkeys = x.keys;
    GCObject[] oldvalues = x.values;
    int oldcap = x.cap;
    x.keys = new GCObject[cap];
    x.values = new GCObject[cap];
    x.ready = Arrays.copyOf(x.ready, cap / 2);
    x.cap = cap;
    for (int i = 0; i < oldcap; i++)
      if (oldkeys[i] != null)
        ephput(x, oldkeys[i], oldvalues[i]);
    return true;
  }

  /* an entry whose key and value are not marked waits for its key */
  private static void ephwait(global_State g, GCObject key, GCObject value)
  {
    EphIndex x = g.ephidx;
    if (!x.strong && 2 * (x.n + 1) > x.cap && !ephgrow(g, x, x.cap != 0 ? 2 * x.cap : 64))
      x.strong = true;
    if (x.strong)
      reallymarkobject(g, value); /* (no room: it lives one cycle more) */
    else
    {
      ephput(x, key, value);
      x.n++;
    }
  }

  /* 'o' is being marked: if entries wait for it, they are ready */
  private static void ephmarked(global_State g, GCObject o)
  {
    EphIndex x = g.ephidx;
    if (x.n == 0 || !ephkey(o))
      return;
    int mask = x.cap - 1;
    for (int i = o.hash & mask; x.keys[i] != null; i = (i + 1) & mask)
      if (x.keys[i] == o)
      {
        x.ready[x.nready++] = o; /* (a key is marked once: no more than its entries) */
        return;
      }
  }

  /* marks propagate, and the values of the entries whose keys were marked are marked, until none is left */
  private static void ephdrain(global_State g)
  {
    EphIndex x = g.ephidx;
    for (;;)
    {
      if (g.gray.gclist != null)
        propagatemark(g);
      else if (x.nready > 0)
      {
        GCObject k = x.ready[--x.nready];
        int mask = x.cap - 1;
        for (int i = k.hash & mask; x.keys[i] != null; i = (i + 1) & mask)
          if (x.keys[i] == k && iswhite(x.values[i]))
            reallymarkobject(g, x.values[i]);
      }
      else
        break;
    }
  }

  private static void convergeephemerons(global_State g)
  {
    if (g.ephemeron.gclist == null)
      return;
    EphIndex x = new EphIndex();
    g.ephidx = x;
    GCObject w;
    GCObject next = g.ephemeron.gclist;
    g.ephemeron.gclist = null;
    while ((w = next) != null)
    { /* each table: an entry marks its value, or waits for its key */
      assert w.tt == LUA_VTABLE;
      Table h = (Table) w;
      next = h.gclist;
      nw2black(h);
      traverseephemeron(g, h, false);
    }
    ephdrain(g); /* (the tables it finds wait too) */
    g.ephidx = null;
    if (x.cap != 0)
      g.frealloc.permit(ephroom(x.cap), 0, 0); /* (its vectors go with it) */
    /* the tables go to their lists as stock leaves them (all is marked) */
    next = g.ephemeron.gclist;
    g.ephemeron.gclist = null;
    while ((w = next) != null)
    {
      Table h = (Table) w;
      next = h.gclist;
      nw2black(h);
      boolean marked = traverseephemeron(g, h, false);
      assert !marked;
    }
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Sweep Functions
  ** =======================================================
  */

  /*
  ** clear entries with unmarked keys from all weaktables in list 'l'
  */
  private static void clearbykeys(global_State g, GCObject l)
  {
    for (; l != null; l = ((Table) l).gclist)
    {
      assert l.tt == LUA_VTABLE;
      Table h = (Table) l;
      int i, nsize = sizenode(h);
      for (i = 0; i < nsize; i++)
      {
        Node n = gnode(h, i);
        if (iscleared(g, gckeyN(n))) /* unmarked key? */
          setempty(n);         /* remove entry */
        if (isempty(n))        /* is entry empty? */
          clearkey(n);               /* clear its key */
      }
    }
  }

  /*
  ** clear entries with unmarked values from all weaktables in list 'l' up
  ** to element 'f'
  */
  private static void clearbyvalues(global_State g, GCObject l, GCObject f)
  {
    for (; l != f; l = ((Table) l).gclist)
    {
      assert l.tt == LUA_VTABLE;
      Table h = (Table) l;
      int i, nsize = sizenode(h);
      int asize = h.asize();
      for (i = 0; i < asize; i++)
      {
        GCObject o = gcvalarr(h, i);
        if (iscleared(g, o))             /* value was collected? */
          setArrTag(h, i, LUA_VEMPTY); /* remove entry */
      }
      for (i = 0; i < nsize; i++)
      {
        Node n = gnode(h, i);
        if (iscleared(g, gcvalueN(n))) /* unmarked value? */
          setempty(n);                 /* remove entry */
        if (isempty(n))                /* is entry empty? */
          clearkey(n);                       /* clear its key */
      }
    }
  }

  private static void freeupval(lua_State L, UpVal uv)
  {
    if (upisopen(uv))
      uv.luaF_unlinkupval();
    luaM_free_UpVal(L, uv);
  }

  private static void freeobj(lua_State L, GCObject o)
  {
    long newmem = gettotalbytes(L.l_G) - objsize(o);
    switch (o.tt)
    {
    case LUA_VPROTO:
      ((Proto) o).luaF_freeproto(L);
      break;
    case LUA_VUPVAL:
      freeupval(L, (UpVal) o);
      break;
    case LUA_VBUDGET:
      freegroup(L, (BudgetGroup) o);
      luaM_free_BudgetGroup(L, (BudgetGroup) o);
      break;
    case LUA_VLCL:
    {
      LClosure cl = (LClosure) o;
      luaM_alloc_UpVal_ptr_span(L.l_G, L, cl.upvals, 0, false);
      luaM_free_LClosure(L, cl);
      break;
    }
    case LUA_VCCL:
    {
      CClosure cl = (CClosure) o;
      luaM_alloc_TValue_span(L.l_G, L, cl.upvalue, 0, false);
      luaM_free_CClosure(L, cl);
      break;
    }
    case LUA_VTABLE:
      ((Table) o).luaH_free(L);
      break;
    case LUA_VTHREAD:
      luaE_freethread(L, (lua_State) o);
      break;
    case LUA_VUSERDATA:
    {
      Udata u = (Udata) o;
      luaM_alloc_TValue_span(L.l_G, L, u.uv, 0, false);
      if (u.data.held())
      {
        luaM_alloc_char_span(L.l_G, L, u.data, 0, false);
        u.data = new ByteOwner();
      }
      if (u.obj != null)
        u.obj.freed(L);
      if (u.objsize != 0)
        luaM_freeobj(L, u.objsize);
      luaM_free_Udata(L, u);
      break;
    }
    case LUA_VSHRSTR:
    {
      assert novariant(o.tt) == LUA_TSTRING;
      TString ts = (TString) o;
      ts.luaS_remove(L);
      luaM_free_TString(L, ts, strisexternal(ts) ? 0 : ts.data.size());
      break;
    }
    case LUA_VLCF:
    {
      CFuncInfo cfi = (CFuncInfo) o;
      luaM_free_CFuncInfo(L, cfi);
      break;
    }
    default:
      assert false;
    }
    assert gettotalbytes(L.l_G) == newmem;
  }

  private static GCObject sweeplist(lua_State L, GCObject p, long countin)
  {
    global_State g = L.l_G;
    byte ow = otherwhite(g);
    byte white = luaC_white(g); /* current white */
    while (p.next != null && countin-- > 0)
    {
      GCObject curr = p.next;
      byte marked = curr.marked;
      if (isdeadm(ow, marked))
      {                   /* is 'curr' dead? */
        p.next = curr.next;  /* remove 'curr' from list */
        freeobj(L, curr); /* erase 'curr' */
      }
      else
      { /* change mark to 'white' and age to 'new' */
        curr.marked = (byte) ((marked & ~maskgcbits) | white | G_NEW);
        p = curr; /* go to next element */
      }
    }
    return (p.next == null) ? null : p;
  }

  /*
  ** sweep a list until a live object (or end of list)
  */
  private static GCObject sweeptolive(lua_State L, GCObject p)
  {
    GCObject old = p;
    do
    {
      p = sweeplist(L, p, 1);
    } while (p == old);
    return p;
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Finalization
  ** =======================================================
  */

  /*
  ** If possible, shrink string table.
  */
  private static void checkSizes(lua_State L, global_State g)
  {
  }

  /*
  ** Get the next udata to be finalized from the 'tobefnz' list, and
  ** link it back into the 'allgc' list.
  */
  private static GCObject udata2finalize(global_State g)
  {
    GCObject o = g.tobefnz.next; /* get first element */
    assert tofinalize(o);
    g.tobefnz.next = o.next; /* remove it from 'tobefnz' list */
    o.next = g.allgc.next;   /* return it to 'allgc' list */
    g.allgc.next = o;
    o.marked &= (byte) (~bitmask(FINALIZEDBIT)); /* object is "normal" again */
    if (issweepphase(g))
      makewhite(g, o); /* "sweep" object */
    else if (getage(o) == G_OLD1)
      g.firstold1 = o; /* it is the first OLD1 object in the list */
    return o;
  }

  private static void dothecall(lua_State L)
  {
    luaD_callnoyield(L, L.top - 2, 0);
  }

  private static final class DoTheCall implements ProtectedCall
  {
    @Override
    public void run(lua_State L)
    {
      dothecall(L);
    }
  }

  /*
  ** The collector calls the '__gc' of a kernel or host object (one with no
  ** owner) at once, here, as stock Lua does, with the budget of the running
  ** thread lifted: no budget point inside it, so nothing can freeze there
  ** and the C frames under it need no continuation. What it registers in the
  ** meantime is for the kernel too.
  */
  private static void GCTM(lua_State L)
  {
    global_State g = L.l_G;
    TValue tm;
    TValue v = new TValue();
    assert !g.gcemergency;
    setgcovalue(L, v, udata2finalize(g));
    tm = luaT_gettmbyobj(L, v, TM_GC);
    if (!notm(tm))
    { /* is there a finalizer? */
      byte status;
      boolean oldah = L.allowhook;
      byte oldgcstp = g.gcstp;
      BudgetGroup oldbeff = L.beff;
      g.gcstp |= GCSTPGC;           /* avoid GC steps */
      L.allowhook = false;              /* stop debug hooks during GC metamethod */
      if (oldbeff != null)
        luaE_setbeff(L, null);    /* the budget is lifted */
      setslot(L, L.top++, tm);   /* push finalizer... */
      setslot(L, L.top++, v);   /* ... and its argument */
      L.ci.callstatus |= CIST_FIN; /* will run a finalizer */
      DoTheCall call = new DoTheCall();
      L.nfz++; /* a finalizer cannot freeze */
      status = luaD_pcall(L, call, L.top - 2, 0);
      L.nfz--;
      L.ci.callstatus &= ~CIST_FIN; /* not running a finalizer anymore */
      if (oldbeff != null)
        luaE_setbeff(L, oldbeff);
      L.allowhook = oldah;           /* restore hooks */
      g.gcstp = oldgcstp;            /* restore state */
      if (status != LUA_OK)
      { /* error while running __gc? */
        luaE_warnerror(L, Lt.mm_gc);
        L.top--; /* pops error object */
      }
    }
  }

  /*
  ** The '__gc' of a user object waits in the queue of its owner group, and
  ** a thread of the group calls them, one after the other, in this C
  ** function ('luaD_drain' calls it at a restartable point, and
  ** 'collectgarbage' after a collection). Each is a protected call with a
  ** continuation, so it is ordinary user code: it can freeze, and an error
  ** in it is a warning, as in stock Lua; the object is not tried again.
  */

  private static void drainfinish(lua_State L, int status)
  {
    if (status != LUA_OK && status != LUA_YIELD)
    { /* error while running __gc? */
      lua_lock(L); /* (this runs in a C function: the warning expects the lock) */
      luaE_warnerror(L, Lt.mm_gc);
      lua_unlock(L);
      L.top--; /* pops error object */
    }
  }

  /* after a thaw: 'oldah' is the 'allowhook' of the thread before the calls */
  private static int gcdrain_cont(lua_State L, int status, long ctx)
  {
    drainfinish(L, status);
    return gcdrain_run(L, ctx != 0);
  }

  private static final lua_KFunction gcdrain_cont = Lgc::gcdrain_cont;

  static
  {
    lua_registerk("gc.gcdrain_cont", gcdrain_cont);
  }

  private static int gcdrain_run(lua_State L, boolean oldah)
  {
    L.allowhook = false; /* stop debug hooks during GC metamethod (and no nested drain) */
    lua_freezeonly(L); /* a '__gc' can freeze but not yield, as in stock Lua */
    for (;;)
    {
      BudgetGroup bg = L.beff;
      if (bg == null || bg.fnz.next == null)
        break;
      TValue v = new TValue();
      setgcovalue(L, v, luaC_dequeue(L, bg));
      TValue tm = luaT_gettmbyobj(L, v, TM_GC);
      if (notm(tm))
        continue; /* (it lost its '__gc') */
      setslot(L, L.top++, tm);
      setslot(L, L.top++, v);
      L.ci.callstatus |= CIST_FIN; /* will run a finalizer */
      drainfinish(L, lua_pcallk(L, 1, 0, 0, oldah ? 1 : 0, gcdrain_cont));
    }
    L.ci.callstatus &= ~CIST_FIN;
    L.allowhook = oldah; /* (the drain can run where hooks are off: in a hook) */
    return 0;
  }

  private static int gcdrain(lua_State L)
  {
    return gcdrain_run(L, L.allowhook);
  }

  private static final lua_CFunction gcdrain = Lgc::gcdrain;

  private static final CFuncInfo cfi_gcdrain = new CFuncInfo(gcdrain, "gc.gcdrain");

  /* the function that calls the waiting '__gc' (hooks do not see it) */
  public static lua_CFunction luaC_drainfunction()
  {
    return gcdrain;
  }

  /* push the function that calls the '__gc' waiting in the group of 'L' */
  public static void luaC_pushdrain(lua_State L)
  {
    setslotf(L, L.top, cfi_gcdrain);
    L.top++;
  }

  /* do the '__gc' of the group in effect wait? */
  public static boolean luaC_hasdrain(lua_State L)
  {
    return L.beff != null && L.beff.fnz.next != null;
  }

  /*
  ** call all pending finalizers
  */
  private static void callallpendingfinalizers(lua_State L)
  {
    global_State g = L.l_G;
    while (g.tobefnz.next != null)
      GCTM(L);
  }

  /*
  ** find last 'next' field in list 'p' list (to add elements in its end)
  */
  private static GCObject findlast(GCObject p)
  {
    while (p.next != null)
      p = p.next;
    return p;
  }

  /*
  ** The group that owns an object (nullptr: none, or it is gone).
  */
  private static BudgetGroup ownergroup(global_State g, int id)
  {
    return (id < g.groupbyid.length) ? g.groupbyid[id] : null;
  }

  /*
  ** Separate the dead objects with a '__gc' from the 'finobj' list. A kernel
  ** or host object (no owner) goes to the end of 'tobefnz', and the
  ** collector calls its '__gc'. A user object goes to the end of the queue
  ** of its group, and a thread of the group calls its '__gc' (see
  ** 'luaC_drain'). An object whose group is gone is just dead, without
  ** its '__gc': it goes back to the ordinary list, to be swept.
  */
  private static void separatetobefnz(global_State g, boolean all)
  {
    GCObject curr;
    GCObject p = g.finobj;
    GCObject lastnext = findlast(g.tobefnz);
    while ((curr = p.next) != g.finobjold1)
    { /* traverse all finalizable objects */
      assert tofinalize(curr);
      if (!(iswhite(curr) || all)) /* not being collected? */
        p = curr;           /* don't bother with it */
      else
      {
        if (curr == g.finobjsur)    /* removing 'finobjsur'? */
          g.finobjsur = curr.next; /* correct it */
        p.next = curr.next;             /* remove 'curr' from 'finobj' list */
        BudgetGroup bg = (curr.owner != 0) ? ownergroup(g, curr.owner) : null;
        if (curr.owner == 0)
        { /* link at the end of 'tobefnz' list */
          curr.next = lastnext.next;
          lastnext.next = curr;
          lastnext = curr;
        }
        else if (bg != null)
        { /* link at the end of the queue of its group */
          curr.next = null;
          if (bg.fnztail != null)
            bg.fnztail.next = curr;
          else
            bg.fnz.next = curr;
          bg.fnztail = curr;
        }
        else
        { /* its group is gone: an ordinary object again */
          curr.marked &= (byte) (~bitmask(FINALIZEDBIT));
          curr.next = g.allgc.next;
          g.allgc.next = curr;
        }
      }
    }
  }

  /*
  ** Make the objects 'objs' (n of them, marked for finalization) due now, as
  ** if the collector had found them dead, to run in this order: a kernel or
  ** host object goes to the end of 'tobefnz', a user object to the end of the
  ** queue of its group, and one whose group is gone is an ordinary object
  ** again (see 'separatetobefnz').
  */
  public static void luaC_makedue(lua_State L, GCObject[] objs, int n)
  {
    global_State g = L.l_G;
    IdentityHashMap<GCObject, Integer> order = new IdentityHashMap<>();
    for (int i = 0; i < n; i++)
      order.putIfAbsent(objs[i], i);
    GCObject[] taken = new GCObject[n];
    GCObject p = g.finobj;
    GCObject curr;
    while ((curr = p.next) != g.finobjold1)
    { /* take them out of the 'finobj' list */
      Integer it = order.get(curr);
      if (it == null)
        p = curr;
      else
      {
        if (curr == g.finobjsur)
          g.finobjsur = curr.next;
        p.next = curr.next;
        taken[it] = curr;
      }
    }
    GCObject lastnext = findlast(g.tobefnz);
    for (GCObject o : taken)
    {
      if (o == null)
        continue; /* (not marked, or not in the list the collector is sweeping) */
      BudgetGroup bg = (o.owner != 0) ? ownergroup(g, o.owner) : null;
      if (o.owner == 0)
      {
        o.next = lastnext.next;
        lastnext.next = o;
        lastnext = o;
      }
      else if (bg != null)
      { /* (white, as the collector keeps its queues: see 'whitequeues') */
        o.marked = (byte) ((o.marked & ~maskgcbits) | bitmask(FINALIZEDBIT) | luaC_white(g));
        o.next = null;
        if (bg.fnztail != null)
          bg.fnztail.next = o;
        else
          bg.fnz.next = o;
        bg.fnztail = o;
      }
      else
      {
        o.marked &= (byte) (~bitmask(FINALIZEDBIT));
        o.next = g.allgc.next;
        g.allgc.next = o;
      }
    }
  }

  /*
  ** Put objects 'objs' of the young part of 'finobj' (marked for
  ** finalization) in the order given, as if they had been marked in it, the
  ** last one last ('objs[n - 1]' at the head of the list). A restore marks
  ** some objects as it meets them, in another order.
  */
  public static void luaC_ordermarked(lua_State L, GCObject[] objs, int n)
  {
    global_State g = L.l_G;
    IdentityHashMap<GCObject, Integer> where = new IdentityHashMap<>();
    for (int i = 0; i < n; i++)
      where.putIfAbsent(objs[i], i);
    boolean[] taken = new boolean[n];
    GCObject p = g.finobj;
    GCObject curr;
    while ((curr = p.next) != g.finobjold1)
    { /* take them out of the 'finobj' list */
      Integer it = where.get(curr);
      if (it == null)
        p = curr;
      else
      {
        if (curr == g.finobjsur)
          g.finobjsur = curr.next;
        p.next = curr.next;
        taken[it] = true;
      }
    }
    for (int i = 0; i < n; i++)
      if (taken[i])
      { /* (and back at the head, in order) */
        objs[i].next = g.finobj.next;
        g.finobj.next = objs[i];
      }
  }

  /*
  ** If pointer 'p' points to 'o', move it to the next element.
  */
  private static GCObject checkpointer(GCObject p, GCObject o)
  {
    return (o == p) ? o.next : p;
  }

  /*
  ** Correct pointers to objects inside 'allgc' list when
  ** object 'o' is being removed from the list.
  */
  private static void correctpointers(global_State g, GCObject o)
  {
    g.survival = checkpointer(g.survival, o);
    g.old1 = checkpointer(g.old1, o);
    g.reallyold = checkpointer(g.reallyold, o);
    g.firstold1 = checkpointer(g.firstold1, o);
  }

  /*
  ** if object 'o' has a finalizer, remove it from 'allgc' list (must
  ** search the list to find it) and link it in 'finobj' list.
  */
  public static void luaC_checkfinalizer(lua_State L, GCObject o, Table mt)
  {
    global_State g = L.l_G;
    if (tofinalize(o) ||                 /* obj. is already marked... */
        gfasttm(g, mt, TM_GC) == null || /* or has no finalizer... */
        (g.gcstp & GCSTPCLS) != 0)           /* or closing state? */
      return;                            /* nothing to be done */
    else
    { /* move 'o' to 'finobj' list */
      GCObject p;
      if (issweepphase(g))
      {
        makewhite(g, o);                           /* "sweep" object 'o' */
        if (g.sweepgc == o)                /* should not remove 'sweepgc' object */
          g.sweepgc = sweeptolive(L, g.sweepgc); /* change 'sweepgc' */
      }
      else
        correctpointers(g, o);
      /* search for pointer pointing to 'o' */
      for (p = g.allgc; p.next != o; p = p.next)
      { /* empty */
      }
      p.next = o.next;        /* remove 'o' from 'allgc' list */
      o.next = g.finobj.next; /* link it in 'finobj' list */
      g.finobj.next = o;
      o.marked = (byte) (o.marked | bitmask(FINALIZEDBIT)); /* mark it as such */
      o.owner = (L.beff != null) ? L.beff.id : 0; /* its owner: the group in effect */
    }
  }

  /*
  ** An owner id for the new budget group 'bg': the ids go up, and an id is
  ** not given again while an object of an older group may still carry it
  ** ('ownerskip', made when the ids ran out). False if there is none.
  */
  public static boolean luaC_newowner(lua_State L, BudgetGroup bg)
  {
    global_State g = L.l_G;
    final int MAXOWNER = 65534;
    int id = g.nextownerid;
    while (id <= MAXOWNER && g.ownerskip.get(id))
      id++;
    if (id > MAXOWNER)
    { /* the round is over: skip the ids of live groups and of objects they owned */
      g.ownerskip.clear();
      for (GCObject o = g.finobj.next; o != null; o = o.next)
        g.ownerskip.set(o.owner);
      for (BudgetGroup other : g.groupbyid)
        if (other != null)
          g.ownerskip.set(other.id);
      id = 1;
      while (id <= MAXOWNER && g.ownerskip.get(id))
        id++;
      if (id > MAXOWNER)
        return false;
    }
    if (id >= g.groupbyid.length)
    { /* room for the id (a table by id, growing as the ids do) */
      int n = g.groupbyid.length == 0 ? 16 : g.groupbyid.length * 2;
      while (n <= id)
        n *= 2;
      g.groupbyid = luaM_alloc_BudgetGroup_ptr_span(g, L, g.groupbyid, n, true);
    }
    bg.id = id & 0xFFFF;
    g.groupbyid[id] = bg;
    g.nextownerid = id + 1;
    return true;
  }

  /*
  ** Take the next object of the queue of group 'bg' (a dead object whose
  ** '__gc' has not run), back into the ordinary list, as 'udata2finalize'.
  */
  public static GCObject luaC_dequeue(lua_State L, BudgetGroup bg)
  {
    global_State g = L.l_G;
    GCObject o = bg.fnz.next;
    assert o != null && tofinalize(o);
    bg.fnz.next = o.next;
    if (bg.fnz.next == null)
      bg.fnztail = null;
    o.next = g.allgc.next;
    g.allgc.next = o;
    o.marked &= (byte) (~bitmask(FINALIZEDBIT)); /* an ordinary object again */
    if (issweepphase(g))
      makewhite(g, o); /* "sweep" object (as 'udata2finalize') */
    return o;
  }

  /*
  ** The queues of the budget groups (dead user objects that wait for a thread
  ** of their group to call their '__gc') are not part of the generations nor
  ** of the sweeps. Between cycles their objects are white and new, as swept
  ** objects are ('whitequeues'), so that storing one somewhere goes through
  ** the barriers. In the atomic phase the queue of a group found alive is
  ** marked, as the objects waiting for their finalizers are, before weak
  ** values are cleared ('markgroupqueues'); after the objects that died in
  ** this cycle joined the queues, again; then the queue of a group that
  ** nothing reaches is released ('releasequeues'): nothing could call its
  ** '__gc', which are not called, and its objects are ordinary ones again,
  ** swept if nothing else reaches them.
  */
  private static void markgroupqueues(global_State g)
  {
    for (BudgetGroup bg : g.groupbyid)
      if (bg != null)
        bg.qmarked = false;
    boolean again;
    do
    {
      again = false;
      for (BudgetGroup bg : g.groupbyid)
      {
        if (bg == null || bg.qmarked || iswhite(bg))
          continue; /* (a group that is not alive does not keep its queue) */
        bg.qmarked = true;
        for (GCObject o = bg.fnz.next; o != null; o = o.next)
          markobject(g, o);
        again = true;
      }
      if (again)
      {
        propagateall(g);
        convergeephemerons(g);
      }
    } while (again);
  }

  private static void releasequeues(global_State g)
  {
    for (BudgetGroup bg : g.groupbyid)
    { /* the queues of the groups that are not alive */
      if (bg == null || bg.qmarked || bg.fnz.next == null)
        continue;
      GCObject o = bg.fnz.next;
      while (o != null)
      {
        GCObject next = o.next;
        o.marked &= (byte) (~bitmask(FINALIZEDBIT));
        o.next = g.allgc.next;
        g.allgc.next = o;
        o = next;
      }
      bg.fnz.next = null;
      bg.fnztail = null;
    }
  }

  /* (after the atomic phase flipped the current white) */
  private static void whitequeues(global_State g)
  {
    for (BudgetGroup bg : g.groupbyid)
      if (bg != null)
        for (GCObject o = bg.fnz.next; o != null; o = o.next)
          o.marked = (byte) ((o.marked & ~maskgcbits) | bitmask(FINALIZEDBIT) | luaC_white(g));
  }

  /*
  ** A budget group is being freed: it leaves the table of owners. The
  ** objects it owned that are not dead yet keep its id, and nothing gives
  ** that id again while they may be there (see 'luaC_newowner').
  */
  private static void freegroup(lua_State L, BudgetGroup bg)
  {
    global_State g = L.l_G;
    assert bg.fnz.next == null; /* ('atomic' released it; 'luaC_freeallobjects' deletes it) */
    if (bg.id != 0 && bg.id < g.groupbyid.length && g.groupbyid[bg.id] == bg)
      g.groupbyid[bg.id] = null;
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Generational Collector
  ** =======================================================
  */

  /*
  ** Fields 'GCmarked' and 'GCmajorminor' are used to control the pace and
  ** the mode of the collector. They play several roles, depending on the
  ** mode of the collector:
  ** * KGC_INC:
  **     GCmarked: number of marked bytes during a cycle.
  **     GCmajorminor: not used.
  ** * KGC_GENMINOR
  **     GCmarked: number of bytes that became old since last major collection.
  **     GCmajorminor: number of bytes marked in last major collection.
  ** * KGC_GENMAJOR
  **     GCmarked: number of bytes that became old since last major collection.
  **     GCmajorminor: number of bytes marked in last major collection.
  */

  /*
  ** Set the "time" to wait before starting a new incremental cycle;
  ** cycle will start when number of bytes in use hits the threshold of
  ** approximately (marked * pause / 100).
  */
  private static void setpause(global_State g)
  {
    long threshold = luaO_applyparam(g.gcparams[LUA_GCPPAUSE], g.GCmarked);
    long debt = threshold - gettotalbytes(g);
    if (debt < 0)
      debt = 0;
    luaE_setdebt(g, debt);
  }

  /*
  ** Sweep a list of objects to enter generational mode.  Deletes dead
  ** objects and turns the non dead to old. All non-dead threads---which
  ** are now old---must be in a gray list. Everything else is not in a
  ** gray list. Open upvalues are also kept gray.
  */
  private static void sweep2old(lua_State L, GCObject p)
  {
    GCObject curr;
    global_State g = L.l_G;
    while ((curr = p.next) != null)
    {
      if (iswhite(curr))
      { /* is 'curr' dead? */
        assert isdead(g, curr);
        p.next = curr.next;  /* remove 'curr' from list */
        freeobj(L, curr); /* erase 'curr' */
      }
      else
      { /* all surviving objects become old */
        setage(curr, G_OLD);
        if (curr.tt == LUA_VTHREAD)
        { /* threads must be watched */
          assert curr.tt == LUA_VTHREAD;
          lua_State th = (lua_State) curr;
          linkgclist_(th, g.grayagain); /* insert into 'grayagain' list */
        }
        else if (curr.tt == LUA_VUPVAL && upisopen((UpVal) curr))
          set2gray(curr); /* open upvalues are always gray */
        else              /* everything else is black */
          nw2black(curr);
        p = curr; /* go to next element */
      }
    }
  }

  /*
  ** Sweep for generational mode. Delete dead objects. (Because the
  ** collection is not incremental, there are no "new white" objects
  ** during the sweep. So, any white object must be dead.) For
  ** non-dead objects, advance their ages and clear the color of
  ** new objects. (Old objects keep their colors.)
  ** The ages of G_TOUCHED1 and G_TOUCHED2 objects cannot be advanced
  ** here, because these old-generation objects are usually not swept
  ** here.  They will all be advanced in 'correctgraylist'. That function
  ** will also remove objects turned white here from any gray list.
  */
  private static final class GenSweep
  {
    GCObject last;
    long addedold;

    GenSweep(GCObject last, long addedold)
    {
      this.last = last;
      this.addedold = addedold;
    }
  }

  private static final byte[] nextage = {
      G_SURVIVAL, /* from G_NEW */
      G_OLD1,     /* from G_SURVIVAL */
      G_OLD1,     /* from G_OLD0 */
      G_OLD,      /* from G_OLD1 */
      G_OLD,      /* from G_OLD (do not change) */
      G_TOUCHED1, /* from G_TOUCHED1 (do not change) */
      G_TOUCHED2  /* from G_TOUCHED2 (do not change) */
  };

  private static GenSweep sweepgen(lua_State L, global_State g, GCObject p,
                                   GCObject limit, boolean setfirstold1)
  {
    long addedold = 0;
    int white = luaC_white(g);
    GCObject curr;
    while ((curr = p.next) != limit)
    {
      if (iswhite(curr))
      { /* is 'curr' dead? */
        assert !isold(curr) && isdead(g, curr);
        p.next = curr.next;  /* remove 'curr' from list */
        freeobj(L, curr); /* erase 'curr' */
      }
      else
      { /* correct mark and age */
        int age = getage(curr);
        if (age == G_NEW)
        {                                          /* new objects go back to white */
          int marked = (curr.marked & 0xFF) & ~maskgcbits; /* erase GC bits */
          curr.marked = (byte) (marked | G_SURVIVAL | white);
        }
        else
        {                            /* all other objects will be old, and so keep their color */
          assert age != G_OLD1; /* advanced in 'markold' */
          setage(curr, nextage[age]);
          if (getage(curr) == G_OLD1)
          {
            addedold += objsize(curr); /* bytes becoming old */
            if (setfirstold1 && g.firstold1 == null)
              g.firstold1 = curr; /* first OLD1 object in the list */
          }
        }
        p = curr; /* go to next element */
      }
    }
    return new GenSweep(p, addedold);
  }

  /*
  ** Correct a list of gray objects. Return a pointer to the last element
  ** left on the list, so that we can link another list to the end of
  ** this one.
  ** Because this correction is done after sweeping, young objects might
  ** be turned white and still be in the list. They are only removed.
  ** 'TOUCHED1' objects are advanced to 'TOUCHED2' and remain on the list;
  ** Non-white threads also remain on the list. 'TOUCHED2' objects and
  ** anything else become regular old, are marked black, and are removed
  ** from the list.
  */
  private static GCListed correctgraylist(GCListed p)
  {
    GCObject curr;
    while ((curr = p.gclist) != null)
    {
      GCListed next = getgclist(curr);
      if (iswhite(curr))
        p.gclist = next.gclist; /* remove all white objects */
      else if (getage(curr) == G_TOUCHED1)
      { /* touched in this cycle? */
        assert isgray(curr);
        nw2black(curr); /* make it black, for next barrier */
        setage(curr, G_TOUCHED2);
        p = next; /* keep it in the list and go to next element */
      }
      else if (curr.tt == LUA_VTHREAD)
      {
        assert isgray(curr);
        p = next; /* keep non-white threads on the list */
      }
      else
      {                                 /* everything else is removed */
        assert isold(curr);        /* young objects should be white here */
        if (getage(curr) == G_TOUCHED2) /* advance from TOUCHED2... */
          setage(curr, G_OLD);          /* ... to OLD */
        nw2black(curr);                 /* make object black (to be removed) */
        p.gclist = next.gclist;
      }
    }
    return p;
  }

  /*
  ** Correct all gray lists, coalescing them into 'grayagain'.
  */
  private static void correctgraylists(global_State g)
  {
    GCListed list = correctgraylist(g.grayagain);
    list.gclist = g.weak.gclist;
    g.weak.gclist = null;
    list = correctgraylist(list);
    list.gclist = g.allweak.gclist;
    g.allweak.gclist = null;
    list = correctgraylist(list);
    list.gclist = g.ephemeron.gclist;
    g.ephemeron.gclist = null;
    correctgraylist(list);
  }

  /*
  ** Mark black 'OLD1' objects when starting a new young collection.
  ** Gray objects are already in some gray list, and so will be visited in
  ** the atomic step.
  */
  private static void markold(global_State g, GCObject from, GCObject to)
  {
    GCObject p;
    for (p = from; p != to; p = p.next)
    {
      if (getage(p) == G_OLD1)
      {
        assert !iswhite(p);
        setage(p, G_OLD); /* now they are old */
        if (isblack(p))
          reallymarkobject(g, p);
      }
    }
  }

  /*
  ** Finish a young-generation collection.
  */
  private static void finishgencycle(lua_State L, global_State g)
  {
    correctgraylists(g);
    checkSizes(L, g);
    g.gcstate = GCSpropagate; /* skip restart */
    if (g.tobefnz.next != null && !g.gcemergency && luaD_checkminstack(L))
      callallpendingfinalizers(L);
  }

  /*
  ** Shifts from a minor collection to major collections. It starts in
  ** the "sweep all" state to clear all objects, which are mostly black
  ** in generational mode.
  */
  private static void minor2inc(lua_State L, global_State g, byte kind)
  {
    g.GCmajorminor = g.GCmarked; /* number of live bytes */
    g.gckind = kind;
    g.reallyold = g.old1 = g.survival = null;
    g.finobjrold = g.finobjold1 = g.finobjsur = null;
    entersweep(L); /* continue as an incremental cycle */
    /* set a debt equal to the step size */
    luaE_setdebt(g, luaO_applyparam(g.gcparams[LUA_GCPSTEPSIZE], 100));
  }

  /*
  ** Decide whether to shift to major mode. It shifts if the accumulated
  ** number of added old bytes (counted in 'GCmarked') is larger than
  ** 'minormajor'% of the number of lived bytes after the last major
  ** collection. (This number is kept in 'GCmajorminor'.)
  */
  private static boolean checkminormajor(global_State g)
  {
    long limit = luaO_applyparam(g.gcparams[LUA_GCPMINORMAJOR], g.GCmajorminor);
    if (limit == 0)
      return false; /* special case: 'minormajor' 0 stops major collections */
    return g.GCmarked >= limit;
  }

  /*
  ** Does a young collection. First, mark 'OLD1' objects. Then does the
  ** atomic step. Then, check whether to continue in minor mode. If so,
  ** sweep all lists and advance pointers. Finally, finish the collection.
  */
  private static void youngcollection(lua_State L, global_State g)
  {
    long addedold1 = 0;
    long marked = g.GCmarked; /* preserve 'g->GCmarked' */
    GCObject psurvival;       /* to point to first non-dead survival object */
    assert g.gcstate == GCSpropagate;
    if (g.firstold1 != null)
    {                                         /* are there regular OLD1 objects? */
      markold(g, g.firstold1, g.reallyold); /* mark them */
      g.firstold1 = null;                    /* no more OLD1 objects (for now) */
    }
    markold(g, g.finobj.next, g.finobjrold);
    markold(g, g.tobefnz.next, null);

    atomic(L); /* will lose 'g->marked' */

    /* sweep nursery and get a pointer to its last live element */
    g.gcstate = GCSswpallgc;
    GenSweep gs = sweepgen(L, g, g.allgc, g.survival, true);
    psurvival = gs.last;
    addedold1 += gs.addedold;
    /* sweep 'survival' */
    addedold1 += sweepgen(L, g, psurvival, g.old1, true).addedold;
    g.reallyold = g.old1;
    g.old1 = psurvival.next;   /* 'survival' survivals are old now */
    g.survival = g.allgc.next; /* all news are survivals */

    /* repeat for 'finobj' lists */
    gs = sweepgen(L, g, g.finobj, g.finobjsur, false);
    psurvival = gs.last;
    addedold1 += gs.addedold;
    /* sweep 'survival' */
    addedold1 += sweepgen(L, g, psurvival, g.finobjold1, false).addedold;
    g.finobjrold = g.finobjold1;
    g.finobjold1 = psurvival.next; /* 'survival' survivals are old now */
    g.finobjsur = g.finobj.next;   /* all news are survivals */

    addedold1 += sweepgen(L, g, g.tobefnz, null, false).addedold;

    /* keep total number of added old1 bytes */
    g.GCmarked = marked + addedold1;

    /* decide whether to shift to major mode */
    if (checkminormajor(g))
    {
      minor2inc(L, g, (byte) KGC_GENMAJOR); /* go to major mode */
      g.GCmarked = 0;               /* avoid pause in first major cycle (see 'setpause') */
    }
    else
      finishgencycle(L, g); /* still in minor mode; finish it */
  }

  /*
  ** Clears all gray lists, sweeps objects, and prepare sublists to enter
  ** generational mode. The sweeps remove dead objects and turn all
  ** surviving objects to old. Threads go back to 'grayagain'; everything
  ** else is turned black (not in any gray list).
  */
  private static void atomic2gen(lua_State L, global_State g)
  {
    cleargraylists(g);
    /* sweep all elements making them old */
    g.gcstate = GCSswpallgc;
    sweep2old(L, g.allgc);
    /* everything alive now is old */
    g.reallyold = g.old1 = g.survival = g.allgc.next;
    g.firstold1 = null; /* there are no OLD1 objects anywhere */

    /* repeat for 'finobj' lists */
    sweep2old(L, g.finobj);
    g.finobjrold = g.finobjold1 = g.finobjsur = g.finobj.next;

    sweep2old(L, g.tobefnz);

    g.gckind = KGC_GENMINOR;
    g.GCmajorminor = g.GCmarked; /* "base" for number of bytes */
    g.GCmarked = 0;               /* to count the number of added old1 bytes */
    finishgencycle(L, g);
  }

  /*
  ** Set debt for the next minor collection, which will happen when
  ** total number of bytes grows 'genminormul'% in relation to
  ** the base, GCmajorminor, which is the number of bytes being used
  ** after the last major collection.
  */
  private static void setminordebt(global_State g)
  {
    luaE_setdebt(g, luaO_applyparam(g.gcparams[LUA_GCPMINORMUL], g.GCmajorminor));
  }

  /*
  ** Enter generational mode. Must go until the end of an atomic cycle
  ** to ensure that all objects are correctly marked and weak tables
  ** are cleared. Then, turn all objects into old and finishes the
  ** collection.
  */
  private static void entergen(lua_State L, global_State g)
  {
    luaC_runtilstate(L, GCSpause, true);     /* prepare to start a new cycle */
    luaC_runtilstate(L, GCSpropagate, true); /* start new cycle */
    atomic(L);                            /* propagates all and then do the atomic stuff */
    atomic2gen(L, g);
    setminordebt(g); /* set debt assuming next cycle will be minor */
  }

  /*
  ** Change collector mode to 'newmode'.
  */
  public static void luaC_changemode(lua_State L, int newmode)
  {
    global_State g = L.l_G;
    if (g.gckind == KGC_GENMAJOR) /* doing major collections? */
      g.gckind = KGC_INC;         /* already incremental but in name */
    if (newmode != g.gckind)
    {                             /* does it need to change? */
      if (newmode == KGC_INC)     /* entering incremental mode? */
        minor2inc(L, g, (byte) KGC_INC); /* entering incremental mode */
      else
      {
        assert newmode == KGC_GENMINOR;
        entergen(L, g);
      }
    }
  }

  /*
  ** Does a full collection in generational mode.
  */
  private static void fullgen(lua_State L, global_State g)
  {
    minor2inc(L, g, (byte) KGC_INC);
    entergen(L, g);
  }

  /*
  ** After an atomic incremental step from a major collection,
  ** check whether collector could return to minor collections.
  ** It checks whether the number of bytes 'tobecollected'
  ** is greater than 'majorminor'% of the number of bytes added
  ** since the last collection ('addedbytes').
  */
  private static boolean checkmajorminor(lua_State L, global_State g)
  {
    if (g.gckind == KGC_GENMAJOR)
    { /* generational mode? */
      long numbytes = gettotalbytes(g);
      long addedbytes = numbytes - g.GCmajorminor;
      long limit = (addedbytes < 0) ? 0
                                     : luaO_applyparam(g.gcparams[LUA_GCPMAJORMINOR], addedbytes);
      long tobecollected = numbytes - g.GCmarked;
      if (tobecollected > limit)
      {
        atomic2gen(L, g); /* return to generational mode */
        setminordebt(g);
        return true; /* exit incremental collection */
      }
    }
    g.GCmajorminor = g.GCmarked; /* prepare for next collection */
    return false;                      /* stay doing incremental collections */
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** GC control
  ** =======================================================
  */

  /*
  ** Enter first sweep phase.
  ** The call to 'sweeptolive' makes the pointer point to an object
  ** inside the list (instead of to the header), so that the real sweep do
  ** not need to skip objects created between "now" and the start of the
  ** real sweep.
  */
  private static void entersweep(lua_State L)
  {
    global_State g = L.l_G;
    g.gcstate = GCSswpallgc;
    assert g.sweepgc == null;
    g.sweepgc = sweeptolive(L, g.allgc);
  }

  /*
  ** Delete all objects in list 'p' until (but not including) object
  ** 'limit'.
  */
  private static void deletelist(lua_State L, GCObject p, GCObject limit)
  {
    while (p != limit)
    {
      GCObject next = p.next;
      freeobj(L, p);
      p = next;
    }
  }

  /*
  ** Call all finalizers of the objects in the given Lua state, and
  ** then free all objects, except for the main thread.
  */
  public static void luaC_freeallobjects(lua_State L)
  {
    global_State g = L.l_G;
    g.gcstp = GCSTPCLS; /* no extra finalizers after here */
    luaC_changemode(L, KGC_INC);
    separatetobefnz(g, true); /* separate all objects with finalizers */
    assert g.finobj.next == null;
    callallpendingfinalizers(L);
    for (BudgetGroup bg : g.groupbyid)
    { /* what user objects wait for is not called (their threads are not running) */
      if (bg == null)
        continue;
      deletelist(L, bg.fnz.next, null);
      bg.fnz.next = null;
      bg.fnztail = null;
    }
    assert novariant(mainthread(g).tt) >= LUA_TSTRING;
    deletelist(L, g.allgc.next, mainthread(g));
    assert g.finobj.next == null;   /* no new finalizers */
    deletelist(L, g.fixedgc, null); /* collect fixed objects */
  }

  private static void atomic(lua_State L)
  {
    global_State g = L.l_G;
    GCObject origweak, origall;
    GCObject grayagain = g.grayagain.gclist; /* save original list */
    g.grayagain.gclist = null;
    assert g.ephemeron.gclist == null && g.weak.gclist == null;
    assert !iswhite(mainthread(g));
    g.gcstate = GCSatomic;
    markobject(g, L); /* mark running thread */
    /* registry and global metatables may be changed by API */
    markvalue(g, g.l_registry);
    markmt(g);       /* mark global metatables */
    if (g.budgetg != null)
      markobject(g, g.budgetg); /* the group whose callback runs */
    propagateall(g); /* empties 'gray' list */
    /* remark occasional upvalues of (maybe) dead threads */
    remarkupvals(g);
    propagateall(g); /* propagate changes */
    g.gray.gclist = grayagain;
    propagateall(g); /* traverse 'grayagain' list */
    convergeephemerons(g);
    markgroupqueues(g); /* the objects waiting for a group's '__gc' */
    /* at this point, all strongly accessible objects are marked. */
    /* Clear values from weak tables, before checking finalizers */
    clearbyvalues(g, g.weak.gclist, null);
    clearbyvalues(g, g.allweak.gclist, null);
    origweak = g.weak.gclist;
    origall = g.allweak.gclist;
    separatetobefnz(g, false); /* separate objects to be finalized */
    markbeingfnz(g);       /* mark objects that will be finalized */
    propagateall(g);       /* remark, to propagate 'resurrection' */
    convergeephemerons(g);
    markgroupqueues(g);    /* the queues again, with those that died now */
    releasequeues(g);      /* and those of the groups that are not alive */
    for (BudgetGroup bg : g.groupbyid)
      if (bg != null && bg.root != null && iswhite(bg.root))
        bg.root = null; /* (the thread a group was set on is dead: a weak reference) */
    /* at this point, all resurrected objects are marked. */
    /* remove dead objects from weak tables */
    clearbykeys(g, g.ephemeron.gclist); /* clear keys from all ephemeron */
    clearbykeys(g, g.allweak.gclist);   /* clear keys from all 'allweak' */
    /* clear values from resurrected weak tables */
    clearbyvalues(g, g.weak.gclist, origweak);
    clearbyvalues(g, g.allweak.gclist, origall);
    g.currentwhite = otherwhite(g); /* flip current white */
    whitequeues(g);
    assert g.gray.gclist == null;
  }

  /*
  ** Do a sweep step. The normal case (not fast) sweeps at most GCSWEEPMAX
  ** elements. The fast case sweeps the whole list.
  */
  private static void sweepstep(lua_State L, global_State g,
                                byte nextstate, GCObject nextlist, boolean fast)
  {
    if (g.sweepgc != null)
      g.sweepgc = sweeplist(L, g.sweepgc, fast ? MAX_LMEM : GCSWEEPMAX);
    else
    { /* enter next state */
      g.gcstate = nextstate;
      g.sweepgc = nextlist;
    }
  }

  /*
  ** Performs one incremental "step" in an incremental garbage collection.
  ** For indivisible work, a step goes to the next state. When marking
  ** (propagating), a step traverses one object. When sweeping, a step
  ** sweeps GCSWEEPMAX objects, to avoid a big overhead for sweeping
  ** objects one by one. (Sweeping is inexpensive, no matter the
  ** object.) When 'fast' is true, 'singlestep' tries to finish a state
  ** "as fast as possible". In particular, it skips the propagation
  ** phase and leaves all objects to be traversed by the atomic phase:
  ** That avoids traversing twice some objects, such as threads and
  ** weak tables.
  */

  private static final int step2pause = -3; /* finished collection; entered pause state */
  private static final int atomicstep = -2; /* atomic step */
  private static final int step2minor = -1; /* moved to minor collections */

  private static long singlestep(lua_State L, boolean fast)
  {
    global_State g = L.l_G;
    long stepresult;
    assert !g.gcstopem; /* collector is not reentrant */
    g.gcstopem = true;          /* no emergency collections while collecting */
    switch (g.gcstate)
    {
    case GCSpause:
    {
      restartcollection(g);
      g.gcstate = GCSpropagate;
      stepresult = 1;
      break;
    }
    case GCSpropagate:
    {
      if (fast || g.gray.gclist == null)
      {
        g.gcstate = GCSenteratomic; /* finish propagate phase */
        stepresult = 1;
      }
      else
        stepresult = propagatemark(g); /* traverse one gray object */
      break;
    }
    case GCSenteratomic:
    {
      atomic(L);
      if (checkmajorminor(L, g))
        stepresult = step2minor;
      else
      {
        entersweep(L);
        stepresult = atomicstep;
      }
      break;
    }
    case GCSswpallgc:
    { /* sweep "regular" objects */
      sweepstep(L, g, (byte) GCSswpfinobj, g.finobj, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswpfinobj:
    { /* sweep objects with finalizers */
      sweepstep(L, g, (byte) GCSswptobefnz, g.tobefnz, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswptobefnz:
    { /* sweep objects to be finalized */
      sweepstep(L, g, (byte) GCSswpend, null, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswpend:
    { /* finish sweeps */
      checkSizes(L, g);
      g.gcstate = GCScallfin;
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCScallfin:
    { /* call finalizers */
      if (g.tobefnz.next != null && !g.gcemergency && luaD_checkminstack(L))
      {
        g.gcstopem = false; /* ok collections during finalizers */
        GCTM(L);         /* call one finalizer */
        stepresult = CWUFIN;
      }
      else
      {                        /* no more finalizers or emergency mode or not enough stack
                                  to run finalizers */
        g.gcstate = GCSpause; /* finish collection */
        stepresult = step2pause;
      }
      break;
    }
    default:
      assert false;
      return 0;
    }
    g.gcstopem = false;
    return stepresult;
  }

  /*
  ** Advances the garbage collector until it reaches the given state.
  ** (The option 'fast' is only for testing; in normal code, 'fast'
  ** here is always true.)
  */
  public static void luaC_runtilstate(lua_State L, int state, boolean fast)
  {
    global_State g = L.l_G;
    assert g.gckind == KGC_INC;
    while (state != g.gcstate)
      singlestep(L, fast);
  }

  /*
  ** Performs a basic incremental step. The step size is
  ** converted from bytes to "units of work"; then the function loops
  ** running single steps until adding that many units of work or
  ** finishing a cycle (pause state). Finally, it sets the debt that
  ** controls when next step will be performed.
  */
  private static void incstep(lua_State L, global_State g)
  {
    long stepsize = luaO_applyparam(g.gcparams[LUA_GCPSTEPSIZE], 100);
    long work2do = luaO_applyparam(g.gcparams[LUA_GCPSTEPMUL], stepsize / Cost.Ref);
    long stres;
    boolean fast = work2do == 0; /* special case: do a full collection */
    do
    {                              /* repeat until enough work */
      stres = singlestep(L, fast); /* perform one single step */
      if (stres == step2minor)     /* returned to minor collections? */
        return;                    /* nothing else to be done here */
      else if (stres == step2pause || (stres == atomicstep && !fast))
        break; /* end of cycle or atomic */
      else
        work2do -= stres;
    } while (fast || work2do > 0);
    if (g.gcstate == GCSpause)
      setpause(g); /* pause until next cycle */
    else
      luaE_setdebt(g, stepsize);
  }

  /*
  ** Performs a basic GC step if collector is running. (If collector was
  ** stopped by the user, set a reasonable debt to avoid it being called
  ** at every single check.)
  */
  public static void luaC_step(lua_State L)
  {
    global_State g = L.l_G;
    assert !g.gcemergency;
    if (!gcrunning(g))
    {                          /* not running? */
      if ((g.gcstp & GCSTPUSR) != 0) /* stopped by the user? */
        luaE_setdebt(g, 20000);
    }
    else
    {
      luai_tracegc(L, true); /* for internal debugging */
      switch (g.gckind)
      {
      case KGC_INC:
      case KGC_GENMAJOR:
        incstep(L, g);
        break;
      case KGC_GENMINOR:
        youngcollection(L, g);
        setminordebt(g);
        break;
      }
      luai_tracegc(L, false); /* for internal debugging */
    }
  }

  /*
  ** Perform a full collection in incremental mode.
  ** Before running the collection, check 'keepinvariant'; if it is true,
  ** there may be some objects marked as black, so the collector has
  ** to sweep all objects to turn them back to white (as white has not
  ** changed, nothing will be collected).
  */
  private static void fullinc(lua_State L, global_State g)
  {
    if (keepinvariant(g)) /* black objects? */
      entersweep(L);      /* sweep everything to turn them back to white */
    /* finish any pending sweep phase to start a new cycle */
    luaC_runtilstate(L, GCSpause, true);
    luaC_runtilstate(L, GCScallfin, true); /* run up to finalizers */
    luaC_runtilstate(L, GCSpause, true);   /* finish collection */
    setpause(g);
  }

  /*
  ** Performs a full GC cycle; if 'isemergency', set a flag to avoid
  ** some operations which could change the interpreter state in some
  ** unexpected ways (running finalizers and shrinking some structures).
  */
  public static void luaC_fullgc(lua_State L, boolean isemergency)
  {
    global_State g = L.l_G;
    assert !g.gcemergency;
    g.gcemergency = isemergency; /* set flag */
    switch (g.gckind)
    {
    case KGC_GENMINOR:
      fullgen(L, g);
      break;
    case KGC_INC:
      fullinc(L, g);
      break;
    case KGC_GENMAJOR:
      g.gckind = KGC_INC;
      fullinc(L, g);
      g.gckind = KGC_GENMAJOR;
      break;
    }
    g.gcemergency = false;
  }

  /*
  ** A whole cycle that makes no '__gc' call: the calls it finds due (and
  ** those a cycle under way had found) are left to the collector's next
  ** step, which comes at the next allocation, as those of any cycle are
  ** (a user object's go to its budget group, made at its points). Dead
  ** objects are freed and weak tables cleared now. In generational mode it
  ** is a major collection, which is incremental: the collector goes on with
  ** it, and returns to minor collections as after any.
  */
  public static void luaC_fullgcdue(lua_State L)
  {
    global_State g = L.l_G;
    assert !g.gcemergency;
    boolean gen = (g.gckind != KGC_INC);
    if (g.gckind == KGC_GENMINOR)
      minor2inc(L, g, (byte) KGC_INC);
    g.gckind = KGC_INC; /* (the cycle runs whole: see 'checkmajorminor') */
    g.gcemergency = true;  /* (a cycle under way finishes calling none) */
    if (keepinvariant(g))
      entersweep(L);
    luaC_runtilstate(L, GCSpause, true);
    g.gcemergency = false;
    luaC_runtilstate(L, GCScallfin, true); /* (the '__gc' calls are next) */
    if (gen)
      g.gckind = KGC_GENMAJOR;
    luaE_setdebt(g, 0);
  }

  /* }====================================================== */
}
