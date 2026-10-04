/*
** $Id: ldebug.h $
** Auxiliary functions from Debug Interface module
** See Copyright Notice in lua.h
*/

#pragma once

#include "lstate.h"

namespace lua::vm
{

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

int luaG_getfuncline(const Proto* f, int pc);
struct LocalSlot
{
  const InternalString* name;
  StkId pos;
};

LocalSlot luaG_findlocal(lua_State* L, CallInfo* ci, int n);

constexpr StkId UPVALORIGIN = NOSLOT - 1;

static l_forceinline StkId upvalorigin(int uvidx)
{
  return UPVALORIGIN - uvidx;
}

[[noreturn]] void luaG_typeerror(lua_State* L, const TValue* o, byte_view opname, StkId src);
[[noreturn]] void luaG_callerror(lua_State* L, const TValue* o, StkId src);
[[noreturn]] void luaG_forerror(lua_State* L, const TValue* o, byte_view what);
[[noreturn]] void luaG_concaterror(lua_State* L, const TValue* p1, const TValue* p2, StkId src1, StkId src2);
[[noreturn]] void luaG_opinterror(lua_State* L, const TValue* p1, const TValue* p2, byte_view msg, StkId src1, StkId src2);
[[noreturn]] void luaG_tointerror(lua_State* L, const TValue* p1, const TValue* p2, StkId src1, StkId src2);
[[noreturn]] void luaG_ordererror(lua_State* L, const TValue* p1, const TValue* p2);

static l_forceinline [[noreturn]] void luaG_forerror(lua_State* L, StkId idx, byte_view what)
{
  TValue v = getslot(L, idx);
  luaG_forerror(L, &v, what);
}

static l_forceinline [[noreturn]] void luaG_callerror(lua_State* L, StkId idx)
{
  TValue v = getslot(L, idx);
  luaG_callerror(L, &v, idx);
}

static l_forceinline [[noreturn]] void luaG_concaterror(lua_State* L, StkId p1, StkId p2)
{
  TValue v1 = getslot(L, p1);
  TValue v2 = getslot(L, p2);
  luaG_concaterror(L, &v1, &v2, p1, p2);
}

[[noreturn]] void luaG_errnnil(lua_State* L, LClosure* cl, int k);
[[noreturn]] void luaG_runerror(lua_State* L, byte_view fmt, const FmtArgs& args);
[[noreturn]] void luaG_runerror(lua_State* L, byte_view fmt);
byte_view luaG_addinfo(lua_State* L, byte_view msg, TString* src, int line);
[[noreturn]] void luaG_errormsg(lua_State* L);
int luaG_traceexec(lua_State* L, int pc);
int luaG_tracecall(lua_State* L);

}
