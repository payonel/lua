/*
** $Id: loslib.c $
** Standard Operating System library
** See Copyright Notice in lua.h
*/



#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"
#include "lua_internal.h"
#include "os_api.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

/*
** {==================================================================
** List of valid conversion specifiers for the 'strftime' function;
** options are grouped by length; group of length 2 start with '||'.
** ===================================================================
*/
/* }================================================================== */

/*
** {==================================================================
** Configuration for time-related stuff
** ===================================================================
*/



/* }================================================================== */

/*
** {==================================================================
** Configuration for 'tmpnam':
** By default, Lua uses tmpnam except when POSIX is available, where
** it uses mkstemp.
** ===================================================================
*/
#include <unistd.h>

constexpr int LUA_TMPNAMBUFSIZE = 32;

/* }================================================================== */

static int os_execute(lua_State* L)
{
  int nocmd = lua_isnoneornil(L, 1);
  if (!nocmd)
  {
    const InternalString* cmd = luaL_checkistring(L, 1);
    OsSystemResult r = os_system(cmd->view());
    return luaL_execresulti(L, r.stat, r.err);
  }
  else
  {
    lua_pushboolean(L, os_shell_available()); /* true if there is a shell */
    return 1;
  }
}

static int os_remove(lua_State* L)
{
  const InternalString* filename = luaL_checkistring(L, 1);
  OsResult r = os_remove(filename->view());
  return luaL_fileresulti(L, r.ok, filename->view(), r.err);
}

static int os_rename(lua_State* L)
{
  const InternalString* fromname = luaL_checkistring(L, 1);
  const InternalString* toname = luaL_checkistring(L, 2);
  OsResult r = os_rename(fromname->view(), toname->view());
  return luaL_fileresulti(L, r.ok, {}, r.err);
}

static int os_tmpname(lua_State* L)
{
  std::optional<InternalString> tname = os_tmpname();
  if (l_unlikely(!tname.has_value()))
    return luaL_errori(L, lt::unable_to_generate_a_unique_filename);
  lua_pushistring(L, std::move(*tname));
  return 1;
}

static int os_getenv(lua_State* L)
{
  std::optional<std::string_view> v = os_getenv(luaL_checkistring(L, 1)->view());
  if (v.has_value())
    lua_pushistring(L, std::move(*v));
  else
    lua_pushnil(L);
  return 1;
}

static int os_clock(lua_State* L)
{
  lua_pushnumber(L, ((lua_Number) clock()) / (lua_Number) CLOCKS_PER_SEC);
  return 1;
}

/*
** {======================================================
** Time/Date operations
** { year=%Y, month=%m, day=%d, hour=%H, min=%M, sec=%S,
**   wday=%w+1, yday=%j, isdst=? }
** =======================================================
*/

static void setfield(lua_State* L, std::string_view key, int value, int delta)
{
  lua_pushinteger(L, (lua_Integer) value + delta);
  lua_setfieldi(L, -2, key);
}

static void setboolfield(lua_State* L, std::string_view key, int value)
{
  if (value < 0) /* undefined? */
    return;      /* does not set field */
  lua_pushboolean(L, value);
  lua_setfieldi(L, -2, key);
}

/*
** Set all fields from structure 'tm' in the table on top of the stack
*/
static void setallfields(lua_State* L, struct tm* stm)
{
  setfield(L, lt::year, stm->tm_year, 1900);
  setfield(L, lt::month, stm->tm_mon, 1);
  setfield(L, lt::day, stm->tm_mday, 0);
  setfield(L, lt::hour, stm->tm_hour, 0);
  setfield(L, lt::min, stm->tm_min, 0);
  setfield(L, lt::sec, stm->tm_sec, 0);
  setfield(L, lt::yday, stm->tm_yday, 1);
  setfield(L, lt::wday, stm->tm_wday, 1);
  setboolfield(L, lt::isdst, stm->tm_isdst);
}

static int getboolfield(lua_State* L, std::string_view key)
{
  int res;
  res = (lua_getfieldi(L, -1, key) == LUA_TNIL) ? -1 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}

static int getfield(lua_State* L, std::string_view key, int d, int delta)
{
  int t = lua_getfieldi(L, -1, key); /* get field and its type */
  std::optional<lua_Integer> ores = lua_tointegerx(L, -1);
  lua_Integer res = ores.value_or(0);
  if (!ores)
  {                                /* field is not an integer? */
    if (l_unlikely(t != LUA_TNIL))
    { /* some other value? */
      lua_pushlstring(L, key, 0, key.size());
      return luaL_errori(L, lt::field_s_is_not_an_integer);
    }
    else if (l_unlikely(d < 0))
    { /* absent field; no default? */
      lua_pushlstring(L, key, 0, key.size());
      return luaL_errori(L, lt::field_s_missing_in_date_table);
    }
    res = d;
  }
  else
  {
    if (!(res >= 0 ? res - delta <= INT_MAX : INT_MIN + delta <= res))
    {
      lua_pushlstring(L, key, 0, key.size());
      return luaL_errori(L, lt::field_s_is_out_of_bound);
    }
    res -= delta;
  }
  lua_pop(L, 1);
  return (int) res;
}

static size_t checkoption(lua_State* L, std::string_view fmt, size_t pos,
                          std::span<char> buff, size_t boff)
{
  std::string_view option = lt::strftime_options;
  size_t opos = 0;
  size_t convlen = fmt.size() - pos;
  unsigned oplen = 1; /* length of options being checked */
  for (; opos < option.size() && oplen <= convlen; opos += oplen)
  {
    if (option[opos] == '|') /* next block? */
      oplen++;               /* will check options with next length (+1) */
    else if (memcmp(fmt.data() + pos, option.data() + opos, oplen) == 0)
    {
      memcpy(buff.data() + boff, fmt.data() + pos, oplen);
      buff[boff + oplen] = '\0';
      return pos + oplen; /* return next item */
    }
  }
  lua_pushlstring(L, fmt, pos, fmt.size() - pos);
  lua_pushfstringi(L, lt::invalid_conversion_specifier_s);
  luaL_argerrori(L, 1, *lua_tolstring(L, -1));
  return pos; /* to avoid warnings */
}

static time_t l_checktime(lua_State* L, int arg)
{
  lua_Integer t = luaL_checkinteger(L, arg);
  luaL_argchecki(L, (time_t) t == t, arg, lt::time_out_of_bounds);
  return (time_t) t;
}

/* maximum size for an individual 'strftime' item */
constexpr int SIZETIMEFMT = 250;

static int os_date(lua_State* L)
{
  std::string_view sfmt = luaL_optlstring(L, 1, lt::fmt_c);
  size_t sp = 0;
  time_t t = (lua_isnoneornil(L, (2)) ? (time(nullptr)) : l_checktime(L, (2)));
  struct tm tmr, *stm;
  if (sv_byteat(sfmt, sp) == '!')
  { /* UTC? */
    stm = gmtime_r(&t, &tmr);
    sp++; /* skip '!' */
  }
  else
    stm = localtime_r(&t, &tmr);
  if (stm == nullptr) /* invalid date? */
    return luaL_errori(L, lt::date_result_cannot_be_represented_in_this_installation);
  if (sfmt.size() - sp == 2 && sfmt[sp] == '*' && sfmt[sp + 1] == 't')
  {
    lua_createtable(L, 0, 9); /* 9 = number of fields */
    setallfields(L, stm);
  }
  else
  {
    char cc[4]; /* buffer for individual conversion specifiers */
    luaL_Buffer b;
    cc[0] = '%';
    luaL_buffinit(L, &b);
    while (sp < sfmt.size())
    {
      if (sfmt[sp] != '%') /* not a conversion specifier? */
        luaL_addchar(&b, sfmt[sp++]);
      else
      {
        size_t reslen;
        size_t off = luaL_prepbuffsize(&b, SIZETIMEFMT);
        sp++; /* skip '%' */
        sp = checkoption(L, sfmt, sp, cc, 1);
        reslen = strftime(luaL_buffarea(&b).data() + off, SIZETIMEFMT, cc, stm);
        luaL_addsize(&b, reslen);
      }
    }
    luaL_pushresult(&b);
  }
  return 1;
}

static int os_time(lua_State* L)
{
  time_t t;
  if (lua_isnoneornil(L, 1)) /* called without args? */
    t = time(nullptr);          /* get current time */
  else
  {
    struct tm ts;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1); /* make sure table is at the top */
    ts.tm_year = getfield(L, lt::year, -1, 1900);
    ts.tm_mon = getfield(L, lt::month, -1, 1);
    ts.tm_mday = getfield(L, lt::day, -1, 0);
    ts.tm_hour = getfield(L, lt::hour, 12, 0);
    ts.tm_min = getfield(L, lt::min, 0, 0);
    ts.tm_sec = getfield(L, lt::sec, 0, 0);
    ts.tm_isdst = getboolfield(L, lt::isdst);
    t = mktime(&ts);
    setallfields(L, &ts); /* update fields with normalized values */
  }
  if (t != (time_t) (lua_Integer) t || t == (time_t) (-1))
    return luaL_errori(L, lt::time_result_cannot_be_represented_in_this_installation);
  lua_pushinteger(L, (lua_Integer) t);
  return 1;
}

static int os_difftime(lua_State* L)
{
  time_t t1 = l_checktime(L, 1);
  time_t t2 = l_checktime(L, 2);
  lua_pushnumber(L, (lua_Number) difftime(t1, t2));
  return 1;
}

/* }====================================================== */

static int os_setlocale(lua_State* L)
{
  static const int cat[] = {LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY,
                            LC_NUMERIC, LC_TIME};
  static const std::string_view catnames[] = {
      lt::all, lt::collate, lt::ctype,
      lt::monetary, lt::numeric, lt::time_kw};
  int nol = lua_isnoneornil(L, 1);
  const InternalString* l = nol ? nullptr : luaL_checkistring(L, 1);
  int op = luaL_checkoptioni(L, 2, lt::all, catnames);
  std::optional<InternalString> res = os_setlocale(
      cat[op], nol ? std::nullopt : std::optional<std::string_view>(l->view()));
  if (res.has_value())
  {
    std::string_view rsv = res->view();
    lua_pushlstring(L, rsv, 0, rsv.size());
  }
  else
    lua_pushnil(L);
  return 1;
}

static int os_exit(lua_State* L)
{
  int status;
  if (lua_isboolean(L, 1))
    status = (lua_toboolean(L, 1) ? EXIT_SUCCESS : EXIT_FAILURE);
  else
    status = (int) luaL_optinteger(L, 1, EXIT_SUCCESS);
  if (lua_toboolean(L, 2))
    lua_close(L);
  if (L)
    exit(status); /* 'if' to avoid warnings for unreachable 'return' */
  return 0;
}

static const luaL_Regi syslib[] = {
  {lt::clock, os_clock},
  {lt::date, os_date},
  {lt::difftime, os_difftime},
  {lt::execute, os_execute},
  {lt::exit_kw, os_exit},
  {lt::getenv, os_getenv},
  {lt::remove_kw, os_remove},
  {lt::rename_kw, os_rename},
  {lt::setlocale, os_setlocale},
  {lt::time_kw, os_time},
  {lt::tmpname, os_tmpname}
};

/* }====================================================== */

LUAMOD_API int luaopen_os(lua_State* L)
{
  luaL_newlibi(L, syslib);
  return 1;
}
