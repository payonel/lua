package lua.api;

import static lua.api.LuaApi.*;
import static lua.api.LuaInternal.*;
import static lua.object.Llimits.*;
import static lua.object.Lnames.*;
import static lua.platform.OsApi.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lmem.*;
import static lua.vm.Ldebug.*;

import java.util.OptionalDouble;
import java.util.OptionalLong;
import java.util.function.BiFunction;

import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.object.CFuncInfo;
import lua.platform.OsExecStatus;
import lua.platform.OsFile;
import lua.platform.OsOpenResult;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class LuaAuxLib
{
  private LuaAuxLib()
  {
  }

  /* global table */
  public static final ByteView LUA_GNAME = ByteView.of("_G");

  /* extra error code for 'luaL_loadfilex' */
  public static final int LUA_ERRFILE = LUA_ERRERR + 1;

  /* key, in the registry, for table of loaded modules */
  public static final ByteView LUA_LOADED_TABLE = ByteView.of("_LOADED");

  /* key, in the registry, for table of preloaded loaders */
  public static final ByteView LUA_PRELOAD_TABLE = ByteView.of("_PRELOAD");

  public static final int LUAL_NUMSIZES = 8 * 16 + 8;

  /* predefined references */
  public static final int LUA_NOREF = -2;
  public static final int LUA_REFNIL = -1;

  public static <R> R luaL_opt(lua_State L, BiFunction<lua_State, Integer, R> f, int n, R d)
  {
    return lua_isnoneornil(L, n) != 0 ? d : f.apply(L, n);
  }

  /*
  ** ===============================================================
  ** some useful macros
  ** ===============================================================
  */

  /*
  ** Perform arithmetic operations on lua_Integer values with wrap-around
  ** semantics, as the Lua core does.
  */

  /*
  ** {======================================================
  ** Generic Buffer manipulation
  ** =======================================================
  */

  public static void luaL_addchar(luaL_Buffer B, byte c)
  {
    if (B.used >= B.bytes.size())
      luaL_prepbuffsize(B, 1);
    B.bytes.set(B.used++, c);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** File handles for IO library
  ** =======================================================
  */

  /*
  ** A file handle is a userdata with metatable 'LUA_FILEHANDLE' and
  ** initial structure 'luaL_Stream' (it may contain other fields
  ** after that initial structure).
  */

  public static final ByteView LUA_FILEHANDLE = ByteView.of("FILE*");

  /* }====================================================== */

  /*
  ** {============================================================
  ** Compatibility with deprecated conversions
  ** =============================================================
  */
  /* }============================================================ */

  /*
  ** This file uses only the official API of Lua.
  ** Any function declared here could be written as an application function.
  */

  /*
  ** {======================================================
  ** Traceback
  ** =======================================================
  */

  private static final int LEVELS1 = 10; /* size of the first part of the stack */
  private static final int LEVELS2 = 11; /* size of the second part of the stack */

  /*
  ** Search for 'objidx' in table at index -1. ('objidx' must be an
  ** absolute index.) Return 1 + string at top if it found a good name.
  */
  private static int findfield(lua_State L, int objidx, int level)
  {
    if (level == 0 || lua_istable(L, -1) == 0)
      return 0;     /* not found */
    lua_pushnil(L); /* start 'next' loop */
    while (lua_next(L, -2) != 0)
    { /* for each pair in table */
      if (lua_type(L, -2) == LUA_TSTRING)
      { /* ignore non-string keys */
        if (lua_rawequal(L, objidx, -1) != 0)
        {                /* found object? */
          lua_pop(L, 1); /* remove value (but keep name) */
          return 1;
        }
        else if (findfield(L, objidx, level - 1) != 0)
        { /* try recursively */
          /* stack: lib_name, lib_table, field_name (top) */
          lua_pushistringref(L, Lt.dot); /* place '.' between the two names */
          lua_replace(L, -3);      /* (in the slot occupied by table) */
          lua_concat(L, 3);        /* lib_name.field_name */
          return 1;
        }
      }
      lua_pop(L, 1); /* remove value */
    }
    return 0; /* not found */
  }

  /*
  ** Search for a name for a function in all loaded modules
  */
  private static int pushglobalfuncname(lua_State L, lua_State L1, InternalDebug ar)
  {
    int top = lua_gettop(L);
    lua_pushframefunction(L, L1, ar); /* (the frame is of 'L1': its stack has the function) */
    lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    luaL_checkstacki(L, 6, Lt.not_enough_stack); /* slots for 'findfield' */
    if (findfield(L, top + 1, 2) != 0)
    {
      ByteView name = lua_tolstring(L, -1);
      if (name.starts_with(Lt._G_dot))
      {                                                    /* name start with '_G.'? */
        lua_pushlstring(L, name, 3, name.size() - 3);      /* push name without prefix */
        lua_remove(L, -2);                                 /* remove original name */
      }
      lua_copy(L, -1, top + 1); /* copy name to proper place */
      lua_settop(L, top + 1);   /* remove table "loaded" and name copy */
      return 1;
    }
    else
    {
      lua_settop(L, top); /* remove function and global table */
      return 0;
    }
  }

  private static void pushfuncname(lua_State L, lua_State L1, InternalDebug ar)
  {
    if (ar.namewhat != null)
    {
      lua_pushlstring(L, ar.namewhat, 0, ar.namewhat.size());
      lua_pushlstring(L, ar.name, 0, ar.name.size());
      lua_pushfstringi(L, Lt.fmt_s_quoted_s); /* use it */
    }
    else if (ar.what.at(0) == 'm')                           /* main? */
      lua_pushistringref(L, Lt.main_chunk);
    else if (pushglobalfuncname(L, L1, ar) != 0)
    {                       /* try a global name */
      lua_pushvalue(L, -1);
      lua_pushfstringi(L, Lt.function_s);
      lua_remove(L, -2); /* remove name */
    }
    else if (ar.what.at(0) != 'C')
    { /* for Lua functions, use <file:line> */
      ByteView ss = ar.short_src_out.str_view();
      lua_pushlstring(L, ss, 0, ss.size());
      lua_pushinteger(L, ar.linedefined);
      lua_pushfstringi(L, Lt.function_s_d);
    }
    else /* nothing left... */
      lua_pushistringref(L, Lt.qmark);
  }

  private static int lastlevel(lua_State L)
  {
    int li = 1, le = 1;
    /* find an upper bound */
    while (lua_getstacki(L, le) != null)
    {
      li = le;
      le *= 2;
    }
    /* do a binary search */
    while (li < le)
    {
      int m = (li + le) / 2;
      if (lua_getstacki(L, m) != null)
        li = m + 1;
      else
        le = m;
    }
    return le - 1;
  }

  public static void luaL_traceback(lua_State L, lua_State L1,
                                    HostString msg, int level)
  {
    luaL_tracebacki(L, L1, (msg != null) ? msg.to_utf8() : null, level);
  }

  public static void luaL_tracebacki(lua_State L, lua_State L1, ByteView msg, int level)
  {
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      InternalDebug oar;
      int last = lastlevel(L1);
      int limit2show = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
      luaL_buffinit(L, b);
      if (msg != null)
      {
        luaL_addlstring(b, msg, 0, msg.size());
        luaL_addchar(b, (byte) '\n');
      }
      luaL_addlstring(b, Lt.stack_traceback, 0, Lt.stack_traceback.size());
      while ((oar = lua_getstacki(L1, level++)) != null)
      {
        if (limit2show-- == 0)
        {                                     /* too many levels? */
          int n = last - level - LEVELS2 + 1; /* number of levels to skip */
          lua_pushinteger(L, n);
          lua_pushfstringi(L, Lt.n_t_t_skipping_d_levels);
          luaL_addvalue(b); /* add warning about skip */
          level += n;        /* and skip to last levels */
        }
        else
        {
          InternalDebug ar = lua_getinfoi(L1, Lt.Slnt_chars, oar);
          ByteView ss = ar.short_src_out.str_view();
          if (ar.currentline <= 0)
          {
            lua_pushlstring(L, ss, 0, ss.size());
            lua_pushfstringi(L, Lt.n_t_s_in);
          }
          else
          {
            lua_pushlstring(L, ss, 0, ss.size());
            lua_pushinteger(L, ar.currentline);
            lua_pushfstringi(L, Lt.n_t_s_d_in);
          }
          luaL_addvalue(b);
          pushfuncname(L, L1, ar);
          luaL_addvalue(b);
          if (ar.istailcall != 0)
            luaL_addlstring(b, Lt.n_t_tail_calls, 0, Lt.n_t_tail_calls.size());
        }
      }
      luaL_pushresult(b);
    }
    finally
    {
      b.release();
    }
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Error-report functions
  ** =======================================================
  */

  public static int luaL_argerror(lua_State L, int arg, HostString extramsg)
  {
    return luaL_argerrori(L, arg, extramsg.to_utf8());
  }

  public static int luaL_argerrori(lua_State L, int arg, ByteView extramsg)
  {
    ByteView argword = Lt.argument;
    InternalDebug oar = lua_getstacki(L, 0);
    if (oar == null)
    { /* no stack frame? */
      lua_pushinteger(L, arg);
      lua_pushlstring(L, extramsg, 0, extramsg.size());
      return luaL_errori(L, Lt.bad_argument_d_s);
    }
    InternalDebug ar = lua_getinfoi(L, Lt.nt, oar);
    if (arg <= ar.extraargs) /* error in an extra argument? */
      argword = Lt.extra_argument;
    else
    {
      arg -= ar.extraargs; /* do not count extra arguments */
      if (Lt.method.equals(ar.namewhat))
      {               /* colon syntax? */
        arg--;        /* do not count (extra) self argument */
        if (arg == 0)
        { /* error in self argument? */
          lua_pushlstring(L, ar.name, 0, ar.name.size());
          lua_pushlstring(L, extramsg, 0, extramsg.size());
          return luaL_errori(L, Lt.calling_s_on_bad_self_s);
        }
        /* else go through; error in a regular argument */
      }
    }
    if (ar.name == null)
      ar.name = (pushglobalfuncname(L, L, ar) != 0) ? lua_tolstring(L, -1) : Lt.qmark;
    lua_pushistringref(L, argword);
    lua_pushinteger(L, arg);
    lua_pushlstring(L, ar.name, 0, ar.name.size());
    lua_pushlstring(L, extramsg, 0, extramsg.size());
    return luaL_errori(L, Lt.bad_s_d_to_s_s);
  }

  public static int luaL_typeerror(lua_State L, int arg, HostString tname)
  {
    return luaL_typeerrori(L, arg, tname.to_utf8());
  }

  public static int luaL_typeerrori(lua_State L, int arg, ByteView tname)
  {
    ByteView typearg; /* name for the type of the actual argument */
    if (luaL_getmetafieldi(L, arg, Lt.mm_name) == LUA_TSTRING)
      typearg = lua_toistring(L, -1).view(); /* use the given type name */
    else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
      typearg = Lt.light_userdata; /* special name for messages */
    else
      typearg = lua_typenamei(L, lua_type(L, arg)); /* standard name */
    lua_pushlstring(L, tname, 0, tname.size());
    lua_pushlstring(L, typearg, 0, typearg.size());
    InternalString msg = lua_pushfstringi(L, Lt.s_expected_got_s);
    return luaL_argerrori(L, arg, msg.view());
  }

  private static void tag_error(lua_State L, int arg, int tag)
  {
    luaL_typeerrori(L, arg, lua_typenamei(L, tag));
  }

  /*
  ** The use of 'lua_pushfstring' ensures this function does not
  ** need reserved stack space when called.
  */

  public static void luaL_where(lua_State L, int level)
  {
    InternalDebug oar = lua_getstacki(L, level);
    if (oar != null)
    {                                                /* check function at level */
      InternalDebug ar = lua_getinfoi(L, Lt.Sl_chars, oar); /* get info about it */
      if (ar.currentline > 0)
      { /* is there info? */
        ByteView ss = ar.short_src_out.str_view();
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushinteger(L, ar.currentline);
        lua_pushfstringi(L, Lt.fmt_s_d_colon_sp);
        return;
      }
    }
    lua_pushfstringi(L, Lt.empty_str); /* else, no information available... */
  }

  public static int luaL_error(lua_State L, HostString fmt)
  {
    return luaL_errori(L, fmt.to_utf8());
  }

  public static int luaL_errori(lua_State L, ByteView fmt)
  {
    lua_pushfstringi(L, fmt);
    luaL_where(L, 1);
    lua_rotate(L, -2, 1);
    lua_concat(L, 2);
    return lua_error(L);
  }

  public static int luaL_fileresulti(lua_State L, int stat, ByteView fname, int en)
  {
    if (stat != 0)
    {
      lua_pushboolean(L, 1);
      return 1;
    }
    else
    {
      luaL_pushfail(L);
      ByteView errstr = (en != 0) ? os_strerror(en) : Lt.no_extra_info;
      if (fname != null)
      {
        lua_pushlstring(L, fname, 0, fname.size());
        lua_pushlstring(L, errstr, 0, errstr.size());
        lua_pushfstringi(L, Lt.fmt_s_colon_s);
      }
      else
        lua_pushlstring(L, errstr, 0, errstr.size());
      lua_pushinteger(L, en);
      return 3;
    }
  }

  public static int luaL_fileresult(lua_State L, int stat, HostString fname)
  {
    return luaL_fileresulti(L, stat, (fname != null) ? fname.to_utf8() : null, errno);
  }

  public static int luaL_execresult(lua_State L, int stat)
  {
    return luaL_execresulti(L, stat, errno);
  }

  public static int luaL_execresulti(lua_State L, int stat, int en)
  {
    if (stat != 0 && en != 0) /* error with an 'errno'? */
      return luaL_fileresulti(L, 0, null, en);
    else
    {
      OsExecStatus es = os_decode_exec(stat);
      ByteView what = es.exited ? Lt.exit_kw : Lt.signal_kw;
      if (es.exited && es.code == 0) /* successful termination? */
        lua_pushboolean(L, 1);
      else
        luaL_pushfail(L);
      lua_pushistringref(L, what);
      lua_pushinteger(L, es.code);
      return 3; /* return true/fail,what,code */
    }
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Userdata's metatable manipulation
  ** =======================================================
  */

  public static int luaL_newmetatable(lua_State L, HostString tname)
  {
    return luaL_newmetatablei(L, tname.to_utf8());
  }

  public static int luaL_newmetatablei(lua_State L, ByteView tname)
  {
    if (luaL_getmetatablei(L, tname) != LUA_TNIL) /* name already in use? */
      return 0;                                  /* leave previous value on top, but return 0 */
    lua_pop(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushlstring(L, tname, 0, tname.size());
    lua_setfieldi(L, -2, Lt.mm_name); /* metatable.__name = tname */
    lua_pushvalue(L, -1);
    lua_setfieldi(L, LUA_REGISTRYINDEX, tname); /* registry.name = metatable */
    return 1;
  }

  public static void luaL_setmetatable(lua_State L, HostString tname)
  {
    luaL_setmetatablei(L, tname.to_utf8());
  }

  public static void luaL_setmetatablei(lua_State L, ByteView tname)
  {
    luaL_getmetatablei(L, tname);
    lua_setmetatable(L, -2);
  }

  public static Object luaL_testudata(lua_State L, int ud, HostString tname)
  {
    return luaL_testudatai(L, ud, tname.to_utf8());
  }

  public static Object luaL_testudatai(lua_State L, int ud, ByteView tname)
  {
    Object p = lua_touserdata(L, ud);
    if (p != null)
    { /* value is a userdata? */
      if (lua_getmetatable(L, ud) != 0)
      {                               /* does it have a metatable? */
        luaL_getmetatablei(L, tname);  /* get correct metatable */
        if (lua_rawequal(L, -1, -2) == 0) /* not the same? */
          p = null;                   /* value is a userdata with wrong metatable */
        lua_pop(L, 2);                /* remove both metatables */
        return p;
      }
    }
    return null; /* value is not a userdata with a metatable */
  }

  public static Object luaL_checkudata(lua_State L, int ud, HostString tname)
  {
    return luaL_checkudatai(L, ud, tname.to_utf8());
  }

  public static Object luaL_checkudatai(lua_State L, int ud, ByteView tname)
  {
    Object p = luaL_testudatai(L, ud, tname);
    luaL_argexpectedi(L, p != null, ud, tname);
    return p;
  }

  public static lua_UserObject luaL_testuserobject(lua_State L, int ud, HostString tname)
  {
    return luaL_testuserobjecti(L, ud, tname.to_utf8());
  }

  public static lua_UserObject luaL_testuserobjecti(lua_State L, int ud, ByteView tname)
  {
    if (luaL_testudatai(L, ud, tname) == null)
      return null;
    return lua_touserobject(L, ud);
  }

  public static lua_UserObject luaL_checkuserobject(lua_State L, int ud, HostString tname)
  {
    return luaL_checkuserobjecti(L, ud, tname.to_utf8());
  }

  public static lua_UserObject luaL_checkuserobjecti(lua_State L, int ud, ByteView tname)
  {
    lua_UserObject p = luaL_testuserobjecti(L, ud, tname);
    luaL_argexpectedi(L, p != null, ud, tname);
    return p;
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Argument check functions
  ** =======================================================
  */

  public static int luaL_checkoptioni(lua_State L, int arg,
                                      ByteView def,
                                      ByteView[] lst)
  {
    ByteView name = (def != null)
                        ? luaL_optlstring(L, arg, def)
                        : luaL_checklstring(L, arg);
    for (int i = 0; i < lst.length; i++)
      if (name.equals(lst[i]))
        return i;
    lua_pushlstring(L, name, 0, name.size());
    InternalString msg = lua_pushfstringi(L, Lt.invalid_option_s);
    return luaL_argerrori(L, arg, msg.view());
  }

  public static int luaL_checkoption(lua_State L, int arg,
                                     HostString def,
                                     HostString[] lst)
  {
    ByteView name = (def != null)
                        ? luaL_optlstring(L, arg, def.to_utf8())
                        : luaL_checklstring(L, arg);
    for (int i = 0; i < lst.length; i++)
      if (name.equals(lst[i].to_utf8()))
        return i;
    lua_pushlstring(L, name, 0, name.size());
    InternalString msg = lua_pushfstringi(L, Lt.invalid_option_s);
    return luaL_argerrori(L, arg, msg.view());
  }

  /*
  ** Ensures the stack has at least 'space' extra slots, raising an error
  ** if it cannot fulfill the request. (The error handling needs a few
  ** extra slots to format the error message. In case of an error without
  ** this extra space, Lua will generate the same 'stack overflow' error,
  ** but without 'msg'.)
  */

  public static void luaL_checkstack(lua_State L, int space, HostString msg)
  {
    luaL_checkstacki(L, space, (msg != null) ? msg.to_utf8() : null);
  }

  public static void luaL_checkstacki(lua_State L, int space, ByteView msg)
  {
    if (lua_checkstack(L, space) == 0)
    {
      if (msg != null && lua_checkstack(L, 1) != 0)
      {
        lua_pushlstring(L, msg, 0, msg.size());
        luaL_errori(L, Lt.stack_overflow_s);
      }
      else
        luaL_errori(L, Lt.stack_overflow);
    }
  }

  public static void luaL_checktype(lua_State L, int arg, int t)
  {
    if (lua_type(L, arg) != t)
      tag_error(L, arg, t);
  }

  public static void luaL_checkany(lua_State L, int arg)
  {
    if (lua_type(L, arg) == LUA_TNONE)
      luaL_argerrori(L, arg, Lt.value_expected);
  }

  public static ByteView luaL_checklstring(lua_State L, int arg)
  {
    ByteView s = lua_tolstring(L, arg);
    if (s == null)
      tag_error(L, arg, LUA_TSTRING);
    return s;
  }

  public static ByteView luaL_optlstring(
      lua_State L, int arg, ByteView def)
  {
    if (lua_isnoneornil(L, arg) != 0)
      return def;
    else
      return luaL_checklstring(L, arg);
  }

  public static InternalString luaL_checkistring(lua_State L, int arg)
  {
    InternalString s = lua_toistring(L, arg);
    if (s == null)
      tag_error(L, arg, LUA_TSTRING);
    return s;
  }

  public static InternalString luaL_optistring(lua_State L, int arg, InternalString def)
  {
    if (lua_isnoneornil(L, arg) != 0)
      return def;
    else
      return luaL_checkistring(L, arg);
  }

  public static double luaL_checknumber(lua_State L, int arg)
  {
    OptionalDouble d = lua_tonumberx(L, arg);
    if (d.isEmpty())
      tag_error(L, arg, LUA_TNUMBER);
    return d.getAsDouble();
  }

  public static double luaL_optnumber(lua_State L, int arg, double def)
  {
    return (lua_isnoneornil(L, arg) != 0 ? def : luaL_checknumber(L, arg));
  }

  private static void interror(lua_State L, int arg)
  {
    if (lua_isnumber(L, arg) != 0)
      luaL_argerrori(L, arg, Lt.number_has_no_integer_representation);
    else
      tag_error(L, arg, LUA_TNUMBER);
  }

  public static long luaL_checkinteger(lua_State L, int arg)
  {
    OptionalLong d = lua_tointegerx(L, arg);
    if (d.isEmpty())
    {
      interror(L, arg);
    }
    return d.getAsLong();
  }

  public static long luaL_optinteger(lua_State L, int arg,
                                     long def)
  {
    return (lua_isnoneornil(L, arg) != 0 ? def : luaL_checkinteger(L, arg));
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Generic Buffer manipulation
  ** =======================================================
  */

  private static void bb_growTo(luaL_Buffer B, long cap)
  {
    if (cap == B.bytes.size())
      return;
    assert B.L != null;
    global_State g = B.L.l_G;
    ByteOwner nb = luaM_alloc_char_span(g, null, B.bytes, cap, false);
    if (!nb.held() && cap > 0)
    {
      lua_pushistringref(B.L, Lt.not_enough_memory);
      lua_error(B.L); /* raise a memory error */
    }
    B.bytes = ByteOwner.move(nb);
  }

  private static void bb_append(luaL_Buffer B, ByteView b)
  {
    if (!b.empty())
    {
      if (b.size() > B.bytes.size() - B.used)
        bb_growTo(B, (long) B.used + b.size());
      B.bytes.copy_from(b, B.used);
      B.used += b.size();
    }
  }

  static void bb_dealloc(luaL_Buffer B)
  {
    if (!B.bytes.empty())
    {
      global_State g = B.L.l_G;
      luaM_alloc_char_span(g, null, B.bytes, 0, false);
      B.bytes = new ByteOwner();
    }
  }

  private static long newbuffsize(luaL_Buffer B, long sz)
  {
    long newsize = B.bytes.size();
    int len = B.used;
    if (Long.compareUnsigned(sz, MAX_SIZE - len) >= 0)
      return luaL_errori(B.L, Lt.resulting_string_too_large);
    if (newsize <= MAX_SIZE / 3 * 2) /* no overflow? */
      newsize += newsize >> 1;     /* new size *= 1.5 */
    if (newsize < len + sz)          /* not big enough? */
      newsize = len + sz;
    if (newsize < LUAL_BUFFERSIZE)
      newsize = LUAL_BUFFERSIZE;
    return newsize;
  }

  private static int prepbuff(luaL_Buffer B, long sz)
  {
    if (Long.compareUnsigned(sz, B.bytes.size() - B.used) > 0)
    {
      long newsize = newbuffsize(B, sz);
      bb_growTo(B, newsize);
    }

    assert sz <= B.bytes.size() - B.used;
    return B.used;
  }

  public static int luaL_prepbuffsize(luaL_Buffer B, long sz)
  {
    return prepbuff(B, sz);
  }

  public static void luaL_addlstring(luaL_Buffer B, ByteView s, int pos, int l)
  {
    if (l > 0)
    {
      prepbuff(B, l);
      bb_append(B, s.substr(pos, l));
    }
  }

  public static void luaL_addstring(luaL_Buffer B, HostString s)
  {
    ByteView sv = s.to_utf8();
    luaL_addlstring(B, sv, 0, sv.size());
  }

  /* the buffer's bytes become the string's: they stop counting as the buffer's */
  public static void luaL_pushresult(luaL_Buffer B)
  {
    lua_State L = B.L;
    int len = B.used;
    ByteOwner block = ByteOwner.move(B.bytes);
    B.bytes = new ByteOwner();
    B.used = 0;
    if (!block.empty())
      luaM_alloc_char_span(L.l_G, null, block, 0, false);
    lua_pushiblock(L, ByteOwner.move(block), len);
  }

  public static void luaL_pushresultsize(luaL_Buffer B, int sz)
  {
    luaL_addsize(B, sz);
    luaL_pushresult(B);
  }

  public static void luaL_addvalue(luaL_Buffer B)
  {
    lua_State L = B.L;
    ByteView s = lua_tolstring(L, -1);
    prepbuff(B, s.size());
    bb_append(B, s);
    lua_pop(L, 1); /* pop string */
  }

  private static lua_UserObject makebox(lua_State L, long[] ints, int nints, ByteView bytes)
  {
    if (nints != 5 || ints[4] < 0 || ints[4] != bytes.size())
      return null;
    luaL_Box box = new luaL_Box();
    for (int i = 0; i < 4; i++)
      box.ints[i] = ints[i];
    if (!bytes.empty())
    {
      ByteOwner nb = luaM_alloc_char_span(L.l_G, null, new ByteSpan(), bytes.size(), false);
      if (!nb.held())
      {
        lua_pushistringref(L, Lt.not_enough_memory);
        lua_error(L); /* raise a memory error */
      }
      nb.copy_from(bytes, 0);
      box.bytes = ByteOwner.move(nb);
      box.used = bytes.size();
    }
    return box;
  }

  private static final lua_ObjectFactory makebox = LuaAuxLib::makebox;

  private static final class BoxKind
  {
    BoxKind()
    {
      lua_registerobjectkind("box", makebox);
    }
  }

  private static final BoxKind boxkind = new BoxKind();

  /* what a box is counted, besides the bytes it keeps */
  private static final long LUAL_BOX_SIZE = 72;

  /* pushes a new box */
  public static luaL_Box luaL_newbox(lua_State L)
  {
    luaL_Box box = new luaL_Box();
    luaL_Box p = box;
    lua_newuserobject(L, box, LUAL_BOX_SIZE, 0);
    return p;
  }

  public static luaL_Box luaL_tobox(lua_State L, int idx)
  {
    lua_UserObject o = lua_touserobject(L, idx);
    luaL_Box box = (o instanceof luaL_Box) ? (luaL_Box) o : null;
    assert box != null;
    return box;
  }

  /* moves the contents of 'B' into 'box' ('B' is left empty) */
  public static void luaL_buffsave(luaL_Buffer B, luaL_Box box)
  {
    assert box.bytes.empty();
    box.bytes = ByteOwner.move(B.bytes);
    box.used = B.used;
    B.bytes = new ByteOwner();
    B.used = 0;
  }

  /* moves the contents saved in 'box' back into 'B' */
  public static void luaL_buffload(luaL_Buffer B, luaL_Box box)
  {
    assert B.bytes.empty();
    B.bytes = ByteOwner.move(box.bytes);
    B.used = box.used;
    box.bytes = new ByteOwner();
    box.used = 0;
  }

  public static void luaL_buffinit(lua_State L, luaL_Buffer B)
  {
    assert B.bytes.empty() || L == B.L;
    B.L = L;
    B.used = 0;
  }

  public static int luaL_buffinitsize(lua_State L, luaL_Buffer B, long sz)
  {
    luaL_buffinit(L, B);
    return prepbuff(B, sz);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Reference system
  ** =======================================================
  */

  /*
  ** The previously freed references form a linked list: t[1] is the index
  ** of a first free index, t[t[1]] is the index of the second element,
  ** etc. A zero signals the end of the list.
  */

  public static int luaL_ref(lua_State L, int t)
  {
    int ref;
    if (lua_isnil(L, -1) != 0)
    {
      lua_pop(L, 1);     /* remove from stack */
      return LUA_REFNIL; /* 'nil' has a unique fixed reference */
    }
    t = lua_absindex(L, t);
    if (lua_rawgeti(L, t, 1) == LUA_TNUMBER) /* already initialized? */
      ref = (int) lua_tointeger(L, -1);      /* ref = t[1] */
    else
    {                                    /* first access */
      assert lua_toboolean(L, -1) == 0; /* must be nil or false */
      ref = 0;                           /* list is empty */
      lua_pushinteger(L, 0);             /* initialize as an empty list */
      lua_rawseti(L, t, 1);              /* ref = t[1] = 0 */
    }
    lua_pop(L, 1); /* remove element from stack */
    if (ref != 0)
    {                         /* any free element? */
      lua_rawgeti(L, t, ref); /* remove it from list */
      lua_rawseti(L, t, 1);   /* (t[1] = t[ref]) */
    }
    else                                /* no free elements */
      ref = (int) lua_rawlen(L, t) + 1; /* get a new reference */
    lua_rawseti(L, t, ref);
    return ref;
  }

  public static void luaL_unref(lua_State L, int t, int ref)
  {
    if (ref >= 0)
    {
      t = lua_absindex(L, t);
      lua_rawgeti(L, t, 1);
      assert lua_isinteger(L, -1) != 0;
      lua_rawseti(L, t, ref); /* t[ref] = t[1] */
      lua_pushinteger(L, ref);
      lua_rawseti(L, t, 1); /* t[1] = ref */
    }
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Load functions
  ** =======================================================
  */

  private static final class LoadF implements lua_Reader
  {
    int n;        /* number of pre-read characters */
    OsFile f;         /* file being read */
    ByteOwner buff = new ByteOwner(BUFSIZ); /* area for reading file */

    @Override
    public ByteView read(lua_State L)
    {
      return getF(L, this);
    }
  }

  private static ByteView getF(lua_State L, LoadF lf)
  {
    if (lf.n > 0)
    {                          /* are there pre-read characters to be read? */
      int len = lf.n;
      lf.n = 0;               /* no more pre-read characters */
      return lf.buff.view().substr(0, len); /* return them (chars already in buffer) */
    }

    /* read a block from file */
    /* 'fread' can return > 0 *and* set the EOF flag. If next call to
        'getF' called 'fread', it might still wait for user input.
        The next check avoids this problem. */
    if (os_feof(lf.f))
      return null;
    int n = os_fread(lf.f, lf.buff.span(), 0, lf.buff.size()); /* read block */
    if (n == 0)
      return null;

    return lf.buff.view().substr(0, n);
  }

  private static int errfile(lua_State L, ByteView what, int fnameindex, int err)
  {
    ByteView filename = lua_tolstring(L, fnameindex).substr(1);
    if (err != 0)
    {
      lua_pushlstring(L, what, 0, what.size());
      lua_pushlstring(L, filename, 0, filename.size());
      ByteView errmsg = os_strerror(err);
      lua_pushlstring(L, errmsg, 0, errmsg.size());
      lua_pushfstringi(L, Lt.cannot_s_s_s);
    }
    else
    {
      lua_pushlstring(L, what, 0, what.size());
      lua_pushlstring(L, filename, 0, filename.size());
      lua_pushfstringi(L, Lt.cannot_s_s);
    }
    lua_remove(L, fnameindex);
    return LUA_ERRFILE;
  }

  /*
  ** Skip an optional BOM at the start of a stream. If there is an
  ** incomplete BOM (the first character is correct but the rest is
  ** not), returns the first character anyway to force an error
  ** (as no chunk can start with 0xEF).
  */
  private static int skipBOM(OsFile f)
  {
    int c = os_getbyte(f);                                     /* read first character */
    if (c == 0xEF && os_getbyte(f) == 0xBB && os_getbyte(f) == 0xBF) /* correct BOM? */
      return os_getbyte(f);                                    /* ignore BOM and return next char */
    else                                                 /* no (valid) BOM */
      return c;                                          /* return first character */
  }

  /*
  ** reads the first character of file 'f' and skips an optional BOM mark
  ** in its beginning plus its first line if it starts with '#'. Returns
  ** true if it skipped the first line.  In any case, '*cp' has the
  ** first "valid" character of the file (after the optional BOM and
  ** a first-line comment).
  */
  private static final class FirstChar
  {
    int skipped;
    int c;

    FirstChar(int skipped, int c)
    {
      this.skipped = skipped;
      this.c = c;
    }
  }

  private static FirstChar skipcomment(OsFile f)
  {
    int c = skipBOM(f);
    if (c == '#')
    { /* first line is a comment (Unix exec. file)? */
      do
      { /* skip first line */
        c = os_getbyte(f);
      } while (c != OS_EOF && c != '\n');
      return new FirstChar(1, os_getbyte(f)); /* there was a comment */
    }
    else
      return new FirstChar(0, c); /* no comment */
  }

  /*
  ** A file load in a C function with continuation 'k' and 'ctx' ('loadfile',
  ** 'dofile' and 'require'): with a budget, a file named is read whole first
  ** (see 'lua_loadkread'), and its parse has check points, where a freeze
  ** stops it; it goes on in 'k', which ends it with 'luaL_loadfileikresume'
  ** and 'base', the slot after the stack the load began with (its chunk name
  ** is there while it loads, its box above). It ends as 'luaL_loadfilei':
  ** the function, or the error message, at 'base'. (No continuation, or
  ** stdin: the file is read as the parse goes, in one step.)
  */
  private static lua_LoadSpec filespec(lua_State L, ByteView mode, int base, long ctx,
                                       lua_KFunction k)
  {
    lua_LoadSpec ls = new lua_LoadSpec();
    ls.readeridx = LUA_LOADBOXED;
    ls.pieceidx = 0;
    ls.boxidx = base + 1;
    ls.name = lua_toistring(L, base).view();
    ls.mode = mode;
    ls.ctx = ctx;
    ls.k = k;
    return ls;
  }

  private static int loadfileend(lua_State L, int status, int base)
  {
    lua_replace(L, base); /* the function or the message */
    lua_settop(L, base);
    return status;
  }

  public static int luaL_loadfileikresume(lua_State L, int status, ByteView mode, int base, long ctx,
                                          lua_KFunction k)
  {
    return loadfileend(L, lua_loadkresume(L, status, filespec(L, mode, base, ctx, k)), base);
  }

  public static int luaL_loadfilei(lua_State L, ByteView filename, ByteView mode)
  {
    return luaL_loadfileik(L, filename, mode, 0, null);
  }

  /* 'luaL_loadfilei' in a C function with continuation 'k' (see lauxlib.cpp) */
  public static int luaL_loadfileik(lua_State L, ByteView filename, ByteView mode, long ctx,
                                    lua_KFunction k)
  {
    LoadF lf = new LoadF();
    int status, readstatus;
    int c;
    int fnameindex = lua_gettop(L) + 1; /* index of filename on the stack */
    final boolean bNullname = filename == null;
    if (bNullname)
    {
      lua_pushistringref(L, Lt.stdin_1);
      lf.f = os_stdin();
    }
    else
    {
      lua_pushistringref(L, filename);
      lua_pushfstringi(L, Lt.fmt_at_s);
      OsOpenResult o = os_fopen(filename, Lt.r_mode);
      lf.f = o.f;
      if (lf.f == null)
        return errfile(L, Lt.open_kw, fnameindex, o.err);
    }
    lf.n = 0;
    FirstChar first = skipcomment(lf.f); /* read initial portion */
    c = first.c;
    if (first.skipped != 0)
      lf.buff.set(lf.n++, '\n');  /* add newline to correct line numbers */
    if (c == LUA_SIGNATURE.at(0))
    {           /* binary file? */
      lf.n = 0; /* remove possible newline */
      if (!bNullname)
      { /* "real" file? */
        int rerr = os_freopen(lf.f, filename, Lt.rb_mode); /* reopen in binary mode */
        if (rerr != 0)
          return errfile(L, Lt.reopen, fnameindex, rerr);
        c = skipcomment(lf.f).c; /* re-read initial portion */
      }
    }
    if (c != OS_EOF)
      lf.buff.set(lf.n++, (byte) c); /* 'c' is the first character */
    if (k != null && !bNullname)
    {
      lua_pushnil(L); /* its box */
      lua_LoadSpec ls = filespec(L, mode, fnameindex, ctx, k);
      int boxed;
      try
      {
        boxed = lua_loadkread(L, ls, lf);
      }
      catch (Throwable e)
      {
        os_fclose(lf.f);
        throw e;
      }
      if (boxed != 0)
      {
        readstatus = os_ferror(lf.f) ? 1 : 0;
        os_fclose(lf.f);
        if (readstatus != 0)
        {
          lua_settop(L, fnameindex);
          return errfile(L, Lt.read_kw, fnameindex, 0);
        }
        return loadfileend(L, lua_loadk(L, ls), fnameindex);
      }
      lua_pop(L, 1); /* (no budget) */
    }
    status = lua_loadi(L, lf, lua_toistring(L, -1).view(), mode);
    readstatus = os_ferror(lf.f) ? 1 : 0;
    if (!bNullname)
      os_fclose(lf.f); /* close file (even in case of errors) */
    if (readstatus != 0)
    {
      lua_settop(L, fnameindex); /* ignore results from 'lua_load' */
      return errfile(L, Lt.read_kw, fnameindex, 0);
    }
    lua_remove(L, fnameindex);
    return status;
  }

  public static int luaL_loadfilex(lua_State L, HostString filename, HostString mode)
  {
    ByteView fn = (filename != null) ? filename.to_utf8() : null;
    ByteView m = (mode != null) ? mode.to_utf8() : null;
    return luaL_loadfilei(L, fn, m);
  }

  private static final class LoadS implements lua_Reader
  {
    ByteView chunk = Lt.empty_str;

    @Override
    public ByteView read(lua_State L)
    {
      return getS(L, this);
    }
  }

  private static ByteView getS(lua_State L, LoadS ls)
  {
    if (ls.chunk.empty())
      return null;
    ByteView chunk = ls.chunk;
    ls.chunk = Lt.empty_str;
    return chunk;
  }

  public static int luaL_loadbufferi(lua_State L, ByteView buff, ByteView name, ByteView mode)
  {
    LoadS ls = new LoadS();
    ls.chunk = buff;
    return lua_loadi(L, ls, name, mode);
  }

  public static int luaL_loadbufferx(lua_State L, ByteView buff, HostString name, HostString mode)
  {
    return luaL_loadbufferi(L, buff,
                            (name != null) ? name.to_utf8() : null,
                            (mode != null) ? mode.to_utf8() : null);
  }

  public static int luaL_loadstring(lua_State L, HostString s)
  {
    return luaL_loadbufferi(L, s.to_utf8(), s.to_utf8(), Lt.t_char);
  }

  /* }====================================================== */

  public static int luaL_getmetafield(lua_State L, int obj, HostString event)
  {
    return luaL_getmetafieldi(L, obj, event.to_utf8());
  }

  public static int luaL_getmetafieldi(lua_State L, int obj, ByteView event)
  {
    if (lua_getmetatable(L, obj) == 0) /* no metatable? */
      return LUA_TNIL;
    else
    {
      int tt;
      lua_pushlstring(L, event, 0, event.size());
      tt = lua_rawget(L, -2);
      if (tt == LUA_TNIL) /* is metafield nil? */
        lua_pop(L, 2);    /* remove metatable and metafield */
      else
        lua_remove(L, -2); /* remove only metatable */
      return tt;           /* return metafield type */
    }
  }

  public static int luaL_callmeta(lua_State L, int obj, HostString event)
  {
    return luaL_callmetai(L, obj, event.to_utf8());
  }

  public static int luaL_callmetai(lua_State L, int obj, ByteView event)
  {
    obj = lua_absindex(L, obj);
    if (luaL_getmetafieldi(L, obj, event) == LUA_TNIL) /* no metafield? */
      return 0;
    lua_pushvalue(L, obj);
    lua_call(L, 1, 1);
    return 1;
  }

  public static long luaL_len(lua_State L, int idx)
  {
    lua_len(L, idx);
    return luaL_lenresult(L);
  }

  /* 'luaL_len' with '__len' called with continuation 'k' */
  public static long luaL_lenk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    lua_lenk(L, idx, ctx, k);
    return luaL_lenresult(L);
  }

  /* the length 'lua_len' pushed, which must be an integer (popped) */
  public static long luaL_lenresult(lua_State L)
  {
    OptionalLong l = lua_tointegerx(L, -1);
    if (l.isEmpty())
      luaL_errori(L, Lt.object_length_is_not_an_integer);
    lua_pop(L, 1); /* remove object */
    return l.getAsLong();
  }

  /*
  ** Pushes the string form of the value at 'idx': the result of its
  ** '__tostring', called with continuation 'k' (unchecked: see
  ** 'luaL_tostringresult'), or the default form. Returns whether it called
  ** '__tostring'.
  */
  public static int luaL_toistringk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    idx = lua_absindex(L, idx);
    if (luaL_getmetafieldi(L, idx, Lt.mm_tostring) != LUA_TNIL)
    { /* metafield? */
      lua_pushvalue(L, idx);
      lua_callk(L, 1, 1, ctx, k);
      return 1;
    }
    luaL_tostringdefault(L, idx);
    return 0;
  }

  /* pushes the string form of the value at 'idx' that does not use '__tostring' */
  public static void luaL_tostringdefault(lua_State L, int idx)
  {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx))
    {
    case LUA_TNUMBER:
    {
      ByteOwner buff = new ByteOwner(LUA_N2SBUFFSZ);
      ByteView n = lua_numbertocstringi(L, idx, buff.span());
      lua_pushlstring(L, n, 0, n.size());
      break;
    }
    case LUA_TSTRING:
      lua_pushvalue(L, idx);
      break;
    case LUA_TBOOLEAN:
      lua_pushistringref(L, lua_toboolean(L, idx) != 0 ? Lt.true_kw : Lt.false_kw);
      break;
    case LUA_TNIL:
      lua_pushistringref(L, Lt.nil);
      break;
    default:
    {
      int tt = luaL_getmetafieldi(L, idx, Lt.mm_name); /* try name */
      ByteView kind = (tt == LUA_TSTRING)
                          ? lua_tolstring(L, -1)
                          : lua_typenamei(L, lua_type(L, idx));
      if (lua_islightcfunction(L, idx) != 0)
      {
        InternalString fa = lua_funcaddressi(L, idx);
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushlstring(L, fa.view(), 0, fa.size());
        lua_pushfstringi(L, Lt.fmt_s_colon_s);
      }
      else
      {
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushvalue(L, idx);
        lua_pushfstringi(L, Lt.fmt_s_colon_p);
      }
      if (tt != LUA_TNIL)
        lua_remove(L, -2); /* remove '__name' */
      break;
    }
    }
  }

  /* the string 'luaL_toistringk' pushed, which must be a string */
  public static InternalString luaL_tostringresult(lua_State L)
  {
    if (lua_isstring(L, -1) == 0)
      luaL_errori(L, Lt.tostring_must_return_a_string);
    return lua_toistring(L, -1);
  }

  public static InternalString luaL_toistring(lua_State L, int idx)
  {
    luaL_toistringk(L, idx, 0, null);
    return luaL_tostringresult(L);
  }

  public static ByteView luaL_tolstring(lua_State L, int idx)
  {
    return luaL_toistring(L, idx).view();
  }

  /*
  ** set functions from list 'l' into table at top - 'nup'; each
  ** function gets the 'nup' elements at the top as upvalues.
  ** Returns with only the table at the stack.
  */

  public static void luaL_setfuncsi(lua_State L, luaL_Regi[] funcs, int nup)
  {
    luaL_checkstacki(L, nup, Lt.too_many_upvalues);
    for (luaL_Regi reg : funcs)
    {
      if (reg.func == null) /* placeholder? */
        lua_pushboolean(L, 0);
      else
      {
        for (int i = 0; i < nup; i++) /* copy upvalues to the top */
          lua_pushvalue(L, -nup);
        lua_pushcclosure(L, reg.func, nup); /* closure with those upvalues */
      }
      lua_setfieldi(L, -(nup + 2), reg.name);
    }
    lua_pop(L, nup); /* remove upvalues */
  }

  public static void luaL_newlibi(lua_State L, luaL_Regi[] funcs)
  {
    luaL_checkversion(L);
    lua_createtable(L, 0, funcs.length);
    luaL_setfuncsi(L, funcs, 0);
  }

  public static void luaL_setfuncscfi(lua_State L, luaL_RegCFI[] funcs)
  {
    for (luaL_RegCFI reg : funcs)
    {
      if (reg.cfi == null)
        lua_pushboolean(L, 0);
      else
        lua_pushcfuncinfo(L, reg.cfi);
      lua_setfieldi(L, -2, reg.name);
    }
  }

  public static void luaL_newlibcfi(lua_State L, luaL_RegCFI[] funcs)
  {
    luaL_checkversion(L);
    lua_createtable(L, 0, funcs.length);
    luaL_setfuncscfi(L, funcs);
  }

  public static void luaL_setfuncs(lua_State L, luaL_Reg[] funcs, int nup)
  {
    luaL_checkstacki(L, nup, Lt.too_many_upvalues);
    for (luaL_Reg reg : funcs)
    {
      if (reg.func == null) /* placeholder? */
        lua_pushboolean(L, 0);
      else
      {
        for (int i = 0; i < nup; i++) /* copy upvalues to the top */
          lua_pushvalue(L, -nup);
        lua_pushcclosure(L, reg.func, nup); /* closure with those upvalues */
      }
      lua_setfieldi(L, -(nup + 2), reg.name.to_utf8());
    }
    lua_pop(L, nup); /* remove upvalues */
  }

  public static void luaL_newlibtable(lua_State L, luaL_Reg[] funcs)
  {
    lua_createtable(L, 0, funcs.length);
  }

  public static void luaL_newlib(lua_State L, luaL_Reg[] funcs)
  {
    luaL_checkversion(L);
    luaL_newlibtable(L, funcs);
    luaL_setfuncs(L, funcs, 0);
  }

  /*
  ** ensure that stack[idx][fname] has a table and push that table
  ** into the stack
  */

  public static int luaL_getsubtable(lua_State L, int idx, HostString fname)
  {
    return luaL_getsubtablei(L, idx, fname.to_utf8());
  }

  public static int luaL_getsubtablei(lua_State L, int idx, ByteView fname)
  {
    if (lua_getfieldi(L, idx, fname) == LUA_TTABLE)
      return 1; /* table already there */
    else
    {
      lua_pop(L, 1); /* remove previous result */
      idx = lua_absindex(L, idx);
      lua_newtable(L);
      lua_pushvalue(L, -1);        /* copy to be left at top */
      lua_setfieldi(L, idx, fname); /* assign new table to field */
      return 0;                    /* false, because did not find table there */
    }
  }

  /*
  ** Stripped-down 'require': After checking "loaded" table, calls 'openf'
  ** to open a module, registers the result in 'package.loaded' table and,
  ** if 'glb' is true, also registers the result in the global table.
  ** Leaves resulting module on the top.
  */

  public static void luaL_requiref(lua_State L, HostString modname,
                                   lua_CFunction openf, int glb)
  {
    luaL_requirefi(L, modname.to_utf8(), openf, glb);
  }

  public static void luaL_requirefi(lua_State L, ByteView modname,
                                    lua_CFunction openf, int glb)
  {
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfieldi(L, -1, modname); /* LOADED[modname] */
    if (lua_toboolean(L, -1) == 0)
    {                /* package not already loaded? */
      lua_pop(L, 1); /* remove field */
      lua_pushcfunction(L, openf);
      lua_pushlstring(L, modname, 0, modname.size());   /* argument to open function */
      lua_call(L, 1, 1);            /* call 'openf' to open module */
      lua_pushvalue(L, -1);         /* make copy of module (call result) */
      lua_setfieldi(L, -3, modname); /* LOADED[modname] = module */
    }
    lua_remove(L, -2); /* remove LOADED table */
    if (glb != 0)
    {
      lua_pushvalue(L, -1);      /* copy of module */
      lua_setglobali(L, modname); /* _G[modname] = module */
    }
  }

  /*
  ** The freeze-ready 'luaL_requiref' and 'luaL_getsubtable' are internal C
  ** functions that the helpers call with 'lua_callk': each step that can
  ** run user code (an '__index', an '__newindex', the opening function) has
  ** its own continuation, and the user's 'k' goes on with the results.
  ** Stack of 'requireff': modname, openf, glb, then LOADED and the module.
  */
  private static int requireff_start(lua_State L)
  {
    return requireff(L, LUA_OK, 0);
  }

  private static final lua_CFunction requireff_start = LuaAuxLib::requireff_start;

  private static int requireff(lua_State L, int status, long ctx)
  {
    ByteView modname = lua_tolstring(L, 1);
    int at = (int) ctx;
    for (;;)
    {
      switch (at)
      {
      case 0:
        luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE); /* 4 */
        lua_getfieldik(L, 4, modname, 1, requireff); /* LOADED[modname] */
      case 1:
        if (lua_toboolean(L, -1) != 0) /* package already loaded? */
        {
          at = 3;
          continue;
        }
        lua_pop(L, 1); /* remove field */
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 1); /* argument to open function */
        lua_callk(L, 1, 1, 2, requireff); /* call 'openf' to open module */
      case 2:
        lua_pushvalue(L, -1); /* make copy of module (call result) */
        lua_setfieldik(L, 4, modname, 3, requireff); /* LOADED[modname] = module */
      case 3:
        lua_remove(L, 4); /* remove LOADED table */
        if (lua_toboolean(L, 3) != 0)
        {
          lua_pushvalue(L, -1); /* copy of module */
          lua_setglobalik(L, modname, 4, requireff); /* _G[modname] = module */
        }
      default:
        return 1;
      }
    }
  }

  private static final lua_KFunction requireff = LuaAuxLib::requireff;

  private static final KNamer knamer_requireff = new KNamer("auxlib.requireff", requireff);

  private static final CFuncInfo cfi_requireff = new CFuncInfo(requireff_start, "auxlib.requireff");

  public static void luaL_requirefk(lua_State L, HostString modname, lua_CFunction openf, int glb,
                                    long ctx, lua_KFunction k)
  {
    lua_pushcfuncinfo(L, cfi_requireff);
    lua_pushlstring(L, modname.to_utf8(), 0, modname.to_utf8().size());
    lua_pushcfunction(L, openf);
    lua_pushboolean(L, glb);
    lua_callk(L, 3, 1, ctx, k);
  }

  /* stack of 'getsubtableff': the table, the field name */
  private static int getsubtableff_start(lua_State L)
  {
    return getsubtableff(L, LUA_OK, 0);
  }

  private static final lua_CFunction getsubtableff_start = LuaAuxLib::getsubtableff_start;

  private static int getsubtableff(lua_State L, int status, long ctx)
  {
    ByteView fname = lua_tolstring(L, 2);
    switch ((int) ctx)
    {
    case 0:
      lua_getfieldik(L, 1, fname, 1, getsubtableff);
    case 1:
      if (lua_type(L, -1) == LUA_TTABLE)
      { /* table already there */
        lua_pushboolean(L, 1);
        return 2;
      }
      lua_pop(L, 1); /* remove previous result */
      lua_newtable(L);
      lua_pushvalue(L, -1); /* copy to be left at top */
      lua_setfieldik(L, 1, fname, 2, getsubtableff); /* assign new table to field */
    default:
      lua_pushboolean(L, 0); /* did not find table there */
      return 2;
    }
  }

  private static final lua_KFunction getsubtableff = LuaAuxLib::getsubtableff;

  private static final KNamer knamer_getsubtableff = new KNamer("auxlib.getsubtableff", getsubtableff);

  private static final CFuncInfo cfi_getsubtableff = new CFuncInfo(getsubtableff_start, "auxlib.getsubtableff");

  public static int luaL_getsubtablek(lua_State L, int idx, HostString fname, long ctx, lua_KFunction k)
  {
    lua_pushcfuncinfo(L, cfi_getsubtableff);
    lua_pushvalue(L, idx);
    lua_pushlstring(L, fname.to_utf8(), 0, fname.to_utf8().size());
    lua_callk(L, 2, 2, ctx, k);
    int found = lua_toboolean(L, -1);
    return found;
  }

  /*
  ** Freeze-ready forms of the helpers that can run user code. Like the
  ** 'k' calls of lua.h, they return as the plain helper does, unless what
  ** they call freezes (or yields): then 'k' is called when it thaws, with
  ** the stack as the helper leaves it, and finishes the job as the notes
  ** say.
  **  - luaL_callmetak: as luaL_callmeta; 'k' runs only if the metamethod
  **    was called, with its result on the top.
  **  - luaL_tolstringk: pushes the string; 'k' gets the value of
  **    __tostring on the top and calls luaL_tolstringresult for the string
  **    (it checks that it is one).
  **  - luaL_lenk: 'k' gets the length on the top and calls luaL_lenresult
  **    for the integer (it checks and pops it).
  **  - luaL_getsubtablek: as luaL_getsubtable, but it leaves the table and
  **    then a boolean (true if the table was already there) and returns
  **    that boolean; 'k' finds the same two values.
  **  - luaL_requirefk: as luaL_requiref; 'k' finds the module on the top.
  **  - luaL_dofilek, luaL_dostringk: as luaL_dofile and luaL_dostring;
  **    their pcall has the continuation, and 'k' gets the status as for
  **    lua_pcallk.
  */
  public static int luaL_callmetak(lua_State L, int obj, HostString e, long ctx, lua_KFunction k)
  {
    obj = lua_absindex(L, obj);
    if (luaL_getmetafieldi(L, obj, e.to_utf8()) == LUA_TNIL) /* no metafield? */
      return 0;
    lua_pushvalue(L, obj);
    lua_callk(L, 1, 1, ctx, k);
    return 1;
  }

  public static ByteView luaL_tolstringk(lua_State L, int idx, long ctx, lua_KFunction k)
  {
    luaL_toistringk(L, idx, ctx, k);
    return luaL_tolstringresult(L);
  }

  public static ByteView luaL_tolstringresult(lua_State L)
  {
    return luaL_tostringresult(L).view();
  }

  public static int luaL_dofilek(lua_State L, HostString filename, long ctx, lua_KFunction k)
  {
    int status = luaL_loadfile(L, filename);
    if (status == LUA_OK)
      status = lua_pcallk(L, 0, LUA_MULTRET, 0, ctx, k);
    return status;
  }

  public static int luaL_dostringk(lua_State L, HostString str, long ctx, lua_KFunction k)
  {
    int status = luaL_loadstring(L, str);
    if (status == LUA_OK)
      status = lua_pcallk(L, 0, LUA_MULTRET, 0, ctx, k);
    return status;
  }

  public static void luaL_addgsubi(luaL_Buffer b, ByteView s,
                                   ByteView p, ByteView r)
  {
    int pos = 0;
    int wild;
    while ((wild = s.find(p, pos)) != ByteView.npos)
    {
      luaL_addlstring(b, s, pos, wild - pos); /* push prefix */
      luaL_addlstring(b, r, 0, r.size()); /* push replacement in place of pattern */
      pos = wild + p.size();              /* continue after 'p' */
    }
    luaL_addlstring(b, s, pos, s.size() - pos); /* push last suffix */
  }

  public static void luaL_addgsub(luaL_Buffer b, HostString s, HostString p, HostString r)
  {
    luaL_addgsubi(b, s.to_utf8(), p.to_utf8(), r.to_utf8());
  }

  public static ByteView luaL_gsubi(lua_State L, ByteView s, ByteView p, ByteView r)
  {
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      luaL_addgsubi(b, s, p, r);
      luaL_pushresult(b);
      return lua_tolstring(L, -1);
    }
    finally
    {
      b.release();
    }
  }

  public static HostString luaL_gsub(lua_State L, HostString s, HostString p, HostString r)
  {
    return new HostString(luaL_gsubi(L, s.to_utf8(), p.to_utf8(), r.to_utf8()));
  }

  private static final class AuxAlloc implements lua_Alloc
  {
    @Override
    public boolean permit(long osize, long nsize, int tag)
    {
      return true;
    }
  }

  private static final AuxAlloc luaL_alloc = new AuxAlloc();

  /*
  ** Standard panic function just prints an error message. The test
  ** with 'lua_type' avoids possible memory errors in 'lua_tostring'.
  */
  private static int panic(lua_State L)
  {
    ByteView msg = (lua_type(L, -1) == LUA_TSTRING)
                       ? lua_toistring(L, -1).view()
                       : Lt.error_object_is_not_a_string;
    os_print_stderr(Lt.PANIC_unprotected_error_in_call_to_Lua_API);
    os_print_stderr(msg);
    os_print_stderr(Lt.n_2);
    os_flush_stderr();
    return 0; /* return to Lua to abort */
  }

  private static final lua_CFunction panic = LuaAuxLib::panic;

  /*
  ** Warning functions:
  ** warnfoff: warning system is off
  ** warnfon: ready to start a new message
  ** warnfcont: previous message is to be continued
  */
  private static final WarnOff warnfoff = new WarnOff();
  private static final WarnOn warnfon = new WarnOn();
  private static final WarnCont warnfcont = new WarnCont();

  /*
  ** Check whether message is a control message. If so, execute the
  ** control or ignore it if unknown.
  */
  private static int checkcontrol(lua_State L, HostString message, int tocont)
  {
    ByteView m = message.to_utf8();
    if (tocont != 0 || m.empty() || m.at(0) != '@') /* not a control message? */
      return 0;
    else
    {
      ByteView tail = m.substr(1);
      if (tail.equals(Lt.off_2))
        lua_setwarnf(L, warnfoff); /* turn warnings off */
      else if (tail.equals(Lt.on_2))
        lua_setwarnf(L, warnfon); /* turn warnings on */
      return 1;                      /* it was a control message */
    }
  }

  private static final class WarnOff implements lua_WarnFunction
  {
    @Override
    public void warn(lua_State L, HostString message, int tocont)
    {
      checkcontrol(L, message, tocont);
    }
  }

  private static final class WarnCont implements lua_WarnFunction
  {
    /*
    ** Writes the message and handle 'tocont', finishing the message
    ** if needed and setting the next warn function.
    */
    @Override
    public void warn(lua_State L, HostString message, int tocont)
    {
      os_print_stderr(message.to_utf8());
      os_flush_stderr();
      if (tocont != 0)                          /* not the last part? */
        lua_setwarnf(L, warnfcont);     /* to be continued */
      else
      {                                /* last part */
        os_print_stderr(Lt.n_3); /* finish message with end-of-line */
        os_flush_stderr();
        lua_setwarnf(L, warnfon);   /* next call is a new message */
      }
    }
  }

  private static final class WarnOn implements lua_WarnFunction
  {
    @Override
    public void warn(lua_State L, HostString message, int tocont)
    {
      if (checkcontrol(L, message, tocont) != 0) /* control message? */
        return;                                           /* nothing else to be done */
      os_print_stderr(Lt.Lua_warning); /* start a new warning */
      os_flush_stderr();
      warnfcont.warn(L, message, tocont);         /* finish processing */
    }
  }

  /*
  ** A function to compute an unsigned int with some level of
  ** randomness. Relies on the current time.
  */

  private static int luai_makeseed()
  {
    long nanos = System.nanoTime();
    long millis = System.currentTimeMillis();
    int[] buff = new int[4];
    buff[0] = (int) (nanos & 0xFFFFFFFFL);
    buff[1] = (int) ((nanos >>> 32) & 0xFFFFFFFFL);
    buff[2] = (int) (millis & 0xFFFFFFFFL);
    buff[3] = (int) ((millis >>> 32) & 0xFFFFFFFFL);
    int res = buff[0];
    for (int i = 1; i < 4; i++)
      res ^= (res >>> 3) + (res << 7) + buff[i];
    return res;
  }

  public static int luaL_makeseed(lua_State L)
  {
    return luai_makeseed();
  }

  /*
  ** Use the name with parentheses so that headers can redefine it
  ** as a macro.
  */

  public static lua_State luaL_newstate()
  {
    lua_State L = _lua_newstate(luaL_alloc, luaL_makeseed(null));
    if (L != null)
    {
      lua_atpanic(L, panic);
      lua_setwarnf(L, warnfon);
    }
    return L;
  }

  public static void luaL_checkversion_(lua_State L, double ver, int sz)
  {
    double v = lua_version(L);
    if (sz != LUAL_NUMSIZES) /* check numeric types */
      luaL_errori(L, Lt.core_and_library_have_incompatible_numeric_types);
    else if (v != ver)
    {
      lua_pushnumber(L, ver);
      lua_pushnumber(L, v);
      luaL_errori(L, Lt.version_mismatch_app_needs_f_Lua_core_provides_f);
    }
  }

  public static void luaL_checkversion(lua_State L)
  {
    luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES);
  }

  public static int luaL_loadfile(lua_State L, HostString filename)
  {

    if (filename == null)
      return luaL_loadfilei(L, null, null);
    ByteView sv = filename.to_utf8();
    return luaL_loadfilei(L, sv, null);
  }

  public static void luaL_argcheck(lua_State L, boolean cond, int arg, HostString extramsg)
  {
    if (!cond)
      luaL_argerrori(L, arg, extramsg.to_utf8());
  }

  public static void luaL_argchecki(lua_State L, boolean cond, int arg, ByteView extramsg)
  {
    if (!cond)
      luaL_argerrori(L, arg, extramsg);
  }

  public static void luaL_argexpected(lua_State L, boolean cond, int arg, HostString tname)
  {
    if (!cond)
      luaL_typeerrori(L, arg, tname.to_utf8());
  }

  public static void luaL_argexpectedi(lua_State L, boolean cond, int arg, ByteView tname)
  {
    if (!cond)
      luaL_typeerrori(L, arg, tname);
  }

  public static HostString luaL_checkstring(lua_State L, int arg)
  {
    return new HostString(luaL_checklstring(L, arg));
  }

  public static HostString luaL_optstring(lua_State L, int arg,
                                          HostString d)
  {
    if (lua_isnoneornil(L, arg) != 0)
      return d;
    return luaL_checkstring(L, arg);
  }

  public static HostString luaL_typename(lua_State L, int idx)
  {
    return lua_typename(L, lua_type(L, idx));
  }

  public static int luaL_dofile(lua_State L, HostString filename)
  {
    int status = luaL_loadfile(L, filename);
    if (status == LUA_OK)
      status = lua_pcall(L, 0, LUA_MULTRET, 0);
    return status;
  }

  public static int luaL_dostring(lua_State L, HostString str)
  {
    int status = luaL_loadstring(L, str);
    if (status == LUA_OK)
      status = lua_pcall(L, 0, LUA_MULTRET, 0);
    return status;
  }

  public static int luaL_getmetatable(lua_State L, HostString tname)
  {
    return luaL_getmetatablei(L, tname.to_utf8());
  }

  public static int luaL_getmetatablei(lua_State L, ByteView tname)
  {
    return lua_getfieldi(L, LUA_REGISTRYINDEX, tname);
  }

  public static int luaL_loadbuffer(lua_State L, ByteView buff, HostString name)
  {
    return luaL_loadbufferi(L, buff,
                            (name != null) ? name.to_utf8() : null,
                            null);
  }

  /* push the value used to represent failure/error */
  public static void luaL_pushfail(lua_State L)
  {
    lua_pushnil(L);
  }

  public static int luaL_bufflen(luaL_Buffer bf)
  {
    return bf.used;
  }

  public static ByteSpan luaL_buffaddr(luaL_Buffer bf)
  {
    return bf.bytes.span();
  }

  public static void luaL_addsize(luaL_Buffer B, int s)
  {
    B.used += s;
  }

  public static void luaL_buffsub(luaL_Buffer B, int s)
  {
    B.used -= s;
  }

  public static int luaL_prepbuffer(luaL_Buffer B)
  {
    return luaL_prepbuffsize(B, LUAL_BUFFERSIZE);
  }

  /* }====================================================================== */
}
