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
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

/*
** The hook table at registry[HOOKKEY] maps threads to their current
** hook function.
*/
static const InternalString* const HOOKKEY = lit<"_HOOKKEY">();

/*
** If L1 != L, L1 can be in any state, and therefore there are no
** guarantees about its stack space; any push in L1 must be
** checked.
*/
static void checkstack(lua_State* L, lua_State* L1, int n)
{
  if (l_unlikely(L != L1 && !lua_checkstack(L1, n)))
    luaL_errori(L, lit<"stack overflow">()->view());
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
  luaL_argexpectedi(L, t == LUA_TNIL || t == LUA_TTABLE, 2, lit<"nil or table">()->view());
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
static lua_State* getthread(lua_State* L, int* arg)
{
  if (lua_isthread(L, 1))
  {
    *arg = 1;
    return lua_tothread(L, 1);
  }
  else
  {
    *arg = 0;
    return L; /* function will operate over current thread */
  }
}

/*
** Variations of 'lua_settable', used by 'db_getinfo' to put results
** from 'lua_getinfo' into result table. Key is always a string;
** value can be a string, an int, or a boolean.
*/
static void settabss(lua_State* L, const InternalString* k, std::string_view v)
{
  lua_pushlstring(L, v, 0, v.size());
  lua_setfieldi(L, -2, k->view());
}

static void settabsi(lua_State* L, const InternalString* k, int v)
{
  lua_pushinteger(L, v);
  lua_setfieldi(L, -2, k->view());
}

static void settabsb(lua_State* L, const InternalString* k, int v)
{
  lua_pushboolean(L, v);
  lua_setfieldi(L, -2, k->view());
}

/*
** In function 'db_getinfo', the call to 'lua_getinfo' may push
** results on the stack; later it creates the result table to put
** these objects. Function 'treatstackoption' puts the result from
** 'lua_getinfo' on top of the result table so that it can call
** 'lua_setfield'.
*/
static void treatstackoption(lua_State* L, lua_State* L1, const InternalString* fname)
{
  if (L == L1)
    lua_rotate(L, -2, 1); /* exchange object and table */
  else
    lua_xmove(L1, L, 1);      /* move object to the "main" stack */
  lua_setfieldi(L, -2, fname->view()); /* put object into table */
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
  int arg;
  lua_State* L1 = getthread(L, &arg);
  std::string_view options = luaL_optlstring(L, arg + 2, lit<"flnSrtu">()->view());
  checkstack(L, L1, 3);
  luaL_argchecki(L, sv_byteat(options, 0) != '>', arg + 2, lit<"invalid option '>'">()->view());
  std::string optbuf;
  if (lua_isfunction(L, arg + 1))
  {                                               /* info about a function? */
    optbuf.reserve(options.size() + 1);           /* add '>' to 'options' */
    optbuf.push_back('>');
    optbuf.append(options);
    options = optbuf;
    lua_pushvalue(L, arg + 1);                    /* move function to 'L1' stack */
    lua_xmove(L, L1, 1);
  }
  else
  { /* stack level */
    std::optional<InternalDebug> oar =
        lua_getstacki(L1, (int) luaL_checkinteger(L, arg + 1));
    if (!oar)
    {
      luaL_pushfail(L); /* level out of range */
      return 1;
    }
    ar = *oar;
  }
  std::optional<InternalDebug> res = lua_getinfoi(L1, options, ar);
  if (!res)
    return luaL_argerrori(L, arg + 2, lit<"invalid option">()->view());
  ar = *res;
  lua_newtable(L); /* table to collect results */
  if ((options.find('S') != std::string_view::npos))
  {
    lua_pushlstring(L, ar.source, 0, ar.source.size());
    lua_setfieldi(L, -2, lit<"source">()->view());
    settabss(L, lit<"short_src">(), std::string_view{ar.short_src});
    settabsi(L, lit<"linedefined">(), ar.linedefined);
    settabsi(L, lit<"lastlinedefined">(), ar.lastlinedefined);
    settabss(L, lit<"what">(), ar.what);
  }
  if ((options.find('l') != std::string_view::npos))
    settabsi(L, lit<"currentline">(), ar.currentline);
  if ((options.find('u') != std::string_view::npos))
  {
    settabsi(L, lit<"nups">(), ar.nups);
    settabsi(L, lit<"nparams">(), ar.nparams);
    settabsb(L, lit<"isvararg">(), ar.isvararg);
  }
  if ((options.find('n') != std::string_view::npos))
  {
    if (ar.name.data() != nullptr)
      settabss(L, lit<"name">(), ar.name);
    settabss(L, lit<"namewhat">(), ar.namewhat);
  }
  if ((options.find('r') != std::string_view::npos))
  {
    settabsi(L, lit<"ftransfer">(), ar.ftransfer);
    settabsi(L, lit<"ntransfer">(), ar.ntransfer);
  }
  if ((options.find('t') != std::string_view::npos))
  {
    settabsb(L, lit<"istailcall">(), ar.istailcall);
    settabsi(L, lit<"extraargs">(), ar.extraargs);
  }
  if ((options.find('L') != std::string_view::npos))
    treatstackoption(L, L1, lit<"activelines">());
  if ((options.find('f') != std::string_view::npos))
    treatstackoption(L, L1, lit<"func">());
  return 1; /* return table */
}

static int db_getlocal(lua_State* L)
{
  int arg;
  lua_State* L1 = getthread(L, &arg);
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
      return luaL_argerrori(L, arg + 1, lit<"level out of range">()->view());
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
  int arg;
  lua_State* L1 = getthread(L, &arg);
  int level = (int) luaL_checkinteger(L, arg + 1);
  int nvar = (int) luaL_checkinteger(L, arg + 2);
  std::optional<InternalDebug> ar = lua_getstacki(L1, level);
  if (l_unlikely(!ar)) /* out of range? */
    return luaL_argerrori(L, arg + 1, lit<"level out of range">()->view());
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
  const InternalString* name =
      get ? lua_getupvaluei(L, 1, n) : lua_setupvaluei(L, 1, n);
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
static void* checkupval(lua_State* L, int argf, int argnup, int* pnup)
{
  void* id;
  int nup = (int) luaL_checkinteger(L, argnup); /* upvalue index */
  luaL_checktype(L, argf, LUA_TFUNCTION);       /* closure */
  id = lua_upvalueid(L, argf, nup);
  if (pnup)
  {
    luaL_argchecki(L, id != nullptr, argnup, lit<"invalid upvalue index">()->view());
    *pnup = nup;
  }
  return id;
}

static int db_upvalueid(lua_State* L)
{
  void* id = checkupval(L, 1, 2, nullptr);
  if (id != nullptr)
    lua_pushlightuserdata(L, id);
  else
    luaL_pushfail(L);
  return 1;
}

static int db_upvaluejoin(lua_State* L)
{
  int n1, n2;
  checkupval(L, 1, 2, &n1);
  checkupval(L, 3, 4, &n2);
  luaL_argchecki(L, !lua_iscfunction(L, 1), 1, lit<"Lua function expected">()->view());
  luaL_argchecki(L, !lua_iscfunction(L, 3), 3, lit<"Lua function expected">()->view());
  lua_upvaluejoin(L, 1, n1, 3, n2);
  return 0;
}

/*
** Call hook function registered at hook table for the current
** thread (if there is one)
*/
static void hookf(lua_State* L, lua_Debug* ar)
{
  static const InternalString* const hooknames[] = {
      lit<"call">(), lit<"return">(), lit<"line">(), lit<"count">(),
      lit<"tail call">()};
  lua_getfieldi(L, LUA_REGISTRYINDEX, HOOKKEY->view());
  lua_pushthread(L);
  if (lua_rawget(L, -2) == LUA_TFUNCTION)
  {                                                /* is there a hook function? */
    lua_pushistringref(L, hooknames[(int) ar->event]->view()); /* push event name */
    if (ar->currentline >= 0)
      lua_pushinteger(L, ar->currentline); /* push current line */
    else
      lua_pushnil(L);
    InternalDebug iar{};
    iar.i_ci = ar->i_ci;
    assert(lua_getinfoi(L, lit<"lS">()->view(), iar).has_value());
    lua_call(L, 2, 0); /* call hook function */
  }
}

/*
** Convert a string mask (for 'sethook') into a bit mask
*/
static int makemask(std::string_view smask, int count)
{
  int mask = 0;
  if (smask.find('c') != std::string_view::npos)
    mask |= LUA_MASKCALL;
  if (smask.find('r') != std::string_view::npos)
    mask |= LUA_MASKRET;
  if (smask.find('l') != std::string_view::npos)
    mask |= LUA_MASKLINE;
  if (count > 0)
    mask |= LUA_MASKCOUNT;
  return mask;
}

static size_t unmakemask(int mask, std::span<char> smask)
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
  int arg, mask, count;
  lua_Hook func;
  lua_State* L1 = getthread(L, &arg);
  if (lua_isnoneornil(L, arg + 1))
  { /* no hook? */
    lua_settop(L, arg + 1);
    func = nullptr;
    mask = 0;
    count = 0; /* turn off hooks */
  }
  else
  {
    std::string_view smask = luaL_checklstring(L, arg + 2);
    luaL_checktype(L, arg + 1, LUA_TFUNCTION);
    count = (int) luaL_optinteger(L, arg + 3, 0);
    func = hookf;
    mask = makemask(smask, count);
  }
  if (!luaL_getsubtablei(L, LUA_REGISTRYINDEX, HOOKKEY->view()))
  {
    /* table just created; initialize it */
    lua_pushistringref(L, lit<"k">()->view());
    lua_setfieldi(L, -2, lit<"__mode">()->view()); /** hooktable.__mode = "k" */
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
  int arg;
  lua_State* L1 = getthread(L, &arg);
  char buff[5];
  int mask = lua_gethookmask(L1);
  lua_Hook hook = lua_gethook(L1);
  if (hook == nullptr)
  { /* no hook? */
    luaL_pushfail(L);
    return 1;
  }
  else if (hook != hookf) /* external hook? */
    lua_pushistringref(L, lit<"external hook">()->view());
  else
  { /* hook table must exist */
    lua_getfieldi(L, LUA_REGISTRYINDEX, HOOKKEY->view());
    checkstack(L, L1, 1);
    lua_pushthread(L1);
    lua_xmove(L1, L, 1);
    lua_rawget(L, -2); /* 1st result = hooktable[L1] */
    lua_remove(L, -2); /* remove hook table */
  }
  size_t masklen = unmakemask(mask, buff);
  lua_pushlstring(L, std::string_view{buff, masklen}, 0, masklen); /* 2nd result = mask */
  lua_pushinteger(L, lua_gethookcount(L1));  /* 3rd result = count */
  return 3;
}

static int db_debug(lua_State* L)
{
  for (;;)
  {
    char buffer[250];
    os_print_stderr(lit<"lua_debug> ">()->view());
    os_flush_stderr();
    if (!os_readline(os_stdin(), std::span<char>{buffer, sizeof(buffer)}) || std::string_view(buffer) == lit<"cont\n">()->view())
    {
      return 0;
    }
    if (luaL_loadbufferi(L, std::string_view(buffer), lit<"=(debug command)">()->view(), lit<"t">()->view()) || lua_pcall(L, 0, 0, 0))
    {
      os_print_stderr(luaL_toistring(L, -1)->view());
      os_print_stderr(lit<"\n">()->view());
      os_flush_stderr();
    }
    lua_settop(L, 0); /* remove eventual returns */
  }
}

static int db_traceback(lua_State* L)
{
  int arg;
  lua_State* L1 = getthread(L, &arg);
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

static const luaL_Reg dblib[] = {
    {lit<"debug">()->view(), db_debug},
    {lit<"getuservalue">()->view(), db_getuservalue},
    {lit<"gethook">()->view(), db_gethook},
    {lit<"getinfo">()->view(), db_getinfo},
    {lit<"getlocal">()->view(), db_getlocal},
    {lit<"getregistry">()->view(), db_getregistry},
    {lit<"getmetatable">()->view(), db_getmetatable},
    {lit<"getupvalue">()->view(), db_getupvalue},
    {lit<"upvaluejoin">()->view(), db_upvaluejoin},
    {lit<"upvalueid">()->view(), db_upvalueid},
    {lit<"setuservalue">()->view(), db_setuservalue},
    {lit<"sethook">()->view(), db_sethook},
    {lit<"setlocal">()->view(), db_setlocal},
    {lit<"setmetatable">()->view(), db_setmetatable},
    {lit<"setupvalue">()->view(), db_setupvalue},
    {lit<"traceback">()->view(), db_traceback},
    {{}, nullptr}};

LUAMOD_API int luaopen_debug(lua_State* L)
{
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(dblib) / sizeof((dblib)[0]) - 1), luaL_setfuncs(L, dblib, 0));
  return 1;
}
