/*
** $Id: ldo.h $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include "llimits.h"
#include "lobject.h"
#include "lstate.h"
#include "lzio.h"

namespace lua::runtime
{


/* type of protected functions, to be ran by 'runprotected' */
struct ProtectedCall
{
  virtual void run(lua_State* L) = 0;

protected:
  ~ProtectedCall() = default;
};

LUAI_FUNC l_noret luaD_errerr(lua_State* L);
LUAI_FUNC void luaD_seterrorobj(lua_State* L, TStatus errcode, StkId oldtop);
LUAI_FUNC TStatus luaD_protectedparser(lua_State* L, ZIO* z, std::optional<byte_view> name, std::optional<byte_view> mode);
LUAI_FUNC void luaD_hook(lua_State* L, int event, int line, int fTransfer, int nTransfer);
LUAI_FUNC void luaD_hookcall(lua_State* L, CallInfo* ci);
LUAI_FUNC int luaD_pretailcall(lua_State* L, CallInfo* ci, StkId func, int narg1, int delta);
LUAI_FUNC CallInfo* luaD_precall(lua_State* L, StkId func, int nResults);
LUAI_FUNC void luaD_call(lua_State* L, StkId func, int nResults);
LUAI_FUNC void luaD_callnoyield(lua_State* L, StkId func, int nResults);
LUAI_FUNC TStatus luaD_closeprotected(lua_State* L, StkId level, TStatus status);
LUAI_FUNC TStatus luaD_pcall(lua_State* L, ProtectedCall* func, StkId oldtop, StkId ef);
LUAI_FUNC void luaD_poscall(lua_State* L, CallInfo* ci, int nres);
LUAI_FUNC int luaD_reallocstack(lua_State* L, int newsize, int raiseerror);
LUAI_FUNC int luaD_growstack(lua_State* L, int n, int raiseerror);

static l_forceinline void luaD_checkstack(lua_State* L, int n)
{
  if (l_unlikely(L->stack_last - L->top <= n))
    luaD_growstack(L, n, 1);
}

static l_forceinline void checkstackp(lua_State* L, int n, StkId p)
{
  (void) p;
  luaD_checkstack(L, n);
}
LUAI_FUNC void luaD_shrinkstack(lua_State* L);
LUAI_FUNC void luaD_inctop(lua_State* L);
LUAI_FUNC int luaD_checkminstack(lua_State* L);
LUAI_FUNC void luaD_anchorobj(lua_State* L, Table* anchor, GCObject* obj);

LUAI_FUNC l_noret luaD_throw(lua_State* L, TStatus errcode);
LUAI_FUNC l_noret luaD_throwbaselevel(lua_State* L, TStatus errcode);
LUAI_FUNC TStatus luaD_rawrunprotected(lua_State* L, ProtectedCall* f);

}
