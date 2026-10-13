package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.bytes.InternalString.sv_byteat;
import static com.payonel.lungolua.platform.LocaleUtil.*;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Lstate.luaE_freezeonly;
import static com.payonel.lungolua.runtime.Lstate.lua_close;

import java.util.OptionalLong;

import com.payonel.lungolua.api.KNamer;
import com.payonel.lungolua.api.luaL_Buffer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteSpan;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.platform.OsApi;
import com.payonel.lungolua.platform.OsDate;
import com.payonel.lungolua.platform.OsResult;
import com.payonel.lungolua.platform.OsSystemResult;
import com.payonel.lungolua.runtime.lua_State;

/*
** $Id: loslib.c $
** Standard Operating System library
** See Copyright Notice in lua.h
*/
public final class Loslib
{
  private Loslib()
  {
  }

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

  private static final int LUA_TMPNAMBUFSIZE = 32;

  /* }================================================================== */

  private static int os_execute(lua_State L)
  {
    int nocmd = lua_isnoneornil(L, 1);
    if (nocmd == 0)
    {
      InternalString cmd = luaL_checkistring(L, 1);
      OsSystemResult r = os_system(cmd.view());
      return luaL_execresulti(L, r.stat, r.err);
    }
    else
    {
      lua_pushboolean(L, os_shell_available() ? 1 : 0); /* true if there is a shell */
      return 1;
    }
  }

  private static final lua_CFunction os_execute = Loslib::os_execute;

  private static int os_remove(lua_State L)
  {
    InternalString filename = luaL_checkistring(L, 1);
    OsResult r = OsApi.os_remove(filename.view());
    return luaL_fileresulti(L, r.ok ? 1 : 0, filename.view(), r.err);
  }

  private static final lua_CFunction os_remove = Loslib::os_remove;

  private static int os_rename(lua_State L)
  {
    InternalString fromname = luaL_checkistring(L, 1);
    InternalString toname = luaL_checkistring(L, 2);
    OsResult r = OsApi.os_rename(fromname.view(), toname.view());
    return luaL_fileresulti(L, r.ok ? 1 : 0, null, r.err);
  }

  private static final lua_CFunction os_rename = Loslib::os_rename;

  private static int os_tmpname(lua_State L)
  {
    InternalString tname = OsApi.os_tmpname();
    if (tname == null)
      return luaL_errori(L, Lt.unable_to_generate_a_unique_filename);
    lua_pushistring(L, tname);
    return 1;
  }

  private static final lua_CFunction os_tmpname = Loslib::os_tmpname;

  private static int os_getenv(lua_State L)
  {
    ByteView v = OsApi.os_getenv(luaL_checkistring(L, 1).view());
    if (v != null)
      lua_pushistring(L, new InternalString(v));
    else
      lua_pushnil(L);
    return 1;
  }

  private static final lua_CFunction os_getenv = Loslib::os_getenv;

  private static int os_clock(lua_State L)
  {
    lua_pushnumber(L, os_cpuclock());
    return 1;
  }

  private static final lua_CFunction os_clock = Loslib::os_clock;

  /*
  ** {======================================================
  ** Time/Date operations
  ** { year=%Y, month=%m, day=%d, hour=%H, min=%M, sec=%S,
  **   wday=%w+1, yday=%j, isdst=? }
  ** =======================================================
  */

  private static void setfield(lua_State L, ByteView key, int value, int delta)
  {
    lua_pushinteger(L, (long) value + delta);
    lua_setfieldi(L, -2, key);
  }

  private static void setboolfield(lua_State L, ByteView key, int value)
  {
    if (value < 0) /* undefined? */
      return;      /* does not set field */
    lua_pushboolean(L, value);
    lua_setfieldi(L, -2, key);
  }

  /*
  ** Set all fields from structure 'tm' in the table on top of the stack
  */
  private static void setallfields(lua_State L, OsDate stm)
  {
    setfield(L, Lt.year, stm.year, 1900);
    setfield(L, Lt.month, stm.month, 1);
    setfield(L, Lt.day, stm.day, 0);
    setfield(L, Lt.hour, stm.hour, 0);
    setfield(L, Lt.min, stm.min, 0);
    setfield(L, Lt.sec, stm.sec, 0);
    setfield(L, Lt.yday, stm.yday, 1);
    setfield(L, Lt.wday, stm.wday, 1);
    setboolfield(L, Lt.isdst, stm.isdst);
  }

  /*
  ** The value of field 'key' just read (on the top), as an integer for a
  ** date ('d' when absent, -1: no default), checked; it replaces the value.
  */
  private static int checkfield(lua_State L, ByteView key, int d, int delta)
  {
    int t = lua_type(L, -1);
    OptionalLong ores = lua_tointegerx(L, -1);
    long res = ores.orElse(0);
    if (ores.isEmpty())
    {                                /* field is not an integer? */
      if (t != LUA_TNIL)
      { /* some other value? */
        lua_pushlstring(L, key, 0, key.size());
        return luaL_errori(L, Lt.field_s_is_not_an_integer);
      }
      else if (d < 0)
      { /* absent field; no default? */
        lua_pushlstring(L, key, 0, key.size());
        return luaL_errori(L, Lt.field_s_missing_in_date_table);
      }
      res = d;
    }
    else
    {
      if (!(res >= 0 ? res - delta <= Integer.MAX_VALUE : Integer.MIN_VALUE + delta <= res))
      {
        lua_pushlstring(L, key, 0, key.size());
        return luaL_errori(L, Lt.field_s_is_out_of_bound);
      }
      res -= delta;
    }
    lua_pop(L, 1);
    lua_pushinteger(L, res);
    return (int) res;
  }

  private static int checkoption(lua_State L, ByteView fmt, int pos, ByteSpan buff, int boff)
  {
    ByteView option = Lt.strftime_options;
    int opos = 0;
    int convlen = fmt.size() - pos;
    int oplen = 1; /* length of options being checked */
    for (; opos < option.size() && oplen <= convlen; opos += oplen)
    {
      if (option.at(opos) == '|') /* next block? */
        oplen++;               /* will check options with next length (+1) */
      else if (fmt.substr(pos, oplen).equals(option.substr(opos, oplen)))
      {
        buff.copy_from(fmt.substr(pos, oplen), boff);
        buff.set(boff + oplen, '\0');
        return pos + oplen; /* return next item */
      }
    }
    lua_pushlstring(L, fmt, pos, fmt.size() - pos);
    lua_pushfstringi(L, Lt.invalid_conversion_specifier_s);
    luaL_argerrori(L, 1, lua_tolstring(L, -1));
    return pos; /* to avoid warnings */
  }

  private static long l_checktime(lua_State L, int arg)
  {
    return luaL_checkinteger(L, arg);
  }

  /* maximum size for an individual 'strftime' item */
  private static final int SIZETIMEFMT = 250;

  private static int os_date(lua_State L)
  {
    ByteView sfmt = luaL_optlstring(L, 1, Lt.fmt_c);
    int sp = 0;
    long t = (lua_isnoneornil(L, 2) != 0 ? os_now() : l_checktime(L, 2));
    OsDate stm;
    if (sv_byteat(sfmt, sp) == '!')
    { /* UTC? */
      stm = os_gmtime(t);
      sp++; /* skip '!' */
    }
    else
      stm = os_localtime(t);
    if (stm == null) /* invalid date? */
      return luaL_errori(L, Lt.date_result_cannot_be_represented_in_this_installation);
    if (sfmt.size() - sp == 2 && sfmt.at(sp) == '*' && sfmt.at(sp + 1) == 't')
    {
      lua_createtable(L, 0, 9); /* 9 = number of fields */
      setallfields(L, stm);
    }
    else
    {
      ByteOwner cc = new ByteOwner(4); /* buffer for individual conversion specifiers */
      luaL_Buffer b = new luaL_Buffer();
      try
      {
        cc.set(0, '%');
        luaL_buffinit(L, b);
        while (sp < sfmt.size())
        {
          if (sfmt.at(sp) != '%') /* not a conversion specifier? */
            luaL_addchar(b, sfmt.at(sp++));
          else
          {
            int reslen;
            int off = luaL_prepbuffsize(b, SIZETIMEFMT);
            sp++; /* skip '%' */
            sp = checkoption(L, sfmt, sp, cc.span(), 1);
            ByteSpan buff = luaL_buffaddr(b);
            reslen = os_strftime(buff, off, cc.str_view(), stm);
            luaL_addsize(b, reslen);
          }
        }
        luaL_pushresult(b);
      }
      finally
      {
        b.release();
      }
    }
    return 1;
  }

  private static final lua_CFunction os_date = Loslib::os_date;

  /*
  ** 'os.time' with a table, which may call back ('__index', '__newindex'):
  ** the table at 1, then the fields read, as integers (2 to 8), then the
  ** normalized fields to write (2 to 10, with the result at 11, nil if it
  ** cannot be represented). 'step' says where it goes on: reading field
  ** 'step' (0 to 6), or writing field 'step - 7' (0 to 8).
  */
  private static final class TimeField
  {
    ByteView key;
    int d;     /* default (-1: none) */
    int delta; /* (an offset for reads and writes) */

    TimeField(ByteView key, int d, int delta)
    {
      this.key = key;
      this.d = d;
      this.delta = delta;
    }
  }

  private static final TimeField[] timeread = {
    new TimeField(Lt.year, -1, 1900), new TimeField(Lt.month, -1, 1), new TimeField(Lt.day, -1, 0),
    new TimeField(Lt.hour, 12, 0), new TimeField(Lt.min, 0, 0), new TimeField(Lt.sec, 0, 0)};

  private static final ByteView[] timewrite = {
    Lt.year, Lt.month, Lt.day, Lt.hour, Lt.min, Lt.sec, Lt.yday, Lt.wday, Lt.isdst};

  private static final int TIME_NREAD = 7;  /* the six fields, and 'isdst' */
  private static final int TIME_NWRITE = 9;

  /* the field read at 'step' (on the top): check it */
  private static void timeread_done(lua_State L, int step)
  {
    if (step < TIME_NREAD - 1)
      checkfield(L, timeread[step].key, timeread[step].d, timeread[step].delta);
    else
    { /* 'isdst' */
      int res = lua_isnil(L, -1) != 0 ? -1 : lua_toboolean(L, -1);
      lua_pop(L, 1);
      lua_pushinteger(L, res);
    }
  }

  private static int os_time_from(lua_State L, int step)
  {
    for (; step < TIME_NREAD; step++)
    {
      ByteView key = (step < TIME_NREAD - 1) ? timeread[step].key : Lt.isdst;
      lua_getfieldik(L, 1, key, step, os_timek);
      timeread_done(L, step);
    }
    if (step == TIME_NREAD)
    { /* all read: normalize them */
      OsDate ts = new OsDate();
      ts.year = (int) lua_tointeger(L, 2);
      ts.month = (int) lua_tointeger(L, 3);
      ts.day = (int) lua_tointeger(L, 4);
      ts.hour = (int) lua_tointeger(L, 5);
      ts.min = (int) lua_tointeger(L, 6);
      ts.sec = (int) lua_tointeger(L, 7);
      ts.isdst = (int) lua_tointeger(L, 8);
      OptionalLong t = os_mktime(ts);
      lua_settop(L, 1);
      lua_pushinteger(L, (long) ts.year + 1900);
      lua_pushinteger(L, (long) ts.month + 1);
      lua_pushinteger(L, (long) ts.day);
      lua_pushinteger(L, (long) ts.hour);
      lua_pushinteger(L, (long) ts.min);
      lua_pushinteger(L, (long) ts.sec);
      lua_pushinteger(L, (long) ts.yday + 1);
      lua_pushinteger(L, (long) ts.wday + 1);
      lua_pushinteger(L, ts.isdst);
      if (t.isPresent())
        lua_pushinteger(L, t.getAsLong());
      else
        lua_pushnil(L);
    }
    /* update fields with normalized values */
    for (int w = step - TIME_NREAD; w < TIME_NWRITE; w++)
    {
      if (w == TIME_NWRITE - 1)
      { /* 'isdst' */
        int isdst = (int) lua_tointeger(L, 2 + w);
        if (isdst < 0) /* undefined? */
          break;       /* does not set field */
        lua_pushboolean(L, isdst);
      }
      else
        lua_pushvalue(L, 2 + w);
      lua_setfieldik(L, 1, timewrite[w], TIME_NREAD + w + 1, os_timek);
    }
    if (lua_isnil(L, 11) != 0)
      return luaL_errori(L, Lt.time_result_cannot_be_represented_in_this_installation);
    lua_pushvalue(L, 11);
    return 1;
  }

  /* 'os.time' goes on after a field read or written */
  private static int os_timek(lua_State L, int status, long ctx)
  {
    int step = (int) ctx;
    if (step < TIME_NREAD)
      timeread_done(L, step++);
    return os_time_from(L, step);
  }

  private static final lua_KFunction os_timek = Loslib::os_timek;

  private static final KNamer knamer_os_timek = new KNamer("os.os_timek", os_timek);

  private static int os_time(lua_State L)
  {
    if (lua_isnoneornil(L, 1) != 0)
    { /* called without args? */
      lua_pushinteger(L, os_now()); /* get current time */
      return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1); /* make sure table is at the top */
    luaE_freezeonly(L);
    return os_time_from(L, 0);
  }

  private static final lua_CFunction os_time = Loslib::os_time;

  private static int os_difftime(lua_State L)
  {
    long t1 = l_checktime(L, 1);
    long t2 = l_checktime(L, 2);
    lua_pushnumber(L, os_timediff(t1, t2));
    return 1;
  }

  private static final lua_CFunction os_difftime = Loslib::os_difftime;

  /* }====================================================== */

  private static final int LOCALE_NAME_MAXLEN = 8;

  private static int os_setlocale(lua_State L)
  {
    int[] cat = {LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY,
                 LC_NUMERIC, LC_TIME};
    ByteView[] catnames = {
        Lt.all, Lt.collate, Lt.ctype,
        Lt.monetary, Lt.numeric, Lt.time_kw};
    int nol = lua_isnoneornil(L, 1);
    InternalString l = (nol != 0) ? null : luaL_checkistring(L, 1);
    int op = luaL_checkoptioni(L, 2, Lt.all, catnames);
    ByteView res = null;
    if (nol != 0 || l.view().size() <= LOCALE_NAME_MAXLEN)
      res = locale_setlocale(cat[op], (nol != 0) ? null : l.view());
    if (res != null)
    {
      if (nol == 0)
        L.l_G.locale_name = new InternalString(res);
      lua_pushlstring(L, res, 0, res.size());
    }
    else
      lua_pushnil(L);
    return 1;
  }

  private static final lua_CFunction os_setlocale = Loslib::os_setlocale;

  private static int os_exit(lua_State L)
  {
    int status;
    if (lua_isboolean(L, 1) != 0)
      status = (lua_toboolean(L, 1) != 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    else
      status = (int) luaL_optinteger(L, 1, EXIT_SUCCESS);
    if (lua_toboolean(L, 2) != 0)
      lua_close(L);
    if (L != null)
      exit(status); /* 'if' to avoid warnings for unreachable 'return' */
    return 0;
  }

  private static final lua_CFunction os_exit = Loslib::os_exit;

  private static final CFuncInfo cfi_os_clock = new CFuncInfo(os_clock, "os.os_clock");
  private static final CFuncInfo cfi_os_date = new CFuncInfo(os_date, "os.os_date");
  private static final CFuncInfo cfi_os_difftime = new CFuncInfo(os_difftime, "os.os_difftime");
  private static final CFuncInfo cfi_os_execute = new CFuncInfo(os_execute, "os.os_execute");
  private static final CFuncInfo cfi_os_exit = new CFuncInfo(os_exit, "os.os_exit");
  private static final CFuncInfo cfi_os_getenv = new CFuncInfo(os_getenv, "os.os_getenv");
  private static final CFuncInfo cfi_os_remove = new CFuncInfo(os_remove, "os.os_remove");
  private static final CFuncInfo cfi_os_rename = new CFuncInfo(os_rename, "os.os_rename");
  private static final CFuncInfo cfi_os_setlocale = new CFuncInfo(os_setlocale, "os.os_setlocale");
  private static final CFuncInfo cfi_os_time = new CFuncInfo(os_time, "os.os_time");
  private static final CFuncInfo cfi_os_tmpname = new CFuncInfo(os_tmpname, "os.os_tmpname");

  private static final luaL_RegCFI[] syslib = {
    new luaL_RegCFI(Lt.clock, cfi_os_clock),
    new luaL_RegCFI(Lt.date, cfi_os_date),
    new luaL_RegCFI(Lt.difftime, cfi_os_difftime),
    new luaL_RegCFI(Lt.execute, cfi_os_execute),
    new luaL_RegCFI(Lt.exit_kw, cfi_os_exit),
    new luaL_RegCFI(Lt.getenv, cfi_os_getenv),
    new luaL_RegCFI(Lt.remove_kw, cfi_os_remove),
    new luaL_RegCFI(Lt.rename_kw, cfi_os_rename),
    new luaL_RegCFI(Lt.setlocale, cfi_os_setlocale),
    new luaL_RegCFI(Lt.time_kw, cfi_os_time),
    new luaL_RegCFI(Lt.tmpname, cfi_os_tmpname),
  };

  /* }====================================================== */

  public static int luaopen_os(lua_State L)
  {
    luaL_newlibcfi(L, syslib);
    return 1;
  }

  public static final lua_CFunction luaopen_os = Loslib::luaopen_os;
}
