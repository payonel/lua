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
LUAI_FUNC const char* luaG_findlocal(lua_State* L, CallInfo* ci, int n,
                                     StkId* pos);
/*
** Error reporters take an explicit operand origin ('src'): the stack
** index of the offending value when it lives in the current frame, or
** NOSLOT when it does not (constants, immediates, metatable values). The
** origin drives error attribution ("global 'x'", "local 'y'") — it
** replaces recovering the register by scanning the stack for pointer
** identity, which by-value operand rows (and the managed port) cannot
** support.
*/
LUAI_FUNC l_noret luaG_typeerror(lua_State* L, const TValue* o,
                                 const char* opname, StkId src = NOSLOT);
LUAI_FUNC l_noret luaG_callerror(lua_State* L, const TValue* o, StkId src = NOSLOT);
LUAI_FUNC l_noret luaG_forerror(lua_State* L, const TValue* o,
                                const char* what);
LUAI_FUNC l_noret luaG_concaterror(lua_State* L, const TValue* p1,
                                   const TValue* p2,
                                   StkId src1 = NOSLOT, StkId src2 = NOSLOT);
LUAI_FUNC l_noret luaG_opinterror(lua_State* L, const TValue* p1,
                                  const TValue* p2,
                                  const char* msg,
                                  StkId src1 = NOSLOT, StkId src2 = NOSLOT);
LUAI_FUNC l_noret luaG_tointerror(lua_State* L, const TValue* p1,
                                  const TValue* p2,
                                  StkId src1 = NOSLOT, StkId src2 = NOSLOT);
LUAI_FUNC l_noret luaG_ordererror(lua_State* L, const TValue* p1,
                                  const TValue* p2);

/*
** Slot-form overloads: materialize row copies of the offending slots
** (the originals stay anchored in the stack while the message is built).
*/
static l_forceinline l_noret luaG_forerror(lua_State* L, StkId idx, const char* what)
{
  TValue v = getslot(L, idx);
  luaG_forerror(L, &v, what);
}

static l_forceinline l_noret luaG_callerror(lua_State* L, StkId idx)
{
  TValue v = getslot(L, idx);
  luaG_callerror(L, &v);
}

static l_forceinline l_noret luaG_concaterror(lua_State* L, StkId p1, StkId p2)
{
  TValue v1 = getslot(L, p1);
  TValue v2 = getslot(L, p2);
  luaG_concaterror(L, &v1, &v2);
}

LUAI_FUNC l_noret luaG_errnnil(lua_State* L, LClosure* cl, int k);
LUAI_FUNC l_noret luaG_runerror(lua_State* L, const char* fmt,
                                const FmtArgs& args);
LUAI_FUNC l_noret luaG_runerror(lua_State* L, const char* fmt);
LUAI_FUNC const char* luaG_addinfo(lua_State* L, const char* msg,
                                   TString* src, int line);
LUAI_FUNC l_noret luaG_errormsg(lua_State* L);
LUAI_FUNC int luaG_traceexec(lua_State* L, int pc);
LUAI_FUNC int luaG_tracecall(lua_State* L);
