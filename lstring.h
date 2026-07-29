/*
** $Id: lstring.h $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#pragma once

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

/*
** Memory-allocation error message must be preallocated (it cannot
** be created after memory is exhausted)
*/
constexpr char MEMERRMSG[] = "not enough memory";

/*
** test whether a string is a reserved word
*/
static l_forceinline int isreserved(const TString* s)
{
  return s->extra > 0;
}

/*
** equality for strings
*/
static l_forceinline int eqshrstr(const TString* a, const TString* b)
{
  return a == b;
}

LUAI_FUNC int luaS_eqstr(TString* a, TString* b);
LUAI_FUNC void luaS_clearcache(global_State* g);
LUAI_FUNC void luaS_init(lua_State* L);
LUAI_FUNC void luaS_remove(lua_State* L, TString* ts);
LUAI_FUNC Udata* luaS_newudata(lua_State* L, size_t s,
                               unsigned short nuvalue);
LUAI_FUNC TString* luaS_newlstr(lua_State* L, const char* str, size_t l);
LUAI_FUNC TString* luaS_new(lua_State* L, const char* str);
LUAI_FUNC TString* luaS_new(lua_State* L, const InternalString* str);
LUAI_FUNC TString* luaS_createstr(lua_State* L, const char* str, size_t l);
