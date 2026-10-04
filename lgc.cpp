/*
** $Id: lgc.c $
** Garbage Collector
** See Copyright Notice in lua.h
*/


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "literals.h"

/*
** Maximum number of elements to sweep in each single step.
** (Large enough to dissipate fixed overheads but small enough
** to allow small steps for the collector.)
*/
constexpr int GCSWEEPMAX = 20;

/*
** Cost (in work units) of running one finalizer.
*/
constexpr int CWUFIN = 10;

/* mask with all color bits */
constexpr int maskcolors = bitmask(BLACKBIT) | WHITEBITS;

/* mask with all GC bits */
constexpr int maskgcbits = maskcolors | AGEBITS;







static void reallymarkobject(global_State* g, GCObject* o);
static void atomic(lua_State* L);
static void entersweep(lua_State* L);

static inline void makewhite(global_State* g, GCObject* x)
{
  x->marked = ((x->marked & ~maskcolors) | luaC_white(g));
}

/* make an object gray (neither white nor black) */
static inline void set2gray(GCObject* x)
{
  x->marked = (x->marked & ~maskcolors);
}

/* make an object black (coming from any color) */
static inline void set2black(GCObject* x)
{
  x->marked = ((x->marked & ~WHITEBITS) | bitmask(BLACKBIT));
}

static inline int keyiswhite(const Node* n)
{
  return (n->key_tt & BIT_ISCOLLECTABLE) && iswhite(gckey(n));
}

static inline GCObject* gcvalueN(const TValue* o)
{
  return (o->tt_ & BIT_ISCOLLECTABLE) ? gcvalue(o) : nullptr;
}

static inline GCObject* gcvalarr(Table* t, int i)
{
  return (getArrTag(t, i) & BIT_ISCOLLECTABLE) ? getArrVal(t, i).ref : nullptr;
}

static inline void markvalue(global_State* g, TValue* o)
{
  checkliveness(mainthread(g), o);
  if ((o->tt_ & BIT_ISCOLLECTABLE) && iswhite(gcvalue(o)))
    reallymarkobject(g, gcvalue(o));
}

static inline void markkey(global_State* g, Node* n)
{
  if (keyiswhite(n))
    reallymarkobject(g, gckey(n));
}

static inline void markobject(global_State* g, GCObject* t)
{
  if (iswhite(t))
    reallymarkobject(g, t);
}

static inline void markobjectN(global_State* g, GCObject* t)
{
  if (t)
    markobject(g, t);
}

/*
** {======================================================
** Generic functions
** =======================================================
*/

/* one after last element in a hash array */
static inline Node* gnodelast(Table* h)
{
  return gnode(h, sizenode(h));
}

static l_mem objsize(GCObject* o)
{
  l_mem res;
  switch (o->tt)
  {
  case LUA_VTABLE:
  {
    res = static_cast<Table*>(o)->luaH_size();
    break;
  }
  case LUA_VLCL:
  {
    LClosure* cl = static_cast<LClosure*>(o);
    res = sizeLclosure((int)cl->upvals.size());
    break;
  }
  case LUA_VCCL:
  {
    CClosure* cl = static_cast<CClosure*>(o);
    res = sizeCclosure((int)cl->upvalue.size());
    break;
  }
  case LUA_VUSERDATA:
  {
    Udata* u = static_cast<Udata*>(o);
    res = cost::Udata + u->uv.size() * cost::TValue + u->data.size();
    break;
  }
  case LUA_VPROTO:
  {
    res = static_cast<Proto*>(o)->luaF_protosize();
    break;
  }
  case LUA_VTHREAD:
  {
    res = luaE_threadsize(static_cast<lua_State*>(o));
    break;
  }
  case LUA_VSHRSTR:
  {
    assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
    TString* ts = static_cast<TString*>(o);
    res = cost::TString + (strisexternal(ts) ? 0 : ts->data.size());
    break;
  }
  case LUA_VUPVAL:
  {
    res = cost::UpVal;
    break;
  }
  case LUA_VLCF:
  {
    res = cost::CFuncInfo;
    break;
  }
  default:
    res = 0;
    assert(0);
  }
  return (res);
}

static GCListed* getgclist(GCObject* o)
{
  assert(o->tt == LUA_VTABLE || o->tt == LUA_VLCL || o->tt == LUA_VCCL ||
         o->tt == LUA_VTHREAD || o->tt == LUA_VPROTO ||
         (o->tt == LUA_VUSERDATA && static_cast<Udata*>(o)->uv.size() > 0));
  return static_cast<GCListed*>(o);
}

/*
** Link a collectable object 'o' with a known type into the list 'p'.
** (Must be a macro to access the 'gclist' field in different types.)
*/

static void linkgclist_(GCListed* o, GCListed* list)
{
  assert(!isgray(o)); /* cannot be in a gray list */
  o->gclist = list->gclist;
  list->gclist = o;
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
static void clearkey(Node* n)
{
  assert(((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL));
  if (((n)->key_tt & BIT_ISCOLLECTABLE))
    ((n)->key_tt = LUA_TDEADKEY); /* unused key; remove it */
}

/*
** tells whether a key or value can be cleared from a weak
** table. Non-collectable objects are never removed from weak
** tables. Strings behave as 'values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
static int iscleared(global_State* g, GCObject* o)
{
  if (o == nullptr)
    return 0; /* non-collectable value */
  else if (((o->tt) & NOVARIANT_MASK) == LUA_TSTRING)
  {
    markobject(g, o);
    return 0;
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
void luaC_barrier_(lua_State* L, GCObject* o, GCObject* v)
{
  global_State* g = L->l_G;
  assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g))
  {                         /* must keep invariant? */
    reallymarkobject(g, v); /* restore invariant */
    if (isold(o))
    {
      assert(!isold(v)); /* white object could not be old */
      setage(v, G_OLD0);     /* restore generational invariant */
    }
  }
  else
  { /* sweep phase */
    assert(issweepphase(g));
    if (g->gckind != KGC_GENMINOR) /* incremental mode? */
      makewhite(g, o);             /* mark 'o' as white to avoid other barriers */
  }
}

/*
** barrier that moves collector backward, that is, mark the black object
** pointing to a white object as gray again.
*/
void luaC_barrierback_(lua_State* L, GCObject* o)
{
  global_State* g = L->l_G;
  assert(isblack(o) && !isdead(g, o));
  assert((g->gckind != KGC_GENMINOR) || (isold(o) && getage(o) != G_TOUCHED1));
  if (getage(o) == G_TOUCHED2) /* already in gray list? */
    set2gray(o);               /* make it gray to become touched1 */
  else                         /* link it in 'grayagain' and paint it gray */
    linkgclist_(getgclist(o), &g->grayagain);
  if (isold(o))            /* generational mode? */
    setage(o, G_TOUCHED1); /* touched in current cycle */
}

void luaC_fix(lua_State* L, GCObject* o)
{
  global_State* g = L->l_G;
  assert(g->allgc.next == o); /* object must be 1st in 'allgc' list! */
  set2gray(o);               /* they will be gray forever */
  setage(o, G_OLD);          /* and old forever */
  g->allgc.next = o->next;        /* remove object from 'allgc' list */
  o->next = g->fixedgc;      /* link it to 'fixedgc' list */
  g->fixedgc = o;
}

/*
** link an already-allocated collectable object (of given type) into
** the 'allgc' list.
*/
GCObject* luaC_link(lua_State* L, GCObject* o, lu_byte tt)
{
  global_State* g = L->l_G;
  o->marked = luaC_white(g);
  o->tt = tt;
  o->next = g->allgc.next;
  g->allgc.next = o;
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
static void reallymarkobject(global_State* g, GCObject* o)
{
  g->GCmarked += objsize(o);
  switch (o->tt)
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
  case LUA_VUPVAL:
  {
    UpVal* uv = static_cast<UpVal*>(o);
    if (upisopen(uv))
      set2gray(uv); /* open upvalues are kept gray */
    else
      set2black(uv);        /* closed upvalues are visited here */
    TValue uvv = upvalgetval(uv);
    markvalue(g, &uvv); /* mark its content */
    break;
  }
  case LUA_VUSERDATA:
  {
    Udata* u = static_cast<Udata*>(o);
    if (u->uv.size() == 0)
    {                               /* no user values? */
      markobjectN(g, u->metatable); /* mark its metatable */
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
    linkgclist_(getgclist(o), &g->gray); /* to be visited later */
    break;
  }
  default:
    assert(0);
    break;
  }
}

/*
** mark metamethods for basic types
*/
static void markmt(global_State* g)
{
  int i;
  for (i = 0; i < LUA_NUMTYPES; i++)
    markobjectN(g, g->mt[i]);
}

/*
** mark all objects in list of being-finalized
*/
static void markbeingfnz(global_State* g)
{
  GCObject* o;
  for (o = g->tobefnz.next; o != nullptr; o = o->next)
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
static void remarkupvals(global_State* g)
{
  lua_State* thread;
  lua_State* prev = nullptr;
  while ((thread = (prev != nullptr) ? prev->twups : g->twups) != nullptr)
  {
    if (!iswhite(thread) && thread->openupval != nullptr)
      prev = thread; /* keep marked thread with upvalues in the list */
    else
    { /* thread is not marked or without upvalues */
      UpVal* uv;
      assert(!isold(thread) || thread->openupval == nullptr);
      if (prev != nullptr)
        prev->twups = thread->twups; /* remove thread from the list */
      else
        g->twups = thread->twups;
      thread->twups = thread; /* mark that it is out of list */
      for (uv = thread->openupval; uv != nullptr; uv = uv->opennext)
      {
        assert(getage(uv) <= getage(thread));
        if (!iswhite(uv))
        { /* upvalue already visited? */
          assert(upisopen(uv) && isgray(uv));
          TValue uvv = upvalgetval(uv);
          markvalue(g, &uvv); /* mark its value */
        }
      }
    }
  }
}

static void cleargraylists(global_State* g)
{
  g->gray.gclist = g->grayagain.gclist = nullptr;
  g->weak.gclist = g->allweak.gclist = g->ephemeron.gclist = nullptr;
}

/*
** mark root set and reset all gray lists, to start a new collection.
** 'GCmarked' is initialized to count the total number of live bytes
** during a cycle.
*/
static void restartcollection(global_State* g)
{
  cleargraylists(g);
  g->GCmarked = 0;
  markobject(g, mainthread(g));
  markvalue(g, &g->l_registry);
  markmt(g);
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
static void genlink(global_State* g, GCObject* o)
{
  assert(isblack(o));
  if (getage(o) == G_TOUCHED1)
  {                                 /* touched in this cycle? */
    linkgclist_(getgclist(o), &g->grayagain); /* link it back in 'grayagain' */
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
static void traverseweakvalue(global_State* g, Table* h)
{
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = (h->array.size() > 0);
  for (n = gnode(h, 0); n < limit; n++)
  {                       /* traverse hash part */
    if (((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* entry is empty? */
      clearkey(n);        /* clear its key */
    else
    {
      assert(!((n)->key_tt == LUA_TNIL));
      markkey(g, n);
      if (!hasclears && iscleared(g, gcvalueN((&(n)->val)))) /* a white value? */
        hasclears = 1;                                   /* table will have to be cleared */
    }
  }
  if (g->gcstate == GCSpropagate)
    linkgclist_(h, &g->grayagain); /* must retraverse it in atomic phase */
  else if (hasclears)
    linkgclist_(h, &g->weak); /* has to be cleared later */
  else
  {
    assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
    genlink(g, h);
  }
}

/*
** Traverse the array part of a table.
*/
static int traversearray(global_State* g, Table* h)
{
  int asize = h->asize();
  int marked = 0; /* true if some object is marked in this traversal */
  int i;
  for (i = 0; i < asize; i++)
  {
    GCObject* o = gcvalarr(h, i);
    if (o != nullptr && iswhite(o))
    {
      marked = 1;
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
static int traverseephemeron(global_State* g, Table* h, int inv)
{
  int hasclears = 0; /* true if table has white keys */
  int hasww = 0;     /* true if table has entry "white-key -> white-value" */
  int i;
  int nsize = sizenode(h);
  int marked = traversearray(g, h); /* traverse array part */
  /* traverse hash part; if 'inv', traverse descending
     (see 'convergeephemerons') */
  for (i = 0; i < nsize; i++)
  {
    Node* n = inv ? gnode(h, nsize - 1 - i) : gnode(h, i);
    if (((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* entry is empty? */
      clearkey(n);        /* clear its key */
    else if (iscleared(g, (((n)->key_tt & BIT_ISCOLLECTABLE) ? gckey(n) : nullptr)))
    {                          /* key is not marked (yet)? */
      hasclears = 1;           /* table must be cleared */
      if ((n->val.tt_ & BIT_ISCOLLECTABLE) && iswhite(gcvalue(&n->val))) /* value not marked yet? */
        hasww = 1;             /* white-white entry */
    }
    else if ((n->val.tt_ & BIT_ISCOLLECTABLE) && iswhite(gcvalue(&n->val)))
    { /* value not marked yet? */
      marked = 1;
      reallymarkobject(g, gcvalue(&n->val)); /* mark it now */
    }
  }
  /* link table into proper list */
  if (g->gcstate == GCSpropagate)
    linkgclist_(h, &g->grayagain); /* must retraverse it in atomic phase */
  else if (hasww)                /* table has white->white entries? */
    linkgclist_(h, &g->ephemeron); /* have to propagate again */
  else if (hasclears)            /* table has white keys? */
    linkgclist_(h, &g->allweak);   /* may have to clean white keys */
  else
  {
    assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
    genlink(g, h); /* check whether collector still needs to see it */
  }
  return marked;
}

static void traversestrongtable(global_State* g, Table* h)
{
  Node *n, *limit = gnodelast(h);
  traversearray(g, h);
  for (n = gnode(h, 0); n < limit; n++)
  {                       /* traverse hash part */
    if (((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL)) /* entry is empty? */
      clearkey(n);        /* clear its key */
    else
    {
      assert(!((n)->key_tt == LUA_TNIL));
      markkey(g, n);
      markvalue(g, (&(n)->val));
    }
  }
  assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  genlink(g, h);
}

/*
** (result & 1) iff weak values; (result & 2) iff weak keys.
*/
static int getmode(global_State* g, Table* h)
{
  const TValue* mode = gfasttm(g, h->metatable, TM_MODE);
  if (mode == nullptr || !(((mode)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
    return 0; /* ignore non-string modes */
  else
  {
    byte_view smode = tsvalue(mode)->data.view();
    bool weakkey = smode.find('k') != byte_view::npos;
    bool weakvalue = smode.find('v') != byte_view::npos;
    return (((int)(weakkey)) << 1) | ((int)(weakvalue));
  }
}

static l_mem traversetable(global_State* g, Table* h)
{
  markobjectN(g, h->metatable);
  switch (getmode(g, h))
  {
  case 0: /* not weak */
    traversestrongtable(g, h);
    break;
  case 1: /* weak values */
    traverseweakvalue(g, h);
    break;
  case 2: /* weak keys */
    traverseephemeron(g, h, 0);
    break;
  case 3: /* all weak; nothing to traverse */
    if (g->gcstate == GCSpropagate)
      linkgclist_(h, &g->grayagain); /* must visit again its metatable */
    else
      linkgclist_(h, &g->allweak); /* must clear collected entries */
    break;
  }
  return (1 + 2 * sizenode(h) + h->asize());
}

static l_mem traverseudata(global_State* g, Udata* u)
{
  int i;
  markobjectN(g, u->metatable); /* mark its metatable */
  for (i = 0; i < (int)u->uv.size(); i++)
    markvalue(g, &u->uv[i]);
  assert((u->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  genlink(g, u);
  return 1 + (int)u->uv.size();
}

static l_mem traverseproto(global_State* g, Proto* f)
{
  int i;
  markobjectN(g, f->source);
  for (i = 0; i < (int)f->k.size(); i++) /* mark literals */
    markvalue(g, &f->k[i]);
  for (i = 0; i < (int)f->upvalues.size(); i++) /* mark upvalue names */
    markobjectN(g, f->upvalues[i].name);
  for (i = 0; i < (int)f->p.size(); i++) /* mark nested protos */
    markobjectN(g, f->p[i]);
  for (i = 0; i < (int)f->locvars.size(); i++) /* mark local-variable names */
    markobjectN(g, f->locvars[i].varname);
  return 1 + (int)f->k.size() + (int)f->upvalues.size() + (int)f->p.size() + (int)f->locvars.size();
}

static l_mem traverseCclosure(global_State* g, CClosure* cl)
{
  int i;
  for (i = 0; i < (int)cl->upvalue.size(); i++) /* mark its upvalues */
    markvalue(g, &cl->upvalue[i]);
  return 1 + (int)cl->upvalue.size();
}

static l_mem traverseLclosure(global_State* g, LClosure* cl)
{
  int i;
  markobjectN(g, cl->p); /* mark its prototype */
  for (i = 0; i < (int)cl->upvals.size(); i++)
  { /* visit its upvalues */
    UpVal* uv = cl->upvals[i];
    markobjectN(g, uv); /* mark upvalue */
  }
  return 1 + (int)cl->upvals.size();
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
static l_mem traversethread(global_State* g, lua_State* th)
{
  UpVal* uv;
  StkId o = 0;
  if (isold(th) || g->gcstate == GCSpropagate)
    linkgclist_(th, &g->grayagain); /* insert into 'grayagain' list */
  if (th->stack.data() == nullptr)
    return 0; /* stack not completely built yet */
  assert(g->gcstate == GCSatomic ||
             th->openupval == nullptr || isintwups(th));
  for (; o < th->top; o++) /* mark live elements in the stack */
  {
    TValue v = getslot(th, o);
    markvalue(g, &v);
  }
  for (uv = th->openupval; uv != nullptr; uv = uv->opennext)
    markobject(g, uv); /* open upvalues cannot be collected */
  if (g->gcstate == GCSatomic)
  { /* final traversal? */
    if (!g->gcemergency)
      luaD_shrinkstack(th); /* do not change stack in emergency cycle */
    for (o = th->top; o < th->stack_last + EXTRA_STACK; o++)
      setslottag(th, o, LUA_VNIL); /* clear dead stack slice */
    /* 'remarkupvals' may have removed thread from 'twups' list */
    if (!isintwups(th) && th->openupval != nullptr)
    {
      th->twups = g->twups; /* link it back to the list */
      g->twups = th;
    }
  }
  return 1 + ((l_mem)(th->top));
}

/*
** traverse one gray object, turning it to black. Return an estimate
** of the number of slots traversed.
*/
static l_mem propagatemark(global_State* g)
{
  GCObject* o = g->gray.gclist;
  nw2black(o);
  g->gray.gclist = getgclist(o)->gclist; /* remove from 'gray' list */
  switch (o->tt)
  {
  case LUA_VTABLE:
    return traversetable(g, static_cast<Table*>(o));
  case LUA_VUSERDATA:
    return traverseudata(g, static_cast<Udata*>(o));
  case LUA_VLCL:
    return traverseLclosure(g, static_cast<LClosure*>(o));
  case LUA_VCCL:
    return traverseCclosure(g, static_cast<CClosure*>(o));
  case LUA_VPROTO:
    return traverseproto(g, static_cast<Proto*>(o));
  case LUA_VTHREAD:
    return traversethread(g, static_cast<lua_State*>(o));
  default:
    assert(0);
    return 0;
  }
}

static void propagateall(global_State* g)
{
  while (g->gray.gclist)
    propagatemark(g);
}

/*
** Traverse all ephemeron tables propagating marks from keys to values.
** Repeat until it converges, that is, nothing new is marked. 'dir'
** inverts the direction of the traversals, trying to speed up
** convergence on chains in the same table.
*/
static void convergeephemerons(global_State* g)
{
  int changed;
  int dir = 0;
  do
  {
    GCObject* w;
    GCObject* next = g->ephemeron.gclist; /* get ephemeron list */
    g->ephemeron.gclist = nullptr;           /* tables may return to this list when traversed */
    changed = 0;
    while ((w = next) != nullptr)
    { /* for each ephemeron table */
      assert(w->tt == LUA_VTABLE);
      Table* h = static_cast<Table*>(w);
      next = h->gclist; /* list is rebuilt during loop */
      nw2black(h);      /* out of the list (for now) */
      if (traverseephemeron(g, h, dir))
      {                  /* marked some value? */
        propagateall(g); /* propagate changes */
        changed = 1;     /* will have to revisit all ephemeron tables */
      }
    }
    dir = !dir; /* invert direction next time */
  } while (changed); /* repeat until no more changes */
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
static void clearbykeys(global_State* g, GCObject* l)
{
  for (; l; l = static_cast<Table*>(l)->gclist)
  {
    assert(l->tt == LUA_VTABLE);
    Table* h = static_cast<Table*>(l);
    Node* limit = gnodelast(h);
    Node* n;
    for (n = gnode(h, 0); n < limit; n++)
    {
      if (iscleared(g, (((n)->key_tt & BIT_ISCOLLECTABLE) ? gckey(n) : nullptr))) /* unmarked key? */
        (n->val.tt_ = LUA_VEMPTY);         /* remove entry */
      if (((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL))        /* is entry empty? */
        clearkey(n);               /* clear its key */
    }
  }
}

/*
** clear entries with unmarked values from all weaktables in list 'l' up
** to element 'f'
*/
static void clearbyvalues(global_State* g, GCObject* l, GCObject* f)
{
  for (; l != f; l = static_cast<Table*>(l)->gclist)
  {
    assert(l->tt == LUA_VTABLE);
    Table* h = static_cast<Table*>(l);
    Node *n, *limit = gnodelast(h);
    int i;
    int asize = h->asize();
    for (i = 0; i < asize; i++)
    {
      GCObject* o = gcvalarr(h, i);
      if (iscleared(g, o))             /* value was collected? */
        setArrTag(h, i, LUA_VEMPTY); /* remove entry */
    }
    for (n = gnode(h, 0); n < limit; n++)
    {
      if (iscleared(g, gcvalueN((&(n)->val)))) /* unmarked value? */
        (n->val.tt_ = LUA_VEMPTY);                 /* remove entry */
      if (((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL))                /* is entry empty? */
        clearkey(n);                       /* clear its key */
    }
  }
}

static void freeupval(lua_State* L, UpVal* uv)
{
  if (upisopen(uv))
    uv->luaF_unlinkupval();
  luaM_free_UpVal(L, uv);
}

static void freeobj(lua_State* L, GCObject* o)
{
  l_mem newmem = gettotalbytes(L->l_G) - objsize(o);
  switch (o->tt)
  {
  case LUA_VPROTO:
    static_cast<Proto*>(o)->luaF_freeproto(L);
    break;
  case LUA_VUPVAL:
    freeupval(L, static_cast<UpVal*>(o));
    break;
  case LUA_VLCL:
  {
    LClosure* cl = static_cast<LClosure*>(o);
    luaM_alloc_UpVal_ptr_span(L->l_G, L, cl->upvals, 0, false);
    luaM_free_LClosure(L, cl);
    break;
  }
  case LUA_VCCL:
  {
    CClosure* cl = static_cast<CClosure*>(o);
    luaM_alloc_TValue_span(L->l_G, L, cl->upvalue, 0, false);
    luaM_free_CClosure(L, cl);
    break;
  }
  case LUA_VTABLE:
    static_cast<Table*>(o)->luaH_free(L);
    break;
  case LUA_VTHREAD:
    luaE_freethread(L, static_cast<lua_State*>(o));
    break;
  case LUA_VUSERDATA:
  {
    Udata* u = static_cast<Udata*>(o);
    luaM_alloc_TValue_span(L->l_G, L, u->uv, 0, false);
    if (u->data.data() != nullptr)
    {
      (void)luaM_alloc_char_span(L->l_G, L, u->data, 0, false);
      u->data = {};
    }
    luaM_free_Udata(L, u);
    break;
  }
  case LUA_VSHRSTR:
  {
    assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
    TString* ts = static_cast<TString*>(o);
    ts->luaS_remove(L);
    if (!strisexternal(ts))
      luaM_release(L, ts->data.size());
    luaM_free_TString(L, ts);
    break;
  }
  case LUA_VLCF:
  {
    CFuncInfo* cfi = static_cast<CFuncInfo*>(o);
    luaM_free_CFuncInfo(L, cfi);
    break;
  }
  default:
    assert(0);
  }
  assert(gettotalbytes(L->l_G) == newmem);
}

static GCObject* sweeplist(lua_State* L, GCObject* p, l_mem countin)
{
  global_State* g = L->l_G;
  lu_byte ow = otherwhite(g);
  lu_byte white = luaC_white(g); /* current white */
  while (p->next != nullptr && countin-- > 0)
  {
    GCObject* curr = p->next;
    lu_byte marked = curr->marked;
    if (isdeadm(ow, marked))
    {                   /* is 'curr' dead? */
      p->next = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr); /* erase 'curr' */
    }
    else
    { /* change mark to 'white' and age to 'new' */
      curr->marked = ((marked & ~maskgcbits) | white | G_NEW);
      p = curr; /* go to next element */
    }
  }
  return (p->next == nullptr) ? nullptr : p;
}

/*
** sweep a list until a live object (or end of list)
*/
static GCObject* sweeptolive(lua_State* L, GCObject* p)
{
  GCObject* old = p;
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
static void checkSizes(lua_State* L, global_State* g)
{
  ((void)(L));
  ((void)(g));
}

/*
** Get the next udata to be finalized from the 'tobefnz' list, and
** link it back into the 'allgc' list.
*/
static GCObject* udata2finalize(global_State* g)
{
  GCObject* o = g->tobefnz.next; /* get first element */
  assert(tofinalize(o));
  g->tobefnz.next = o->next; /* remove it from 'tobefnz' list */
  o->next = g->allgc.next;   /* return it to 'allgc' list */
  g->allgc.next = o;
  ((o->marked) &= ((lu_byte)(~bitmask(FINALIZEDBIT)))); /* object is "normal" again */
  if (issweepphase(g))
    makewhite(g, o); /* "sweep" object */
  else if (getage(o) == G_OLD1)
    g->firstold1 = o; /* it is the first OLD1 object in the list */
  return o;
}

static void dothecall(lua_State* L)
{
  luaD_callnoyield(L, L->top - 2, 0);
}

struct DoTheCall : ProtectedCall
{
  void run(lua_State* L) override { dothecall(L); }
};

static void GCTM(lua_State* L)
{
  global_State* g = L->l_G;
  const TValue* tm;
  TValue v;
  assert(!g->gcemergency);
  setgcovalue(L, &v, udata2finalize(g));
  tm = luaT_gettmbyobj(L, &v, TM_GC);
  if (!notm(tm))
  { /* is there a finalizer? */
    TStatus status;
    lu_byte oldah = L->allowhook;
    lu_byte oldgcstp = g->gcstp;
    g->gcstp |= GCSTPGC;           /* avoid GC steps */
    L->allowhook = 0;              /* stop debug hooks during GC metamethod */
    setslot(L, L->top++, tm);   /* push finalizer... */
    setslot(L, L->top++, &v);   /* ... and its argument */
    L->ci->callstatus |= CIST_FIN; /* will run a finalizer */
    DoTheCall call;
    status = luaD_pcall(L, &call, (L->top - 2), 0);
    L->ci->callstatus &= ~CIST_FIN; /* not running a finalizer anymore */
    L->allowhook = oldah;           /* restore hooks */
    g->gcstp = oldgcstp;            /* restore state */
    if (l_unlikely(status != LUA_OK))
    { /* error while running __gc? */
      luaE_warnerror(L, lt::mm_gc);
      L->top--; /* pops error object */
    }
  }
}

/*
** call all pending finalizers
*/
static void callallpendingfinalizers(lua_State* L)
{
  global_State* g = L->l_G;
  while (g->tobefnz.next)
    GCTM(L);
}

/*
** find last 'next' field in list 'p' list (to add elements in its end)
*/
static GCObject* findlast(GCObject* p)
{
  while (p->next != nullptr)
    p = p->next;
  return p;
}

static void separatetobefnz(global_State* g, int all)
{
  GCObject* curr;
  GCObject* p = &g->finobj;
  GCObject* lastnext = findlast(&g->tobefnz);
  while ((curr = p->next) != g->finobjold1)
  { /* traverse all finalizable objects */
    assert(tofinalize(curr));
    if (!(iswhite(curr) || all)) /* not being collected? */
      p = curr;           /* don't bother with it */
    else
    {
      if (curr == g->finobjsur)    /* removing 'finobjsur'? */
        g->finobjsur = curr->next; /* correct it */
      p->next = curr->next;             /* remove 'curr' from 'finobj' list */
      curr->next = lastnext->next;      /* link at the end of 'tobefnz' list */
      lastnext->next = curr;
      lastnext = curr;
    }
  }
}

/*
** If pointer 'p' points to 'o', move it to the next element.
*/
static GCObject* checkpointer(GCObject* p, GCObject* o)
{
  return (o == p) ? o->next : p;
}

/*
** Correct pointers to objects inside 'allgc' list when
** object 'o' is being removed from the list.
*/
static void correctpointers(global_State* g, GCObject* o)
{
  g->survival = checkpointer(g->survival, o);
  g->old1 = checkpointer(g->old1, o);
  g->reallyold = checkpointer(g->reallyold, o);
  g->firstold1 = checkpointer(g->firstold1, o);
}

/*
** if object 'o' has a finalizer, remove it from 'allgc' list (must
** search the list to find it) and link it in 'finobj' list.
*/
void luaC_checkfinalizer(lua_State* L, GCObject* o, Table* mt)
{
  global_State* g = L->l_G;
  if (tofinalize(o) ||                 /* obj. is already marked... */
      gfasttm(g, mt, TM_GC) == nullptr || /* or has no finalizer... */
      (g->gcstp & GCSTPCLS))           /* or closing state? */
    return;                            /* nothing to be done */
  else
  { /* move 'o' to 'finobj' list */
    GCObject* p;
    if (issweepphase(g))
    {
      makewhite(g, o);                           /* "sweep" object 'o' */
      if (g->sweepgc == o)                /* should not remove 'sweepgc' object */
        g->sweepgc = sweeptolive(L, g->sweepgc); /* change 'sweepgc' */
    }
    else
      correctpointers(g, o);
    /* search for pointer pointing to 'o' */
    for (p = &g->allgc; p->next != o; p = p->next)
    { /* empty */
    }
    p->next = o->next;        /* remove 'o' from 'allgc' list */
    o->next = g->finobj.next; /* link it in 'finobj' list */
    g->finobj.next = o;
    o->marked = (o->marked | bitmask(FINALIZEDBIT)); /* mark it as such */
  }
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
static void setpause(global_State* g)
{
  l_mem threshold = luaO_applyparam((g)->gcparams[LUA_GCPPAUSE], g->GCmarked);
  l_mem debt = threshold - gettotalbytes(g);
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
static void sweep2old(lua_State* L, GCObject* p)
{
  GCObject* curr;
  global_State* g = L->l_G;
  while ((curr = p->next) != nullptr)
  {
    if (iswhite(curr))
    { /* is 'curr' dead? */
      assert(isdead(g, curr));
      p->next = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr); /* erase 'curr' */
    }
    else
    { /* all surviving objects become old */
      setage(curr, G_OLD);
      if (curr->tt == LUA_VTHREAD)
      { /* threads must be watched */
        assert(curr->tt == LUA_VTHREAD);
        lua_State* th = static_cast<lua_State*>(curr);
        linkgclist_(th, &g->grayagain); /* insert into 'grayagain' list */
      }
      else if (curr->tt == LUA_VUPVAL && upisopen(static_cast<UpVal*>(curr)))
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
struct GenSweep
{
  GCObject* last;
  l_mem addedold;
};

static GenSweep sweepgen(lua_State* L, global_State* g, GCObject* p,
                           GCObject* limit, bool setfirstold1)
{
  static const lu_byte nextage[] = {
      G_SURVIVAL, /* from G_NEW */
      G_OLD1,     /* from G_SURVIVAL */
      G_OLD1,     /* from G_OLD0 */
      G_OLD,      /* from G_OLD1 */
      G_OLD,      /* from G_OLD (do not change) */
      G_TOUCHED1, /* from G_TOUCHED1 (do not change) */
      G_TOUCHED2  /* from G_TOUCHED2 (do not change) */
  };
  l_mem addedold = 0;
  int white = luaC_white(g);
  GCObject* curr;
  while ((curr = p->next) != limit)
  {
    if (iswhite(curr))
    { /* is 'curr' dead? */
      assert(!isold(curr) && isdead(g, curr));
      p->next = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr); /* erase 'curr' */
    }
    else
    { /* correct mark and age */
      int age = getage(curr);
      if (age == G_NEW)
      {                                          /* new objects go back to white */
        int marked = curr->marked & ~maskgcbits; /* erase GC bits */
        curr->marked = ((lu_byte)(marked | G_SURVIVAL | white));
      }
      else
      {                            /* all other objects will be old, and so keep their color */
        assert(age != G_OLD1); /* advanced in 'markold' */
        setage(curr, nextage[age]);
        if (getage(curr) == G_OLD1)
        {
          addedold += objsize(curr); /* bytes becoming old */
          if (setfirstold1 && g->firstold1 == nullptr)
            g->firstold1 = curr; /* first OLD1 object in the list */
        }
      }
      p = curr; /* go to next element */
    }
  }
  return {p, addedold};
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
static GCListed* correctgraylist(GCListed* p)
{
  GCObject* curr;
  while ((curr = p->gclist) != nullptr)
  {
    GCListed* next = getgclist(curr);
    if (iswhite(curr))
      p->gclist = next->gclist; /* remove all white objects */
    else if (getage(curr) == G_TOUCHED1)
    { /* touched in this cycle? */
      assert(isgray(curr));
      nw2black(curr); /* make it black, for next barrier */
      setage(curr, G_TOUCHED2);
      p = next; /* keep it in the list and go to next element */
    }
    else if (curr->tt == LUA_VTHREAD)
    {
      assert(isgray(curr));
      p = next; /* keep non-white threads on the list */
    }
    else
    {                                 /* everything else is removed */
      assert(isold(curr));        /* young objects should be white here */
      if (getage(curr) == G_TOUCHED2) /* advance from TOUCHED2... */
        setage(curr, G_OLD);          /* ... to OLD */
      nw2black(curr);                 /* make object black (to be removed) */
      p->gclist = next->gclist;
    }
  }
  return p;
}

/*
** Correct all gray lists, coalescing them into 'grayagain'.
*/
static void correctgraylists(global_State* g)
{
  GCListed* list = correctgraylist(&g->grayagain);
  list->gclist = g->weak.gclist;
  g->weak.gclist = nullptr;
  list = correctgraylist(list);
  list->gclist = g->allweak.gclist;
  g->allweak.gclist = nullptr;
  list = correctgraylist(list);
  list->gclist = g->ephemeron.gclist;
  g->ephemeron.gclist = nullptr;
  correctgraylist(list);
}

/*
** Mark black 'OLD1' objects when starting a new young collection.
** Gray objects are already in some gray list, and so will be visited in
** the atomic step.
*/
static void markold(global_State* g, GCObject* from, GCObject* to)
{
  GCObject* p;
  for (p = from; p != to; p = p->next)
  {
    if (getage(p) == G_OLD1)
    {
      assert(!iswhite(p));
      setage(p, G_OLD); /* now they are old */
      if (isblack(p))
        reallymarkobject(g, p);
    }
  }
}

/*
** Finish a young-generation collection.
*/
static void finishgencycle(lua_State* L, global_State* g)
{
  correctgraylists(g);
  checkSizes(L, g);
  g->gcstate = GCSpropagate; /* skip restart */
  if (g->tobefnz.next != nullptr && !g->gcemergency && luaD_checkminstack(L))
    callallpendingfinalizers(L);
}

/*
** Shifts from a minor collection to major collections. It starts in
** the "sweep all" state to clear all objects, which are mostly black
** in generational mode.
*/
static void minor2inc(lua_State* L, global_State* g, lu_byte kind)
{
  g->GCmajorminor = g->GCmarked; /* number of live bytes */
  g->gckind = kind;
  g->reallyold = g->old1 = g->survival = nullptr;
  g->finobjrold = g->finobjold1 = g->finobjsur = nullptr;
  entersweep(L); /* continue as an incremental cycle */
  /* set a debt equal to the step size */
  luaE_setdebt(g, luaO_applyparam((g)->gcparams[LUA_GCPSTEPSIZE], 100));
}

/*
** Decide whether to shift to major mode. It shifts if the accumulated
** number of added old bytes (counted in 'GCmarked') is larger than
** 'minormajor'% of the number of lived bytes after the last major
** collection. (This number is kept in 'GCmajorminor'.)
*/
static int checkminormajor(global_State* g)
{
  l_mem limit = luaO_applyparam((g)->gcparams[LUA_GCPMINORMAJOR], g->GCmajorminor);
  if (limit == 0)
    return 0; /* special case: 'minormajor' 0 stops major collections */
  return (g->GCmarked >= limit);
}

/*
** Does a young collection. First, mark 'OLD1' objects. Then does the
** atomic step. Then, check whether to continue in minor mode. If so,
** sweep all lists and advance pointers. Finally, finish the collection.
*/
static void youngcollection(lua_State* L, global_State* g)
{
  l_mem addedold1 = 0;
  l_mem marked = g->GCmarked; /* preserve 'g->GCmarked' */
  GCObject* psurvival;       /* to point to first non-dead survival object */
  assert(g->gcstate == GCSpropagate);
  if (g->firstold1)
  {                                         /* are there regular OLD1 objects? */
    markold(g, g->firstold1, g->reallyold); /* mark them */
    g->firstold1 = nullptr;                    /* no more OLD1 objects (for now) */
  }
  markold(g, g->finobj.next, g->finobjrold);
  markold(g, g->tobefnz.next, nullptr);

  atomic(L); /* will lose 'g->marked' */

  /* sweep nursery and get a pointer to its last live element */
  g->gcstate = GCSswpallgc;
  GenSweep gs = sweepgen(L, g, &g->allgc, g->survival, true);
  psurvival = gs.last;
  addedold1 += gs.addedold;
  /* sweep 'survival' */
  addedold1 += sweepgen(L, g, psurvival, g->old1, true).addedold;
  g->reallyold = g->old1;
  g->old1 = psurvival->next;   /* 'survival' survivals are old now */
  g->survival = g->allgc.next; /* all news are survivals */

  /* repeat for 'finobj' lists */
  gs = sweepgen(L, g, &g->finobj, g->finobjsur, false);
  psurvival = gs.last;
  addedold1 += gs.addedold;
  /* sweep 'survival' */
  addedold1 += sweepgen(L, g, psurvival, g->finobjold1, false).addedold;
  g->finobjrold = g->finobjold1;
  g->finobjold1 = psurvival->next; /* 'survival' survivals are old now */
  g->finobjsur = g->finobj.next;   /* all news are survivals */

  addedold1 += sweepgen(L, g, &g->tobefnz, nullptr, false).addedold;

  /* keep total number of added old1 bytes */
  g->GCmarked = marked + addedold1;

  /* decide whether to shift to major mode */
  if (checkminormajor(g))
  {
    minor2inc(L, g, KGC_GENMAJOR); /* go to major mode */
    g->GCmarked = 0;               /* avoid pause in first major cycle (see 'setpause') */
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
static void atomic2gen(lua_State* L, global_State* g)
{
  cleargraylists(g);
  /* sweep all elements making them old */
  g->gcstate = GCSswpallgc;
  sweep2old(L, &g->allgc);
  /* everything alive now is old */
  g->reallyold = g->old1 = g->survival = g->allgc.next;
  g->firstold1 = nullptr; /* there are no OLD1 objects anywhere */

  /* repeat for 'finobj' lists */
  sweep2old(L, &g->finobj);
  g->finobjrold = g->finobjold1 = g->finobjsur = g->finobj.next;

  sweep2old(L, &g->tobefnz);

  g->gckind = KGC_GENMINOR;
  g->GCmajorminor = g->GCmarked; /* "base" for number of bytes */
  g->GCmarked = 0;               /* to count the number of added old1 bytes */
  finishgencycle(L, g);
}

/*
** Set debt for the next minor collection, which will happen when
** total number of bytes grows 'genminormul'% in relation to
** the base, GCmajorminor, which is the number of bytes being used
** after the last major collection.
*/
static void setminordebt(global_State* g)
{
  luaE_setdebt(g, luaO_applyparam((g)->gcparams[LUA_GCPMINORMUL], g->GCmajorminor));
}

/*
** Enter generational mode. Must go until the end of an atomic cycle
** to ensure that all objects are correctly marked and weak tables
** are cleared. Then, turn all objects into old and finishes the
** collection.
*/
static void entergen(lua_State* L, global_State* g)
{
  luaC_runtilstate(L, GCSpause, 1);     /* prepare to start a new cycle */
  luaC_runtilstate(L, GCSpropagate, 1); /* start new cycle */
  atomic(L);                            /* propagates all and then do the atomic stuff */
  atomic2gen(L, g);
  setminordebt(g); /* set debt assuming next cycle will be minor */
}

/*
** Change collector mode to 'newmode'.
*/
void luaC_changemode(lua_State* L, int newmode)
{
  global_State* g = L->l_G;
  if (g->gckind == KGC_GENMAJOR) /* doing major collections? */
    g->gckind = KGC_INC;         /* already incremental but in name */
  if (newmode != g->gckind)
  {                             /* does it need to change? */
    if (newmode == KGC_INC)     /* entering incremental mode? */
      minor2inc(L, g, KGC_INC); /* entering incremental mode */
    else
    {
      assert(newmode == KGC_GENMINOR);
      entergen(L, g);
    }
  }
}

/*
** Does a full collection in generational mode.
*/
static void fullgen(lua_State* L, global_State* g)
{
  minor2inc(L, g, KGC_INC);
  entergen(L, g);
}

/*
** After an atomic incremental step from a major collection,
** check whether collector could return to minor collections.
** It checks whether the number of bytes 'tobecollected'
** is greater than 'majorminor'% of the number of bytes added
** since the last collection ('addedbytes').
*/
static int checkmajorminor(lua_State* L, global_State* g)
{
  if (g->gckind == KGC_GENMAJOR)
  { /* generational mode? */
    l_mem numbytes = gettotalbytes(g);
    l_mem addedbytes = numbytes - g->GCmajorminor;
    l_mem limit = luaO_applyparam((g)->gcparams[LUA_GCPMAJORMINOR], addedbytes);
    l_mem tobecollected = numbytes - g->GCmarked;
    if (tobecollected > limit)
    {
      atomic2gen(L, g); /* return to generational mode */
      setminordebt(g);
      return 1; /* exit incremental collection */
    }
  }
  g->GCmajorminor = g->GCmarked; /* prepare for next collection */
  return 0;                      /* stay doing incremental collections */
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
static void entersweep(lua_State* L)
{
  global_State* g = L->l_G;
  g->gcstate = GCSswpallgc;
  assert(g->sweepgc == nullptr);
  g->sweepgc = sweeptolive(L, &g->allgc);
}

/*
** Delete all objects in list 'p' until (but not including) object
** 'limit'.
*/
static void deletelist(lua_State* L, GCObject* p, GCObject* limit)
{
  while (p != limit)
  {
    GCObject* next = p->next;
    freeobj(L, p);
    p = next;
  }
}

/*
** Call all finalizers of the objects in the given Lua state, and
** then free all objects, except for the main thread.
*/
void luaC_freeallobjects(lua_State* L)
{
  global_State* g = L->l_G;
  g->gcstp = GCSTPCLS; /* no extra finalizers after here */
  luaC_changemode(L, KGC_INC);
  separatetobefnz(g, 1); /* separate all objects with finalizers */
  assert(g->finobj.next == nullptr);
  callallpendingfinalizers(L);
  assert((mainthread(g)->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  deletelist(L, g->allgc.next, mainthread(g));
  assert(g->finobj.next == nullptr);   /* no new finalizers */
  deletelist(L, g->fixedgc, nullptr); /* collect fixed objects */
}

static void atomic(lua_State* L)
{
  global_State* g = L->l_G;
  GCObject *origweak, *origall;
  GCObject* grayagain = g->grayagain.gclist; /* save original list */
  g->grayagain.gclist = nullptr;
  assert(g->ephemeron.gclist == nullptr && g->weak.gclist == nullptr);
  assert(!iswhite(mainthread(g)));
  g->gcstate = GCSatomic;
  markobject(g, L); /* mark running thread */
  /* registry and global metatables may be changed by API */
  markvalue(g, &g->l_registry);
  markmt(g);       /* mark global metatables */
  propagateall(g); /* empties 'gray' list */
  /* remark occasional upvalues of (maybe) dead threads */
  remarkupvals(g);
  propagateall(g); /* propagate changes */
  g->gray.gclist = grayagain;
  propagateall(g); /* traverse 'grayagain' list */
  convergeephemerons(g);
  /* at this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers */
  clearbyvalues(g, g->weak.gclist, nullptr);
  clearbyvalues(g, g->allweak.gclist, nullptr);
  origweak = g->weak.gclist;
  origall = g->allweak.gclist;
  separatetobefnz(g, 0); /* separate objects to be finalized */
  markbeingfnz(g);       /* mark objects that will be finalized */
  propagateall(g);       /* remark, to propagate 'resurrection' */
  convergeephemerons(g);
  /* at this point, all resurrected objects are marked. */
  /* remove dead objects from weak tables */
  clearbykeys(g, g->ephemeron.gclist); /* clear keys from all ephemeron */
  clearbykeys(g, g->allweak.gclist);   /* clear keys from all 'allweak' */
  /* clear values from resurrected weak tables */
  clearbyvalues(g, g->weak.gclist, origweak);
  clearbyvalues(g, g->allweak.gclist, origall);
  g->currentwhite = (otherwhite(g)); /* flip current white */
  assert(g->gray.gclist == nullptr);
}

/*
** Do a sweep step. The normal case (not fast) sweeps at most GCSWEEPMAX
** elements. The fast case sweeps the whole list.
*/
static void sweepstep(lua_State* L, global_State* g,
                      lu_byte nextstate, GCObject* nextlist, int fast)
{
  if (g->sweepgc)
    g->sweepgc = sweeplist(L, g->sweepgc, fast ? MAX_LMEM : GCSWEEPMAX);
  else
  { /* enter next state */
    g->gcstate = nextstate;
    g->sweepgc = nextlist;
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

constexpr int step2pause = -3; /* finished collection; entered pause state */
constexpr int atomicstep = -2; /* atomic step */
constexpr int step2minor = -1; /* moved to minor collections */

static l_mem singlestep(lua_State* L, int fast)
{
  global_State* g = L->l_G;
  l_mem stepresult;
  assert(!g->gcstopem); /* collector is not reentrant */
  g->gcstopem = 1;          /* no emergency collections while collecting */
  switch (g->gcstate)
  {
  case GCSpause:
  {
    restartcollection(g);
    g->gcstate = GCSpropagate;
    stepresult = 1;
    break;
  }
  case GCSpropagate:
  {
    if (fast || g->gray.gclist == nullptr)
    {
      g->gcstate = GCSenteratomic; /* finish propagate phase */
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
    sweepstep(L, g, GCSswpfinobj, &g->finobj, fast);
    stepresult = GCSWEEPMAX;
    break;
  }
  case GCSswpfinobj:
  { /* sweep objects with finalizers */
    sweepstep(L, g, GCSswptobefnz, &g->tobefnz, fast);
    stepresult = GCSWEEPMAX;
    break;
  }
  case GCSswptobefnz:
  { /* sweep objects to be finalized */
    sweepstep(L, g, GCSswpend, nullptr, fast);
    stepresult = GCSWEEPMAX;
    break;
  }
  case GCSswpend:
  { /* finish sweeps */
    checkSizes(L, g);
    g->gcstate = GCScallfin;
    stepresult = GCSWEEPMAX;
    break;
  }
  case GCScallfin:
  { /* call finalizers */
    if (g->tobefnz.next && !g->gcemergency && luaD_checkminstack(L))
    {
      g->gcstopem = 0; /* ok collections during finalizers */
      GCTM(L);         /* call one finalizer */
      stepresult = CWUFIN;
    }
    else
    {                        /* no more finalizers or emergency mode or not enough stack
                                to run finalizers */
      g->gcstate = GCSpause; /* finish collection */
      stepresult = step2pause;
    }
    break;
  }
  default:
    assert(0);
    return 0;
  }
  g->gcstopem = 0;
  return stepresult;
}

/*
** Advances the garbage collector until it reaches the given state.
** (The option 'fast' is only for testing; in normal code, 'fast'
** here is always true.)
*/
void luaC_runtilstate(lua_State* L, int state, int fast)
{
  global_State* g = L->l_G;
  assert(g->gckind == KGC_INC);
  while (state != g->gcstate)
    singlestep(L, fast);
}

/*
** Performs a basic incremental step. The step size is
** converted from bytes to "units of work"; then the function loops
** running single steps until adding that many units of work or
** finishing a cycle (pause state). Finally, it sets the debt that
** controls when next step will be performed.
*/
static void incstep(lua_State* L, global_State* g)
{
  l_mem stepsize = luaO_applyparam((g)->gcparams[LUA_GCPSTEPSIZE], 100);
  l_mem work2do = luaO_applyparam((g)->gcparams[LUA_GCPSTEPMUL], stepsize / cost::Ref);
  l_mem stres;
  int fast = (work2do == 0); /* special case: do a full collection */
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
  if (g->gcstate == GCSpause)
    setpause(g); /* pause until next cycle */
  else
    luaE_setdebt(g, stepsize);
}

/*
** Performs a basic GC step if collector is running. (If collector was
** stopped by the user, set a reasonable debt to avoid it being called
** at every single check.)
*/
void luaC_step(lua_State* L)
{
  global_State* g = L->l_G;
  assert(!g->gcemergency);
  if (!gcrunning(g))
  {                          /* not running? */
    if (g->gcstp & GCSTPUSR) /* stopped by the user? */
      luaE_setdebt(g, 20000);
  }
  else
  {
    luai_tracegc(L, 1); /* for internal debugging */
    switch (g->gckind)
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
    luai_tracegc(L, 0); /* for internal debugging */
  }
}

/*
** Perform a full collection in incremental mode.
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
static void fullinc(lua_State* L, global_State* g)
{
  if (keepinvariant(g)) /* black objects? */
    entersweep(L);      /* sweep everything to turn them back to white */
  /* finish any pending sweep phase to start a new cycle */
  luaC_runtilstate(L, GCSpause, 1);
  luaC_runtilstate(L, GCScallfin, 1); /* run up to finalizers */
  luaC_runtilstate(L, GCSpause, 1);   /* finish collection */
  setpause(g);
}

/*
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
*/
void luaC_fullgc(lua_State* L, int isemergency)
{
  global_State* g = L->l_G;
  assert(!g->gcemergency);
  g->gcemergency = ((lu_byte)(isemergency)); /* set flag */
  switch (g->gckind)
  {
  case KGC_GENMINOR:
    fullgen(L, g);
    break;
  case KGC_INC:
    fullinc(L, g);
    break;
  case KGC_GENMAJOR:
    g->gckind = KGC_INC;
    fullinc(L, g);
    g->gckind = KGC_GENMAJOR;
    break;
  }
  g->gcemergency = 0;
}

/* }====================================================== */
