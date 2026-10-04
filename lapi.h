/*
** $Id: lapi.h $
** Auxiliary functions from Lua API
** See Copyright Notice in lua.h
*/

#pragma once

#include "llimits.h"
#include "lstate.h"

namespace lua::api
{

static l_forceinline void api_incr_top(lua_State* L)
{
  L->top++;
  assert((L->top <= L->ci->top) && "stack overflow");
}

static l_forceinline void adjustresults(lua_State* L, int nres)
{
  if (nres <= LUA_MULTRET && L->ci->top < L->top)
    L->ci->top = L->top;
}

/* Ensure the stack has at least 'n' elements */
static l_forceinline void api_checknelems(lua_State* L, int n)
{
  (void) L;
  (void) n;
  assert((n < (L->top - L->ci->func)) && "not enough elements in the stack");
}

/* Ensure the stack has at least 'n' elements to be popped. (Some
** functions only update a slot after checking it for popping, but that
** is only an optimization for a pop followed by a push.)
*/
static l_forceinline void api_checkpop(lua_State* L, int n)
{
  (void) L;
  (void) n;
  assert((n < L->top - L->ci->func && L->tbclist < L->top - n) && "not enough free elements in the stack");
}

}
