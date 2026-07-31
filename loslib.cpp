/*
** $Id: loslib.c $
** Standard Operating System library
** See Copyright Notice in lua.h
*/


#include "lprefix.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"
#include "lua_internal.h"
#include "llimits.h"
#include "lualib.h"

/*
** {==================================================================
** List of valid conversion specifiers for the 'strftime' function;
** options are grouped by length; group of length 2 start with '||'.
** ===================================================================
*/
#if !defined(LUA_STRFTIMEOPTIONS) /* { */

#define LUA_STRFTIMEOPTIONS "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%" \
                            "||"                                    \
                            "EcECExEXEyEY"                          \
                            "OdOeOHOIOmOMOSOuOUOVOwOWOy" /* two-char options */

#endif /* } */
/* }================================================================== */

/*
** {==================================================================
** Configuration for time-related stuff
** ===================================================================
*/

/*
** type to represent time_t in Lua
*/
#if !defined(LUA_NUMTIME) /* { */

#define l_timet           lua_Integer
#define l_pushtime(L, t)  lua_pushinteger(L, (lua_Integer) (t))
#define l_gettime(L, arg) luaL_checkinteger(L, arg)

#else /* }{ */

#define l_timet           lua_Number
#define l_pushtime(L, t)  lua_pushnumber(L, (lua_Number) (t))
#define l_gettime(L, arg) luaL_checknumber(L, arg)

#endif /* } */

#if !defined(l_gmtime) /* { */
/*
** By default, Lua uses gmtime/localtime, except when POSIX is available,
** where it uses gmtime_r/localtime_r
*/

#define l_gmtime(t, r)    gmtime_r(t, r)
#define l_localtime(t, r) localtime_r(t, r)

#endif /* } */

/* }================================================================== */

/*
** {==================================================================
** Configuration for 'tmpnam':
** By default, Lua uses tmpnam except when POSIX is available, where
** it uses mkstemp.
** ===================================================================
*/
#if !defined(lua_tmpnam) /* { */

#include <unistd.h>

constexpr int LUA_TMPNAMBUFSIZE = 32;

#if !defined(LUA_TMPNAMTEMPLATE)
#define LUA_TMPNAMTEMPLATE "/tmp/lua_XXXXXX"
#endif

#define lua_tmpnam(b, e)           \
  {                                \
    strcpy(b, LUA_TMPNAMTEMPLATE); \
    e = mkstemp(b);                \
    if (e != -1)                   \
      close(e);                    \
    e = (e == -1);                 \
  }

#endif /* } */
/* }================================================================== */

#if !defined(l_system)
#define l_system(cmd) system(cmd) /* default definition */
#endif

static int os_execute(lua_State* L)
{
  int nocmd = lua_isnoneornil(L, 1);
  const InternalString* cmd = nocmd ? nullptr : luaL_checkistring(L, 1);
  int stat;
  errno = 0;
  stat = l_system(nocmd ? nullptr : cmd->c_str()); /* OS edge (sanctioned c_str) */
  if (!nocmd)
    return luaL_execresult(L, stat);
  else
  {
    lua_pushboolean(L, stat); /* true if there is a shell */
    return 1;
  }
}

static int os_remove(lua_State* L)
{
  const InternalString* filename = luaL_checkistring(L, 1);
  errno = 0;
  return luaL_fileresulti(L, remove(filename->c_str()) == 0, filename->view());
}

static int os_rename(lua_State* L)
{
  const InternalString* fromname = luaL_checkistring(L, 1);
  const InternalString* toname = luaL_checkistring(L, 2);
  errno = 0;
  return luaL_fileresulti(L, rename(fromname->c_str(), toname->c_str()) == 0, {});
}

static int os_tmpname(lua_State* L)
{
  char buff[LUA_TMPNAMBUFSIZE];
  int err;
  lua_tmpnam(buff, err);
  if (l_unlikely(err))
    return luaL_error(L, "unable to generate a unique filename");
  lua_pushstring(L, buff);
  return 1;
}

static int os_getenv(lua_State* L)
{
  const char* v = getenv(luaL_checkistring(L, 1)->c_str()); /* OS edge (sanctioned c_str) */
  if (v != nullptr)
    lua_pushstring(L, v);
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

/*
** About the overflow check: an overflow cannot occur when time
** is represented by a lua_Integer, because either lua_Integer is
** large enough to represent all int fields or it is not large enough
** to represent a time that cause a field to overflow.  However, if
** times are represented as doubles and lua_Integer is int, then the
** time 0x1.e1853b0d184f6p+55 would cause an overflow when adding 1900
** to compute the year.
*/
static void setfield(lua_State* L, const HostString& key, int value, int delta)
{
#if (defined(LUA_NUMTIME) && LUA_MAXINTEGER <= INT_MAX)
  if (l_unlikely(value > LUA_MAXINTEGER - delta))
  {
    lua_pushstring(L, key);
    luaL_error(L, "field '%s' is out-of-bound");
  }
#endif
  lua_pushinteger(L, (lua_Integer) value + delta);
  lua_setfield(L, -2, key);
}

static void setboolfield(lua_State* L, const HostString& key, int value)
{
  if (value < 0) /* undefined? */
    return;      /* does not set field */
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}

/*
** Set all fields from structure 'tm' in the table on top of the stack
*/
static void setallfields(lua_State* L, struct tm* stm)
{
  setfield(L, "year", stm->tm_year, 1900);
  setfield(L, "month", stm->tm_mon, 1);
  setfield(L, "day", stm->tm_mday, 0);
  setfield(L, "hour", stm->tm_hour, 0);
  setfield(L, "min", stm->tm_min, 0);
  setfield(L, "sec", stm->tm_sec, 0);
  setfield(L, "yday", stm->tm_yday, 1);
  setfield(L, "wday", stm->tm_wday, 1);
  setboolfield(L, "isdst", stm->tm_isdst);
}

static int getboolfield(lua_State* L, const HostString& key)
{
  int res;
  res = (lua_getfield(L, -1, key) == LUA_TNIL) ? -1 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}

static int getfield(lua_State* L, const HostString& key, int d, int delta)
{
  int t = lua_getfield(L, -1, key); /* get field and its type */
  std::optional<lua_Integer> ores = lua_tointegerx(L, -1);
  lua_Integer res = ores.value_or(0);
  if (!ores)
  {                                /* field is not an integer? */
    if (l_unlikely(t != LUA_TNIL))
    { /* some other value? */
      lua_pushstring(L, key);
      return luaL_error(L, "field '%s' is not an integer");
    }
    else if (l_unlikely(d < 0))
    { /* absent field; no default? */
      lua_pushstring(L, key);
      return luaL_error(L, "field '%s' missing in date table");
    }
    res = d;
  }
  else
  {
    if (!(res >= 0 ? res - delta <= INT_MAX : INT_MIN + delta <= res))
    {
      lua_pushstring(L, key);
      return luaL_error(L, "field '%s' is out-of-bound");
    }
    res -= delta;
  }
  lua_pop(L, 1);
  return (int) res;
}

/*
** Check the strftime specifier at fmt[pos..] against the option
** blocks; on a match copy it (NUL-terminated) into 'buff' for the
** strftime call and return the offset past it.
*/
static size_t checkoption(lua_State* L, std::string_view fmt, size_t pos,
                          std::span<char> buff, size_t boff)
{
  std::string_view option = LUA_STRFTIMEOPTIONS;
  size_t opos = 0;
  size_t convlen = fmt.size() - pos;
  unsigned oplen = 1; /* length of options being checked */
  for (; opos < option.size() && oplen <= convlen; opos += oplen)
  {
    if (option[opos] == '|') /* next block? */
      oplen++;               /* will check options with next length (+1) */
    else if (memcmp(fmt.data() + pos, option.data() + opos, oplen) == 0)
    { /* match? copy valid option to buffer (host strftime boundary) */
      memcpy(buff.data() + boff, fmt.data() + pos, oplen);
      buff[boff + oplen] = '\0';
      return pos + oplen; /* return next item */
    }
  }
  lua_pushlstring(L, fmt, pos, fmt.size() - pos);
  lua_pushfstring(L, "invalid conversion specifier '%%%s'");
  /* message anchored on the stack; hand it on as consumer text */
  luaL_argerror(L, 1, HostString(lua_tolstring(L, -1)));
  return pos; /* to avoid warnings */
}

static time_t l_checktime(lua_State* L, int arg)
{
  l_timet t = l_gettime(L, arg);
  luaL_argcheck(L, (time_t) t == t, arg, "time out-of-bounds");
  return (time_t) t;
}

/* maximum size for an individual 'strftime' item */
constexpr int SIZETIMEFMT = 250;

static int os_date(lua_State* L)
{
  std::string_view sfmt = luaL_optlstring(L, 1, "%c");
  size_t sp = 0; /* scan offset into the format */
  time_t t = (lua_isnoneornil(L, (2)) ? (time(nullptr)) : l_checktime(L, (2)));
  struct tm tmr, *stm;
  if (sv_byteat(sfmt, sp) == '!')
  { /* UTC? */
    stm = l_gmtime(&t, &tmr);
    sp++; /* skip '!' */
  }
  else
    stm = l_localtime(&t, &tmr);
  if (stm == nullptr) /* invalid date? */
    return luaL_error(L,
                      "date result cannot be represented in this installation");
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
        /* copy specifier to 'cc' (span past the leading '%') */
        sp = checkoption(L, sfmt, sp, cc, 1); /* write past the '%' */
        /* host boundary: strftime writes into the area (Java: formatter) */
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
    ts.tm_year = getfield(L, "year", -1, 1900);
    ts.tm_mon = getfield(L, "month", -1, 1);
    ts.tm_mday = getfield(L, "day", -1, 0);
    ts.tm_hour = getfield(L, "hour", 12, 0);
    ts.tm_min = getfield(L, "min", 0, 0);
    ts.tm_sec = getfield(L, "sec", 0, 0);
    ts.tm_isdst = getboolfield(L, "isdst");
    t = mktime(&ts);
    setallfields(L, &ts); /* update fields with normalized values */
  }
  if (t != (time_t) (l_timet) t || t == (time_t) (-1))
    return luaL_error(L,
                      "time result cannot be represented in this installation");
  l_pushtime(L, t);
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
  static constexpr std::string_view catnames[] = {
      "all", "collate", "ctype", "monetary", "numeric", "time"};
  static constexpr std::string_view defcat = "all";
  int nol = lua_isnoneornil(L, 1); /* absent = query current locale */
  const InternalString* l = nol ? nullptr : luaL_checkistring(L, 1);
  int op = luaL_checkoption(L, 2, defcat, catnames);
  const char* res = setlocale(cat[op], nol ? nullptr : l->c_str()); /* OS edge */
  if (res != nullptr)
    lua_pushstring(L, res);
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

static const luaL_Reg syslib[] = {
    {"clock", os_clock},
    {"date", os_date},
    {"difftime", os_difftime},
    {"execute", os_execute},
    {"exit", os_exit},
    {"getenv", os_getenv},
    {"remove", os_remove},
    {"rename", os_rename},
    {"setlocale", os_setlocale},
    {"time", os_time},
    {"tmpname", os_tmpname},
    {{}, nullptr}};

/* }====================================================== */

LUAMOD_API int luaopen_os(lua_State* L)
{
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(syslib) / sizeof((syslib)[0]) - 1), luaL_setfuncs(L, syslib, 0));
  return 1;
}
