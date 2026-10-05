/*
** $Id: ldblib.c $
** Interface from Lua to its debug API
** See Copyright Notice in lua.h
*/



#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

/*
** If L1 != L, L1 can be in any state, and therefore there are no
** guarantees about its stack space; any push in L1 must be
** checked.
*/
static void checkstack(lua_State* L, lua_State* L1, int n)
{
  if (l_unlikely(L != L1 && !lua_checkstack(L1, n)))
    luaL_errori(L, lt::stack_overflow);
}

static int db_getregistry(lua_State* L)
{
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  return 1;
}

static int db_getmetatable(lua_State* L)
{
  luaL_checkany(L, 1);
  if (!lua_getmetatable(L, 1))
  {
    lua_pushnil(L); /* no metatable */
  }
  return 1;
}

static int db_setmetatable(lua_State* L)
{
  int t = lua_type(L, 2);
  luaL_argexpectedi(L, t == LUA_TNIL || t == LUA_TTABLE, 2, lt::nil_or_table);
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1; /* return 1st argument */
}

static int db_getuservalue(lua_State* L)
{
  int n = (int) luaL_optinteger(L, 2, 1);
  if (lua_type(L, 1) != LUA_TUSERDATA)
    luaL_pushfail(L);
  else if (lua_getiuservalue(L, 1, n) != LUA_TNONE)
  {
    lua_pushboolean(L, 1);
    return 2;
  }
  return 1;
}

static int db_setuservalue(lua_State* L)
{
  int n = (int) luaL_optinteger(L, 3, 1);
  luaL_checktype(L, 1, LUA_TUSERDATA);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  if (!lua_setiuservalue(L, 1, n))
    luaL_pushfail(L);
  return 1;
}

/*
** Auxiliary function used by several library functions: check for
** an optional thread as function's first argument and set 'arg' with
** 1 if this argument is present (so that functions can skip it to
** access their other arguments)
*/
struct ThreadArg
{
  lua_State* L1;
  int arg;
};

static ThreadArg getthread(lua_State* L)
{
  if (lua_isthread(L, 1))
  {
    return {lua_tothread(L, 1), 1};
  }
  else
  {
    return {L, 0}; /* function will operate over current thread */
  }
}

/*
** Variations of 'lua_settable', used by 'db_getinfo' to put results
** from 'lua_getinfo' into result table. Key is always a string;
** value can be a string, an int, or a boolean.
*/
static void settabss(lua_State* L, byte_view k, byte_view v)
{
  lua_pushlstring(L, v, 0, v.size());
  lua_setfieldi(L, -2, k);
}

static void settabsi(lua_State* L, byte_view k, int v)
{
  lua_pushinteger(L, v);
  lua_setfieldi(L, -2, k);
}

static void settabsb(lua_State* L, byte_view k, int v)
{
  lua_pushboolean(L, v);
  lua_setfieldi(L, -2, k);
}

/*
** In function 'db_getinfo', the call to 'lua_getinfo' may push
** results on the stack; later it creates the result table to put
** these objects. Function 'treatstackoption' puts the result from
** 'lua_getinfo' on top of the result table so that it can call
** 'lua_setfield'.
*/
static void treatstackoption(lua_State* L, lua_State* L1, byte_view fname)
{
  if (L == L1)
    lua_rotate(L, -2, 1); /* exchange object and table */
  else
    lua_xmove(L1, L, 1);      /* move object to the "main" stack */
  lua_setfieldi(L, -2, fname); /* put object into table */
}

/*
** Calls 'lua_getinfo' and collects all results in a new table.
** L1 needs stack space for an optional input (function) plus
** two optional outputs (function and line table) from function
** 'lua_getinfo'.
*/
static int db_getinfo(lua_State* L)
{
  InternalDebug ar;
  ThreadArg ta = getthread(L);
  lua_State* L1 = ta.L1;
  int arg = ta.arg;
  byte_view options = luaL_optlstring(L, arg + 2, lt::flnSrtu);
  InternalString funcOption = InternalString(lt::gt_op).concat(options);
  checkstack(L, L1, 3);
  luaL_argchecki(L, sv_byteat(options, 0) != '>', arg + 2, lt::invalid_option_2);
  if (lua_isfunction(L, arg + 1))
  {                                               /* info about a function? */
    options = funcOption.view();
    lua_pushvalue(L, arg + 1);                    /* move function to 'L1' stack */
    lua_xmove(L, L1, 1);
  }
  else
  { /* stack level */
    std::optional<InternalDebug> oar = lua_getstacki(L1, (int) luaL_checkinteger(L, arg + 1));
    if (!oar)
    {
      luaL_pushfail(L); /* level out of range */
      return 1;
    }
    ar = std::move(*oar);
  }
  std::optional<InternalDebug> res = lua_getinfoi(L1, options, ar);
  if (!res)
    return luaL_argerrori(L, arg + 2, lt::invalid_option_1);
  ar = std::move(*res);
  lua_newtable(L); /* table to collect results */
  if (options.find('S') != byte_view::npos)
  {
    lua_pushlstring(L, ar.source, 0, ar.source.size());
    lua_setfieldi(L, -2, lt::source);
    settabss(L, lt::short_src, ar.short_src_out.str_view());
    settabsi(L, lt::linedefined, ar.linedefined);
    settabsi(L, lt::lastlinedefined, ar.lastlinedefined);
    settabss(L, lt::what, ar.what);
  }
  if (options.find('l') != byte_view::npos)
    settabsi(L, lt::currentline, ar.currentline);
  if (options.find('u') != byte_view::npos)
  {
    settabsi(L, lt::nups, ar.nups);
    settabsi(L, lt::nparams, ar.nparams);
    settabsb(L, lt::isvararg, ar.isvararg);
  }
  if (options.find('n') != byte_view::npos)
  {
    if (ar.name.has_value())
      settabss(L, lt::name_2, ar.name.value());
    settabss(L, lt::namewhat, ar.namewhat.value_or(lt::empty_str));
  }
  if (options.find('r') != byte_view::npos)
  {
    settabsi(L, lt::ftransfer, ar.ftransfer);
    settabsi(L, lt::ntransfer, ar.ntransfer);
  }
  if (options.find('t') != byte_view::npos)
  {
    settabsb(L, lt::istailcall, ar.istailcall);
    settabsi(L, lt::extraargs, ar.extraargs);
  }
  if (options.find('L') != byte_view::npos)
    treatstackoption(L, L1, lt::activelines);
  if (options.find('f') != byte_view::npos)
    treatstackoption(L, L1, lt::func);
  return 1; /* return table */
}

static int db_getlocal(lua_State* L)
{
  ThreadArg ta = getthread(L);
  lua_State* L1 = ta.L1;
  int arg = ta.arg;
  int nvar = (int) luaL_checkinteger(L, arg + 2); /* local-variable index */
  if (lua_isfunction(L, arg + 1))
  {                                                 /* function argument? */
    lua_pushvalue(L, arg + 1); /* push function */
    const InternalString* name = lua_getlocali(L, nullptr, nvar);
    if (name)
      lua_pushistringref(L, name->view());
    else
      lua_pushnil(L);
    return 1; /* return only name (there is no value) */
  }
  else
  { /* stack-level argument */
    int level = (int) luaL_checkinteger(L, arg + 1);
    std::optional<InternalDebug> ar = lua_getstacki(L1, level);
    if (l_unlikely(!ar)) /* out of range? */
      return luaL_argerrori(L, arg + 1, lt::level_out_of_range);
    checkstack(L, L1, 1);
    const InternalString* name = lua_getlocali(L1, &*ar, nvar);
    if (name)
    {
      lua_xmove(L1, L, 1); /* move local value */
      lua_pushistringref(L, name->view()); /* push name */
      lua_rotate(L, -2, 1);    /* re-order */
      return 2;
    }
    else
    {
      luaL_pushfail(L); /* no name (nor value) */
      return 1;
    }
  }
}

static int db_setlocal(lua_State* L)
{
  ThreadArg ta = getthread(L);
  lua_State* L1 = ta.L1;
  int arg = ta.arg;
  int level = (int) luaL_checkinteger(L, arg + 1);
  int nvar = (int) luaL_checkinteger(L, arg + 2);
  std::optional<InternalDebug> ar = lua_getstacki(L1, level);
  if (l_unlikely(!ar)) /* out of range? */
    return luaL_argerrori(L, arg + 1, lt::level_out_of_range);
  luaL_checkany(L, arg + 3);
  lua_settop(L, arg + 3);
  checkstack(L, L1, 1);
  lua_xmove(L, L1, 1);
  const InternalString* name = lua_setlocali(L1, &*ar, nvar);
  if (!name)
  {
    lua_pop(L1, 1); /* pop value (if not popped by 'lua_setlocal') */
    lua_pushnil(L);
  }
  else
    lua_pushistringref(L, name->view());
  return 1;
}

/*
** get (if 'get' is true) or set an upvalue from a closure
*/
static int auxupvalue(lua_State* L, int get)
{
  int n = (int) luaL_checkinteger(L, 2); /* upvalue index */
  luaL_checktype(L, 1, LUA_TFUNCTION);   /* closure */
  const InternalString* name = get ? lua_getupvaluei(L, 1, n) : lua_setupvaluei(L, 1, n);
  if (!name)
    return 0;
  lua_pushistringref(L, name->view());
  lua_insert(L, -(get + 1)); /* no-op if get is false */
  return get + 1;
}

static int db_getupvalue(lua_State* L)
{
  return auxupvalue(L, 1);
}

static int db_setupvalue(lua_State* L)
{
  luaL_checkany(L, 3);
  return auxupvalue(L, 0);
}

/*
** Check whether a given upvalue from a given closure exists and
** returns its index
*/
struct UpvalRef
{
  void* id;
  int nup;
};

static UpvalRef checkupval(lua_State* L, int argf, int argnup)
{
  UpvalRef r;
  r.nup = (int) luaL_checkinteger(L, argnup); /* upvalue index */
  luaL_checktype(L, argf, LUA_TFUNCTION);     /* closure */
  r.id = lua_upvalueid(L, argf, r.nup);
  return r;
}

static UpvalRef checkupvalvalid(lua_State* L, int argf, int argnup)
{
  UpvalRef r = checkupval(L, argf, argnup);
  luaL_argchecki(L, r.id != nullptr, argnup, lt::invalid_upvalue_index);
  return r;
}

static int db_upvalueid(lua_State* L)
{
  void* id = checkupval(L, 1, 2).id;
  if (id != nullptr)
    lua_pushlightuserdata(L, id);
  else
    luaL_pushfail(L);
  return 1;
}

static int db_upvaluejoin(lua_State* L)
{
  int n1 = checkupvalvalid(L, 1, 2).nup;
  int n2 = checkupvalvalid(L, 3, 4).nup;
  luaL_argchecki(L, !lua_iscfunction(L, 1), 1, lt::Lua_function_expected);
  luaL_argchecki(L, !lua_iscfunction(L, 3), 3, lt::Lua_function_expected);
  lua_upvaluejoin(L, 1, n1, 3, n2);
  return 0;
}

/*
** Call hook function registered at hook table for the current
** thread (if there is one)
*/
static void hookf(lua_State* L, lua_Debug* ar)
{
  static const byte_view hooknames[] = {lt::call, lt::return_kw, lt::line, lt::count,lt::tail_call};
  lua_getfieldi(L, LUA_REGISTRYINDEX, lt::_HOOKKEY);
  lua_pushthread(L);
  if (lua_rawget(L, -2) == LUA_TFUNCTION)
  {                                                /* is there a hook function? */
    lua_pushistringref(L, hooknames[ar->event]); /* push event name */
    if (ar->currentline >= 0)
      lua_pushinteger(L, ar->currentline); /* push current line */
    else
      lua_pushnil(L);
    InternalDebug iar{};
    iar.i_ci = ar->i_ci;
    assert(lua_getinfoi(L, lt::lS_chars, iar).has_value());
    lua_call(L, 2, 0); /* call hook function */
  }
}

/*
** Convert a string mask (for 'sethook') into a bit mask
*/
static int makemask(byte_view smask, int count)
{
  int mask = 0;
  if (smask.find('c') != byte_view::npos)
    mask |= LUA_MASKCALL;
  if (smask.find('r') != byte_view::npos)
    mask |= LUA_MASKRET;
  if (smask.find('l') != byte_view::npos)
    mask |= LUA_MASKLINE;
  if (count > 0)
    mask |= LUA_MASKCOUNT;
  return mask;
}

static size_t unmakemask(int mask, byte_span smask)
{
  size_t i = 0;
  if (mask & LUA_MASKCALL)
    smask[i++] = 'c';
  if (mask & LUA_MASKRET)
    smask[i++] = 'r';
  if (mask & LUA_MASKLINE)
    smask[i++] = 'l';
  return i;
}

static int db_sethook(lua_State* L)
{
  int mask, count;
  lua_Hook func;
  ThreadArg ta = getthread(L);
  lua_State* L1 = ta.L1;
  int arg = ta.arg;
  if (lua_isnoneornil(L, arg + 1))
  { /* no hook? */
    lua_settop(L, arg + 1);
    func = nullptr;
    mask = 0;
    count = 0; /* turn off hooks */
  }
  else
  {
    byte_view smask = luaL_checklstring(L, arg + 2);
    luaL_checktype(L, arg + 1, LUA_TFUNCTION);
    count = (int) luaL_optinteger(L, arg + 3, 0);
    func = hookf;
    mask = makemask(smask, count);
  }
  if (!luaL_getsubtablei(L, LUA_REGISTRYINDEX, lt::_HOOKKEY))
  {
    /* table just created; initialize it */
    lua_pushistringref(L, lt::k_char);
    lua_setfieldi(L, -2, lt::mm_mode); /** hooktable.__mode = "k" */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2); /* metatable(hooktable) = hooktable */
  }
  checkstack(L, L1, 1);
  lua_pushthread(L1);
  lua_xmove(L1, L, 1);       /* key (thread) */
  lua_pushvalue(L, arg + 1); /* value (hook function) */
  lua_rawset(L, -3);         /* hooktable[L1] = new Lua hook */
  lua_sethook(L1, func, mask, count);
  return 0;
}

static int db_gethook(lua_State* L)
{
  lua_State* L1 = getthread(L).L1;
  byte_inline<5> buff;
  int mask = lua_gethookmask(L1);
  lua_Hook hook = lua_gethook(L1);
  if (hook == nullptr)
  { /* no hook? */
    luaL_pushfail(L);
    return 1;
  }
  else if (hook != hookf) /* external hook? */
    lua_pushistringref(L, lt::external_hook);
  else
  { /* hook table must exist */
    lua_getfieldi(L, LUA_REGISTRYINDEX, lt::_HOOKKEY);
    checkstack(L, L1, 1);
    lua_pushthread(L1);
    lua_xmove(L1, L, 1);
    lua_rawget(L, -2); /* 1st result = hooktable[L1] */
    lua_remove(L, -2); /* remove hook table */
  }
  size_t masklen = unmakemask(mask, buff);
  byte_view maskview = buff.view().substr(0, masklen);
  lua_pushlstring(L, maskview, 0, maskview.size()); /* 2nd result = mask */
  lua_pushinteger(L, lua_gethookcount(L1));  /* 3rd result = count */
  return 3;
}

static int db_debug(lua_State* L)
{
  for (;;)
  {
    byte_owner buffer(LUA_MAXINPUT);
    os_print_stderr(lt::lua_debug);
    os_flush_stderr();
    if (!os_readline(os_stdin(), buffer) || buffer.str_view() == lt::cont_n)
    {
      return 0;
    }
    byte_view cmd = buffer.str_view();
    if (luaL_loadbufferi(L, cmd, lt::debug_command, lt::t_char) || lua_pcall(L, 0, 0, 0))
    {
      os_print_stderr(luaL_toistring(L, -1)->view());
      os_print_stderr(lt::n_3);
      os_flush_stderr();
    }
    lua_settop(L, 0); /* remove eventual returns */
  }
}

static int db_traceback(lua_State* L)
{
  ThreadArg ta = getthread(L);
  lua_State* L1 = ta.L1;
  int arg = ta.arg;
  if (!lua_isstring(L, arg + 1) && !lua_isnoneornil(L, arg + 1)) /* non-string 'msg'? */
    lua_pushvalue(L, arg + 1); /* return it untouched */
  else
  {
    int level = (int) luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
    if (lua_isnoneornil(L, arg + 1))
      luaL_traceback(L, L1, std::nullopt, level);
    else
    {
      luaL_traceback(L, L1, lua_tostring(L, arg + 1), level);
    }
  }
  return 1;
}

static CFuncInfo cfi_db_debug(db_debug);
static CFuncInfo cfi_db_getuservalue(db_getuservalue);
static CFuncInfo cfi_db_gethook(db_gethook);
static CFuncInfo cfi_db_getinfo(db_getinfo);
static CFuncInfo cfi_db_getlocal(db_getlocal);
static CFuncInfo cfi_db_getregistry(db_getregistry);
static CFuncInfo cfi_db_getmetatable(db_getmetatable);
static CFuncInfo cfi_db_getupvalue(db_getupvalue);
static CFuncInfo cfi_db_upvaluejoin(db_upvaluejoin);
static CFuncInfo cfi_db_upvalueid(db_upvalueid);
static CFuncInfo cfi_db_setuservalue(db_setuservalue);
static CFuncInfo cfi_db_sethook(db_sethook);
static CFuncInfo cfi_db_setlocal(db_setlocal);
static CFuncInfo cfi_db_setmetatable(db_setmetatable);
static CFuncInfo cfi_db_setupvalue(db_setupvalue);
static CFuncInfo cfi_db_traceback(db_traceback);

static const luaL_RegCFI dblib[] = {
  {lt::debug, &cfi_db_debug},
  {lt::getuservalue, &cfi_db_getuservalue},
  {lt::gethook, &cfi_db_gethook},
  {lt::getinfo, &cfi_db_getinfo},
  {lt::getlocal, &cfi_db_getlocal},
  {lt::getregistry, &cfi_db_getregistry},
  {lt::getmetatable, &cfi_db_getmetatable},
  {lt::getupvalue, &cfi_db_getupvalue},
  {lt::upvaluejoin, &cfi_db_upvaluejoin},
  {lt::upvalueid, &cfi_db_upvalueid},
  {lt::setuservalue, &cfi_db_setuservalue},
  {lt::sethook, &cfi_db_sethook},
  {lt::setlocal, &cfi_db_setlocal},
  {lt::setmetatable, &cfi_db_setmetatable},
  {lt::setupvalue, &cfi_db_setupvalue},
  {lt::traceback, &cfi_db_traceback},
};

}

LUAMOD_API int luaopen_debug(lua_State* L)
{
  luaL_newlibcfi(L, dblib);
  return 1;
}

lua_Hook luaL_debughook(void)
{
  return lua::lib::hookf;
}
