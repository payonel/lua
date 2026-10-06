/*
** $Id: loslib.c $
** Standard Operating System library
** See Copyright Notice in lua.h
*/



#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lua_internal.h"
#include "lobject.h"
#include "lstate.h"
#include "locale_util.h"
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

namespace lua::lib
{

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
  OsResult r = ::os_remove(filename->view());
  return luaL_fileresulti(L, r.ok, filename->view(), r.err);
}

static int os_rename(lua_State* L)
{
  const InternalString* fromname = luaL_checkistring(L, 1);
  const InternalString* toname = luaL_checkistring(L, 2);
  OsResult r = ::os_rename(fromname->view(), toname->view());
  return luaL_fileresulti(L, r.ok, {}, r.err);
}

static int os_tmpname(lua_State* L)
{
  std::optional<InternalString> tname = ::os_tmpname();
  if (l_unlikely(!tname.has_value()))
    return luaL_errori(L, lt::unable_to_generate_a_unique_filename);
  lua_pushistring(L, std::move(*tname));
  return 1;
}

static int os_getenv(lua_State* L)
{
  std::optional<byte_view> v = ::os_getenv(luaL_checkistring(L, 1)->view());
  if (v.has_value())
    lua_pushistring(L, std::move(*v));
  else
    lua_pushnil(L);
  return 1;
}

static int os_clock(lua_State* L)
{
  lua_pushnumber(L, os_cpuclock());
  return 1;
}

/*
** {======================================================
** Time/Date operations
** { year=%Y, month=%m, day=%d, hour=%H, min=%M, sec=%S,
**   wday=%w+1, yday=%j, isdst=? }
** =======================================================
*/

static void setfield(lua_State* L, byte_view key, int value, int delta)
{
  lua_pushinteger(L, (lua_Integer) value + delta);
  lua_setfieldi(L, -2, key);
}

static void setboolfield(lua_State* L, byte_view key, int value)
{
  if (value < 0) /* undefined? */
    return;      /* does not set field */
  lua_pushboolean(L, value);
  lua_setfieldi(L, -2, key);
}

/*
** Set all fields from structure 'tm' in the table on top of the stack
*/
static void setallfields(lua_State* L, const OsDate* stm)
{
  setfield(L, lt::year, stm->year, 1900);
  setfield(L, lt::month, stm->month, 1);
  setfield(L, lt::day, stm->day, 0);
  setfield(L, lt::hour, stm->hour, 0);
  setfield(L, lt::min, stm->min, 0);
  setfield(L, lt::sec, stm->sec, 0);
  setfield(L, lt::yday, stm->yday, 1);
  setfield(L, lt::wday, stm->wday, 1);
  setboolfield(L, lt::isdst, stm->isdst);
}

/*
** The value of field 'key' just read (on the top), as an integer for a
** date ('d' when absent, -1: no default), checked; it replaces the value.
*/
static int checkfield(lua_State* L, byte_view key, int d, int delta)
{
  int t = lua_type(L, -1);
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
  lua_pushinteger(L, res);
  return (int) res;
}

static size_t checkoption(lua_State* L, byte_view fmt, size_t pos, byte_span buff, size_t boff)
{
  byte_view option = lt::strftime_options;
  size_t opos = 0;
  size_t convlen = fmt.size() - pos;
  size_t oplen = 1; /* length of options being checked */
  for (; opos < option.size() && oplen <= convlen; opos += oplen)
  {
    if (option[opos] == '|') /* next block? */
      oplen++;               /* will check options with next length (+1) */
    else if (fmt.substr(pos, oplen).equals(option.substr(opos, oplen)))
    {
      byte_span(buff).copy_from(fmt.substr(pos, oplen), boff);
      buff[boff + oplen] = '\0';
      return pos + oplen; /* return next item */
    }
  }
  lua_pushlstring(L, fmt, pos, fmt.size() - pos);
  lua_pushfstringi(L, lt::invalid_conversion_specifier_s);
  luaL_argerrori(L, 1, *lua_tolstring(L, -1));
  return pos; /* to avoid warnings */
}

static lua_Integer l_checktime(lua_State* L, int arg)
{
  return luaL_checkinteger(L, arg);
}

/* maximum size for an individual 'strftime' item */
constexpr int SIZETIMEFMT = 250;

static int os_date(lua_State* L)
{
  byte_view sfmt = luaL_optlstring(L, 1, lt::fmt_c);
  size_t sp = 0;
  lua_Integer t = (lua_isnoneornil(L, 2) ? os_now() : l_checktime(L, 2));
  std::optional<OsDate> stm;
  if (sv_byteat(sfmt, sp) == '!')
  { /* UTC? */
    stm = os_gmtime(t);
    sp++; /* skip '!' */
  }
  else
    stm = os_localtime(t);
  if (!stm.has_value()) /* invalid date? */
    return luaL_errori(L, lt::date_result_cannot_be_represented_in_this_installation);
  if (sfmt.size() - sp == 2 && sfmt[sp] == '*' && sfmt[sp + 1] == 't')
  {
    lua_createtable(L, 0, 9); /* 9 = number of fields */
    setallfields(L, &*stm);
  }
  else
  {
    byte_inline<4> cc; /* buffer for individual conversion specifiers */
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
        byte_span buff = luaL_buffaddr(&b);
        reslen = os_strftime(buff, off, cc.str_view(), *stm);
        luaL_addsize(&b, reslen);
      }
    }
    luaL_pushresult(&b);
  }
  return 1;
}

/*
** 'os.time' with a table, which may call back ('__index', '__newindex'):
** the table at 1, then the fields read, as integers (2 to 8), then the
** normalized fields to write (2 to 10, with the result at 11, nil if it
** cannot be represented). 'step' says where it goes on: reading field
** 'step' (0 to 6), or writing field 'step - 7' (0 to 8).
*/
struct TimeField
{
  byte_view key;
  int d;     /* default (-1: none) */
  int delta; /* (an offset for reads and writes) */
};

static const TimeField timeread[] = {
  {lt::year, -1, 1900}, {lt::month, -1, 1}, {lt::day, -1, 0},
  {lt::hour, 12, 0}, {lt::min, 0, 0}, {lt::sec, 0, 0}};

static const byte_view timewrite[] = {
  lt::year, lt::month, lt::day, lt::hour, lt::min, lt::sec, lt::yday, lt::wday, lt::isdst};

constexpr int TIME_NREAD = 7;  /* the six fields, and 'isdst' */
constexpr int TIME_NWRITE = 9;

static int os_timek(lua_State* L, int status, lua_KContext ctx);

/* the field read at 'step' (on the top): check it */
static void timeread_done(lua_State* L, int step)
{
  if (step < TIME_NREAD - 1)
    checkfield(L, timeread[step].key, timeread[step].d, timeread[step].delta);
  else
  { /* 'isdst' */
    int res = lua_isnil(L, -1) ? -1 : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_pushinteger(L, res);
  }
}

static int os_time_from(lua_State* L, int step)
{
  for (; step < TIME_NREAD; step++)
  {
    byte_view key = (step < TIME_NREAD - 1) ? timeread[step].key : lt::isdst;
    lua_getfieldik(L, 1, key, step, os_timek);
    timeread_done(L, step);
  }
  if (step == TIME_NREAD)
  { /* all read: normalize them */
    OsDate ts = {};
    ts.year = (int)lua_tointeger(L, 2);
    ts.month = (int)lua_tointeger(L, 3);
    ts.day = (int)lua_tointeger(L, 4);
    ts.hour = (int)lua_tointeger(L, 5);
    ts.min = (int)lua_tointeger(L, 6);
    ts.sec = (int)lua_tointeger(L, 7);
    ts.isdst = (int)lua_tointeger(L, 8);
    std::optional<lua_Integer> t = os_mktime(&ts);
    lua_settop(L, 1);
    lua_pushinteger(L, (lua_Integer)ts.year + 1900);
    lua_pushinteger(L, (lua_Integer)ts.month + 1);
    lua_pushinteger(L, (lua_Integer)ts.day);
    lua_pushinteger(L, (lua_Integer)ts.hour);
    lua_pushinteger(L, (lua_Integer)ts.min);
    lua_pushinteger(L, (lua_Integer)ts.sec);
    lua_pushinteger(L, (lua_Integer)ts.yday + 1);
    lua_pushinteger(L, (lua_Integer)ts.wday + 1);
    lua_pushinteger(L, ts.isdst);
    if (t.has_value())
      lua_pushinteger(L, *t);
    else
      lua_pushnil(L);
  }
  /* update fields with normalized values */
  for (int w = step - TIME_NREAD; w < TIME_NWRITE; w++)
  {
    if (w == TIME_NWRITE - 1)
    { /* 'isdst' */
      int isdst = (int)lua_tointeger(L, 2 + w);
      if (isdst < 0) /* undefined? */
        break;       /* does not set field */
      lua_pushboolean(L, isdst);
    }
    else
      lua_pushvalue(L, 2 + w);
    lua_setfieldik(L, 1, timewrite[w], TIME_NREAD + w + 1, os_timek);
  }
  if (lua_isnil(L, 11))
    return luaL_errori(L, lt::time_result_cannot_be_represented_in_this_installation);
  lua_pushvalue(L, 11);
  return 1;
}

/* 'os.time' goes on after a field read or written */
static int os_timek(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  int step = (int)ctx;
  if (step < TIME_NREAD)
    timeread_done(L, step++);
  return os_time_from(L, step);
}

static int os_time(lua_State* L)
{
  if (lua_isnoneornil(L, 1))
  { /* called without args? */
    lua_pushinteger(L, os_now()); /* get current time */
    return 1;
  }
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1); /* make sure table is at the top */
  luaE_freezeonly(L);
  return os_time_from(L, 0);
}

static int os_difftime(lua_State* L)
{
  lua_Integer t1 = l_checktime(L, 1);
  lua_Integer t2 = l_checktime(L, 2);
  lua_pushnumber(L, os_timediff(t1, t2));
  return 1;
}

/* }====================================================== */

constexpr size_t LOCALE_NAME_MAXLEN = 8;

static int os_setlocale(lua_State* L)
{
  static const int cat[] = {LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY,
                            LC_NUMERIC, LC_TIME};
  static const byte_view catnames[] = {
      lt::all, lt::collate, lt::ctype,
      lt::monetary, lt::numeric, lt::time_kw};
  int nol = lua_isnoneornil(L, 1);
  const InternalString* l = nol ? nullptr : luaL_checkistring(L, 1);
  int op = luaL_checkoptioni(L, 2, lt::all, catnames);
  std::optional<byte_view> res;
  if (nol || l->view().size() <= LOCALE_NAME_MAXLEN)
    res = locale_setlocale(cat[op], nol ? std::nullopt : std::optional<byte_view>(l->view()));
  if (res.has_value())
  {
    if (!nol)
      L->l_G->locale_name = InternalString(res.value());
    lua_pushlstring(L, res.value(), 0, res->size());
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

static CFuncInfo cfi_os_clock(os_clock);
static CFuncInfo cfi_os_date(os_date);
static CFuncInfo cfi_os_difftime(os_difftime);
static CFuncInfo cfi_os_execute(os_execute);
static CFuncInfo cfi_os_exit(os_exit);
static CFuncInfo cfi_os_getenv(os_getenv);
static CFuncInfo cfi_os_remove(os_remove);
static CFuncInfo cfi_os_rename(os_rename);
static CFuncInfo cfi_os_setlocale(os_setlocale);
static CFuncInfo cfi_os_time(os_time);
static CFuncInfo cfi_os_tmpname(os_tmpname);

static const luaL_RegCFI syslib[] = {
  {lt::clock, &cfi_os_clock},
  {lt::date, &cfi_os_date},
  {lt::difftime, &cfi_os_difftime},
  {lt::execute, &cfi_os_execute},
  {lt::exit_kw, &cfi_os_exit},
  {lt::getenv, &cfi_os_getenv},
  {lt::remove_kw, &cfi_os_remove},
  {lt::rename_kw, &cfi_os_rename},
  {lt::setlocale, &cfi_os_setlocale},
  {lt::time_kw, &cfi_os_time},
  {lt::tmpname, &cfi_os_tmpname},
};

/* }====================================================== */

}

LUAMOD_API int luaopen_os(lua_State* L)
{
  luaL_newlibcfi(L, syslib);
  return 1;
}

