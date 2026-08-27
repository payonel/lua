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
constexpr int MAXUPVAL = 255;

static l_forceinline int upisopen(const UpVal* up)
{
  return up->previous != nullptr;
}

static l_forceinline StkId uplevel(const UpVal* up)
{
  assert(upisopen(up));
  return up->stkid;
}

static l_forceinline TValue upvalgetval(const UpVal* uv)
{
  return upisopen(uv) ? getslot(uv->owner, uv->stkid) : uv->value;
}

static l_forceinline void upvalsetval(lua_State* L, UpVal* uv, const TValue* v)
{
  if (upisopen(uv))
    setslot(uv->owner, uv->stkid, v);
  else
  {
    setobj(L, &uv->value, v);
  }
}

/*
** maximum number of misses before giving up the cache of closures
** in prototypes
*/
constexpr int MAXMISS = 10;

/* special status to close upvalues preserving the top of the stack */
constexpr int CLOSEKTOP = (LUA_ERRERR + 1);

LUAI_FUNC void luaF_newtbcupval(lua_State* L, StkId level);
LUAI_FUNC void luaF_closeupval(lua_State* L, StkId level);
LUAI_FUNC StkId luaF_close(lua_State* L, StkId level, TStatus status, int yy);
