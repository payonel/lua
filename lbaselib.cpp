/*
** $Id: lbaselib.c $
** Basic library
** See Copyright Notice in lua.h
*/



#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"
#include "lstate.h"
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"
#include "locale_util.h"

namespace lua::lib
{

/*
** Prints argument 'i', whose string is on the top, and the arguments
** after it. (The continuation of '__tostring' calls: 'i' in 'ctx'.)
*/
static int printk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  int n = lua_gettop(L) - 1; /* number of arguments (below the string) */
  int i = (int)ctx;
  for (;;)
  {
    const InternalString* s = luaL_tostringresult(L);
    if (i > 1)                /* not the first element? */
      os_print_stdout(lt::t); /* add a tab before it */
    os_print_stdout(s->view());
    lua_pop(L, 1); /* pop result */
    if (++i > n)
      break;
    luaL_toistringk(L, i, i, printk); /* convert it to string */
  }
  os_print_stdout(lt::n_3);
  os_flush_stdout();
  return 0;
}

static int luaB_print(lua_State* L)
{
  if (lua_gettop(L) == 0)
  {
    os_print_stdout(lt::n_3);
    os_flush_stdout();
    return 0;
  }
  luaE_freezeonly(L);
  luaL_toistringk(L, 1, 1, printk); /* convert it to string */
  return printk(L, LUA_OK, 1);
}

/*
** Creates a warning with all given arguments.
** Check first for errors; otherwise an error may interrupt
** the composition of a warning, leaving it unfinished.
*/
static int luaB_warn(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int i;
  luaL_checkstring(L, 1); /* at least one argument */
  for (i = 2; i <= n; i++)
    luaL_checkstring(L, i); /* make sure all arguments are strings */
  for (i = 1; i < n; i++)   /* compose warning */
    lua_warning(L, *lua_tostring(L, i), 1);
  lua_warning(L, *lua_tostring(L, n), 0); /* close warning */
  return 0;
}

struct IntParse
{
  size_t end;
  lua_Integer value;
};

static IntParse b_str2int(byte_view s, int base)
{
  const byte_view spacechars = lt::whitespace_chars;
  lua_Unsigned n = 0;
  int neg = 0;
  size_t p = 0;
  while (p < s.size() && spacechars.find(s[p]) != byte_view::npos)
    p++; /* skip initial spaces */
  if (sv_byteat(s, p) == '-')
  {
    p++;
    neg = 1;
  } /* handle sign */
  else if (sv_byteat(s, p) == '+')
    p++;
  if (!locale_isalnum(sv_byteat(s, p))) /* no digit? */
    return {byte_view::npos, 0};
  do
  {
    int digit = (locale_isdigit(sv_byteat(s, p))
                                   ? s[p] - '0'
                                   : (locale_toupper(sv_byteat(s, p)) - 'A') + 10);
    if (digit >= base)
      return {byte_view::npos, 0}; /* invalid numeral */
    n = n * base + digit;
    p++;
  } while (locale_isalnum(sv_byteat(s, p)));
  while (p < s.size() && spacechars.find(s[p]) != byte_view::npos)
    p++; /* skip trailing spaces */
  return {p, (lua_Integer) (neg ? (0u - n) : n)};
}

static int luaB_tonumber(lua_State* L)
{
  if (lua_isnoneornil(L, 2))
  { /* standard conversion? */
    if (lua_type(L, 1) == LUA_TNUMBER)
    {                   /* already a number? */
      lua_settop(L, 1); /* yes; return it */
      return 1;
    }
    else
    {
      auto s = lua_tolstring(L, 1);
      if (s && lua_stringtonumber(L, *s) == s->size() + 1)
        return 1; /* successful conversion to number */
      /* else not a number */
      luaL_checkany(L, 1); /* (but there must be some parameter) */
    }
  }
  else
  {
    lua_Integer base = luaL_checkinteger(L, 2);
    luaL_checktype(L, 1, LUA_TSTRING); /* no numbers as strings */
    byte_view s = *lua_tolstring(L, 1);
    luaL_argchecki(L, 2 <= base && base <= 36, 2, lt::base_out_of_range);
    IntParse n = b_str2int(s, (int)base);
    if (n.end == s.size())
    {
      lua_pushinteger(L, n.value);
      return 1;
    } /* else not a number */
  } /* else not a number */
  luaL_pushfail(L); /* not a number */
  return 1;
}

static int luaB_error(lua_State* L)
{
  int level = (int) luaL_optinteger(L, 2, 1);
  lua_settop(L, 1);
  if (lua_type(L, 1) == LUA_TSTRING && level > 0)
  {
    luaL_where(L, level); /* add extra information */
    lua_pushvalue(L, 1);
    lua_concat(L, 2);
  }
  return lua_error(L);
}

static int luaB_getmetatable(lua_State* L)
{
  luaL_checkany(L, 1);
  if (!lua_getmetatable(L, 1))
  {
    lua_pushnil(L);
    return 1; /* no metatable */
  }
  luaL_getmetafieldi(L, 1, lt::mm_metatable);
  return 1; /* returns either __metatable field (if present) or metatable */
}

static int luaB_setmetatable(lua_State* L)
{
  int t = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_argexpectedi(L, t == LUA_TNIL || t == LUA_TTABLE, 2, lt::nil_or_table);
  if (l_unlikely(luaL_getmetafieldi(L, 1, lt::mm_metatable) != LUA_TNIL))
    return luaL_errori(L, lt::cannot_change_a_protected_metatable);
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;
}

static int luaB_rawequal(lua_State* L)
{
  luaL_checkany(L, 1);
  luaL_checkany(L, 2);
  lua_pushboolean(L, lua_rawequal(L, 1, 2));
  return 1;
}

static int luaB_rawlen(lua_State* L)
{
  int t = lua_type(L, 1);
  luaL_argexpectedi(L, t == LUA_TTABLE || t == LUA_TSTRING, 1, lt::table_or_string);
  lua_pushinteger(L, lua_rawlen(L, 1));
  return 1;
}

static int luaB_rawget(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  lua_rawget(L, 1);
  return 1;
}

static int luaB_rawset(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  luaL_checkany(L, 3);
  lua_settop(L, 3);
  lua_rawset(L, 1);
  return 1;
}

static int pushmode(lua_State* L, int oldmode)
{
  if (oldmode == -1)
    luaL_pushfail(L); /* invalid call to 'lua_gc' */
  else
    lua_pushistringref(L, (oldmode == LUA_GCINC) ? lt::incremental : lt::generational);
  return 1;
}

/*
** check whether call to 'lua_gc' was valid (not inside a finalizer)
*/

/*
** A collection of a budgeted thread queues the '__gc' of its group's dead
** objects: they run before 'collectgarbage' returns (the result is on the
** top; the calls can freeze, and then the function goes on in its
** continuation).
*/
static int drain_cont(lua_State*, int, lua_KContext)
{
  return 1;
}

static int drainafter(lua_State* L)
{
  if (lua_hasdrain(L))
  {
    lua_pushdrain(L);
    lua_callk(L, 0, 0, 0, drain_cont);
  }
  return 1;
}

static int luaB_collectgarbage(lua_State* L)
{
  static const byte_view opts[] = {
      lt::stop, lt::restart, lt::collect,
      lt::count, lt::step, lt::isrunning, lt::generational,
      lt::incremental, lt::param};
  static const char optsnum[] = {LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT,
                                 LUA_GCCOUNT, LUA_GCSTEP, LUA_GCISRUNNING, LUA_GCGEN, LUA_GCINC,
                                 LUA_GCPARAM};
  int o = optsnum[luaL_checkoptioni(L, 1, lt::collect, opts)];
  switch (o)
  {
  case LUA_GCCOUNT:
  {
    int k = lua_gc(L, o, 0, 0, 0);
    int b = lua_gc(L, LUA_GCCOUNTB, 0, 0, 0);
    { if (k == -1) break; };
    lua_pushnumber(L, (lua_Number) k + ((lua_Number) b / 1024));
    return 1;
  }
  case LUA_GCSTEP:
  {
    lua_Integer n = luaL_optinteger(L, 2, 0);
    int res = lua_gc(L, o, n, 0, 0);
    { if (res == -1) break; };
    lua_pushboolean(L, res);
    return drainafter(L);
  }
  case LUA_GCISRUNNING:
  {
    int res = lua_gc(L, o, 0, 0, 0);
    { if (res == -1) break; };
    lua_pushboolean(L, res);
    return 1;
  }
  case LUA_GCGEN:
  {
    return pushmode(L, lua_gc(L, o, 0, 0, 0));
  }
  case LUA_GCINC:
  {
    return pushmode(L, lua_gc(L, o, 0, 0, 0));
  }
  case LUA_GCPARAM:
  {
    static const byte_view params[] = {
        lt::minormul, lt::majorminor, lt::minormajor,
        lt::pause, lt::stepmul, lt::stepsize};
    static const char pnum[] = {
        LUA_GCPMINORMUL, LUA_GCPMAJORMINOR, LUA_GCPMINORMAJOR,
        LUA_GCPPAUSE, LUA_GCPSTEPMUL, LUA_GCPSTEPSIZE};
    int p = pnum[luaL_checkoptioni(L, 2, {}, params)];
    lua_Integer value = luaL_optinteger(L, 3, -1);
    lua_pushinteger(L, lua_gc(L, o, 0, p, (int) value));
    return 1;
  }
  default:
  {
    int res = lua_gc(L, o, 0, 0, 0);
    { if (res == -1) break; };
    lua_pushinteger(L, res);
    return drainafter(L);
  }
  }
  luaL_pushfail(L); /* invalid call (inside a finalizer) */
  return 1;
}

static int luaB_type(lua_State* L)
{
  int t = lua_type(L, 1);
  luaL_argchecki(L, t != LUA_TNONE, 1, lt::value_expected);
  lua_pushistringref(L, lua_typenamei(L, t));
  return 1;
}

static int luaB_next(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2); /* create a 2nd argument if there isn't one */
  if (lua_next(L, 1))
    return 2;
  else
  {
    lua_pushnil(L);
    return 1;
  }
}

static int pairscont(lua_State*, int, lua_KContext)
{
  return 4; /* __pairs did all the work, just return its results */
}

static CFuncInfo cfi_next(luaB_next);

static int luaB_pairs(lua_State* L)
{
  luaL_checkany(L, 1);
  if (luaL_getmetafieldi(L, 1, lt::mm_pairs) == LUA_TNIL)
  {                                  /* no metamethod? */
    lua_pushcfuncinfo(L, &cfi_next); /* will return generator and */
    lua_pushvalue(L, 1);             /* state */
    lua_pushnil(L);                  /* initial value */
    lua_pushnil(L);                  /* to-be-closed object */
  }
  else
  {
    lua_pushvalue(L, 1);              /* argument 'self' to metamethod */
    lua_callk(L, 1, 4, 0, pairscont); /* get 4 values from metamethod */
  }
  return 4;
}

/*
** Traversal function for 'ipairs'
*/
static int ipairsauxk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return lua_isnil(L, -1) ? 1 : 2;
}

static int ipairsaux(lua_State* L)
{
  lua_Integer i = luaL_checkinteger(L, 2);
  i = l_intadd(i, 1);
  lua_pushinteger(L, i);
  luaE_freezeonly(L);
  return (lua_getik(L, 1, i, 0, ipairsauxk) == LUA_TNIL) ? 1 : 2;
}

/*
** 'ipairs' function. Returns 'ipairsaux', given "table", 0.
** (The given "table" may not be a table.)
*/
static CFuncInfo cfi_ipairsaux(ipairsaux);

static int luaB_ipairs(lua_State* L)
{
  luaL_checkany(L, 1);
  lua_pushcfuncinfo(L, &cfi_ipairsaux); /* iteration function */
  lua_pushvalue(L, 1);             /* state */
  lua_pushinteger(L, 0);           /* initial value */
  return 3;
}

static int load_aux(lua_State* L, int status, int envidx)
{
  if (l_likely(status == LUA_OK))
  {
    if (envidx != 0)
    {                                /* 'env' parameter? */
      lua_pushvalue(L, envidx);      /* environment for loaded function */
      if (!lua_setupvalue(L, -2, 1)) /* set it as 1st upvalue */
        lua_pop(L, 1);               /* remove 'env' if not used by previous call */
    }
    return 1;
  }
  else
  { /* error (message is on top of the stack) */
    luaL_pushfail(L);
    lua_insert(L, -2); /* put before error message */
    return 2;          /* return fail plus error message */
  }
}

static const InternalString* getMode(lua_State* L, int idx)
{
  const InternalString* mode = luaL_optistring(L, idx, nullptr);
  if (mode != nullptr && mode->view().find('B') != byte_view::npos)
  {
    /* Lua code cannot use fixed buffers */
    luaL_argerrori(L, idx, lt::invalid_mode);
  }
  return mode;
}

static int luaB_loadfile(lua_State* L)
{
  std::optional<byte_view> fname = lua_isnoneornil(L, 1) ? std::nullopt : std::optional<byte_view>(luaL_checkistring(L, 1)->view());
  const InternalString* modestr = getMode(L, 2);
  std::optional<byte_view> mode = modestr ? std::optional<byte_view>(modestr->view()) : std::nullopt;
  int env = (!lua_isnone(L, 3) ? 3 : 0); /* 'env' index or 0 if no 'env' */
  int status = luaL_loadfilei(L, fname, mode);
  return load_aux(L, status, env);
}

/*
** {======================================================
** Generic Read function
** =======================================================
*/

/*
** Reader for generic 'load' function.
*/
struct GenericReader : lua_Reader
{
  int firstcall;
  std::optional<byte_view> read(lua_State* L) override;
};

static std::optional<byte_view> generic_reader(lua_State* L, GenericReader* r)
{
  luaL_checkstacki(L, 2, lt::too_many_nested_functions);
  if (r->firstcall)
    r->firstcall = 0;
  else
    lua_pop(L, 1);     /* remove previous result */
  lua_pushvalue(L, 1); /* get function */
  lua_call(L, 0, 1);   /* call it */
  if (lua_isnil(L, -1))
    return std::nullopt;
  else if (l_unlikely(!lua_isstring(L, -1)))
    luaL_errori(L, lt::reader_function_must_return_a_string);
  return *lua_tolstring(L, -1);
}

std::optional<byte_view> GenericReader::read(lua_State* L)
{
  return generic_reader(L, this);
}

static int luaB_load(lua_State* L)
{
  int status;
  auto s = lua_tolstring(L, 1);
  const InternalString* mode = getMode(L, 3);
  int env = (!lua_isnone(L, 4) ? 4 : 0); /* 'env' index or 0 if no 'env' */
  std::optional<byte_view> optmode = (mode != nullptr) ? std::optional<byte_view>(mode->view()) : std::nullopt;
  if (s)
  {
    const InternalString* chunkname = luaL_optistring(L, 2, lua_toistring(L, 1));
    std::optional<byte_view> optname = (chunkname != nullptr) ? std::optional<byte_view>(chunkname->view()) : std::nullopt;
    status = luaL_loadbufferi(L, *s, optname, optmode);
  }
  else
  {                    /* loading from a reader function */
    GenericReader reader; /* userdata for generic_reader */
    reader.firstcall = 1;
    const InternalString* chunkname = luaL_optistring(L, 2, &lt::load_1_i);
    std::optional<byte_view> optname = (chunkname != nullptr) ? std::optional<byte_view>(chunkname->view()) : std::nullopt;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    status = lua_loadi(L, &reader, optname, optmode);
  }
  return load_aux(L, status, env);
}

/* }====================================================== */

static int dofilecont(lua_State* L, int d1, lua_KContext d2)
{
  (void) d1;
  (void) d2; /* only to match 'lua_Kfunction' prototype */
  return lua_gettop(L) - 1;
}

static int luaB_dofile(lua_State* L)
{
  std::optional<byte_view> fname = lua_isnoneornil(L, 1) ? std::nullopt : std::optional<byte_view>(luaL_checkistring(L, 1)->view());
  lua_settop(L, 1);
  if (l_unlikely(luaL_loadfilei(L, fname, std::nullopt) != LUA_OK))
    return lua_error(L);
  lua_callk(L, 0, LUA_MULTRET, 0, dofilecont);
  return dofilecont(L, 0, 0);
}

static int luaB_assert(lua_State* L)
{
  if (l_likely(lua_toboolean(L, 1))) /* condition is true? */
    return lua_gettop(L);            /* return all arguments */
  else
  {                                          /* error */
    luaL_checkany(L, 1);                     /* there must be a condition */
    lua_remove(L, 1);                        /* remove it */
    lua_pushistringref(L, lt::assertion_failed); /* default message */
    lua_settop(L, 1);                        /* leave only message (default if no other one) */
    return luaB_error(L);                    /* call 'error' */
  }
}

static int luaB_select(lua_State* L)
{
  int n = lua_gettop(L);
  if (lua_type(L, 1) == LUA_TSTRING && sv_byteat(*lua_tolstring(L, 1), 0) == '#')
  {
    lua_pushinteger(L, n - 1);
    return 1;
  }
  else
  {
    lua_Integer i = luaL_checkinteger(L, 1);
    if (i < 0)
      i = n + i;
    else if (i > n)
      i = n;
    luaL_argchecki(L, 1 <= i, 1, lt::index_out_of_range);
    return n - (int) i;
  }
}

/*
** Continuation function for 'pcall' and 'xpcall'. Both functions
** already pushed a 'true' before doing the call, so in case of success
** 'finishpcall' only has to return everything in the stack minus
** 'extra' values (where 'extra' is exactly the number of items to be
** ignored).
*/
static int finishpcall(lua_State* L, int status, lua_KContext extra)
{
  if (l_unlikely(status != LUA_OK && status != LUA_YIELD))
  {                        /* error? */
    lua_pushboolean(L, 0); /* first result (false) */
    lua_pushvalue(L, -2);  /* error message */
    return 2;              /* return false, msg */
  }
  else
    return lua_gettop(L) - (int) extra; /* return all results */
}

static int luaB_pcall(lua_State* L)
{
  int status;
  luaL_checkany(L, 1);
  lua_pushboolean(L, 1); /* first result if no errors */
  lua_insert(L, 1);      /* put it in place */
  status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finishpcall);
  return finishpcall(L, status, 0);
}

/*
** Do a protected call with error handling. After 'lua_rotate', the
** stack will have <f, err, true, f, [args...]>; so, the function passes
** 2 to 'finishpcall' to skip the 2 first values when returning results.
*/
static int luaB_xpcall(lua_State* L)
{
  int status;
  int n = lua_gettop(L);
  luaL_checktype(L, 2, LUA_TFUNCTION); /* check error function */
  lua_pushboolean(L, 1);               /* first result */
  lua_pushvalue(L, 1);                 /* function */
  lua_rotate(L, 3, 2);                 /* move them below function's arguments */
  status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishpcall);
  return finishpcall(L, status, 2);
}

static int tostringk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  luaL_tostringresult(L);
  return 1;
}

static int luaB_tostring(lua_State* L)
{
  luaL_checkany(L, 1);
  luaE_freezeonly(L);
  if (luaL_toistringk(L, 1, 0, tostringk))
    luaL_tostringresult(L);
  return 1;
}

static CFuncInfo cfi_assert(luaB_assert);
static CFuncInfo cfi_collectgarbage(luaB_collectgarbage);
static CFuncInfo cfi_dofile(luaB_dofile);
static CFuncInfo cfi_error(luaB_error);
static CFuncInfo cfi_getmetatable(luaB_getmetatable);
static CFuncInfo cfi_ipairs(luaB_ipairs);
static CFuncInfo cfi_loadfile(luaB_loadfile);
static CFuncInfo cfi_load(luaB_load);
static CFuncInfo cfi_pairs(luaB_pairs);
static CFuncInfo cfi_pcall(luaB_pcall);
static CFuncInfo cfi_print(luaB_print);
static CFuncInfo cfi_warn(luaB_warn);
static CFuncInfo cfi_rawequal(luaB_rawequal);
static CFuncInfo cfi_rawlen(luaB_rawlen);
static CFuncInfo cfi_rawget(luaB_rawget);
static CFuncInfo cfi_rawset(luaB_rawset);
static CFuncInfo cfi_select(luaB_select);
static CFuncInfo cfi_setmetatable(luaB_setmetatable);
static CFuncInfo cfi_tonumber(luaB_tonumber);
static CFuncInfo cfi_tostring(luaB_tostring);
static CFuncInfo cfi_type(luaB_type);
static CFuncInfo cfi_xpcall(luaB_xpcall);

static const luaL_RegCFI base_funcs[] = {
  {lt::assert, &cfi_assert},
  {lt::collectgarbage, &cfi_collectgarbage},
  {lt::dofile, &cfi_dofile},
  {lt::error_kw, &cfi_error},
  {lt::getmetatable, &cfi_getmetatable},
  {lt::ipairs, &cfi_ipairs},
  {lt::loadfile, &cfi_loadfile},
  {lt::load_2, &cfi_load},
  {lt::next_kw, &cfi_next},
  {lt::pairs, &cfi_pairs},
  {lt::pcall, &cfi_pcall},
  {lt::print, &cfi_print},
  {lt::warn, &cfi_warn},
  {lt::rawequal, &cfi_rawequal},
  {lt::rawlen, &cfi_rawlen},
  {lt::rawget, &cfi_rawget},
  {lt::rawset, &cfi_rawset},
  {lt::select, &cfi_select},
  {lt::setmetatable, &cfi_setmetatable},
  {lt::tonumber, &cfi_tonumber},
  {lt::tostring, &cfi_tostring},
  {lt::type, &cfi_type},
  {lt::xpcall, &cfi_xpcall},
  /* placeholders */
  {LUA_GNAME, nullptr},
  {lt::_VERSION, nullptr}
};

}

LUAMOD_API int luaopen_base(lua_State* L)
{
  /* open lib into global table */
  lua_pushglobaltable(L);
  luaL_setfuncscfi(L, base_funcs);
  /* set global _G */
  lua_pushvalue(L, -1);
  lua_setfieldi(L, -2, LUA_GNAME);
  /* set global _VERSION */
  lua_pushlstring(L, LUA_VERSION, 0, LUA_VERSION.size());
  lua_setfieldi(L, -2, lt::_VERSION);
  return 1;
}

