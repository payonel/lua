/*
** $Id: lapi.h $
** Auxiliary functions from Lua API
** See Copyright Notice in lua.h
*/

#pragma once

#include "llimits.h"
#include "lstate.h"

#define api_check(l, e, msg) ((void) (l), lua_assert((e) && msg))

/* Increments 'L->top', checking for stack overflows */
#define api_incr_top(L) (L->top++, api_check(L, L->top <= L->ci->top, "stack overflow"))

/*
** If a call returns too many multiple returns, the callee may not have
** stack space to accommodate all results. In this case, this macro
** increases its stack space ('L->ci->top').
*/
#define adjustresults(L, nres)                        \
  {                                                   \
    if ((nres) <= LUA_MULTRET && L->ci->top < L->top) \
      L->ci->top = L->top;                            \
  }

/* Ensure the stack has at least 'n' elements */
#define api_checknelems(L, n) api_check(L, ((StkId)(n)) < (L->top - L->ci->func), "not enough elements in the stack")

/* Ensure the stack has at least 'n' elements to be popped. (Some
** functions only update a slot after checking it for popping, but that
** is only an optimization for a pop followed by a push.)
*/
#define api_checkpop(L, n) api_check(L, ((StkId)(n)) < L->top - L->ci->func && L->tbclist < L->top - (n), "not enough free elements in the stack")
