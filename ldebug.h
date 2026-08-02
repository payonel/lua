/*
** $Id: ldebug.h $
** Auxiliary functions from Debug Interface module
** See Copyright Notice in lua.h
*/

#pragma once

#include "lstate.h"

/* Active Lua function (given call info) */
static l_forceinline LClosure* ci_func(lua_State* L, const CallInfo* ci)
{
  return slotclLval(L, ci->func);
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
LUAI_FUNC const InternalString* luaG_findlocal(lua_State* L, CallInfo* ci, int n,
                                     StkId* pos);

constexpr StkId UPVALORIGIN = ~(StkId)0 - 256;

static l_forceinline StkId upvalorigin(int uvidx)
{
  return UPVALORIGIN + (StkId)uvidx;
}

LUAI_FUNC l_noret luaG_typeerror(lua_State* L, const TValue* o,
                                 const InternalString* opname, StkId src);
LUAI_FUNC l_noret luaG_callerror(lua_State* L, const TValue* o, StkId src);
LUAI_FUNC l_noret luaG_forerror(lua_State* L, const TValue* o,
                                const InternalString* what);
LUAI_FUNC l_noret luaG_concaterror(lua_State* L, const TValue* p1,
                                   const TValue* p2,
                                   StkId src1, StkId src2);
LUAI_FUNC l_noret luaG_opinterror(lua_State* L, const TValue* p1,
                                  const TValue* p2,
                                  const InternalString* msg,
                                  StkId src1, StkId src2);
LUAI_FUNC l_noret luaG_tointerror(lua_State* L, const TValue* p1,
                                  const TValue* p2,
                                  StkId src1, StkId src2);
LUAI_FUNC l_noret luaG_ordererror(lua_State* L, const TValue* p1,
                                  const TValue* p2);

static l_forceinline l_noret luaG_forerror(lua_State* L, StkId idx,
                                           const InternalString* what)
{
  TValue v = getslot(L, idx);
  luaG_forerror(L, &v, what);
}

static l_forceinline l_noret luaG_callerror(lua_State* L, StkId idx)
{
  TValue v = getslot(L, idx);
  luaG_callerror(L, &v, idx);
}

static l_forceinline l_noret luaG_concaterror(lua_State* L, StkId p1, StkId p2)
{
  TValue v1 = getslot(L, p1);
  TValue v2 = getslot(L, p2);
  luaG_concaterror(L, &v1, &v2, p1, p2);
}

LUAI_FUNC l_noret luaG_errnnil(lua_State* L, LClosure* cl, int k);
LUAI_FUNC l_noret luaG_runerror(lua_State* L, std::string_view fmt, const FmtArgs& args);
LUAI_FUNC l_noret luaG_runerror(lua_State* L, std::string_view fmt);
LUAI_FUNC const InternalString* luaG_addinfo(lua_State* L, std::string_view msg, TString* src, int line);
LUAI_FUNC l_noret luaG_errormsg(lua_State* L);
LUAI_FUNC int luaG_traceexec(lua_State* L, int pc);
LUAI_FUNC int luaG_tracecall(lua_State* L);
