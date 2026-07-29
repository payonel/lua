/*
** $Id: lfunc.c $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/


#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"

/*
** Closures are two luaM blocks: the header (a GC object) and its
** upvalue array. The header is created first, in a consistent
** array-less state (nupvalues == 0), so the collector can free it at
** any point. The array is then allocated with the collector stopped
** ('gcstopem', the ldo.cpp stack-reallocation precedent): the header
** is not yet anchored anywhere, and the emergency collection that a
** failing allocation would run could sweep it. On failure we throw with the
** header still consistent — the next collection cycle reclaims it.
** No unwind cleanup is needed on any path (unprotected errors reach
** the panic function, which may exit with longjmp — C++ unwinding
** cannot be relied on here).
*/
CClosure* luaF_newCclosure(lua_State* L, int nupvals)
{
  global_State* g = G(L);
  GCObject* o = luaC_newobj(L, LUA_VCCL, sizeof(CClosure));
  assert(o->tt == LUA_VCCL);
  CClosure* c = static_cast<CClosure*>(o);
  c->nupvalues = 0; /* no upvalue array yet */
  c->upvalue = nullptr;
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1; /* no emergency collection: the header is unanchored */
  TValue* upvalue = (TValue*)luaM_realloc_(L, nullptr, 0, (size_t)(unsigned)nupvals * sizeof(TValue));
  g->gcstopem = oldgcstop;
  if (upvalue == nullptr && nupvals > 0)
    luaD_throw(L, LUA_ERRMEM);
  c->upvalue = upvalue;
  c->nupvalues = ((lu_byte)(nupvals));
  return c;
}

LClosure* luaF_newLclosure(lua_State* L, int nupvals)
{
  global_State* g = G(L);
  GCObject* o = luaC_newobj(L, LUA_VLCL, sizeof(LClosure));
  assert(o->tt == LUA_VLCL);
  LClosure* c = static_cast<LClosure*>(o);
  c->p = nullptr;
  c->nupvalues = 0; /* no upvalue array yet */
  c->upvals = nullptr;
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1; /* no emergency collection: the header is unanchored */
  UpVal** upvals = (UpVal**)luaM_realloc_(L, nullptr, 0, (size_t)(unsigned)nupvals * sizeof(UpVal*));
  g->gcstopem = oldgcstop;
  if (upvals == nullptr && nupvals > 0)
    luaD_throw(L, LUA_ERRMEM);
  c->upvals = upvals;
  c->nupvalues = ((lu_byte)(nupvals));
  while (nupvals--)
    c->upvals[nupvals] = nullptr;
  return c;
}

/*
** fill a closure with new closed upvalues
*/
void luaF_initupvals(lua_State* L, LClosure* cl)
{
  int i;
  for (i = 0; i < cl->nupvalues; i++)
  {
    GCObject* o = luaC_newobj(L, LUA_VUPVAL, sizeof(UpVal));
    assert(o->tt == LUA_VUPVAL);
    UpVal* uv = static_cast<UpVal*>(o);
    uv->opennext = nullptr;
    uv->previous = nullptr;     /* nullptr = closed */
    uv->owner = nullptr;
    uv->stkid = 0;
    ((&uv->value)->tt_ = LUA_VNIL); /* closed: value lives in own storage */
    cl->upvals[i] = uv;
    luaC_objbarrier(L, cl, uv);
  }
}

/*
** Create a new upvalue at the given level, and link it to the list of
** open upvalues of 'L' after entry 'prev'.
**/
static UpVal* newupval(lua_State* L, StkId level, UpVal** prev)
{
  GCObject* o = luaC_newobj(L, LUA_VUPVAL, sizeof(UpVal));
  assert(o->tt == LUA_VUPVAL);
  UpVal* uv = static_cast<UpVal*>(o);
  UpVal* next = *prev;
  uv->owner = L;           /* current value lives in this thread's stack */
  uv->stkid = level;
  uv->opennext = next;        /* link it to list of open upvalues */
  uv->previous = prev;
  if (next)
    next->previous = &uv->opennext;
  *prev = uv;
  if (!isintwups(L))
  {                         /* thread not in list of threads with upvalues? */
    L->twups = G(L)->twups; /* link it to the list */
    G(L)->twups = L;
  }
  return uv;
}

/*
** Find and reuse, or create if it does not exist, an upvalue
** at the given level.
*/
UpVal* luaF_findupval(lua_State* L, StkId level)
{
  UpVal** pp = &L->openupval;
  UpVal* p;
  assert(isintwups(L) || L->openupval == nullptr);
  while ((p = *pp) != nullptr && uplevel(p) >= level)
  { /* search for it */
    assert(!isdead(G(L), p));
    if (uplevel(p) == level) /* corresponding upvalue? */
      return p;              /* return it */
    pp = &p->opennext;
  }
  /* not found: create a new upvalue after 'pp' */
  return newupval(L, level, pp);
}

/*
** Call closing method for object 'obj' with error object 'err'. The
** boolean 'yy' controls whether the call is yieldable.
** (This function assumes EXTRA_STACK.)
*/
static void callclosemethod(lua_State* L, TValue* obj, TValue* err, int yy)
{
  StkId top = L->top;
  StkId func = top;
  const TValue* tm = luaT_gettmbyobj(L, obj, TM_CLOSE);
  setslot(L, top++, tm);    /* will call metamethod... */
  setslot(L, top++, obj);   /* with 'self' as the 1st argument */
  if (err != nullptr)           /* if there was an error... */
    setslot(L, top++, err); /* then error object will be 2nd argument */
  L->top = top;            /* add function and arguments */
  if (yy)
    luaD_call(L, func, 0);
  else
    luaD_callnoyield(L, func, 0);
}

/*
** Check whether object at given level has a close metamethod and raise
** an error if not.
*/
static void checkclosemth(lua_State* L, StkId level)
{
  const TValue* tm = luaT_gettmbyobj(L, level, TM_CLOSE);
  if ((((tm)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
  {                                            /* no metamethod? */
    int idx = ((int)(level - L->ci->func)); /* variable index */
    const char* vname = luaG_findlocal(L, L->ci, idx, nullptr);
    if (vname == nullptr)
      vname = "?";
    luaG_runerror(L, lit<"variable '%s' got a non-closable value">(),
                  FmtArgs().str(vname));
  }
}

/*
** Prepare and call a closing method.
** If status is CLOSEKTOP, the call to the closing method will be pushed
** at the top of the stack. Otherwise, values can be pushed right after
** the 'level' of the upvalue being closed, as everything after that
** won't be used again.
*/
static void prepcallclosemth(lua_State* L, StkId level, TStatus status,
                             int yy)
{
  StkId uv = level; /* value being closed */
  TValue errobj;    /* row copy of the error object, valid when 'haserr' */
  int haserr;
  switch (status)
  {
  case LUA_OK:
    L->top = level + 1; /* call will be at this level */
    /* FALLTHROUGH */
  case CLOSEKTOP:  /* don't need to change top */
    haserr = 0; /* no error object */
    break;
  default:                                  /* 'luaD_seterrorobj' will set top to level + 2 */
    luaD_seterrorobj(L, status, level + 1); /* set error object after 'uv' */
    errobj = getslot(L, level + 1);
    haserr = 1;
    break;
  }
  TValue uvv = getslot(L, uv);
  callclosemethod(L, &uvv, haserr ? &errobj : nullptr, yy);
}

/*
** Insert a variable in the list of to-be-closed variables.
*/
void luaF_newtbcupval(lua_State* L, StkId level)
{
  assert(level > L->tbclist);
  if ((slottag(L, level) == LUA_VFALSE) || ((slottag(L, level) & NOVARIANT_MASK) == LUA_TNIL))
    return;                /* false doesn't need to be closed */
  checkclosemth(L, level); /* value must have a close method */
  int limit = ((size_t)INT_MAX <= MAX_SIZET / sizeof(StkId))
                  ? INT_MAX
                  : (int)(MAX_SIZET / sizeof(StkId));
  L->tbc = (StkId*)luaM_growaux_(L, L->tbc, L->ntbc, &L->sizetbc,
                                 sizeof(StkId), limit, "to-be-closed variables");
  L->tbc[L->ntbc++] = level;
  L->tbclist = level;
}

void luaF_unlinkupval(UpVal* uv)
{
  assert(upisopen(uv));
  *uv->previous = uv->opennext;
  if (uv->opennext)
    uv->opennext->previous = uv->previous;
}

/*
** Close all upvalues up to the given stack level.
*/
void luaF_closeupval(lua_State* L, StkId level)
{
  UpVal* uv;
  while ((uv = L->openupval) != nullptr && uplevel(uv) >= level)
  {
    TValue* slot = &uv->value; /* new position for value */
    assert(uplevel(uv) < L->top);
    luaF_unlinkupval(uv);       /* remove upvalue from 'openupval' list */
    setobj(L, slot, uv->stkid); /* move value to upvalue slot */
    uv->previous = nullptr;        /* mark as closed */
    uv->opennext = nullptr;
    uv->owner = nullptr;
    if (!iswhite(uv))
    {               /* neither white nor dead? */
      nw2black(uv); /* closed upvalues cannot be gray */
      luaC_barrier(L, uv, slot);
    }
  }
}

/*
** Remove first element from the tbclist.
*/
static void poptbclist(lua_State* L)
{
  assert(L->ntbc > 0 && L->tbc[L->ntbc - 1] == L->tbclist);
  L->ntbc--;
  L->tbclist = (L->ntbc > 0) ? L->tbc[L->ntbc - 1] : 0;
}

/*
** Close all upvalues and to-be-closed variables up to the given stack
** level. Return restored 'level'.
*/
StkId luaF_close(lua_State* L, StkId level, TStatus status, int yy)
{
  StkId levelrel = (level);
  luaF_closeupval(L, level); /* first, close the upvalues */
  while (L->tbclist >= level)
  {                                       /* traverse tbc's down to that level */
    StkId tbc = L->tbclist;             /* get variable index */
    poptbclist(L);                        /* remove it from list */
    prepcallclosemth(L, tbc, status, yy); /* close variable */
    level = (static_cast<StkId>(levelrel));
  }
  return level;
}

Proto* luaF_newproto(lua_State* L)
{
  GCObject* o = luaC_newobj(L, LUA_VPROTO, sizeof(Proto));
  assert(o->tt == LUA_VPROTO);
  Proto* f = static_cast<Proto*>(o);
  f->k = nullptr;
  f->sizek = 0;
  f->p = nullptr;
  f->sizep = 0;
  f->code = nullptr;
  f->sizecode = 0;
  f->lineinfo = nullptr;
  f->sizelineinfo = 0;
  f->abslineinfo = nullptr;
  f->sizeabslineinfo = 0;
  f->upvalues = nullptr;
  f->sizeupvalues = 0;
  f->numparams = 0;
  f->flag = 0;
  f->maxstacksize = 0;
  f->locvars = nullptr;
  f->sizelocvars = 0;
  f->linedefined = 0;
  f->lastlinedefined = 0;
  f->source = nullptr;
  return f;
}

lu_mem luaF_protosize(Proto* p)
{
  lu_mem sz = (lu_mem)sizeof(Proto)
              + (unsigned int)p->sizep * sizeof(Proto*)
              + (unsigned int)p->sizek * sizeof(TValue)
              + (unsigned int)p->sizelocvars * sizeof(LocVar)
              + (unsigned int)p->sizeupvalues * sizeof(Upvaldesc);
  if (!(p->flag & PF_FIXED))
  {
    sz += ((unsigned int)(p->sizecode)) * sizeof(Instruction);
    sz += ((unsigned int)(p->sizelineinfo)) * sizeof(lu_byte);
    sz += ((unsigned int)(p->sizeabslineinfo)) * sizeof(AbsLineInfo);
  }
  return sz;
}

void luaF_freeproto(lua_State* L, Proto* f)
{
  if (!(f->flag & PF_FIXED))
  {
    luaM_free_(L, (f->code), (((size_t)(f->sizecode))) * sizeof(*(f->code)));
    luaM_free_(L, (f->lineinfo), (((size_t)(f->sizelineinfo))) * sizeof(*(f->lineinfo)));
    luaM_free_(L, (f->abslineinfo), (((size_t)(f->sizeabslineinfo))) * sizeof(*(f->abslineinfo)));
  }
  luaM_free_(L, (f->p), (((size_t)(f->sizep))) * sizeof(*(f->p)));
  luaM_free_(L, (f->k), (((size_t)(f->sizek))) * sizeof(*(f->k)));
  luaM_free_(L, (f->locvars), (((size_t)(f->sizelocvars))) * sizeof(*(f->locvars)));
  luaM_free_(L, (f->upvalues), (((size_t)(f->sizeupvalues))) * sizeof(*(f->upvalues)));
  luaM_free_(L, (f), sizeof(*(f)));
}

/*
** Look for n-th local variable at line 'line' in function 'func'.
** Returns nullptr if not found.
*/
const char* luaF_getlocalname(const Proto* f, int local_number, int pc)
{
  int i;
  for (i = 0; i < f->sizelocvars && f->locvars[i].startpc <= pc; i++)
  {
    if (pc < f->locvars[i].endpc)
    { /* is variable active? */
      local_number--;
      if (local_number == 0)
        return getstr(f->locvars[i].varname);
    }
  }
  return nullptr; /* not found */
}
