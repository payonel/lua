/*
** $Id: lcorolib.c $
** Coroutine Library
** See Copyright Notice in lua.h
*/



#include <stdlib.h>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

static lua_State* getco(lua_State* L)
{
  lua_State* co = lua_tothread(L, 1);
  luaL_argexpectedi(L, co, 1, lt::thread);
  return co;
}

/*
** The results of a resume of 'co' that ended with 'rr': the number of
** results for non-error cases or -1 for errors.
*/
static int auxresumed(lua_State* L, lua_State* co, lua_ResumeResult rr)
{
  if (l_likely(rr.status == LUA_OK || rr.status == LUA_YIELD))
  {
    int nres = rr.nres;
    if (l_unlikely(!lua_checkstack(L, nres + 1)))
    {
      lua_pop(co, nres); /* remove results anyway */
      lua_pushistringref(L, lt::too_many_results_to_resume);
      return -1; /* error flag */
    }
    lua_xmove(co, L, nres); /* move yielded values */
    return nres;
  }
  else
  {
    lua_xmove(co, L, 1); /* move error message */
    return -1;           /* error flag */
  }
}

/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors. A freeze of a coroutine below passes through
** the resume; when it thaws, 'k' goes on with the result.
*/
static int auxresume(lua_State* L, lua_State* co, int narg, lua_KFunction k)
{
  if (l_unlikely(!lua_checkstack(co, narg)))
  {
    lua_pushistringref(L, lt::too_many_arguments_to_resume);
    return -1; /* error flag */
  }
  lua_xmove(L, co, narg);
  return auxresumed(L, co, lua_resumek(co, L, narg, 0, k));
}

static int coresume_end(lua_State* L, int r)
{
  if (l_unlikely(r < 0))
  {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2; /* return false + error message */
  }
  else
  {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1; /* return true + 'resume' returns */
  }
}

static int coresume_cont(lua_State* L, int status, lua_KContext)
{
  lua_State* co = lua_tothread(L, 1);
  return coresume_end(L, auxresumed(L, co, {status, lua_resumenres(co, status)}));
}

LUA_KNAME("coroutine", coresume_cont);

static int luaB_coresume(lua_State* L)
{
  lua_State* co = getco(L);
  return coresume_end(L, auxresume(L, co, lua_gettop(L) - 1, coresume_cont));
}

/* the error of a wrapped coroutine, in the caller's stack: propagate it */
static int auxwrap_fail(lua_State* L, int stat)
{
  if (stat != LUA_ERRMEM && /* not a memory error and ... */
      lua_type(L, -1) == LUA_TSTRING)
  {                   /* ... error object is a string? */
    luaL_where(L, 1); /* add extra info, if available */
    lua_insert(L, -2);
    lua_concat(L, 2);
  }
  return lua_error(L); /* propagate error */
}

/* the variables of the coroutine are closed, after a thaw (a '__close' froze) */
static int auxwrap_closed(lua_State* L, int status, lua_KContext)
{
  lua_State* co = lua_tothread(L, lua_upvalueindex(1));
  assert(status != LUA_OK);
  lua_xmove(co, L, 1); /* move error message to the caller */
  return auxwrap_fail(L, status);
}

LUA_KNAME("coroutine", auxwrap_closed);

static int auxwrap_end(lua_State* L, lua_State* co, int r)
{
  if (l_unlikely(r < 0))
  { /* error? */
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD)
    { /* error in the coroutine? close its tbc variables (a '__close' can freeze) */
      stat = lua_closethreadk(co, L, 0, auxwrap_closed).status;
      assert(stat != LUA_OK);
      lua_xmove(co, L, 1); /* move error message to the caller */
    }
    return auxwrap_fail(L, stat);
  }
  return r;
}

static int auxwrap_cont(lua_State* L, int status, lua_KContext)
{
  lua_State* co = lua_tothread(L, lua_upvalueindex(1));
  return auxwrap_end(L, co, auxresumed(L, co, {status, lua_resumenres(co, status)}));
}

LUA_KNAME("coroutine", auxwrap_cont);

static int luaB_auxwrap(lua_State* L)
{
  lua_State* co = lua_tothread(L, lua_upvalueindex(1));
  return auxwrap_end(L, co, auxresume(L, co, lua_gettop(L), auxwrap_cont));
}

static CClosureNamer auxwrap_kind(lua_CClosureKind{"coroutine.auxwrap", luaB_auxwrap, 1, nullptr, nullptr});

static int luaB_cocreate(lua_State* L)
{
  lua_State* NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  NL = lua_newthread(L);
  lua_pushvalue(L, 1); /* move function to top */
  lua_xmove(L, NL, 1); /* move function from L to NL */
  return 1;
}

static int luaB_cowrap(lua_State* L)
{
  luaB_cocreate(L);
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}

static int luaB_yield(lua_State* L)
{
  return lua_yield(L, lua_gettop(L));
}

constexpr int COS_RUN = 0;
constexpr int COS_DEAD = 1;
constexpr int COS_YIELD = 2;
constexpr int COS_NORM = 3;

static const byte_view statname[] = {lt::running, lt::dead, lt::suspended, lt::normal_2};

static int auxstatus(lua_State* L, lua_State* co)
{
  if (L == co)
    return COS_RUN;
  else if (lua_isfrozen(co)) /* as when it froze */
    return (lua_frozenchild(co) != nullptr) ? COS_NORM : COS_RUN;
  else
  {
    switch (lua_status(co))
    {
    case LUA_YIELD:
      return COS_YIELD;
    case LUA_OK:
    {
      if (lua_getstack(co, 0)) /* does it have frames? */
        return COS_NORM;       /* it is running */
      else if (lua_gettop(co) == 0)
        return COS_DEAD;
      else
        return COS_YIELD; /* initial state */
    }
    default: /* some error occurred */
      return COS_DEAD;
    }
  }
}

static int luaB_costatus(lua_State* L)
{
  lua_State* co = getco(L);
  lua_pushistringref(L, statname[auxstatus(L, co)]);
  return 1;
}

static lua_State* getoptco(lua_State* L)
{
  return (lua_isnone(L, 1) ? L : getco(L));
}

static int luaB_yieldable(lua_State* L)
{
  lua_State* co = getoptco(L);
  lua_pushboolean(L, lua_isyieldable(co));
  return 1;
}

static int luaB_corunning(lua_State* L)
{
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}

static int close_end(lua_State* L, lua_State* co, int status)
{
  if (status == LUA_OK)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    lua_pushboolean(L, 0);
    lua_xmove(co, L, 1); /* move error message */
    return 2;
  }
}

/* after a thaw (a '__close' froze) */
static int close_cont(lua_State* L, int status, lua_KContext)
{
  return close_end(L, getoptco(L), status);
}

LUA_KNAME("coroutine", close_cont);

static int luaB_close(lua_State* L)
{
  lua_State* co = getoptco(L);
  int status = auxstatus(L, co);
  switch (status)
  {
  case COS_DEAD:
  case COS_YIELD:
    return close_end(L, co, lua_closethreadk(co, L, 0, close_cont).status);
  case COS_NORM:
    lua_pushistringref(L, statname[status]);
    return luaL_errori(L, lt::cannot_close_a_s_coroutine);
  case COS_RUN:
    if (co != L)
    { /* frozen (running where it stopped) */
      lua_pushistringref(L, statname[status]);
      return luaL_errori(L, lt::cannot_close_a_s_coroutine);
    }
    lua_geti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD); /* get main */
    if (lua_tothread(L, -1) == co)
      return luaL_errori(L, lt::cannot_close_main_thread);
    lua_closeself(L);                   /* close itself (a '__close' can freeze) */
    /* previous call does not return */ /* FALLTHROUGH */
  default:
    assert(0);
    return 0;
  }
}

static CFuncInfo cfi_luaB_cocreate(luaB_cocreate, "coroutine.luaB_cocreate");
static CFuncInfo cfi_luaB_coresume(luaB_coresume, "coroutine.luaB_coresume");
static CFuncInfo cfi_luaB_corunning(luaB_corunning, "coroutine.luaB_corunning");
static CFuncInfo cfi_luaB_costatus(luaB_costatus, "coroutine.luaB_costatus");
static CFuncInfo cfi_luaB_cowrap(luaB_cowrap, "coroutine.luaB_cowrap");
static CFuncInfo cfi_luaB_yield(luaB_yield, "coroutine.luaB_yield");
static CFuncInfo cfi_luaB_yieldable(luaB_yieldable, "coroutine.luaB_yieldable");
static CFuncInfo cfi_luaB_close(luaB_close, "coroutine.luaB_close");

static const luaL_RegCFI co_funcs[] = {
  {lt::create, &cfi_luaB_cocreate},
  {lt::resume, &cfi_luaB_coresume},
  {lt::running, &cfi_luaB_corunning},
  {lt::status, &cfi_luaB_costatus},
  {lt::wrap, &cfi_luaB_cowrap},
  {lt::yield, &cfi_luaB_yield},
  {lt::isyieldable, &cfi_luaB_yieldable},
  {lt::close_kw, &cfi_luaB_close},
};

}

LUAMOD_API int luaopen_coroutine(lua_State* L)
{
  luaL_newlibcfi(L, co_funcs);
  return 1;
}

