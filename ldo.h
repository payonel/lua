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

[[noreturn]] void luaD_errerr(lua_State* L);
void luaD_seterrorobj(lua_State* L, TStatus errcode, StkId oldtop);
TStatus luaD_protectedparser(lua_State* L, ZIO* z, std::optional<byte_view> name, std::optional<byte_view> mode,
                             ::lua::bytes::LoadResume* rz = nullptr);
void luaD_hook(lua_State* L, int event, int line, int fTransfer, int nTransfer);
void luaD_hookcall(lua_State* L, CallInfo* ci);
int luaD_pretailcall(lua_State* L, CallInfo* ci, StkId func, int narg1, int delta);
CallInfo* luaD_precall(lua_State* L, StkId func, int nResults);
void luaD_call(lua_State* L, StkId func, int nResults);
void luaD_callnoyield(lua_State* L, StkId func, int nResults);
TStatus luaD_closeprotected(lua_State* L, StkId level, TStatus status, int yy = 0);
TStatus luaD_pcall(lua_State* L, ProtectedCall* func, StkId oldtop, StkId ef, bool close = true);
TStatus luaD_hostclose(lua_State* L, StkId level, TStatus status);
void luaD_poscall(lua_State* L, CallInfo* ci, int nres);
int luaD_reallocstack(lua_State* L, int newsize, int raiseerror);
int luaD_growstack(lua_State* L, int n, int raiseerror);

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
void luaD_shrinkstack(lua_State* L);
void luaD_inctop(lua_State* L);
void luaD_anchorobj(lua_State* L, Table* anchor, GCObject* obj);

/* status of a freeze unwinding the C stack: not an error, and never a
   result of the API */
constexpr TStatus LUA_FREEZE = 100;

/* status of the exit of the budget callback (freeze): it unwinds the C
   stack to the callback's caller, and only that catches it */
constexpr TStatus LUA_CBEXIT = 101;

lua_State* luaD_budget(lua_State* L);
[[noreturn]] void luaD_exitcallback(lua_State* L);
bool luaD_killed(lua_State* L);
bool luaD_canfreeze(lua_State* L, int* why = nullptr);
void luaD_callerrh(lua_State* L);
void luaD_finishhook(lua_State* L, CallInfo* ci);
[[noreturn]] void luaD_freeze(lua_State* L, lua_State* co);
int luaD_checkminstack(lua_State* L);

[[noreturn]] void luaD_throw(lua_State* L, TStatus errcode);
[[noreturn]] void luaD_throwbaselevel(lua_State* L, TStatus errcode);
TStatus luaD_rawrunprotected(lua_State* L, ProtectedCall* f, bool exitpoint = false);

}
