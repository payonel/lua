/*
** $Id: lfunc.c $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/



#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "literals.h"

namespace lua::object
{

CFuncInfo* CFuncInfo::luaF_newCFuncInfo(lua_State* L, lua_CFunction f)
{
  CFuncInfo* cfi = luaM_malloc_CFuncInfo(L);
  luaC_link(L, cfi, LUA_VLCF);
  cfi->fptr = f;
  return cfi;
}

CClosure* CClosure::luaF_newCclosure(lua_State* L, int nupvals)
{
  global_State* g = L->l_G;
  CClosure* c = luaM_malloc_CClosure(L);
  luaC_link(L, c, LUA_VCCL);
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1;
  std::span<TValue> upvalue = luaM_alloc_TValue_span(L->l_G, L, {}, nupvals, false);
  g->gcstopem = oldgcstop;
  if (upvalue.data() == nullptr && nupvals > 0)
    luaD_throw(L, LUA_ERRMEM);
  c->upvalue = upvalue;
  return c;
}

LClosure* LClosure::luaF_newLclosure(lua_State* L, int nupvals)
{
  global_State* g = L->l_G;
  LClosure* c = luaM_malloc_LClosure(L);
  luaC_link(L, c, LUA_VLCL);
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1;
  std::span<UpVal*> upvals = luaM_alloc_UpVal_ptr_span(L->l_G, L, {}, nupvals, false);
  g->gcstopem = oldgcstop;
  if (upvals.data() == nullptr && nupvals > 0)
    luaD_throw(L, LUA_ERRMEM);
  c->upvals = upvals;
  while (nupvals--)
    c->upvals[nupvals] = nullptr;
  return c;
}

/*
** fill a closure with new closed upvalues
*/
void LClosure::luaF_initupvals(lua_State* L)
{
  for (size_t i = 0; i < upvals.size(); i++)
  {
    UpVal* uv = luaM_malloc_UpVal(L);
    luaC_link(L, uv, LUA_VUPVAL);
    upvals[i] = uv;
    luaC_objbarrier(L, this, uv);
  }
}

/*
** Create a new upvalue at the given level, and link it to the list of
** open upvalues of 'L' after entry 'prev'.
**/
UpVal* UpVal::newupval(lua_State* L, StkId level, UpVal* prev)
{
  UpVal* uv = luaM_malloc_UpVal(L);
  luaC_link(L, uv, LUA_VUPVAL);
  UpVal* next = (prev != nullptr) ? prev->opennext : L->openupval;
  uv->owner = L;
  uv->stkid = level;
  uv->opennext = next;        /* link it to list of open upvalues */
  uv->prevopen = prev;
  if (next)
    next->prevopen = uv;
  if (prev != nullptr)
    prev->opennext = uv;
  else
    L->openupval = uv;
  if (!isintwups(L))
  {                         /* thread not in list of threads with upvalues? */
    L->twups = L->l_G->twups; /* link it to the list */
    L->l_G->twups = L;
  }
  return uv;
}

/*
** Find and reuse, or create if it does not exist, an upvalue
** at the given level.
*/
UpVal* UpVal::luaF_findupval(lua_State* L, StkId level)
{
  UpVal* pp = nullptr;
  UpVal* p = L->openupval;
  assert(isintwups(L) || L->openupval == nullptr);
  while (p != nullptr && uplevel(p) >= level)
  { /* search for it */
    assert(!isdead(L->l_G, p));
    if (uplevel(p) == level) /* corresponding upvalue? */
      return p;              /* return it */
    pp = p;
    p = p->opennext;
  }
  /* not found: create a new upvalue after 'pp' */
  return UpVal::newupval(L, level, pp);
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
  { /* (cannot freeze) */
    L->nfz++;
    luaD_callnoyield(L, func, 0);
    L->nfz--;
  }
}

/*
** Check whether object at given level has a close metamethod and raise
** an error if not.
*/
static void checkclosemth(lua_State* L, StkId level)
{
  const TValue* tm = luaT_gettmbyobj(L, level, TM_CLOSE);
  if (ttisnil(tm))
  {                                            /* no metamethod? */
    int idx = (int)(level - L->ci->func); /* variable index */
    const InternalString* vname_i = luaG_findlocal(L, L->ci, idx).name;
    byte_view vname = vname_i != nullptr ? vname_i->view() : lt::qmark;
    luaG_runerror(L, lt::variable_s_got_a_non_closable_value, FmtArgs().str(vname));
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
  TValue errobj;
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
    luaD_seterrorobj(L, status, level + 1);
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
  if (tagisfalse(slottag(L, level)))
    return;                /* false doesn't need to be closed */
  checkclosemth(L, level); /* value must have a close method */
  {
    L->tbc = luaM_safegrow_StkId_span(L, L->tbc, L->ntbc);
  }
  L->tbc[L->ntbc++] = level;
  L->tbclist = level;
}

void UpVal::luaF_unlinkupval()
{
  assert(upisopen(this));
  if (prevopen != nullptr)
    prevopen->opennext = opennext;
  else
    owner->openupval = opennext;
  if (opennext)
    opennext->prevopen = prevopen;
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
    uv->luaF_unlinkupval();       /* remove upvalue from 'openupval' list */
    slot2obj(L, slot, uv->stkid); /* move value to upvalue slot */
    uv->prevopen = nullptr;
    uv->opennext = nullptr;
    uv->owner = nullptr;
    if (!iswhite(uv))
    {               /* neither white nor dead? */
      nw2black(uv); /* closed upvalues cannot be gray */
      luaC_barrier(L, uv, slot);
    }
  }
}

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
  StkId levelrel = level;
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

Proto* Proto::luaF_newproto(lua_State* L)
{
  Proto* f = luaM_malloc_Proto(L);
  luaC_link(L, f, LUA_VPROTO);
  return f;
}

l_mem Proto::luaF_protosize()
{
  l_mem sz = cost::Proto
              + p.size() * cost::Ref
              + k.size() * cost::TValue
              + locvars.size() * cost::LocVar
              + upvalues.size() * cost::Upvaldesc;
  if (!(flag & PF_FIXED))
  {
    sz += code.size() * cost::Instruction;
    sz += lineinfo.size() * cost::Byte;
    sz += abslineinfo.size() * cost::AbsLineInfo;
  }
  return sz;
}

void Proto::luaF_freeproto(lua_State* L)
{
  if (!(flag & PF_FIXED))
  {
    luaM_alloc_Instruction_span(L->l_G, L, code, 0, false);
    luaM_alloc_ls_byte_span(L->l_G, L, lineinfo, 0, false);
    luaM_alloc_AbsLineInfo_span(L->l_G, L, abslineinfo, 0, false);
  }
  luaM_alloc_Proto_ptr_span(L->l_G, L, p, 0, false);
  luaM_alloc_TValue_span(L->l_G, L, k, 0, false);
  luaM_alloc_LocVar_span(L->l_G, L, locvars, 0, false);
  luaM_alloc_Upvaldesc_span(L->l_G, L, upvalues, 0, false);
  luaM_free_Proto(L, this);
}

const InternalString* Proto::luaF_getlocalname(int local_number, int pc) const
{
  for (size_t i = 0; i < locvars.size() && locvars[i].startpc <= pc; i++)
  {
    if (pc < locvars[i].endpc)
    { /* is variable active? */
      local_number--;
      if (local_number == 0)
        return &locvars[i].varname->data;
    }
  }
  return nullptr; /* not found */
}

}
