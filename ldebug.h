/*
** $Id: ldebug.h $
** Auxiliary functions from Debug Interface module
** See Copyright Notice in lua.h
*/

#pragma once

#include "lstate.h"

static l_forceinline int pcRel(const Instruction* pc, const Proto* p)
{
  return (int)(pc - p->code) - 1;
}

/* Active Lua function (given call info) */
static l_forceinline LClosure* ci_func(lua_State* L, const CallInfo* ci)
{
  const TValue* o = &L->stack.p[ci->func];
  assert(o->tt_ == ctb(LUA_VLCL));
  assert(o->value_.gc->tt == LUA_VLCL);
  return static_cast<LClosure*>(o->value_.gc);
}

static l_forceinline void resethookcount(lua_State* L)
{
  L->hookcount = L->basehookcount;
}

/*
** mark for entries in 'lineinfo' array that has absolute information in
** 'abslineinfo' array
*/
constexpr int ABSLINEINFO = (-0x80);

LUAI_FUNC int luaG_getfuncline(const Proto* f, int pc);
LUAI_FUNC const char* luaG_findlocal(lua_State* L, CallInfo* ci, int n,
                                     StkId* pos);
LUAI_FUNC l_noret luaG_typeerror(lua_State* L, const TValue* o,
                                 const char* opname);
LUAI_FUNC l_noret luaG_callerror(lua_State* L, const TValue* o);
LUAI_FUNC l_noret luaG_forerror(lua_State* L, const TValue* o,
                                const char* what);
LUAI_FUNC l_noret luaG_concaterror(lua_State* L, const TValue* p1,
                                   const TValue* p2);
LUAI_FUNC l_noret luaG_opinterror(lua_State* L, const TValue* p1,
                                  const TValue* p2,
                                  const char* msg);
LUAI_FUNC l_noret luaG_tointerror(lua_State* L, const TValue* p1,
                                  const TValue* p2);
LUAI_FUNC l_noret luaG_ordererror(lua_State* L, const TValue* p1,
                                  const TValue* p2);
LUAI_FUNC l_noret luaG_errnnil(lua_State* L, LClosure* cl, int k);
LUAI_FUNC l_noret luaG_runerror(lua_State* L, const char* fmt, ...);
LUAI_FUNC const char* luaG_addinfo(lua_State* L, const char* msg,
                                   TString* src, int line);
LUAI_FUNC l_noret luaG_errormsg(lua_State* L);
LUAI_FUNC int luaG_traceexec(lua_State* L, const Instruction* pc);
LUAI_FUNC int luaG_tracecall(lua_State* L);
