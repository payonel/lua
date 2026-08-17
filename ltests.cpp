/*
** $Id: ltests.c $
** Internal Module for Debugging of the Lua Implementation
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ltests.h"

#include "lua_internal.h"

#include "lapi.h"
#include "lauxlib.h"
#include "lcode.h"
#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lopcodes.h"
#include "lopnames.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lualib.h"

#include "util_cast.h"
#include "literals.h"

/*
** The whole module only makes sense with LUA_DEBUG on
*/
#if defined(LUA_DEBUG)

static std::string to_string(byte_view sv)
{
  return std::string(sv.data(), sv.size());
}

static byte_view to_view(std::string& str)
{
  return byte_view(str.data(), str.size());
}

static HostString to_hstring(std::string str)
{
  return HostString(byte_view(str.data(), str.size()));
}

static void* l_Trick = 0;

static inline TValue obj_at(lua_State* L, int k)
{
  return getslot(L, L->ci->func + (StkId)k);
}

static int runC(lua_State* L, lua_State* L1, byte_view code);

static void setnameval(lua_State* L, byte_view name, int val)
{
  lua_pushinteger(L, val);
  lua_setfield(L, -2, HostString(name));
}

static void pushobject(lua_State* L, const TValue* o)
{
  setslot(L, L->top, o);
  api_incr_top(L);
}

static void badexit(const char* fmt, const char* s1, const char* s2)
{
  fprintf(stderr, fmt, s1);
  if (s2)
    fprintf(stderr, "extra info: %s\n", s2);
  /* avoid assertion failures when exiting */
  l_memcontrol.numblocks = l_memcontrol.total = 0;
  exit(EXIT_FAILURE);
}

static int tpanic(lua_State* L)
{
  std::string msg = "error object is not a string";
  if (lua_type(L, -1) == LUA_TSTRING)
  {
    msg = to_string(lua_toistring(L, -1)->view());
  }
  return (badexit("PANIC: unprotected error in call to Lua API (%s)\n", msg.c_str(), nullptr), 0); /* do not return to Lua */
}

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
static void warnf(void* ud, const HostString& msg, int tocont)
{
  static char buff[200] = ""; /* should be enough for tests... */

  std::string mstr = to_string(msg.to_utf8());
  byte_view m = to_view(mstr);
  byte_span buffsp(buff, sizeof(buff));

  lua_State* L = ((lua_State*)(ud));
  static int onoff = 0;
  static int mode = 0; /* start in normal mode */
  static int lasttocont = 0;
  if (!lasttocont && !tocont && !m.empty() && m[0] == '@')
  { /* control message? */
    if (buff[0] != '\0')
      badexit("Control warning during warning: %s\naborting...\n", mstr.c_str(), buff);
    if (m == lt::off_1)
      onoff = 0;
    else if (m == lt::on_1)
      onoff = 1;
    else if (m == lt::normal_1)
      mode = 0;
    else if (m == lt::allow)
      mode = 1;
    else if (m == lt::store)
      mode = 2;
    else
      badexit("Invalid control warning in test mode: %s\naborting...\n", mstr.c_str(), nullptr);
    return;
  }
  lasttocont = tocont;
  if (m.size() >= sizeof(buff) - strlen(buff))
    badexit("warnf-buffer overflow (%s)\n", mstr.c_str(), buff);
  strcat(buff, mstr.c_str()); /* add new message to current warning */
  if (!tocont)
  { /* message finished? */
    luaL_checkstack(L, 1, HostString(lt::warn_stack_space));
    lua_getglobal(L, HostString(lt::_WARN));
    if (!lua_toboolean(L, -1))
      lua_pop(L, 1); /* ok, no previous unexpected warning */
    else
    {
      std::string warning = to_string(lua_toistring(L, -1)->view());
      badexit("Unhandled warning in store mode: %s\naborting...\n", warning.c_str(), buff);
    }
    switch (mode)
    {
    case 0:
    {                              /* normal */
      if (buff[0] != '#' && onoff) /* unexpected warning? */
        badexit("Unexpected warning in test mode: %s\naborting...\n",
                buff, nullptr);
    } /* FALLTHROUGH */
    case 1:
    { /* allow */
      if (onoff)
        fprintf(stderr, "Lua warning: %s\n", buff); /* print warning */
      break;
    }
    case 2:
    { /* store */
      luaL_checkstack(L, 1, HostString(lt::warn_stack_space));
      lua_pushstring(L, HostString::from_bytes(buffsp.str_view()));
      lua_setglobal(L, HostString(lt::_WARN)); /* assign message to global '_WARN' */
      break;
    }
    }
    buff[0] = '\0'; /* prepare buffer for next warning */
  }
}

/*
** {======================================================================
** Controlled version for realloc.
** =======================================================================
*/

Memcontrol l_memcontrol =
    {0, 0UL, 0UL, 0UL, 0UL, (~0UL), {0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL}};

static bool debug_realloc(void* ud, size_t oldsize, size_t nsize, int tag)
{
  Memcontrol* mc = ((Memcontrol*)(ud));
  int type = (((size_t)(tag)) < LUA_NUMTYPES) ? tag : 0;
  if (mc->memlimit == 0)
  {                                   /* first time? */
    char* limit = getenv("MEMLIMIT"); /* initialize memory limit */
    mc->memlimit = limit ? strtoul(limit, nullptr, 10) : ULONG_MAX;
  }
  if (nsize == 0)
  {
    mc->objcount[type]--;
    mc->numblocks--;
    mc->total -= oldsize;
    return true;
  }
  if (mc->failnext)
  {
    mc->failnext = 0;
    return false; /* fake a single memory allocation error */
  }
  if (mc->countlimit != ~0UL && nsize != oldsize)
  { /* count limit in use? */
    if (mc->countlimit == 0)
      return false; /* fake a memory allocation error */
    mc->countlimit--;
  }
  if (nsize > oldsize && mc->total + nsize - oldsize > mc->memlimit)
    return false; /* fake a memory allocation error */
  if (oldsize == 0)
  {
    mc->numblocks++;
    mc->objcount[type]++;
  }
  mc->total -= oldsize;
  mc->total += nsize;
  if (mc->total > mc->maxmem)
    mc->maxmem = mc->total;
  return true;
}

/* }====================================================================== */


#include <assert.h>

/*
** Check GC invariants. For incremental mode, a black object cannot
** point to a white one. For generational mode, really old objects
** cannot point to young objects. Both old1 and touched2 objects
** cannot point to new objects (but can point to survivals).
** (Threads and open upvalues, despite being marked "really old",
** continue to be visited in all collections, and therefore can point to
** new objects. They, and only they, are old but gray.)
*/
static int testobjref1(global_State* g, GCObject* f, GCObject* t)
{
  if (isdead(g, t))
    return 0;
  if (issweepphase(g))
    return 1; /* no invariants */
  else if (g->gckind != KGC_GENMINOR)
    return !(isblack(f) && iswhite(t)); /* basic incremental invariant */
  else
  { /* generational mode */
    if ((getage(f) == G_OLD && isblack(f)) && !isold(t))
      return 0;
    if ((getage(f) == G_OLD1 || getage(f) == G_TOUCHED2) &&
        getage(t) == G_NEW)
      return 0;
    return 1;
  }
}

static void printobj(global_State* g, GCObject* o)
{
  std::string tn  = to_string(ttypename(o->tt & NOVARIANT_MASK));
  printf("||%s(%p)-%c%c(%02X)||",
         tn.c_str(), (void*) o,
         isdead(g, o) ? 'd' : isblack(o) ? 'b'
                          : iswhite(o)   ? 'w'
                                         : 'g',
         "ns01oTt"[getage(o)], o -> marked);
  if (o->tt == LUA_VSTRING)
  {
    std::string msg = to_string(static_cast<TString*>(o)->data.view());
    printf(" '%s'", msg.c_str());
  }
}

static void lua_printvalue(TValue* v)
{
  switch (((v)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VNUMINT:
  case LUA_VNUMFLT:
  {
    char buff[LUA_N2SBUFFSZ];
    unsigned len = luaO_tostringbuff(v, buff);
    buff[len] = '\0';
    printf("%s", buff);
    break;
  }
  case LUA_VSHRSTR:
    assert((v->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    printf("'%s'", std::string(tsvalue(v)->data.view().data()).c_str());
    break;
  case LUA_VFALSE:
    printf("%s", "false");
    break;
  case LUA_VTRUE:
    printf("%s", "true");
    break;
  case LUA_VLIGHTUSERDATA:
    printf("light udata: %p", pvalue(v));
    break;
  case LUA_VUSERDATA:
    printf("full udata: %p", uvalue(v));
    break;
  case LUA_VNIL:
    printf("nil");
    break;
  case LUA_VLCF:
    printf("light C function: %p", fvalue(v));
    break;
  case LUA_VCCL:
    printf("C closure: %p", clCvalue(v));
    break;
  case LUA_VLCL:
    printf("Lua function: %p", clLvalue(v));
    break;
  case LUA_VTHREAD:
    printf("thread: %p", thvalue(v));
    break;
  case LUA_VTABLE:
    printf("table: %p", hvalue(v));
    break;
  default:
    assert(0);
  }
}

static int testobjref(global_State* g, GCObject* f, GCObject* t)
{
  int r1 = testobjref1(g, f, t);
  if (!r1)
  {
    printf("%d(%02X) - ", g->gcstate, g->currentwhite);
    printobj(g, f);
    printf("  ->  ");
    printobj(g, t);
    printf("\n");
  }
  return r1;
}

static void checkobjref(global_State* g, GCObject* f, GCObject* t)
{
  assert(testobjref(g, f, t));
}

static inline void checkobjrefN(global_State* g, GCObject* f, GCObject* t)
{
  if (t)
    checkobjref(g, f, t);
}

static void checkvalref(global_State* g, GCObject* f, const TValue* t)
{
  assert(!((t)->tt_ & BIT_ISCOLLECTABLE) || ((((t)->tt_ & WITHVARIANT_MASK) == gcvalue(t)->tt) && testobjref(g, f, gcvalue(t))));
}

static void checktable(global_State* g, Table* h)
{
  unsigned int i;
  unsigned int asize = (unsigned int)h->array.size();
  Node *n, *limit = gnode(h, sizenode(h));
  assert((h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  GCObject* hgc = const_cast<GCObject*>(static_cast<const GCObject*>(h));
  checkobjrefN(g, hgc, h->metatable);
  for (i = 0; i < asize; i++)
  {
    TValue aux;
    arr2obj(h, i, &aux);
    checkvalref(g, hgc, &aux);
  }
  for (n = gnode(h, 0); n < limit; n++)
  {
    if (!((n->val.tt_ & NOVARIANT_MASK) == LUA_TNIL))
    {
      TValue k;
      getnodekey(mainthread(g), &k, n);
      assert(!((n)->key_tt == LUA_TNIL));
      checkvalref(g, hgc, &k);
      checkvalref(g, hgc, (&(n)->val));
    }
  }
}

static void checkudata(global_State* g, Udata* u)
{
  int i;
  assert((u->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  GCObject* hgc = const_cast<GCObject*>(static_cast<const GCObject*>(u));
  checkobjrefN(g, hgc, u->metatable);
  for (i = 0; i < (int)u->uv.size(); i++)
    checkvalref(g, hgc, &u->uv[i]);
}

static void checkproto(global_State* g, Proto* f)
{
  int i;
  assert((f->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  GCObject* fgc = const_cast<GCObject*>(static_cast<const GCObject*>(f));
  checkobjrefN(g, fgc, f->source);
  for (i = 0; i < (int)f->k.size(); i++)
  {
    if (((f->k.data() + i)->tt_ & BIT_ISCOLLECTABLE))
      checkobjref(g, fgc, gcvalue(f->k.data() + i));
  }
  for (i = 0; i < (int)f->upvalues.size(); i++)
    checkobjrefN(g, fgc, f->upvalues[i].name);
  for (i = 0; i < (int)f->p.size(); i++)
    checkobjrefN(g, fgc, f->p[i]);
  for (i = 0; i < (int)f->locvars.size(); i++)
    checkobjrefN(g, fgc, f->locvars[i].varname);
}

static void checkCclosure(global_State* g, CClosure* cl)
{
  assert((cl->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  GCObject* clgc = const_cast<GCObject*>(static_cast<const GCObject*>(cl));
  int i;
  for (i = 0; i < (int)cl->upvalue.size(); i++)
    checkvalref(g, clgc, &cl->upvalue[i]);
}

static void checkLclosure(global_State* g, LClosure* cl)
{
  assert((cl->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  GCObject* clgc = const_cast<GCObject*>(static_cast<const GCObject*>(cl));
  int i;
  checkobjrefN(g, clgc, cl->p);
  for (i = 0; i < (int)cl->upvals.size(); i++)
  {
    UpVal* uv = cl->upvals[i];
    if (uv)
    {
      checkobjrefN(g, clgc, uv);
      if (!upisopen(uv))
      {
        assert((uv->tt & NOVARIANT_MASK) >= LUA_TSTRING);
        TValue uvv = upvalgetval(uv);
        checkvalref(g, const_cast<GCObject*>(static_cast<const GCObject*>(uv)), &uvv);
      }
    }
  }
}

static int lua_checkpc(lua_State* L, CallInfo* ci)
{
  if (!isLua(ci))
    return 1;
  else
  {
    StkId f = ci->func;
    Proto* p = slotclLval(L, f)->p;
    return 0 <= ci->savedpc &&
           ci->savedpc <= (int)p->code.size();
  }
}

static void check_stack(global_State* g, lua_State* L1)
{
  StkId o;
  CallInfo* ci;
  UpVal* uv;
  assert(!isdead(g, L1));
  if (L1->stack.data() == nullptr)
  { /* incomplete thread? */
    assert(L1->openupval == nullptr && L1->ci == nullptr);
    return;
  }
  for (uv = L1->openupval; uv != nullptr; uv = uv->opennext)
    assert(upisopen(uv)); /* must be open */
  assert(L1->top <= L1->stack_last);
  assert(L1->tbclist <= L1->top);
  for (ci = L1->ci; ci != nullptr; ci = ci->previous)
  {
    assert(ci->top <= L1->stack_last);
    assert(lua_checkpc(L1, ci));
  }
  for (o = 0; o < L1->stack_last; o++)
  { /* entire stack must have valid values */
    TValue v = getslot(L1, o);
    checkliveness(L1, &v);
  }
}

static void checkrefs(global_State* g, GCObject* o)
{
  switch (o->tt)
  {
  case LUA_VUSERDATA:
  {
    checkudata(g, static_cast<Udata*>(o));
    break;
  }
  case LUA_VUPVAL:
  {
    TValue uvv = upvalgetval(static_cast<UpVal*>(o));
    checkvalref(g, o, &uvv);
    break;
  }
  case LUA_VTABLE:
  {
    checktable(g, static_cast<Table*>(o));
    break;
  }
  case LUA_VTHREAD:
  {
    check_stack(g, static_cast<lua_State*>(o));
    break;
  }
  case LUA_VLCL:
  {
    checkLclosure(g, static_cast<LClosure*>(o));
    break;
  }
  case LUA_VCCL:
  {
    checkCclosure(g, static_cast<CClosure*>(o));
    break;
  }
  case LUA_VPROTO:
  {
    checkproto(g, static_cast<Proto*>(o));
    break;
  }
  case LUA_VSHRSTR:
  {
    assert(!isgray(o)); /* strings are never gray */
    break;
  }
  case LUA_VLCF:
  {
    assert(!isgray(o)); /* CFuncInfo has no refs, never gray */
    break;
  }
  default:
    assert(0);
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
static void checkobject(global_State* g, GCObject* o, int maybedead,
                        int listage)
{
  if (isdead(g, o))
    assert(maybedead);
  else
  {
    assert(g->gcstate != GCSpause || iswhite(o));
    if (g->gckind == KGC_GENMINOR)
    { /* generational mode? */
      assert(getage(o) >= listage);
      if (isold(o))
      {
        assert(!iswhite(o));
        assert(isblack(o) ||
               getage(o) == G_TOUCHED1 ||
               getage(o) == G_OLD0 ||
               o->tt == LUA_VTHREAD ||
               (o->tt == LUA_VUPVAL && upisopen(static_cast<UpVal*>(o))));
      }
      assert(getage(o) != G_TOUCHED1 || isgray(o));
    }
    checkrefs(g, o);
  }
}

static l_mem checkgraylist(global_State* g, GCObject* o)
{
  int total = 0; /* count number of elements in the list */
  ((void)(g));  /* better to keep it if we need to print an object */
  while (o)
  {
    assert(!!isgray(o) ^ (getage(o) == G_TOUCHED2));
    assert(!((o->marked) & bitmask(TESTBIT)));
    if (keepinvariant(g))
      o->marked = (lu_byte)(o->marked | bitmask(TESTBIT)); /* mark that object is in a gray list */
    total++;
    switch (o->tt)
    {
    case LUA_VTABLE:
      o = static_cast<Table*>(o)->gclist;
      break;
    case LUA_VLCL:
      o = static_cast<LClosure*>(o)->gclist;
      break;
    case LUA_VCCL:
      o = static_cast<CClosure*>(o)->gclist;
      break;
    case LUA_VTHREAD:
      o = static_cast<lua_State*>(o)->gclist;
      break;
    case LUA_VPROTO:
      o = static_cast<Proto*>(o)->gclist;
      break;
    case LUA_VUSERDATA:
      assert(static_cast<Udata*>(o)->uv.size() > 0);
      assert(o->tt == LUA_VUSERDATA);
      o = static_cast<Udata*>(o)->gclist;
      break;
    default:
      assert(0); /* other objects cannot be in a gray list */
    }
  }
  return total;
}

/*
** Check objects in gray lists.
*/
static l_mem checkgrays(global_State* g)
{
  l_mem total = 0; /* count number of elements in all lists */
  if (!keepinvariant(g))
    return total;
  total += checkgraylist(g, g->gray);
  total += checkgraylist(g, g->grayagain);
  total += checkgraylist(g, g->weak);
  total += checkgraylist(g, g->allweak);
  total += checkgraylist(g, g->ephemeron);
  return total;
}

/*
** Check whether 'o' should be in a gray list. If so, increment
** 'count' and check its TESTBIT. (It must have been previously set by
** 'checkgraylist'.)
*/
static void incifingray(global_State* g, GCObject* o, l_mem* count)
{
  if (!keepinvariant(g))
    return; /* gray lists not being kept in these phases */
  if (o->tt == LUA_VUPVAL)
  {
    /* only open upvalues can be gray */
    assert(!isgray(o) || upisopen(static_cast<UpVal*>(o)));
    return; /* upvalues are never in gray lists */
  }
  /* these are the ones that must be in gray lists */
  if (isgray(o) || getage(o) == G_TOUCHED2)
  {
    (*count)++;
    assert(((o->marked) & bitmask(TESTBIT)));
    ((o->marked) &= ((lu_byte)(~bitmask(TESTBIT)))); /* prepare for next cycle */
  }
}

static l_mem checklist(global_State* g, int maybedead, int tof,
                       GCObject* newl, GCObject* survival, GCObject* old, GCObject* reallyold)
{
  GCObject* o;
  l_mem total = 0; /* number of object that should be in  gray lists */
  for (o = newl; o != survival; o = o->next)
  {
    checkobject(g, o, maybedead, G_NEW);
    incifingray(g, o, &total);
    assert(!tof == !tofinalize(o));
  }
  for (o = survival; o != old; o = o->next)
  {
    checkobject(g, o, 0, G_SURVIVAL);
    incifingray(g, o, &total);
    assert(!tof == !tofinalize(o));
  }
  for (o = old; o != reallyold; o = o->next)
  {
    checkobject(g, o, 0, G_OLD1);
    incifingray(g, o, &total);
    assert(!tof == !tofinalize(o));
  }
  for (o = reallyold; o != nullptr; o = o->next)
  {
    checkobject(g, o, 0, G_OLD);
    incifingray(g, o, &total);
    assert(!tof == !tofinalize(o));
  }
  return total;
}

static int lua_checkmemory(lua_State* L)
{
  global_State* g = L->l_G;
  GCObject* o;
  int maybedead;
  l_mem totalin;     /* total of objects that are in gray lists */
  l_mem totalshould; /* total of objects that should be in gray lists */
  if (keepinvariant(g))
  {
    assert(!iswhite(mainthread(g)));
    assert(g->l_registry.tt_ & BIT_ISCOLLECTABLE);
    assert(!iswhite(gcvalue(&g->l_registry)));
  }
  assert(g->l_registry.tt_ & BIT_ISCOLLECTABLE);
  assert(!isdead(g, gcvalue(&g->l_registry)));
  assert(g->sweepgc == nullptr || issweepphase(g));
  totalin = checkgrays(g);

  /* check 'fixedgc' list */
  for (o = g->fixedgc; o != nullptr; o = o->next)
  {
    assert(o->tt == LUA_VSHRSTR && isgray(o) && getage(o) == G_OLD);
  }

  /* check 'allgc' list */
  maybedead = (GCSatomic < g->gcstate && g->gcstate <= GCSswpallgc);
  totalshould = checklist(g, maybedead, 0, g->allgc,
                          g->survival, g->old1, g->reallyold);

  /* check 'finobj' list */
  totalshould += checklist(g, 0, 1, g->finobj,
                           g->finobjsur, g->finobjold1, g->finobjrold);

  /* check 'tobefnz' list */
  for (o = g->tobefnz; o != nullptr; o = o->next)
  {
    checkobject(g, o, 0, G_NEW);
    incifingray(g, o, &totalshould);
    assert(tofinalize(o));
    assert(o->tt == LUA_VUSERDATA || o->tt == LUA_VTABLE);
  }
  if (keepinvariant(g))
    assert(totalin == totalshould);
  return 0;
}

/* }====================================================== */

/*
** {======================================================
** Disassembler
** =======================================================
*/

static std::string buildop(Proto* p, int pc)
{
  char buff[100];
  char* out = buff;
  Instruction i = p->code[pc];
  OpCode o = GET_OPCODE(i);
  std::string namebuf(opnames[o].data());
  const char* name = namebuf.c_str();
  int line = luaG_getfuncline(p, pc);
  int lineinfo = (p->lineinfo.data() != nullptr) ? p->lineinfo[pc] : 0;
  if (lineinfo == ABSLINEINFO)
    out += sprintf(out, "(__");
  else
    out += sprintf(out, "(%2d", lineinfo);
  out += sprintf(out, " - %4d) %4d - ", line, pc);
  switch (getOpMode(o))
  {
  case iABC:
    sprintf(out, "%-12s%4d %4d %4d%s", name,
            GETARG_A(i), GETARG_B(i), GETARG_C(i),
            GETARG_k(i) ? " (k)" : "");
    break;
  case ivABC:
    sprintf(out, "%-12s%4d %4d %4d%s", name,
            GETARG_A(i), GETARG_vB(i), GETARG_vC(i),
            GETARG_k(i) ? " (k)" : "");
    break;
  case iABx:
    sprintf(out, "%-12s%4d %4d", name, GETARG_A(i), GETARG_Bx(i));
    break;
  case iAsBx:
    sprintf(out, "%-12s%4d %4d", name, GETARG_A(i), GETARG_sBx(i));
    break;
  case iAx:
    sprintf(out, "%-12s%4d", name, GETARG_Ax(i));
    break;
  case isJ:
    sprintf(out, "%-12s%4d", name, GETARG_sJ(i));
    break;
  }
  return buff;
}

static int listcode(lua_State* L)
{
  int pc;
  Proto* p;
  luaL_argcheck(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1),
                1, HostString(lt::Lua_function_expected));
  TValue o = obj_at(L, 1);
  p = getproto(&o);
  lua_newtable(L);
  setnameval(L, lt::maxstack, p->maxstacksize);
  setnameval(L, lt::numparams, p->numparams);
  for (pc = 0; pc < (int)p->code.size(); pc++)
  {
    lua_pushinteger(L, pc + 1);
    std::string op = buildop(p, pc);
    byte_view sv(op.data(), op.size());
    lua_pushstring(L, HostString(sv));
    lua_settable(L, -3);
  }
  return 1;
}

static int printcode(lua_State* L)
{
  int pc;
  Proto* p;
  luaL_argcheck(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1),
                1, HostString(lt::Lua_function_expected));
  TValue o = obj_at(L, 1);
  p = getproto(&o);
  printf("maxstack: %d\n", p->maxstacksize);
  printf("numparams: %d\n", p->numparams);
  for (pc = 0; pc < (int)p->code.size(); pc++)
  {
    printf("%s\n", buildop(p, pc).c_str());
  }
  return 0;
}

static int listk(lua_State* L)
{
  Proto* p;
  int i;
  luaL_argcheck(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1),
                1, HostString(lt::Lua_function_expected));
  TValue o = obj_at(L, 1);
  p = getproto(&o);
  lua_createtable(L, (int)p->k.size(), 0);
  for (i = 0; i < (int)p->k.size(); i++)
  {
    pushobject(L, p->k.data() + i);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int listabslineinfo(lua_State* L)
{
  Proto* p;
  int i;
  luaL_argcheck(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1),
                1, HostString(lt::Lua_function_expected));
  TValue o = obj_at(L, 1);
  p = getproto(&o);
  luaL_argcheck(L, p->abslineinfo.data() != nullptr, 1, HostString(lt::function_has_no_debug_info));
  lua_createtable(L, 2 * (int)p->abslineinfo.size(), 0);
  for (i = 0; i < (int)p->abslineinfo.size(); i++)
  {
    lua_pushinteger(L, p->abslineinfo[i].pc);
    lua_rawseti(L, -2, 2 * i + 1);
    lua_pushinteger(L, p->abslineinfo[i].line);
    lua_rawseti(L, -2, 2 * i + 2);
  }
  return 1;
}

static int listlocals(lua_State* L)
{
  Proto* p;
  int pc = ((int)(luaL_checkinteger(L, 2))) - 1;
  int i = 0;
  const InternalString* name;
  luaL_argcheck(L, lua_isfunction(L, 1) && !lua_iscfunction(L, 1),
                1, HostString(lt::Lua_function_expected));
  TValue o = obj_at(L, 1);
  p = getproto(&o);
  while ((name = luaF_getlocalname(p, ++i, pc)) != nullptr)
    lua_pushlstring(L, name->view(), 0, name->size());
  return i - 1;
}

/* }====================================================== */

static void lua_printstack(lua_State* L)
{
  int i;
  int n = lua_gettop(L);
  printf("stack: >>\n");
  for (i = 1; i <= n; i++)
  {
    printf("%3d: ", i);
    TValue v = getslot(L, L->ci->func + i);
    lua_printvalue(&v);
    printf("\n");
  }
  printf("<<\n");
}

static int lua_printallstack(lua_State* L)
{
  StkId p;
  int i = 1;
  CallInfo* ci = &L->base_ci;
  printf("stack: >>\n");
  for (p = 0; p < L->top; p++)
  {
    if (ci != nullptr && p == ci->func)
    {
      printf("  ---\n");
      if (ci == L->ci)
        ci = nullptr; /* printed last frame */
      else
        ci = ci->next;
    }
    printf("%3d: ", i++);
    TValue v = getslot(L, p);
    lua_printvalue(&v);
    printf("\n");
  }
  printf("<<\n");
  return 0;
}

static int get_limits(lua_State* L)
{
  lua_createtable(L, 0, 5);
  setnameval(L, lt::IS32INT, 1);
  setnameval(L, lt::MAXARG_Ax, MAXARG_Ax);
  setnameval(L, lt::MAXARG_Bx, MAXARG_Bx);
  setnameval(L, lt::OFFSET_sBx, OFFSET_sBx);
  setnameval(L, lt::NUM_OPCODES, NUM_OPCODES);
  return 1;
}

static int get_sizes(lua_State* L)
{
  lua_newtable(L);
  setnameval(L, lt::Lua_state, sizeof(lua_State));
  setnameval(L, lt::global_state, sizeof(global_State));
  setnameval(L, lt::TValue, sizeof(TValue));
  setnameval(L, lt::Node, sizeof(Node));
  setnameval(L, lt::stack_Value, sizeof(lua_Integer) + sizeof(GCObject*) + sizeof(lu_byte));
  return 1;
}

static int mem_query(lua_State* L)
{
  if (lua_isnone(L, 1))
  {
    lua_pushinteger(L, ((lua_Integer)(l_memcontrol.total)));
    lua_pushinteger(L, ((lua_Integer)(l_memcontrol.numblocks)));
    lua_pushinteger(L, ((lua_Integer)(l_memcontrol.maxmem)));
    return 3;
  }
  else if (lua_isnumber(L, 1))
  {
    unsigned long limit = ((unsigned long)(luaL_checkinteger(L, 1)));
    global_State* g = L->l_G;
    if (limit == 0)
    {
      l_memcontrol.memlimit = ULONG_MAX;
      g->memlimit = 0;
    }
    else
    {
      long headroom = ((long)(limit)) - ((long)(l_memcontrol.total));
      l_memcontrol.memlimit = limit;
      g->memlimit = (headroom > 0)
                        ? ((size_t)(gettotalbytes(g) + headroom))
                        : 1;
    }
    return 0;
  }
  else
  {
    byte_view t = luaL_checklstring(L, 1);
    int i;
    for (i = LUA_NUMTYPES - 1; i >= 0; i--)
    {
      if (ttypename(i) == t)
      {
        lua_pushinteger(L, ((lua_Integer)(l_memcontrol.objcount[i])));
        return 1;
      }
    }
    lua_pushstring(L, t);
    return luaL_error(L, HostString(lt::unknown_type_s));
  }
}

static int alloc_count(lua_State* L)
{
  if (lua_isnone(L, 1))
    l_memcontrol.countlimit = ((unsigned long)(~0L));
  else
    l_memcontrol.countlimit = ((unsigned long)(luaL_checkinteger(L, 1)));
  return 0;
}

static int alloc_failnext(lua_State*)
{
  l_memcontrol.failnext = 1;
  return 0;
}

static int settrick(lua_State* L)
{
  TValue o = obj_at(L, 1);
  if (((o.tt_ & NOVARIANT_MASK) == LUA_TNIL))
    l_Trick = nullptr;
  else
  {
    assert(o.tt_ & BIT_ISCOLLECTABLE);
    l_Trick = gcvalue(&o);
  }
  return 0;
}

static int gc_color(lua_State* L)
{
  luaL_checkany(L, 1);
  TValue o = obj_at(L, 1);
  if (!(o.tt_ & BIT_ISCOLLECTABLE))
    lua_pushstring(L, HostString(lt::no_collectable));
  else
  {
    assert(o.tt_ & BIT_ISCOLLECTABLE);
    GCObject* obj = gcvalue(&o);
    byte_view color = isdead(L->l_G, obj) ? lt::dead : iswhite(obj) ? lt::white : isblack(obj) ? lt::black : lt::gray;
    lua_pushstring(L, HostString(color));
  }
  return 1;
}

static int gc_age(lua_State* L)
{
  luaL_checkany(L, 1);
  TValue o = obj_at(L, 1);
  if (!(o.tt_ & BIT_ISCOLLECTABLE))
    lua_pushstring(L, HostString(lt::no_collectable));
  else
  {
    static byte_view gennames[] = {lt::new_kw, lt::survival, lt::old0, lt::old1, lt::old, lt::touched1, lt::touched2};
    assert(o.tt_ & BIT_ISCOLLECTABLE);
    GCObject* obj = gcvalue(&o);
    lua_pushstring(L, HostString(gennames[getage(obj)]));
  }
  return 1;
}

static int gc_printobj(lua_State* L)
{
  luaL_checkany(L, 1);
  TValue o = obj_at(L, 1);
  if (!(o.tt_ & BIT_ISCOLLECTABLE))
    printf("no collectable\n");
  else
  {
    assert(o.tt_ & BIT_ISCOLLECTABLE);
    GCObject* obj = gcvalue(&o);
    printobj(L->l_G, obj);
    printf("\n");
  }
  return 0;
}

static const byte_view statenames[] = {
    lt::propagate, lt::enteratomic,
    lt::atomic, lt::sweepallgc,
    lt::sweepfinobj, lt::sweeptobefnz,
    lt::sweepend, lt::callfin,
    lt::pause, lt::empty_str};

static int gc_state(lua_State* L)
{
  static const int states[] = {
      GCSpropagate, GCSenteratomic, GCSatomic, GCSswpallgc, GCSswpfinobj,
      GCSswptobefnz, GCSswpend, GCScallfin, GCSpause, -1};
  static const byte_view defstate = lt::empty_str;
  int option = states[luaL_checkoptioni(L, 1, defstate, statenames)];
  global_State* g = L->l_G;
  if (option == -1)
  {
    lua_pushlstring(L, statenames[g->gcstate], 0, statenames[g->gcstate].size());
    return 1;
  }
  else
  {
    if (g->gckind != KGC_INC)
      luaL_error(L, HostString(lt::cannot_change_states_in_generational_mode));
    lua_lock(L);
    if (option < g->gcstate)
    {                                   /* must cross 'pause'? */
      luaC_runtilstate(L, GCSpause, 1); /* run until pause */
    }
    luaC_runtilstate(L, option, 0); /* do not skip propagation state */
    assert(g->gcstate == option);
    lua_unlock(L);
    return 0;
  }
}

static int tracinggc = 0;
void luai_tracegctest(lua_State* L, int first)
{
  if (!tracinggc)
    return;
  else
  {
    global_State* g = L->l_G;
    lua_unlock(L);
    g->gcstp = GCSTPGC;
    lua_checkstack(L, 10);
    lua_getfield(L, LUA_REGISTRYINDEX, HostString(lt::tracegc));
    lua_pushboolean(L, first);
    lua_call(L, 1, 0);
    g->gcstp = 0;
    lua_lock(L);
  }
}

static int tracegc(lua_State* L)
{
  if (lua_isnil(L, 1))
    tracinggc = 0;
  else
  {
    tracinggc = 1;
    lua_setfield(L, LUA_REGISTRYINDEX, HostString(lt::tracegc));
  }
  return 0;
}

static int hash_query(lua_State* L)
{
  if (lua_isnone(L, 2))
  {
    TString* ts;
    luaL_argcheck(L, lua_type(L, 1) == LUA_TSTRING, 1, HostString(lt::string_expected));
    TValue o = obj_at(L, 1);
    assert((o.tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    ts = tsvalue(&o);
    lua_pushinteger(L, ((int)(luaS_gethash(ts))));
  }
  else
  {
    TValue o = obj_at(L, 1);
    TValue o2 = obj_at(L, 2);
    Table* t;
    luaL_checktype(L, 2, LUA_TTABLE);
    t = hvalue(&o2);
    lua_pushinteger(L, ((lua_Integer)(luaH_mainposition(t, &o))));
  }
  return 1;
}

static int stacklevel(lua_State* L)
{
  int a = 0;
  lua_pushinteger(L, ((lua_Integer)(L->top)));
  lua_pushinteger(L, stacksize(L));
  lua_pushinteger(L, ((lua_Integer)(L->nCcalls)));
  lua_pushinteger(L, L->nci);
  lua_pushinteger(L, (lua_Integer) (size_t) &a);
  return 5;
}

static int resetCI(lua_State* L)
{
  CallInfo* ci = L->ci;
  while (ci->next != nullptr)
  {
    CallInfo* tofree = ci->next;
    ci->next = ci->next->next;
    luaM_free_CallInfo(L, tofree);
    L->nci--;
  }
  return 0;
}

static int reallocstack(lua_State* L)
{
  int n = ((int)(luaL_checkinteger(L, 1)));
  lua_lock(L);
  luaD_reallocstack(L, ((int)(L->top)) + n, 1);
  lua_unlock(L);
  return 0;
}

static int table_query(lua_State* L)
{
  const Table* t;
  int i = ((int)(luaL_optinteger(L, 2, -1)));
  unsigned int asize;
  luaL_checktype(L, 1, LUA_TTABLE);
  TValue o = obj_at(L, 1);
  t = hvalue(&o);
  asize = (unsigned)t->array.size();
  if (i == -1)
  {
    lua_pushinteger(L, ((lua_Integer)(asize)));
    lua_pushinteger(L, ((lua_Integer)(allocsizenode(t))));
    lua_pushinteger(L, ((lua_Integer)(asize > 0 ? t->lenhint : 0)));
    return 3;
  }
  else if (((unsigned int)(i)) < asize)
  {
    lua_pushinteger(L, i);
    if (!(((*getArrTag(t, i)) & NOVARIANT_MASK) == LUA_TNIL))
    {
      TValue v;
      arr2obj(t, ((unsigned int)(i)), &v);
      setslot(L, L->top, &v);
    }
    else
      setslottag(L, L->top, LUA_VNIL);
    api_incr_top(L);
    lua_pushnil(L);
  }
  else if (((unsigned int)(i -= ((int)(asize)))) < sizenode(t))
  {
    TValue k;
    getnodekey(L, &k, gnode(t, i));
    if (!((gnode(t, i)->val.tt_ & NOVARIANT_MASK) == LUA_TNIL) ||
        ((k.tt_ & NOVARIANT_MASK) == LUA_TNIL) ||
        ((k.tt_ & NOVARIANT_MASK) == LUA_TNUMBER))
    {
      pushobject(L, &k);
    }
    else
      lua_pushliteral(L, HostString(lt::undef));
    if (!((gnode(t, i)->val.tt_ & NOVARIANT_MASK) == LUA_TNIL))
      pushobject(L, (&(gnode(t, i))->val));
    else
      lua_pushnil(L);
    lua_pushinteger(L, (t->node[i].next));
  }
  return 3;
}

static int gc_query(lua_State* L)
{
  global_State* g = L->l_G;
  byte_view kind = g->gckind == KGC_INC ? lt::inc : g->gckind == KGC_GENMAJOR ? lt::genmajor : lt::genminor;
  lua_pushstring(L, HostString(kind));
  lua_pushlstring(L, statenames[g->gcstate], 0, statenames[g->gcstate].size());
  lua_pushinteger(L, ((lua_Integer)(gettotalbytes(g))));
  lua_pushinteger(L, ((lua_Integer)(g->GCdebt)));
  lua_pushinteger(L, ((lua_Integer)(g->GCmarked)));
  lua_pushinteger(L, ((lua_Integer)(g->GCmajorminor)));
  return 6;
}

static int test_codeparam(lua_State* L)
{
  lua_Integer p = luaL_checkinteger(L, 1);
  lua_pushinteger(L, luaO_codeparam(((unsigned int)(p))));
  return 1;
}

static int test_applyparam(lua_State* L)
{
  lua_Integer p = luaL_checkinteger(L, 1);
  lua_Integer x = luaL_checkinteger(L, 2);
  lua_pushinteger(L, ((lua_Integer)(luaO_applyparam(((lu_byte)(p)), x))));
  return 1;
}

static int string_query(lua_State* L)
{
  auto& strt = L->l_G->strt;
  int s = ((int)(luaL_optinteger(L, 1, 0))) - 1;
  if (s == -1)
  {
    lua_pushinteger(L, ((int)(strt.buckets.size())));
    lua_pushinteger(L, ((int)(strt.count)));
    return 2;
  }
  else if (s < ((int)(strt.buckets.size())))
  {
    int n = 0;
    for (TString* ts = strt.buckets[((size_t)(s))]; ts != nullptr; ts = ts->hnext)
    {
      setslots(L, L->top, ts);
      api_incr_top(L);
      n++;
    }
    return n;
  }
  else
    return 0;
}

static int getreftable(lua_State* L)
{
  if (lua_istable(L, 2)) /* is there a table as second argument? */
    return 2;            /* use it as the table */
  else
    return LUA_REGISTRYINDEX; /* default is to use the register */
}

static int tref(lua_State* L)
{
  int t = getreftable(L);
  int level = lua_gettop(L);
  luaL_checkany(L, 1);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, luaL_ref(L, t));
  ((void)(level));                       /* to avoid warnings */
  assert(lua_gettop(L) == level + 1); /* +1 for result */
  return 1;
}

static int getref(lua_State* L)
{
  int t = getreftable(L);
  int level = lua_gettop(L);
  lua_rawgeti(L, t, luaL_checkinteger(L, 1));
  ((void)(level)); /* to avoid warnings */
  assert(lua_gettop(L) == level + 1);
  return 1;
}

static int unref(lua_State* L)
{
  int t = getreftable(L);
  int level = lua_gettop(L);
  luaL_unref(L, t, ((int)(luaL_checkinteger(L, 1))));
  ((void)(level)); /* to avoid warnings */
  assert(lua_gettop(L) == level);
  return 0;
}

static int upvalue(lua_State* L)
{
  int n = ((int)(luaL_checkinteger(L, 2)));
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (lua_isnone(L, 3))
  {
    std::optional<HostString> name = lua_getupvalue(L, 1, n);
    if (!name)
      return 0;
    lua_pushstring(L, *name);
    return 2;
  }
  else
  {
    std::optional<HostString> name = lua_setupvalue(L, 1, n);
    if (name)
      lua_pushstring(L, *name);
    else
      lua_pushnil(L);
    return 1;
  }
}

static int newuserdata(lua_State* L)
{
  size_t size = ((size_t)(luaL_optinteger(L, 1, 0)));
  int nuv = ((int)(luaL_optinteger(L, 2, 0)));
  char* p = ((char*)(lua_newuserdatauv(L, size, nuv)));
  while (size--)
    *p++ = '\0';
  return 1;
}

static int pushuserdata(lua_State* L)
{
  lua_Integer u = luaL_checkinteger(L, 1);
  lua_pushlightuserdata(L, util::as_userdata(u));
  return 1;
}

static int udataval(lua_State* L)
{
  lua_pushinteger(L, ((lua_Integer)(((size_t)(lua_touserdata(L, 1))))));
  return 1;
}

static int doonnewstack(lua_State* L)
{
  lua_State* L1 = lua_newthread(L);
  byte_view sv1 = luaL_checklstring(L, 1);
  int status = luaL_loadbufferx(L1, sv1, HostString::from_bytes(sv1), HostString(lt::t_char));
  if (status == LUA_OK)
    status = lua_pcall(L1, 0, 0, 0);
  lua_pushinteger(L, status);
  return 1;
}

static int s2d(lua_State* L)
{
  lua_pushnumber(L, ((lua_Number)(*((const double*)(luaL_checklstring(L, 1).data())))));
  return 1;
}

static int d2s(lua_State* L)
{
  double d = ((double)(luaL_checknumber(L, 1)));
  lua_pushlstring(L, byte_view{((char*)(&d)), sizeof(d)}, 0, sizeof(d));
  return 1;
}

static int num2int(lua_State* L)
{
  lua_pushinteger(L, lua_tointeger(L, 1));
  return 1;
}

static int makeseed(lua_State* L)
{
  lua_pushinteger(L, ((lua_Integer)(luaL_makeseed(L))));
  return 1;
}

static int newstate(lua_State* L)
{
  void* ud;
  lua_Allocator a = lua_getallocf(L); lua_Alloc f = a.f; ud = a.ud;
  lua_State* L1 = lua_newstate(f, ud, 0);
  if (L1)
  {
    lua_atpanic(L1, tpanic);
    lua_pushlightuserdata(L, L1);
  }
  else
    lua_pushnil(L);
  return 1;
}

static lua_State* getstate(lua_State* L)
{
  lua_State* L1 = ((lua_State*)(lua_touserdata(L, 1)));
  luaL_argcheck(L, L1 != nullptr, 1, HostString(lt::state_expected));
  return L1;
}

static int loadlib(lua_State* L)
{
  lua_State* L1 = getstate(L);
  int load = ((int)(luaL_checkinteger(L, 2)));
  int preload = ((int)(luaL_checkinteger(L, 3)));
  luaL_openselectedlibs(L1, load, preload);
  luaL_requiref(L1, HostString(lt::T_char), luaB_opentests, 0);
  assert(lua_type(L1, -1) == LUA_TTABLE);
  /* 'requiref' should not reload module already loaded... */
  luaL_requiref(L1, HostString(lt::T_char), nullptr, 1); /* seg. fault if it reloads */
  /* ...but should return the same module */
  assert(lua_compare(L1, -1, -2, LUA_OPEQ));
  return 0;
}

static int closestate(lua_State* L)
{
  lua_State* L1 = getstate(L);
  lua_close(L1);
  return 0;
}

static int doremote(lua_State* L)
{
  lua_State* L1 = getstate(L);
  byte_view codev = luaL_checklstring(L, 2);
  int status;
  lua_settop(L1, 0);
  status = luaL_loadbufferx(L1, codev, HostString::from_bytes(codev), HostString(lt::t_char));
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
    while (!lua_isnone(L1, ++i))
      lua_pushstring(L, lua_tostring(L1, i));
    lua_pop(L1, i - 1);
    return i - 1;
  }
}

static int log2_aux(lua_State* L)
{
  unsigned int x = (unsigned int) luaL_checkinteger(L, 1);
  lua_pushinteger(L, luaO_ceillog2(x));
  return 1;
}

struct Aux
{
  jmp_buf jb;
  std::string paniccode;
  lua_State* L;
};

/*
** does a long-jump back to "main program".
*/
static int panicback(lua_State* L)
{
  struct Aux* b;
  lua_checkstack(L, 1);                          /* open space for 'Aux' struct */
  lua_getfield(L, LUA_REGISTRYINDEX, HostString(lt::_jmpbuf)); /* get 'Aux' struct */
  b = (struct Aux*) lua_touserdata(L, -1);
  lua_pop(L, 1);               /* remove 'Aux' struct */
  byte_view  panicsv(b->paniccode.data(), b->paniccode.size());
  runC(b->L, L, panicsv); /* run optional panic code */
  longjmp(b->jb, 1);
  return 1; /* to avoid warnings */
}

static int checkpanic(lua_State* L)
{
  struct Aux b;
  void* ud;
  lua_State* L1;
  byte_view code = luaL_checklstring(L, 1);
  lua_Allocator a = lua_getallocf(L); lua_Alloc f = a.f; ud = a.ud;
  b.paniccode = std::string(luaL_optlstring(L, 2, lt::empty_str).data());
  b.L = L;
  L1 = lua_newstate(f, ud, 0); /* create new state */
  if (L1 == nullptr)
  { /* error? */
    lua_pushstring(L, HostString(lt::not_enough_memory));
    return 1;
  }
  lua_atpanic(L1, panicback); /* set its panic function */
  lua_pushlightuserdata(L1, &b);
  lua_setfield(L1, LUA_REGISTRYINDEX, HostString(lt::_jmpbuf)); /* store 'Aux' struct */
  if (setjmp(b.jb) == 0)
  {                    /* set jump buffer */
    runC(L, L1, code); /* run code unprotected */
    lua_pushliteral(L, HostString(lt::no_errors));
  }
  else
  { /* error handling */
    /* move error message to original state */
    lua_pushstring(L, lua_tostring(L1, -1));
  }
  lua_close(L1);
  return 1;
}

static int externKstr(lua_State* L)
{
  size_t len;
  byte_view sv = luaL_checklstring(L, 1);
  const char* s = sv.data();
  len = sv.size();
  lua_pushlstring(L, byte_view{s, len}, 0, len);
  return 1;
}

/*
** Create a buffer with the content of a given string and then
** create an external string using that buffer. Use the allocation
** function from Lua to create and free the buffer.
*/
static int externstr(lua_State* L)
{
  size_t len;
  byte_view sv = luaL_checklstring(L, 1);
  const char* s = sv.data();
  len = sv.size();
  lua_pushlstring(L, byte_view{s, len}, 0, len);
  return 1;
}

/*
** {====================================================================
** function to test the API with C. It interprets a kind of assembler
** language with calls to the API, so the test can be driven by Lua code
** =====================================================================
*/

static void sethookaux(lua_State* L, int mask, int count, byte_view scpt);

struct CScript
{
  byte_view code;
  size_t pos;
};

static int curbyte(const CScript* sc)
{
  return sv_byteat(sc->code, sc->pos);
}

static const byte_view delimits = lt::ws_delims;

static void skip(CScript* sc)
{
  for (;;)
  {
    int c = curbyte(sc);
    if (c != -1 && delimits.find((char) c) != byte_view::npos)
      sc->pos++;
    else if (c == '#')
    { /* comment? */
      while (curbyte(sc) != '\n' && curbyte(sc) != -1)
        sc->pos++; /* until end-of-line */
    }
    else
      break;
  }
}

static int getnum_aux(lua_State* L, lua_State* L1, CScript* sc)
{
  int res = 0;
  int sig = 1;
  skip(sc);
  if (curbyte(sc) == '.')
  {
    res = ((int)(lua_tointeger(L1, -1)));
    lua_pop(L1, 1);
    sc->pos++;
    return res;
  }
  else if (curbyte(sc) == '*')
  {
    res = lua_gettop(L1);
    sc->pos++;
    return res;
  }
  else if (curbyte(sc) == '!')
  {
    sc->pos++;
    if (curbyte(sc) == 'G')
      res = LUA_RIDX_GLOBALS;
    else if (curbyte(sc) == 'M')
      res = LUA_RIDX_MAINTHREAD;
    else
      assert(0);
    sc->pos++;
    return res;
  }
  else if (curbyte(sc) == '-')
  {
    sig = -1;
    sc->pos++;
  }
  if (!lisdigit(curbyte(sc)))
  {
    lua_pushstring(L, HostString(sc->code.substr(sc->pos)));
    luaL_error(L, HostString(lt::number_expected_s));
  }
  while (lisdigit(curbyte(sc)))
  {
    res = res * 10 + (curbyte(sc) - '0');
    sc->pos++;
  }
  return sig * res;
}

static std::string getstring_aux(lua_State* L, CScript* sc)
{
  std::string buff;
  skip(sc);
  int c = curbyte(sc);
  if (c == '"' || c == '\'')
  { /* quoted string? */
    int quote = c;
    sc->pos++;
    while ((c = curbyte(sc)) != quote)
    {
      if (c == -1)
        luaL_error(L, HostString(lt::unfinished_string_in_C_script));
      buff.push_back((char) c);
      sc->pos++;
    }
    sc->pos++;
  }
  else
  {
    while ((c = curbyte(sc)) != -1 && delimits.find((char) c) == byte_view::npos)
    {
      buff.push_back((char) c);
      sc->pos++;
    }
  }
  return buff;
}

static int getindex_aux(lua_State* L, lua_State* L1, CScript* sc)
{
  skip(sc);
  int c = curbyte(sc);
  sc->pos++;
  switch (c)
  {
  case 'R':
    return LUA_REGISTRYINDEX;
  case 'U':
    return lua_upvalueindex(getnum_aux(L, L1, sc));
  default:
  {
    int n;
    sc->pos--; /* to read again */
    n = getnum_aux(L, L1, sc);
    if (n == 0)
      return 0;
    else
      return lua_absindex(L1, n);
  }
  }
}

static byte_view statcodes[] = {lt::OK, lt::YIELD, lt::ERRRUN, lt::ERRSYNTAX, lt::not_enough_memory, lt::ERRERR};

/*
** Avoid these stat codes from being collected, to avoid possible
** memory error when pushing them.
*/
static void regcodes(lua_State* L)
{
  unsigned int i;
  for (i = 0; i < sizeof(statcodes) / sizeof(statcodes[0]); i++)
  {
    lua_pushboolean(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, HostString{statcodes[i]});
  }
}



static int testC(lua_State* L);
static int Cfunck(lua_State* L, int status, lua_KContext ctx);

static const byte_view ops = lt::special_chars;

static int runC(lua_State* L, lua_State* L1, byte_view code)
{
  int status = 0;
  CScript sc{code, 0};
  for (;;)
  {
    std::string instr = getstring_aux(L, &sc);
    byte_view inst(instr.data(), instr.size());
    if (inst.empty())
      return 0;
    else if (inst == lt::absindex)
    {
      lua_pushinteger(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::append)
    {
      int t = getindex_aux(L, L1, &sc);
      int i = ((int)(lua_rawlen(L1, t)));
      lua_rawseti(L1, t, i + 1);
    }
    else if (inst == lt::arith)
    {
      int op;
      skip(&sc);
      op = ((int)(ops.find((char) curbyte(&sc))));
      sc.pos++;
      lua_arith(L1, op);
    }
    else if (inst == lt::call)
    {
      int narg = getnum_aux(L, L1, &sc);
      int nres = getnum_aux(L, L1, &sc);
      lua_call(L1, narg, nres);
    }
    else if (inst == lt::callk)
    {
      int narg = getnum_aux(L, L1, &sc);
      int nres = getnum_aux(L, L1, &sc);
      int i = getindex_aux(L, L1, &sc);
      lua_callk(L1, narg, nres, i, Cfunck);
    }
    else if (inst == lt::checkstack)
    {
      int sz = getnum_aux(L, L1, &sc);
      std::string msgstr = getstring_aux(L, &sc);
      byte_view msg(msgstr.data(), msgstr.size());
      luaL_checkstack(L1, sz,
                      msg.empty() ? std::optional<HostString>{}
                                  : std::optional<HostString>{HostString(msg)});
    }
    else if (inst == lt::rawcheckstack)
    {
      int sz = getnum_aux(L, L1, &sc);
      lua_pushboolean(L1, lua_checkstack(L1, sz));
    }
    else if (inst == lt::compare)
    {
      std::string opt = getstring_aux(L, &sc); /* EQ, LT, or LE */
      int op = (opt[0] == 'E')   ? LUA_OPEQ
               : (opt[1] == 'T') ? LUA_OPLT
                                 : LUA_OPLE;
      int a = getindex_aux(L, L1, &sc);
      int b = getindex_aux(L, L1, &sc);
      lua_pushboolean(L1, lua_compare(L1, a, b, op));
    }
    else if (inst == lt::concat)
    {
      lua_concat(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::copy)
    {
      int f = getindex_aux(L, L1, &sc);
      lua_copy(L1, f, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::func2num)
    {
      lua_CFunction func = lua_tocfunction(L1, getindex_aux(L, L1, &sc));
      lua_pushinteger(L1, ((lua_Integer)(((size_t)(func)))));
    }
    else if (inst == lt::getfield)
    {
      int t = getindex_aux(L, L1, &sc);
      int tp = lua_getfield(L1, t, to_hstring(getstring_aux(L, &sc)));
      assert(tp == lua_type(L1, -1));
    }
    else if (inst == lt::getglobal)
    {
      lua_getglobal(L1, to_hstring(getstring_aux(L, &sc)));
    }
    else if (inst == lt::getmetatable)
    {
      if (lua_getmetatable(L1, getindex_aux(L, L1, &sc)) == 0)
        lua_pushnil(L1);
    }
    else if (inst == lt::gettable)
    {
      int tp = lua_gettable(L1, getindex_aux(L, L1, &sc));
      assert(tp == lua_type(L1, -1));
    }
    else if (inst == lt::gettop)
    {
      lua_pushinteger(L1, lua_gettop(L1));
    }
    else if (inst == lt::gsub)
    {
      int a = getnum_aux(L, L1, &sc);
      int b = getnum_aux(L, L1, &sc);
      int c = getnum_aux(L, L1, &sc);
      luaL_gsub(L1, *lua_tostring(L1, a),
                *lua_tostring(L1, b),
                *lua_tostring(L1, c));
    }
    else if (inst == lt::insert)
    {
      lua_insert(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::iscfunction)
    {
      lua_pushboolean(L1, lua_iscfunction(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isfunction)
    {
      lua_pushboolean(L1, lua_isfunction(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isnil)
    {
      lua_pushboolean(L1, lua_isnil(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isnull)
    {
      lua_pushboolean(L1, lua_isnone(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isnumber)
    {
      lua_pushboolean(L1, lua_isnumber(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isstring)
    {
      lua_pushboolean(L1, lua_isstring(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::istable)
    {
      lua_pushboolean(L1, lua_istable(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isudataval)
    {
      lua_pushboolean(L1, lua_islightuserdata(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::isuserdata)
    {
      lua_pushboolean(L1, lua_isuserdata(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::len)
    {
      lua_len(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::Llen)
    {
      lua_pushinteger(L1, luaL_len(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::loadfile)
    {
      HostString fn = luaL_checkstring(L1, getnum_aux(L, L1, &sc));
      luaL_loadfilex(L1, fn, HostString(lt::t_char));
    }
    else if (inst == lt::loadstring)
    {
      byte_view sv2 = luaL_checklstring(L1, getnum_aux(L, L1, &sc));
      HostString name = to_hstring(getstring_aux(L, &sc));
      HostString mode = to_hstring(getstring_aux(L, &sc));
      luaL_loadbufferx(L1, sv2, name, mode);
    }
    else if (inst == lt::newmetatable)
    {
      lua_pushboolean(L1, luaL_newmetatable(L1, to_hstring(getstring_aux(L, &sc))));
    }
    else if (inst == lt::newtable)
    {
      lua_newtable(L1);
    }
    else if (inst == lt::newthread)
    {
      lua_newthread(L1);
    }
    else if (inst == lt::resetthread)
    {
      lua_pushinteger(L1, lua_resetthread(L1)); /* deprecated */
    }
    else if (inst == lt::newuserdata)
    {
      lua_newuserdata(L1, ((size_t)(getnum_aux(L, L1, &sc))));
    }
    else if (inst == lt::next_kw)
    {
      lua_next(L1, -2);
    }
    else if (inst == lt::objsize)
    {
      lua_pushinteger(L1, ((lua_Integer)(lua_rawlen(L1, getindex_aux(L, L1, &sc)))));
    }
    else if (inst == lt::pcall)
    {
      int narg = getnum_aux(L, L1, &sc);
      int nres = getnum_aux(L, L1, &sc);
      status = lua_pcall(L1, narg, nres, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::pcallk)
    {
      int narg = getnum_aux(L, L1, &sc);
      int nres = getnum_aux(L, L1, &sc);
      int i = getindex_aux(L, L1, &sc);
      status = lua_pcallk(L1, narg, nres, 0, i, Cfunck);
    }
    else if (inst == lt::pop)
    {
      lua_pop(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::printstack)
    {
      int n = getnum_aux(L, L1, &sc);
      if (n != 0)
      {
        TValue v = getslot(L, L->ci->func + n);
        lua_printvalue(&v);
        printf("\n");
      }
      else
        lua_printstack(L1);
    }
    else if (inst == lt::print)
    {
      std::string msg = getstring_aux(L, &sc);
      printf("%s\n", msg.c_str());
    }
    else if (inst == lt::warningC)
    {
      lua_warning(L1, to_hstring(getstring_aux(L, &sc)), 1);
    }
    else if (inst == lt::warning)
    {
      lua_warning(L1, to_hstring(getstring_aux(L, &sc)), 0);
    }
    else if (inst == lt::pushbool)
    {
      lua_pushboolean(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::pushcclosure)
    {
      lua_pushcclosure(L1, testC, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::pushint)
    {
      lua_pushinteger(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::pushnil)
    {
      lua_pushnil(L1);
    }
    else if (inst == lt::pushnum)
    {
      lua_pushnumber(L1, (lua_Number) getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::pushstatus)
    {
      lua_pushistringref(L1, statcodes[status]);
    }
    else if (inst == lt::pushstring)
    {
      lua_pushstring(L1, to_hstring(getstring_aux(L, &sc)));
    }
    else if (inst == lt::pushupvalueindex)
    {
      lua_pushinteger(L1, lua_upvalueindex(getnum_aux(L, L1, &sc)));
    }
    else if (inst == lt::pushvalue)
    {
      lua_pushvalue(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::pushfstringI)
    {
      HostString fmt = lua_tostring(L, -2).value();
      if (luaO_fmtargcount(fmt.to_utf8()) > 0)
        lua_pushinteger(L1, lua_tointeger(L, -1));
      lua_pushfstring(L1, fmt);
    }
    else if (inst == lt::pushfstringS)
    {
      HostString fmt = lua_tostring(L, -2).value();
      if (luaO_fmtargcount(fmt.to_utf8()) > 0)
        lua_pushstring(L1, lua_tostring(L, -1));
      lua_pushfstring(L1, fmt);
    }
    else if (inst == lt::pushfstringP)
    {
      HostString fmt = lua_tostring(L, -2).value();
      if (luaO_fmtargcount(fmt.to_utf8()) > 0)
      {
        if (lua_islightcfunction(L, -1))
        {
          InternalString fa = lua_funcaddressi(L, -1);
          lua_pushistring(L1, std::move(fa));
        }
        else
        {
          lua_pushlightuserdata(L1, const_cast<void*>(lua_topointer(L, -1)));
        }
      }
      lua_pushfstring(L1, fmt);
    }
    else if (inst == lt::rawget)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawget(L1, t);
    }
    else if (inst == lt::rawgeti)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawgeti(L1, t, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::rawgetp)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawgetp(L1, t, util::as_userdata(getnum_aux(L, L1, &sc)));
    }
    else if (inst == lt::rawset)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawset(L1, t);
    }
    else if (inst == lt::rawseti)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawseti(L1, t, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::rawsetp)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_rawsetp(L1, t, util::as_userdata(getnum_aux(L, L1, &sc)));
    }
    else if (inst == lt::remove_kw)
    {
      lua_remove(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::replace)
    {
      lua_replace(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::resume)
    {
      int i = getindex_aux(L, L1, &sc);
      status = lua_resume(lua_tothread(L1, i), L, getnum_aux(L, L1, &sc)).status;
    }
    else if (inst == lt::traceback)
    {
      HostString msg = to_hstring(getstring_aux(L, &sc));
      int level = getnum_aux(L, L1, &sc);
      luaL_traceback(L1, L1, msg, level);
    }
    else if (inst == lt::threadstatus)
    {
      lua_pushistringref(L1, statcodes[lua_status(L1)]);
    }
    else if (inst == lt::alloccount)
    {
      l_memcontrol.countlimit = ((unsigned int)(getnum_aux(L, L1, &sc)));
    }
    else if (inst == lt::return_kw)
    {
      int n = getnum_aux(L, L1, &sc);
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
    else if (inst == lt::rotate)
    {
      int i = getindex_aux(L, L1, &sc);
      lua_rotate(L1, i, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::setfield)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_setfield(L1, t, to_hstring(getstring_aux(L, &sc)));
    }
    else if (inst == lt::seti)
    {
      int t = getindex_aux(L, L1, &sc);
      lua_seti(L1, t, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::setglobal)
    {
      lua_setglobal(L1, to_hstring(getstring_aux(L, &sc)));
    }
    else if (inst == lt::sethook)
    {
      int mask = getnum_aux(L, L1, &sc);
      int count = getnum_aux(L, L1, &sc);
      std::string s = getstring_aux(L, &sc);
      sethookaux(L1, mask, count, to_view(s));
    }
    else if (inst == lt::setmetatable)
    {
      int idx = getindex_aux(L, L1, &sc);
      lua_setmetatable(L1, idx);
    }
    else if (inst == lt::settable)
    {
      lua_settable(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::settop)
    {
      lua_settop(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::testudata)
    {
      int i = getindex_aux(L, L1, &sc);
      lua_pushboolean(L1, luaL_testudata(L1, i, to_hstring(getstring_aux(L, &sc))) != nullptr);
    }
    else if (inst == lt::error_kw)
    {
      lua_error(L1);
    }
    else if (inst == lt::abort_kw)
    {
      abort();
    }
    else if (inst == lt::throw_kw)
    {
      static struct X
      {
        int x;
      } x;
      throw x;
      break;
    }
    else if (inst == lt::tobool)
    {
      lua_pushboolean(L1, lua_toboolean(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::tocfunction)
    {
      /* copy the function value directly to preserve identity */
      lua_pushvalue(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::tointeger)
    {
      lua_pushinteger(L1, lua_tointeger(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::tonumber)
    {
      lua_pushnumber(L1, lua_tonumber(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::topointer)
    {
      int idx = getindex_aux(L, L1, &sc);
      if (lua_islightcfunction(L1, idx))
      {
        InternalString fa = lua_funcaddressi(L1, idx);
        lua_pushistring(L1, std::move(fa));
      }
      else
      {
        const void* p = lua_topointer(L1, idx);
        lua_pushlightuserdata(L1, util::remove_const_cast(p));
      }
    }
    else if (inst == lt::touserdata)
    {
      lua_pushlightuserdata(L1, lua_touserdata(L1, getindex_aux(L, L1, &sc)));
    }
    else if (inst == lt::tostring)
    {
      int i = getindex_aux(L, L1, &sc);
      if (!lua_tolstring(L1, i)) /* not convertible? */
        lua_pushnil(L1);
      else
      {
        HostString s = *lua_tostring(L1, i);
        lua_pushstring(L1, s);
        assert(s == lua_tostring(L1, -1));
      }
    }
    else if (inst == lt::Ltolstring)
    {
      luaL_tolstring(L1, getindex_aux(L, L1, &sc));
    }
    else if (inst == lt::type)
    {
      lua_pushstring(L1, luaL_typename(L1, getnum_aux(L, L1, &sc)));
    }
    else if (inst == lt::xmove)
    {
      int f = getindex_aux(L, L1, &sc);
      int t = getindex_aux(L, L1, &sc);
      lua_State* fs = (f == 0) ? L1 : lua_tothread(L1, f);
      lua_State* ts = (t == 0) ? L1 : lua_tothread(L1, t);
      int n = getnum_aux(L, L1, &sc);
      if (n == 0)
        n = lua_gettop(fs);
      lua_xmove(fs, ts, n);
    }
    else if (inst == lt::isyieldable)
    {
      lua_pushboolean(L1, lua_isyieldable(lua_tothread(L1, getindex_aux(L, L1, &sc))));
    }
    else if (inst == lt::yield)
    {
      return lua_yield(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::yieldk)
    {
      int nres = getnum_aux(L, L1, &sc);
      int i = getindex_aux(L, L1, &sc);
      return lua_yieldk(L1, nres, i, Cfunck);
    }
    else if (inst == lt::toclose)
    {
      lua_toclose(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::closeslot)
    {
      lua_closeslot(L1, getnum_aux(L, L1, &sc));
    }
    else if (inst == lt::argerror)
    {
      int arg = getnum_aux(L, L1, &sc);
      luaL_argerror(L1, arg, to_hstring(getstring_aux(L, &sc)));
    }
    else
    {
      lua_pushstring(L, HostString(inst));
      luaL_error(L, HostString(lt::unknown_instruction_s));
    }
  }
  return 0;
}

static int testC(lua_State* L)
{
  lua_State* L1;
  byte_view pc;
  if (lua_isuserdata(L, 1))
  {
    L1 = getstate(L);
    pc = luaL_checklstring(L, 2);
  }
  else if (lua_isthread(L, 1))
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

static int Cfunc(lua_State* L)
{
  byte_view script = *lua_tolstring(L, lua_upvalueindex(1));
  return runC(L, L, script);
}

static int Cfunck(lua_State* L, int status, lua_KContext ctx)
{
  lua_pushistringref(L, statcodes[status]);
  lua_setglobal(L, HostString(lt::status));
  lua_pushinteger(L, ((lua_Integer)(ctx)));
  lua_setglobal(L, HostString(lt::ctx));
  byte_view script = *lua_tolstring(L, ((int)(ctx)));
  return runC(L, L, script);
}

static int makeCfunc(lua_State* L)
{
  luaL_checkstring(L, 1);
  lua_pushcclosure(L, Cfunc, lua_gettop(L));
  return 1;
}

/* }====================================================== */

/*
** {======================================================
** tests for C hooks
** =======================================================
*/

/*
** C hook that runs the C script stored in registry.C_HOOK[L]
*/
static void Chook(lua_State* L, lua_Debug* ar)
{
  byte_view events[] = {lt::call, lt::ret, lt::line, lt::count, lt::tailcall};
  lua_getfield(L, LUA_REGISTRYINDEX, HostString(lt::C_HOOK));
  lua_pushlightuserdata(L, L);
  lua_gettable(L, -2);                    /* get C_HOOK[L] (script saved by sethookaux) */
  std::string scpt{lua_tostring(L, -1)->to_utf8().data()};
  lua_pop(L, 2);                          /* remove C_HOOK and script */
  lua_pushistringref(L, events[ar->event]); /* may be used by script */
  lua_pushinteger(L, ar->currentline);    /* may be used by script */
  runC(L, L, to_view(scpt));                       /* run script from C_HOOK[L] */
}

/*
** sets 'registry.C_HOOK[L] = scpt' and sets 'Chook' as a hook
*/
static void sethookaux(lua_State* L, int mask, int count, byte_view scpt)
{
  if (scpt.empty())
  {                             /* no script? */
    lua_sethook(L, nullptr, 0, 0); /* turn off hooks */
    return;
  }
  lua_getfield(L, LUA_REGISTRYINDEX, HostString(lt::C_HOOK)); /* get C_HOOK table */
  if (!lua_istable(L, -1))
  {                  /* no hook table? */
    lua_pop(L, 1);   /* remove previous value */
    lua_newtable(L); /* create new C_HOOK table */
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, HostString(lt::C_HOOK)); /* register it */
  }
  lua_pushlightuserdata(L, L);
  lua_pushlstring(L, scpt, 0, scpt.size());
  lua_settable(L, -3); /* C_HOOK[L] = script */
  lua_sethook(L, Chook, mask, count);
}

static int sethook(lua_State* L)
{
  if (lua_isnoneornil(L, 1))
    lua_sethook(L, nullptr, 0, 0); /* turn off hooks */
  else
  {
    byte_view scpt = luaL_checklstring(L, 1);
    byte_view smask = luaL_checklstring(L, 2);
    int count = ((int)(luaL_optinteger(L, 3, 0)));
    int mask = 0;
    if (smask.find('c') != byte_view::npos)
      mask |= LUA_MASKCALL;
    if (smask.find('r') != byte_view::npos)
      mask |= LUA_MASKRET;
    if (smask.find('l') != byte_view::npos)
      mask |= LUA_MASKLINE;
    if (count > 0)
      mask |= LUA_MASKCOUNT;
    sethookaux(L, mask, count, scpt);
  }
  return 0;
}

static int coresume(lua_State* L)
{
  lua_State* co = lua_tothread(L, 1);
  luaL_argcheck(L, co, 1, HostString(lt::coroutine_expected));
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

#include <fcntl.h>
#include <unistd.h>

static int nonblock(lua_State* L)
{
  FILE* f = reinterpret_cast<FILE*>(((luaL_Stream*)(luaL_checkudata(L, 1, HostString(LUA_FILEHANDLE))))->f);
  int fd = fileno(f);
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
  return 0;
}

/* }====================================================== */

static const struct luaL_Regi tests_funcs[] = {
    {lt::checkmemory, lua_checkmemory},
    {lt::closestate, closestate},
    {lt::d2s, d2s},
    {lt::doonnewstack, doonnewstack},
    {lt::doremote, doremote},
    {lt::gccolor, gc_color},
    {lt::gcage, gc_age},
    {lt::gcstate, gc_state},
    {lt::tracegc, tracegc},
    {lt::pobj, gc_printobj},
    {lt::getref, getref},
    {lt::hash, hash_query},
    {lt::log2, log2_aux},
    {lt::limits, get_limits},
    {lt::listcode, listcode},
    {lt::printcode, printcode},
    {lt::printallstack, lua_printallstack},
    {lt::listk, listk},
    {lt::listabslineinfo, listabslineinfo},
    {lt::listlocals, listlocals},
    {lt::loadlib, loadlib},
    {lt::checkpanic, checkpanic},
    {lt::newstate, newstate},
    {lt::newuserdata, newuserdata},
    {lt::num2int, num2int},
    {lt::makeseed, makeseed},
    {lt::pushuserdata, pushuserdata},
    {lt::gcquery, gc_query},
    {lt::querystr, string_query},
    {lt::querytab, table_query},
    {lt::codeparam, test_codeparam},
    {lt::applyparam, test_applyparam},
    {lt::ref, tref},
    {lt::resume, coresume},
    {lt::s2d, s2d},
    {lt::sethook, sethook},
    {lt::stacklevel, stacklevel},
    {lt::resetCI, resetCI},
    {lt::reallocstack, reallocstack},
    {lt::sizes, get_sizes},
    {lt::testC, testC},
    {lt::makeCfunc, makeCfunc},
    {lt::totalmem, mem_query},
    {lt::alloccount, alloc_count},
    {lt::allocfailnext, alloc_failnext},
    {lt::trick, settrick},
    {lt::udataval, udataval},
    {lt::unref, unref},
    {lt::upvalue, upvalue},
    {lt::externKstr, externKstr},
    {lt::externstr, externstr},
    {lt::nonblock, nonblock}};

static void checkfinalmem(void)
{
  if (l_memcontrol.numblocks != 0) {
    fprintf(stderr, "[checkfinalmem] numblocks=%lu total=%lu\n",
            l_memcontrol.numblocks, l_memcontrol.total);
  }
  assert(l_memcontrol.numblocks == 0);
  assert(l_memcontrol.total == 0);
}

int luaB_opentests(lua_State* L)
{
  void* ud;
  lua_Allocator a = lua_getallocf(L); lua_Alloc f = a.f; ud = a.ud;
  lua_atpanic(L, &tpanic);
  lua_setwarnf(L, &warnf, L);
  lua_pushboolean(L, 0);
  lua_setglobal(L, HostString(lt::_WARN)); /* _WARN = false */
  regcodes(L);
  atexit(checkfinalmem);
  assert(f == debug_realloc && ud == &l_memcontrol);
  lua_setallocf(L, f, ud); /* exercise this function */
  luaL_newlibi(L, tests_funcs);
  return 1;
}

lua_State* luai_newstate()
{
  return lua_newstate(debug_realloc, &l_memcontrol, luaL_makeseed(nullptr));
}

void luai_openlibstest(lua_State* L)
{
  luaL_openlibs(L);
  luaL_requiref(L, HostString(lt::T_char), luaB_opentests, 1);
  lua_pop(L, 1);
}

#endif
