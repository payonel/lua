package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.bytes.InternalString.sv_byteat;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.platform.LocaleUtil.*;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.KNamer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_LoadSpec;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.runtime.lua_State;

public final class Lbaselib
{
  private Lbaselib()
  {
  }

  /*
  ** Prints argument 'i', whose string is on the top, and the arguments
  ** after it. (The continuation of '__tostring' calls: 'i' in 'ctx'.)
  */
  private static int printk(lua_State L, int status, long ctx)
  {
    int n = lua_gettop(L) - 1; /* number of arguments (below the string) */
    int i = (int) ctx;
    for (;;)
    {
      InternalString s = luaL_tostringresult(L);
      if (i > 1)                /* not the first element? */
        os_print_stdout(Lt.t); /* add a tab before it */
      os_print_stdout(s.view());
      lua_pop(L, 1); /* pop result */
      if (++i > n)
        break;
      luaL_toistringk(L, i, i, printk); /* convert it to string */
    }
    os_print_stdout(Lt.n_3);
    os_flush_stdout();
    return 0;
  }

  private static final lua_KFunction printk = Lbaselib::printk;

  private static final KNamer knamer_printk = new KNamer("base.printk", printk);

  private static int luaB_print(lua_State L)
  {
    if (lua_gettop(L) == 0)
    {
      os_print_stdout(Lt.n_3);
      os_flush_stdout();
      return 0;
    }
    luaE_freezeonly(L);
    luaL_toistringk(L, 1, 1, printk); /* convert it to string */
    return printk(L, LUA_OK, 1);
  }

  private static final lua_CFunction luaB_print = Lbaselib::luaB_print;

  /*
  ** Creates a warning with all given arguments.
  ** Check first for errors; otherwise an error may interrupt
  ** the composition of a warning, leaving it unfinished.
  */
  private static int luaB_warn(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    int i;
    luaL_checkstring(L, 1); /* at least one argument */
    for (i = 2; i <= n; i++)
      luaL_checkstring(L, i); /* make sure all arguments are strings */
    for (i = 1; i < n; i++)   /* compose warning */
      lua_warning(L, lua_tostring(L, i), 1);
    lua_warning(L, lua_tostring(L, n), 0); /* close warning */
    return 0;
  }

  private static final lua_CFunction luaB_warn = Lbaselib::luaB_warn;

  private static final class IntParse
  {
    int end;
    long value;

    IntParse(int end, long value)
    {
      this.end = end;
      this.value = value;
    }
  }

  private static IntParse b_str2int(ByteView s, int base)
  {
    final ByteView spacechars = Lt.whitespace_chars;
    long n = 0;
    int neg = 0;
    int p = 0;
    while (p < s.size() && spacechars.find(s.at(p)) != ByteView.npos)
      p++; /* skip initial spaces */
    if (sv_byteat(s, p) == '-')
    {
      p++;
      neg = 1;
    } /* handle sign */
    else if (sv_byteat(s, p) == '+')
      p++;
    if (locale_isalnum(sv_byteat(s, p)) == 0) /* no digit? */
      return new IntParse(ByteView.npos, 0);
    do
    {
      int digit = ((locale_isdigit(sv_byteat(s, p)) != 0)
                                     ? s.at(p) - '0'
                                     : (locale_toupper(sv_byteat(s, p)) - 'A') + 10);
      if (digit >= base)
        return new IntParse(ByteView.npos, 0); /* invalid numeral */
      n = n * base + digit;
      p++;
    } while (locale_isalnum(sv_byteat(s, p)) != 0);
    while (p < s.size() && spacechars.find(s.at(p)) != ByteView.npos)
      p++; /* skip trailing spaces */
    return new IntParse(p, (neg != 0) ? (0L - n) : n);
  }

  private static int luaB_tonumber(lua_State L)
  {
    if (lua_isnoneornil(L, 2) != 0)
    { /* standard conversion? */
      if (lua_type(L, 1) == LUA_TNUMBER)
      {                   /* already a number? */
        lua_settop(L, 1); /* yes; return it */
        return 1;
      }
      else
      {
        ByteView s = lua_tolstring(L, 1);
        if (s != null && lua_stringtonumber(L, new HostString(s)) == s.size() + 1)
          return 1; /* successful conversion to number */
        /* else not a number */
        luaL_checkany(L, 1); /* (but there must be some parameter) */
      }
    }
    else
    {
      long base = luaL_checkinteger(L, 2);
      luaL_checktype(L, 1, LUA_TSTRING); /* no numbers as strings */
      ByteView s = lua_tolstring(L, 1);
      luaL_argchecki(L, 2 <= base && base <= 36, 2, Lt.base_out_of_range);
      IntParse n = b_str2int(s, (int) base);
      if (n.end == s.size())
      {
        lua_pushinteger(L, n.value);
        return 1;
      } /* else not a number */
    } /* else not a number */
    luaL_pushfail(L); /* not a number */
    return 1;
  }

  private static final lua_CFunction luaB_tonumber = Lbaselib::luaB_tonumber;

  private static int luaB_error(lua_State L)
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

  private static final lua_CFunction luaB_error = Lbaselib::luaB_error;

  private static int luaB_getmetatable(lua_State L)
  {
    luaL_checkany(L, 1);
    if (lua_getmetatable(L, 1) == 0)
    {
      lua_pushnil(L);
      return 1; /* no metatable */
    }
    luaL_getmetafieldi(L, 1, Lt.mm_metatable);
    return 1; /* returns either __metatable field (if present) or metatable */
  }

  private static final lua_CFunction luaB_getmetatable = Lbaselib::luaB_getmetatable;

  private static int luaB_setmetatable(lua_State L)
  {
    int t = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpectedi(L, t == LUA_TNIL || t == LUA_TTABLE, 2, Lt.nil_or_table);
    if (luaL_getmetafieldi(L, 1, Lt.mm_metatable) != LUA_TNIL)
      return luaL_errori(L, Lt.cannot_change_a_protected_metatable);
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
  }

  private static final lua_CFunction luaB_setmetatable = Lbaselib::luaB_setmetatable;

  private static int luaB_rawequal(lua_State L)
  {
    luaL_checkany(L, 1);
    luaL_checkany(L, 2);
    lua_pushboolean(L, lua_rawequal(L, 1, 2));
    return 1;
  }

  private static final lua_CFunction luaB_rawequal = Lbaselib::luaB_rawequal;

  private static int luaB_rawlen(lua_State L)
  {
    int t = lua_type(L, 1);
    luaL_argexpectedi(L, t == LUA_TTABLE || t == LUA_TSTRING, 1, Lt.table_or_string);
    lua_pushinteger(L, lua_rawlen(L, 1));
    return 1;
  }

  private static final lua_CFunction luaB_rawlen = Lbaselib::luaB_rawlen;

  private static int luaB_rawget(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    lua_settop(L, 2);
    lua_rawget(L, 1);
    return 1;
  }

  private static final lua_CFunction luaB_rawget = Lbaselib::luaB_rawget;

  private static int luaB_rawset(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    luaL_checkany(L, 3);
    lua_settop(L, 3);
    lua_rawset(L, 1);
    return 1;
  }

  private static final lua_CFunction luaB_rawset = Lbaselib::luaB_rawset;

  private static int pushmode(lua_State L, int oldmode)
  {
    if (oldmode == -1)
      luaL_pushfail(L); /* invalid call to 'lua_gc' */
    else
      lua_pushistringref(L, (oldmode == LUA_GCINC) ? Lt.incremental : Lt.generational);
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
  private static int drain_cont(lua_State L, int status, long ctx)
  {
    return 1;
  }

  private static final lua_KFunction drain_cont = Lbaselib::drain_cont;

  private static final KNamer knamer_drain_cont = new KNamer("base.drain_cont", drain_cont);

  private static int drainafter(lua_State L)
  {
    if (lua_hasdrain(L) != 0)
    {
      lua_pushdrain(L);
      lua_callk(L, 0, 0, 0, drain_cont);
    }
    return 1;
  }

  private static final ByteView[] opts = {
      Lt.stop, Lt.restart, Lt.collect,
      Lt.count, Lt.step, Lt.isrunning, Lt.generational,
      Lt.incremental, Lt.param};
  private static final byte[] optsnum = {LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT,
                                         LUA_GCCOUNT, LUA_GCSTEP, LUA_GCISRUNNING, LUA_GCGEN, LUA_GCINC,
                                         LUA_GCPARAM};
  private static final ByteView[] params = {
      Lt.minormul, Lt.majorminor, Lt.minormajor,
      Lt.pause, Lt.stepmul, Lt.stepsize};
  private static final byte[] pnum = {
      LUA_GCPMINORMUL, LUA_GCPMAJORMINOR, LUA_GCPMINORMAJOR,
      LUA_GCPPAUSE, LUA_GCPSTEPMUL, LUA_GCPSTEPSIZE};

  private static int luaB_collectgarbage(lua_State L)
  {
    int o = optsnum[luaL_checkoptioni(L, 1, Lt.collect, opts)];
    switch (o)
    {
    case LUA_GCCOUNT:
    {
      int k = lua_gc(L, o, 0, 0, 0);
      int b = lua_gc(L, LUA_GCCOUNTB, 0, 0, 0);
      { if (k == -1) break; };
      lua_pushnumber(L, (double) k + ((double) b / 1024));
      return 1;
    }
    case LUA_GCSTEP:
    {
      long n = luaL_optinteger(L, 2, 0);
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
      int p = pnum[luaL_checkoptioni(L, 2, null, params)];
      long value = luaL_optinteger(L, 3, -1);
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

  private static final lua_CFunction luaB_collectgarbage = Lbaselib::luaB_collectgarbage;

  private static int luaB_type(lua_State L)
  {
    int t = lua_type(L, 1);
    luaL_argchecki(L, t != LUA_TNONE, 1, Lt.value_expected);
    lua_pushistringref(L, lua_typenamei(L, t));
    return 1;
  }

  private static final lua_CFunction luaB_type = Lbaselib::luaB_type;

  private static int luaB_next(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2); /* create a 2nd argument if there isn't one */
    if (lua_next(L, 1) != 0)
      return 2;
    else
    {
      lua_pushnil(L);
      return 1;
    }
  }

  private static final lua_CFunction luaB_next = Lbaselib::luaB_next;

  private static int pairscont(lua_State L, int status, long ctx)
  {
    return 4; /* __pairs did all the work, just return its results */
  }

  private static final lua_KFunction pairscont = Lbaselib::pairscont;

  private static final KNamer knamer_pairscont = new KNamer("base.pairscont", pairscont);

  private static final CFuncInfo cfi_next = new CFuncInfo(luaB_next, "base.next");

  private static int luaB_pairs(lua_State L)
  {
    luaL_checkany(L, 1);
    if (luaL_getmetafieldi(L, 1, Lt.mm_pairs) == LUA_TNIL)
    {                                  /* no metamethod? */
      lua_pushcfuncinfo(L, cfi_next); /* will return generator and */
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

  private static final lua_CFunction luaB_pairs = Lbaselib::luaB_pairs;

  /*
  ** Traversal function for 'ipairs'
  */
  private static int ipairsauxk(lua_State L, int status, long ctx)
  {
    return (lua_isnil(L, -1) != 0) ? 1 : 2;
  }

  private static final lua_KFunction ipairsauxk = Lbaselib::ipairsauxk;

  private static final KNamer knamer_ipairsauxk = new KNamer("base.ipairsauxk", ipairsauxk);

  private static int ipairsaux(lua_State L)
  {
    long i = luaL_checkinteger(L, 2);
    i = l_intadd(i, 1);
    lua_pushinteger(L, i);
    luaE_freezeonly(L);
    return (lua_getik(L, 1, i, 0, ipairsauxk) == LUA_TNIL) ? 1 : 2;
  }

  private static final lua_CFunction ipairsaux = Lbaselib::ipairsaux;

  /*
  ** 'ipairs' function. Returns 'ipairsaux', given "table", 0.
  ** (The given "table" may not be a table.)
  */
  private static final CFuncInfo cfi_ipairsaux = new CFuncInfo(ipairsaux, "base.ipairsaux");

  private static int luaB_ipairs(lua_State L)
  {
    luaL_checkany(L, 1);
    lua_pushcfuncinfo(L, cfi_ipairsaux); /* iteration function */
    lua_pushvalue(L, 1);             /* state */
    lua_pushinteger(L, 0);           /* initial value */
    return 3;
  }

  private static final lua_CFunction luaB_ipairs = Lbaselib::luaB_ipairs;

  private static int load_aux(lua_State L, int status, int envidx)
  {
    if (status == LUA_OK)
    {
      if (envidx != 0)
      {                                /* 'env' parameter? */
        lua_pushvalue(L, envidx);      /* environment for loaded function */
        if (lua_setupvalue(L, -2, 1) == null) /* set it as 1st upvalue */
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

  private static InternalString getMode(lua_State L, int idx)
  {
    InternalString mode = luaL_optistring(L, idx, null);
    if (mode != null && mode.view().find('B') != ByteView.npos)
    {
      /* Lua code cannot use fixed buffers */
      luaL_argerrori(L, idx, Lt.invalid_mode);
    }
    return mode;
  }

  private static ByteView filemode(lua_State L)
  {
    InternalString modestr = getMode(L, 2);
    return (modestr != null) ? modestr.view() : null;
  }

  /* (a load stopped by a freeze goes on: the chunk name at 4, see 'luaL_loadfileik') */
  private static int loadfile_cont(lua_State L, int status, long ctx)
  {
    return load_aux(L, luaL_loadfileikresume(L, status, filemode(L), 4, ctx, loadfile_cont), (int) ctx);
  }

  private static final lua_KFunction loadfile_cont = Lbaselib::loadfile_cont;

  private static final KNamer knamer_loadfile_cont = new KNamer("base.loadfile_cont", loadfile_cont);

  private static int luaB_loadfile(lua_State L)
  {
    ByteView fname = (lua_isnoneornil(L, 1) != 0) ? null : luaL_checkistring(L, 1).view();
    ByteView mode = filemode(L);
    int env = ((lua_isnone(L, 3) == 0) ? 3 : 0); /* 'env' index or 0 if no 'env' */
    lua_settop(L, 3);
    int status = luaL_loadfileik(L, fname, mode, env, loadfile_cont);
    return load_aux(L, status, env);
  }

  private static final lua_CFunction luaB_loadfile = Lbaselib::luaB_loadfile;

  /*
  ** {======================================================
  ** Generic Read function
  ** =======================================================
  */

  /*
  ** 'load' with a budget: a freeze can stop the load in a call of its reader
  ** function, or at a check point (each token read is a step), and it goes
  ** on in 'load_cont' after the thaw where it stopped, as if nothing had
  ** happened. The piece being read is kept at 5 (stock's reserved slot), the
  ** stopped load in a box at 6.
  */

  /* how to load (arguments already checked): a string chunk or a reader function */
  private static lua_LoadSpec loadspec(lua_State L, int env)
  {
    lua_LoadSpec ls = new lua_LoadSpec();
    InternalString mode = getMode(L, 3);
    ls.mode = (mode != null) ? mode.view() : null;
    InternalString chunkname;
    if (lua_type(L, 1) == LUA_TSTRING)
    {
      chunkname = luaL_optistring(L, 2, lua_toistring(L, 1));
      ls.readeridx = 0;
    }
    else
    {
      chunkname = luaL_optistring(L, 2, Lt.load_1_i);
      ls.readeridx = 1;
    }
    ls.name = (chunkname != null) ? chunkname.view() : null;
    ls.pieceidx = 5;
    ls.boxidx = 6;
    ls.ctx = env;
    ls.k = load_cont;
    return ls;
  }

  private static int load_cont(lua_State L, int status, long ctx)
  {
    return load_aux(L, lua_loadkresume(L, status, loadspec(L, (int) ctx)), (int) ctx);
  }

  private static final lua_KFunction load_cont = Lbaselib::load_cont;

  private static final KNamer knamer_load_cont = new KNamer("base.load_cont", load_cont);

  private static int luaB_load(lua_State L)
  {
    ByteView s = lua_tolstring(L, 1);
    getMode(L, 3);
    int env = ((lua_isnone(L, 4) == 0) ? 4 : 0); /* 'env' index or 0 if no 'env' */
    if (s != null)
      luaL_optistring(L, 2, lua_toistring(L, 1));
    else
    { /* loading from a reader function */
      luaL_optistring(L, 2, Lt.load_1_i);
      luaL_checktype(L, 1, LUA_TFUNCTION);
    }
    lua_settop(L, 4);
    lua_pushnil(L); /* the piece being read */
    lua_pushnil(L); /* the box of a stopped load */
    lua_freezeonly(L);
    return load_aux(L, lua_loadk(L, loadspec(L, env)), env);
  }

  private static final lua_CFunction luaB_load = Lbaselib::luaB_load;

  /* }====================================================== */

  private static int dofilecont(lua_State L, int d1, long d2)
  {
    /* only to match 'lua_Kfunction' prototype */
    return lua_gettop(L) - 1;
  }

  private static final lua_KFunction dofilecont = Lbaselib::dofilecont;

  private static final KNamer knamer_dofilecont = new KNamer("base.dofilecont", dofilecont);

  private static int dofile_call(lua_State L, int status)
  {
    if (status != LUA_OK)
      return lua_error(L);
    lua_callk(L, 0, LUA_MULTRET, 0, dofilecont);
    return dofilecont(L, 0, 0);
  }

  /* (a load stopped by a freeze goes on: the chunk name at 2, see 'luaL_loadfileik') */
  private static int dofile_loadcont(lua_State L, int status, long ctx)
  {
    return dofile_call(L, luaL_loadfileikresume(L, status, null, 2, ctx, dofile_loadcont));
  }

  private static final lua_KFunction dofile_loadcont = Lbaselib::dofile_loadcont;

  private static final KNamer knamer_dofile_loadcont = new KNamer("base.dofile_loadcont", dofile_loadcont);

  private static int luaB_dofile(lua_State L)
  {
    ByteView fname = (lua_isnoneornil(L, 1) != 0) ? null : luaL_checkistring(L, 1).view();
    lua_settop(L, 1);
    return dofile_call(L, luaL_loadfileik(L, fname, null, 0, dofile_loadcont));
  }

  private static final lua_CFunction luaB_dofile = Lbaselib::luaB_dofile;

  private static int luaB_assert(lua_State L)
  {
    if (lua_toboolean(L, 1) != 0) /* condition is true? */
      return lua_gettop(L);            /* return all arguments */
    else
    {                                          /* error */
      luaL_checkany(L, 1);                     /* there must be a condition */
      lua_remove(L, 1);                        /* remove it */
      lua_pushistringref(L, Lt.assertion_failed); /* default message */
      lua_settop(L, 1);                        /* leave only message (default if no other one) */
      return luaB_error(L);                    /* call 'error' */
    }
  }

  private static final lua_CFunction luaB_assert = Lbaselib::luaB_assert;

  private static int luaB_select(lua_State L)
  {
    int n = lua_gettop(L);
    if (lua_type(L, 1) == LUA_TSTRING && sv_byteat(lua_tolstring(L, 1), 0) == '#')
    {
      lua_pushinteger(L, n - 1);
      return 1;
    }
    else
    {
      long i = luaL_checkinteger(L, 1);
      if (i < 0)
        i = n + i;
      else if (i > n)
        i = n;
      luaL_argchecki(L, 1 <= i, 1, Lt.index_out_of_range);
      return n - (int) i;
    }
  }

  private static final lua_CFunction luaB_select = Lbaselib::luaB_select;

  /*
  ** Continuation function for 'pcall' and 'xpcall'. Both functions
  ** already pushed a 'true' before doing the call, so in case of success
  ** 'finishpcall' only has to return everything in the stack minus
  ** 'extra' values (where 'extra' is exactly the number of items to be
  ** ignored).
  */
  private static int finishpcall(lua_State L, int status, long extra)
  {
    if (status != LUA_OK && status != LUA_YIELD)
    {                        /* error? */
      lua_pushboolean(L, 0); /* first result (false) */
      lua_pushvalue(L, -2);  /* error message */
      return 2;              /* return false, msg */
    }
    else
      return lua_gettop(L) - (int) extra; /* return all results */
  }

  private static final lua_KFunction finishpcall = Lbaselib::finishpcall;

  private static final KNamer knamer_finishpcall = new KNamer("base.finishpcall", finishpcall);

  private static int luaB_pcall(lua_State L)
  {
    int status;
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1); /* first result if no errors */
    lua_insert(L, 1);      /* put it in place */
    status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finishpcall);
    return finishpcall(L, status, 0);
  }

  private static final lua_CFunction luaB_pcall = Lbaselib::luaB_pcall;

  /*
  ** Do a protected call with error handling. After 'lua_rotate', the
  ** stack will have <f, err, true, f, [args...]>; so, the function passes
  ** 2 to 'finishpcall' to skip the 2 first values when returning results.
  */
  private static int luaB_xpcall(lua_State L)
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

  private static final lua_CFunction luaB_xpcall = Lbaselib::luaB_xpcall;

  private static int tostringk(lua_State L, int status, long ctx)
  {
    luaL_tostringresult(L);
    return 1;
  }

  private static final lua_KFunction tostringk = Lbaselib::tostringk;

  private static final KNamer knamer_tostringk = new KNamer("base.tostringk", tostringk);

  private static int luaB_tostring(lua_State L)
  {
    luaL_checkany(L, 1);
    luaE_freezeonly(L);
    if (luaL_toistringk(L, 1, 0, tostringk) != 0)
      luaL_tostringresult(L);
    return 1;
  }

  private static final lua_CFunction luaB_tostring = Lbaselib::luaB_tostring;

  private static final CFuncInfo cfi_assert = new CFuncInfo(luaB_assert, "base.assert");
  private static final CFuncInfo cfi_collectgarbage = new CFuncInfo(luaB_collectgarbage, "base.collectgarbage");
  private static final CFuncInfo cfi_dofile = new CFuncInfo(luaB_dofile, "base.dofile");
  private static final CFuncInfo cfi_error = new CFuncInfo(luaB_error, "base.error");
  private static final CFuncInfo cfi_getmetatable = new CFuncInfo(luaB_getmetatable, "base.getmetatable");
  private static final CFuncInfo cfi_ipairs = new CFuncInfo(luaB_ipairs, "base.ipairs");
  private static final CFuncInfo cfi_loadfile = new CFuncInfo(luaB_loadfile, "base.loadfile");
  private static final CFuncInfo cfi_load = new CFuncInfo(luaB_load, "base.load");
  private static final CFuncInfo cfi_pairs = new CFuncInfo(luaB_pairs, "base.pairs");
  private static final CFuncInfo cfi_pcall = new CFuncInfo(luaB_pcall, "base.pcall");
  private static final CFuncInfo cfi_print = new CFuncInfo(luaB_print, "base.print");
  private static final CFuncInfo cfi_warn = new CFuncInfo(luaB_warn, "base.warn");
  private static final CFuncInfo cfi_rawequal = new CFuncInfo(luaB_rawequal, "base.rawequal");
  private static final CFuncInfo cfi_rawlen = new CFuncInfo(luaB_rawlen, "base.rawlen");
  private static final CFuncInfo cfi_rawget = new CFuncInfo(luaB_rawget, "base.rawget");
  private static final CFuncInfo cfi_rawset = new CFuncInfo(luaB_rawset, "base.rawset");
  private static final CFuncInfo cfi_select = new CFuncInfo(luaB_select, "base.select");
  private static final CFuncInfo cfi_setmetatable = new CFuncInfo(luaB_setmetatable, "base.setmetatable");
  private static final CFuncInfo cfi_tonumber = new CFuncInfo(luaB_tonumber, "base.tonumber");
  private static final CFuncInfo cfi_tostring = new CFuncInfo(luaB_tostring, "base.tostring");
  private static final CFuncInfo cfi_type = new CFuncInfo(luaB_type, "base.type");
  private static final CFuncInfo cfi_xpcall = new CFuncInfo(luaB_xpcall, "base.xpcall");

  private static final luaL_RegCFI[] base_funcs = {
    new luaL_RegCFI(Lt.assert_kw, cfi_assert),
    new luaL_RegCFI(Lt.collectgarbage, cfi_collectgarbage),
    new luaL_RegCFI(Lt.dofile, cfi_dofile),
    new luaL_RegCFI(Lt.error_kw, cfi_error),
    new luaL_RegCFI(Lt.getmetatable, cfi_getmetatable),
    new luaL_RegCFI(Lt.ipairs, cfi_ipairs),
    new luaL_RegCFI(Lt.loadfile, cfi_loadfile),
    new luaL_RegCFI(Lt.load_2, cfi_load),
    new luaL_RegCFI(Lt.next_kw, cfi_next),
    new luaL_RegCFI(Lt.pairs, cfi_pairs),
    new luaL_RegCFI(Lt.pcall, cfi_pcall),
    new luaL_RegCFI(Lt.print, cfi_print),
    new luaL_RegCFI(Lt.warn, cfi_warn),
    new luaL_RegCFI(Lt.rawequal, cfi_rawequal),
    new luaL_RegCFI(Lt.rawlen, cfi_rawlen),
    new luaL_RegCFI(Lt.rawget, cfi_rawget),
    new luaL_RegCFI(Lt.rawset, cfi_rawset),
    new luaL_RegCFI(Lt.select, cfi_select),
    new luaL_RegCFI(Lt.setmetatable, cfi_setmetatable),
    new luaL_RegCFI(Lt.tonumber, cfi_tonumber),
    new luaL_RegCFI(Lt.tostring, cfi_tostring),
    new luaL_RegCFI(Lt.type, cfi_type),
    new luaL_RegCFI(Lt.xpcall, cfi_xpcall),
    /* placeholders */
    new luaL_RegCFI(LUA_GNAME, null),
    new luaL_RegCFI(Lt._VERSION, null)
  };

  public static int luaopen_base(lua_State L)
  {
    /* open lib into global table */
    lua_pushglobaltable(L);
    luaL_setfuncscfi(L, base_funcs);
    /* set global _G */
    lua_pushvalue(L, -1);
    lua_setfieldi(L, -2, LUA_GNAME);
    /* set global _VERSION */
    lua_pushlstring(L, LUA_VERSION, 0, LUA_VERSION.size());
    lua_setfieldi(L, -2, Lt._VERSION);
    return 1;
  }

  public static final lua_CFunction luaopen_base = Lbaselib::luaopen_base;
}
