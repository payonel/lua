/*
** $Id: lstring.h $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#pragma once

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "literals.h"

namespace lua::object
{

static l_forceinline bool isreserved(const TString* s)
{
  return s->extra > 0;
}

static l_forceinline bool eqshrstr(const TString* a, const TString* b)
{
  return a == b;
}

bool luaS_eqstr(TString* a, TString* b);
void luaS_init(lua_State* L);

static l_forceinline unsigned int luaS_gethash(TString* ts)
{
  if (l_unlikely(!ts->hashed))
    ts->luaS_forcehash();
  return ts->hash;
}

}
