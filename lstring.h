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

static l_forceinline int isreserved(const TString* s)
{
  return s->extra > 0;
}

static l_forceinline int eqshrstr(const TString* a, const TString* b)
{
  return a == b;
}

LUAI_FUNC int      luaS_eqstr(TString* a, TString* b);
LUAI_FUNC void     luaS_init(lua_State* L);
LUAI_FUNC void     luaS_remove(lua_State* L, TString* ts);
LUAI_FUNC void     luaS_forcehash(TString* ts);
LUAI_FUNC Udata*   luaS_newudata(lua_State* L, size_t s, unsigned short nuvalue);
LUAI_FUNC TString* luaS_createstr(lua_State* L, byte_view str, size_t off, size_t len);
LUAI_FUNC TString* luaS_take(lua_State* L, InternalString&& str);
LUAI_FUNC TString* luaS_takeexternal(lua_State* L, InternalString&& str);

/*
** String hash, computed lazily for long strings (short strings hash at
** creation for interning).
*/
static l_forceinline unsigned int luaS_gethash(TString* ts)
{
  if (l_unlikely(!ts->hashed))
    luaS_forcehash(ts);
  return ts->hash;
}
