/*
** $Id: lfunc.h $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#pragma once

#include "lobject.h"

/* test whether thread is in 'twups' list */
static l_forceinline int isintwups(lua_State* L)
{
  return L->twups != L;
}

/*
** maximum number of upvalues in a closure (both C and Lua). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL 255

static l_forceinline int upisopen(const UpVal* up)
{
  return up->previous != NULL;
}

/* Get the stack index for an open upvalue */
static l_forceinline StkId uplevel(const UpVal* up)
{
  lua_assert(upisopen(up));
  return up->stkid;
}

/*
** Current location of an upvalue's value: a slot in the owner
** thread's stack while open, its own embedded storage when closed.
*/
static l_forceinline TValue* upvalval(UpVal* uv)
{
  return upisopen(uv) ? &uv->owner->stack.p[uv->stkid] : &uv->value;
}

/*
** maximum number of misses before giving up the cache of closures
** in prototypes
*/
#define MAXMISS 10

/* special status to close upvalues preserving the top of the stack */
#define CLOSEKTOP (LUA_ERRERR + 1)

LUAI_FUNC Proto* luaF_newproto(lua_State* L);
LUAI_FUNC CClosure* luaF_newCclosure(lua_State* L, int nupvals);
LUAI_FUNC LClosure* luaF_newLclosure(lua_State* L, int nupvals);
LUAI_FUNC void luaF_initupvals(lua_State* L, LClosure* cl);
LUAI_FUNC UpVal* luaF_findupval(lua_State* L, StkId level);
LUAI_FUNC void luaF_newtbcupval(lua_State* L, StkId level);
LUAI_FUNC void luaF_closeupval(lua_State* L, StkId level);
LUAI_FUNC StkId luaF_close(lua_State* L, StkId level, TStatus status, int yy);
LUAI_FUNC void luaF_unlinkupval(UpVal* uv);
LUAI_FUNC lu_mem luaF_protosize(Proto* p);
LUAI_FUNC void luaF_freeproto(lua_State* L, Proto* f);
LUAI_FUNC const char* luaF_getlocalname(const Proto* func, int local_number,
                                        int pc);
