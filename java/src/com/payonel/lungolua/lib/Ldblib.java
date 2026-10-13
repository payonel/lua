package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.api.LuaConf.*;
import static com.payonel.lungolua.bytes.InternalString.sv_byteat;
import static com.payonel.lungolua.lib.Lstyx.lua_styxcheck;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;

import java.util.function.Consumer;

import com.payonel.lungolua.api.BudgetNamer;
import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.InternalDebug;
import com.payonel.lungolua.api.KNamer;
import com.payonel.lungolua.api.luaL_Buffer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_Debug;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_ResumeResult;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteSpan;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.runtime.lua_State;

public final class Ldblib
{
  private Ldblib()
  {
  }

  /*
  ** If L1 != L, L1 can be in any state, and therefore there are no
  ** guarantees about its stack space; any push in L1 must be
  ** checked.
  */
  private static void checkstack(lua_State L, lua_State L1, int n)
  {
    if (L != L1 && !lua_checkstack(L1, n))
      luaL_errori(L, Lt.stack_overflow);
  }

  private static int db_getregistry(lua_State L)
  {
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    return 1;
  }

  private static final lua_CFunction db_getregistry = Ldblib::db_getregistry;

  private static int db_getmetatable(lua_State L)
  {
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1))
    {
      lua_pushnil(L); /* no metatable */
    }
    return 1;
  }

  private static final lua_CFunction db_getmetatable = Ldblib::db_getmetatable;

  private static int db_setmetatable(lua_State L)
  {
    int t = lua_type(L, 2);
    luaL_argexpectedi(L, t == LUA_TNIL || t == LUA_TTABLE, 2, Lt.nil_or_table);
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1; /* return 1st argument */
  }

  private static final lua_CFunction db_setmetatable = Ldblib::db_setmetatable;

  private static int db_getuservalue(lua_State L)
  {
    int n = (int) luaL_optinteger(L, 2, 1);
    if (lua_type(L, 1) != LUA_TUSERDATA)
      luaL_pushfail(L);
    else if (lua_getiuservalue(L, 1, n) != LUA_TNONE)
    {
      lua_pushboolean(L, true);
      return 2;
    }
    return 1;
  }

  private static final lua_CFunction db_getuservalue = Ldblib::db_getuservalue;

  private static int db_setuservalue(lua_State L)
  {
    int n = (int) luaL_optinteger(L, 3, 1);
    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checkany(L, 2);
    lua_settop(L, 2);
    if (!lua_setiuservalue(L, 1, n))
      luaL_pushfail(L);
    return 1;
  }

  private static final lua_CFunction db_setuservalue = Ldblib::db_setuservalue;

  /*
  ** Auxiliary function used by several library functions: check for
  ** an optional thread as function's first argument and set 'arg' with
  ** 1 if this argument is present (so that functions can skip it to
  ** access their other arguments)
  */
  private static final class ThreadArg
  {
    lua_State L1;
    int arg;

    ThreadArg(lua_State L1, int arg)
    {
      this.L1 = L1;
      this.arg = arg;
    }
  }

  private static ThreadArg getthread(lua_State L)
  {
    if (lua_isthread(L, 1))
    {
      return new ThreadArg(lua_tothread(L, 1), 1);
    }
    else
    {
      return new ThreadArg(L, 0); /* function will operate over current thread */
    }
  }

  /*
  ** Variations of 'lua_settable', used by 'db_getinfo' to put results
  ** from 'lua_getinfo' into result table. Key is always a string;
  ** value can be a string, an int, or a boolean.
  */
  private static void settabss(lua_State L, ByteView k, ByteView v)
  {
    lua_pushlstring(L, v, 0, v.size());
    lua_setfieldi(L, -2, k);
  }

  private static void settabsi(lua_State L, ByteView k, int v)
  {
    lua_pushinteger(L, v);
    lua_setfieldi(L, -2, k);
  }

  private static void settabsb(lua_State L, ByteView k, boolean v)
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
  private static void treatstackoption(lua_State L, lua_State L1, ByteView fname)
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
  private static int db_getinfo(lua_State L)
  {
    InternalDebug ar = new InternalDebug();
    ThreadArg ta = getthread(L);
    lua_State L1 = ta.L1;
    int arg = ta.arg;
    ByteView options = luaL_optlstring(L, arg + 2, Lt.flnSrtu);
    checkstack(L, L1, 3);
    luaL_argchecki(L, sv_byteat(options, 0) != '>', arg + 2, Lt.invalid_option_2);
    if (lua_isfunction(L, arg + 1))
    { /* info about a function? */
      luaL_Buffer b = new luaL_Buffer();
      try
      {
        luaL_buffinit(L, b);
        luaL_addchar(b, (byte) '>');
        luaL_addlstring(b, options, 0, options.size());
        luaL_pushresult(b); /* add '>' to 'options' */
        options = lua_tolstring(L, -1);
        lua_pushvalue(L, arg + 1);                    /* move function to 'L1' stack */
        lua_xmove(L, L1, 1);
      }
      finally
      {
        b.release();
      }
    }
    else
    { /* stack level */
      InternalDebug oar = lua_getstacki(L1, (int) luaL_checkinteger(L, arg + 1));
      if (oar == null)
      {
        luaL_pushfail(L); /* level out of range */
        return 1;
      }
      ar = oar;
    }
    InternalDebug res = lua_getinfoi(L1, options, ar);
    if (res == null)
      return luaL_argerrori(L, arg + 2, Lt.invalid_option_1);
    ar = res;
    lua_newtable(L); /* table to collect results */
    if (options.find('S') != ByteView.npos)
    {
      lua_pushlstring(L, ar.source, 0, ar.source.size());
      lua_setfieldi(L, -2, Lt.source);
      settabss(L, Lt.short_src, ar.short_src_out.str_view());
      settabsi(L, Lt.linedefined, ar.linedefined);
      settabsi(L, Lt.lastlinedefined, ar.lastlinedefined);
      settabss(L, Lt.what, ar.what);
    }
    if (options.find('l') != ByteView.npos)
      settabsi(L, Lt.currentline, ar.currentline);
    if (options.find('u') != ByteView.npos)
    {
      settabsi(L, Lt.nups, ar.nups);
      settabsi(L, Lt.nparams, ar.nparams);
      settabsb(L, Lt.isvararg, ar.isvararg);
    }
    if (options.find('n') != ByteView.npos)
    {
      if (ar.name != null)
        settabss(L, Lt.name_2, ar.name);
      settabss(L, Lt.namewhat, (ar.namewhat != null) ? ar.namewhat : Lt.empty_str);
    }
    if (options.find('r') != ByteView.npos)
    {
      settabsi(L, Lt.ftransfer, ar.ftransfer);
      settabsi(L, Lt.ntransfer, ar.ntransfer);
    }
    if (options.find('t') != ByteView.npos)
    {
      settabsb(L, Lt.istailcall, ar.istailcall);
      settabsi(L, Lt.extraargs, ar.extraargs);
    }
    if (options.find('L') != ByteView.npos)
      treatstackoption(L, L1, Lt.activelines);
    if (options.find('f') != ByteView.npos)
      treatstackoption(L, L1, Lt.func);
    return 1; /* return table */
  }

  private static final lua_CFunction db_getinfo = Ldblib::db_getinfo;

  private static int db_getlocal(lua_State L)
  {
    ThreadArg ta = getthread(L);
    lua_State L1 = ta.L1;
    int arg = ta.arg;
    int nvar = (int) luaL_checkinteger(L, arg + 2); /* local-variable index */
    if (lua_isfunction(L, arg + 1))
    {                                                 /* function argument? */
      lua_pushvalue(L, arg + 1); /* push function */
      InternalString name = lua_getlocali(L, null, nvar);
      if (name != null)
        lua_pushistringref(L, name.view());
      else
        lua_pushnil(L);
      return 1; /* return only name (there is no value) */
    }
    else
    { /* stack-level argument */
      int level = (int) luaL_checkinteger(L, arg + 1);
      InternalDebug ar = lua_getstacki(L1, level);
      if (ar == null) /* out of range? */
        return luaL_argerrori(L, arg + 1, Lt.level_out_of_range);
      checkstack(L, L1, 1);
      InternalString name = lua_getlocali(L1, ar, nvar);
      if (name != null)
      {
        lua_xmove(L1, L, 1); /* move local value */
        lua_pushistringref(L, name.view()); /* push name */
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

  private static final lua_CFunction db_getlocal = Ldblib::db_getlocal;

  private static int db_setlocal(lua_State L)
  {
    ThreadArg ta = getthread(L);
    lua_State L1 = ta.L1;
    int arg = ta.arg;
    int level = (int) luaL_checkinteger(L, arg + 1);
    int nvar = (int) luaL_checkinteger(L, arg + 2);
    InternalDebug ar = lua_getstacki(L1, level);
    if (ar == null) /* out of range? */
      return luaL_argerrori(L, arg + 1, Lt.level_out_of_range);
    luaL_checkany(L, arg + 3);
    lua_settop(L, arg + 3);
    checkstack(L, L1, 1);
    lua_xmove(L, L1, 1);
    InternalString name = lua_setlocali(L1, ar, nvar);
    if (name == null)
    {
      lua_pop(L1, 1); /* pop value (if not popped by 'lua_setlocal') */
      lua_pushnil(L);
    }
    else
      lua_pushistringref(L, name.view());
    return 1;
  }

  private static final lua_CFunction db_setlocal = Ldblib::db_setlocal;

  /*
  ** get (if 'get' is true) or set an upvalue from a closure
  */
  private static int auxupvalue(lua_State L, int get)
  {
    int n = (int) luaL_checkinteger(L, 2); /* upvalue index */
    luaL_checktype(L, 1, LUA_TFUNCTION);   /* closure */
    InternalString name = (get != 0) ? lua_getupvaluei(L, 1, n) : lua_setupvaluei(L, 1, n);
    if (name == null)
      return 0;
    lua_pushistringref(L, name.view());
    lua_insert(L, -(get + 1)); /* no-op if get is false */
    return get + 1;
  }

  private static int db_getupvalue(lua_State L)
  {
    return auxupvalue(L, 1);
  }

  private static final lua_CFunction db_getupvalue = Ldblib::db_getupvalue;

  private static int db_setupvalue(lua_State L)
  {
    luaL_checkany(L, 3);
    return auxupvalue(L, 0);
  }

  private static final lua_CFunction db_setupvalue = Ldblib::db_setupvalue;

  /*
  ** Check whether a given upvalue from a given closure exists and
  ** returns its index
  */
  private static final class UpvalRef
  {
    Object id;
    int nup;
  }

  private static UpvalRef checkupval(lua_State L, int argf, int argnup)
  {
    UpvalRef r = new UpvalRef();
    r.nup = (int) luaL_checkinteger(L, argnup); /* upvalue index */
    luaL_checktype(L, argf, LUA_TFUNCTION);     /* closure */
    r.id = lua_upvalueid(L, argf, r.nup);
    return r;
  }

  private static UpvalRef checkupvalvalid(lua_State L, int argf, int argnup)
  {
    UpvalRef r = checkupval(L, argf, argnup);
    luaL_argchecki(L, r.id != null, argnup, Lt.invalid_upvalue_index);
    return r;
  }

  private static int db_upvalueid(lua_State L)
  {
    Object id = checkupval(L, 1, 2).id;
    if (id != null)
      lua_pushlightuserdata(L, id);
    else
      luaL_pushfail(L);
    return 1;
  }

  private static final lua_CFunction db_upvalueid = Ldblib::db_upvalueid;

  private static int db_upvaluejoin(lua_State L)
  {
    int n1 = checkupvalvalid(L, 1, 2).nup;
    int n2 = checkupvalvalid(L, 3, 4).nup;
    luaL_argchecki(L, !lua_iscfunction(L, 1), 1, Lt.Lua_function_expected);
    luaL_argchecki(L, !lua_iscfunction(L, 3), 3, Lt.Lua_function_expected);
    lua_upvaluejoin(L, 1, n1, 3, n2);
    return 0;
  }

  private static final lua_CFunction db_upvaluejoin = Ldblib::db_upvaluejoin;

  private static final ByteView[] hooknames = {Lt.call, Lt.return_kw, Lt.line, Lt.count, Lt.tail_call};

  /*
  ** Call hook function registered at hook table for the current
  ** thread (if there is one)
  */
  private static void hookf(lua_State L, lua_Debug ar)
  {
    lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._HOOKKEY);
    lua_pushthread(L);
    if (lua_rawget(L, -2) == LUA_TFUNCTION)
    {                                                /* is there a hook function? */
      lua_pushistringref(L, hooknames[ar.event]); /* push event name */
      if (ar.currentline >= 0)
        lua_pushinteger(L, ar.currentline); /* push current line */
      else
        lua_pushnil(L);
      InternalDebug iar = new InternalDebug();
      iar.i_ci = ar.i_ci;
      assert lua_getinfoi(L, Lt.lS_chars, iar) != null;
      lua_callhook(L, 2); /* call hook function (it can freeze) */
    }
  }

  private static final lua_Hook hookf = Ldblib::hookf;

  /*
  ** Convert a string mask (for 'sethook') into a bit mask
  */
  private static int makemask(ByteView smask, int count)
  {
    int mask = 0;
    if (smask.find('c') != ByteView.npos)
      mask |= LUA_MASKCALL;
    if (smask.find('r') != ByteView.npos)
      mask |= LUA_MASKRET;
    if (smask.find('l') != ByteView.npos)
      mask |= LUA_MASKLINE;
    if (count > 0)
      mask |= LUA_MASKCOUNT;
    return mask;
  }

  private static int unmakemask(int mask, ByteSpan smask)
  {
    int i = 0;
    if ((mask & LUA_MASKCALL) != 0)
      smask.set(i++, 'c');
    if ((mask & LUA_MASKRET) != 0)
      smask.set(i++, 'r');
    if ((mask & LUA_MASKLINE) != 0)
      smask.set(i++, 'l');
    return i;
  }

  private static int db_sethook(lua_State L)
  {
    int mask, count;
    lua_Hook func;
    ThreadArg ta = getthread(L);
    lua_State L1 = ta.L1;
    int arg = ta.arg;
    if (lua_isnoneornil(L, arg + 1))
    { /* no hook? */
      lua_settop(L, arg + 1);
      func = null;
      mask = 0;
      count = 0; /* turn off hooks */
    }
    else
    {
      ByteView smask = luaL_checklstring(L, arg + 2);
      luaL_checktype(L, arg + 1, LUA_TFUNCTION);
      count = (int) luaL_optinteger(L, arg + 3, 0);
      func = hookf;
      mask = makemask(smask, count);
    }
    if (!luaL_getsubtablei(L, LUA_REGISTRYINDEX, Lt._HOOKKEY))
    {
      /* table just created; initialize it */
      lua_pushistringref(L, Lt.k_char);
      lua_setfieldi(L, -2, Lt.mm_mode); /** hooktable.__mode = "k" */
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

  private static final lua_CFunction db_sethook = Ldblib::db_sethook;

  private static int db_gethook(lua_State L)
  {
    lua_State L1 = getthread(L).L1;
    ByteOwner buff = new ByteOwner(5);
    int mask = lua_gethookmask(L1);
    lua_Hook hook = lua_gethook(L1);
    if (hook == null)
    { /* no hook? */
      luaL_pushfail(L);
      return 1;
    }
    else if (hook != hookf) /* external hook? */
      lua_pushistringref(L, Lt.external_hook);
    else
    { /* hook table must exist */
      lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._HOOKKEY);
      checkstack(L, L1, 1);
      lua_pushthread(L1);
      lua_xmove(L1, L, 1);
      lua_rawget(L, -2); /* 1st result = hooktable[L1] */
      lua_remove(L, -2); /* remove hook table */
    }
    int masklen = unmakemask(mask, buff.span());
    ByteView maskview = buff.view().substr(0, masklen);
    lua_pushlstring(L, maskview, 0, maskview.size()); /* 2nd result = mask */
    lua_pushinteger(L, lua_gethookcount(L1));  /* 3rd result = count */
    return 3;
  }

  private static final lua_CFunction db_gethook = Ldblib::db_gethook;

  /*
  ** 'debug.debug' runs each command in a protected call, and reports its
  ** error with the message's '__tostring': both with a continuation, which
  ** goes on from the step 'ctx' says.
  */
  private static final int DD_READ = 0;  /* read the next command */
  private static final int DD_RAN = 1;   /* the command ran (or failed: 'status') */
  private static final int DD_ERROR = 2; /* report the error on the top */
  private static final int DD_PRINT = 3; /* print the string on the top */

  private static int db_debug_from(lua_State L, int step)
  {
    for (;;)
    {
      switch (step)
      {
      case DD_ERROR:
        luaE_freezeonly(L);
        luaL_toistringk(L, -1, DD_PRINT, db_debugk);
      case DD_PRINT:
        os_print_stderr(luaL_tostringresult(L).view());
        os_print_stderr(Lt.n_3);
        os_flush_stderr();
      case DD_RAN:
        lua_settop(L, 0); /* remove eventual returns */
      default: /* DD_READ */
      {
        ByteOwner buffer = new ByteOwner(LUA_MAXINPUT);
        os_print_stderr(Lt.lua_debug);
        os_flush_stderr();
        if (!os_readline(os_stdin(), buffer.span()) || buffer.str_view().equals(Lt.cont_n))
          return 0;
        ByteView cmd = buffer.str_view();
        luaE_freezeonly(L);
        if (luaL_loadbufferi(L, cmd, Lt.debug_command, Lt.t_char) != 0 ||
            lua_pcallk(L, 0, 0, 0, DD_RAN, db_debugk) != 0)
          step = DD_ERROR;
        else
          step = DD_RAN;
      }
      }
    }
  }

  private static int db_debugk(lua_State L, int status, long ctx)
  {
    int step = (int)ctx;
    if (step == DD_RAN && status != LUA_OK && status != LUA_YIELD)
      step = DD_ERROR; /* the command failed */
    return db_debug_from(L, step);
  }

  private static final lua_KFunction db_debugk = Ldblib::db_debugk;

  private static final KNamer knamer_db_debugk = new KNamer("debug.db_debugk", db_debugk);

  private static int db_debug(lua_State L)
  {
    return db_debug_from(L, DD_READ);
  }

  private static final lua_CFunction db_debug = Ldblib::db_debug;

  /*
  ** {======================================================
  ** Budget and freezing
  ** =======================================================
  */

  /*
  ** The budget callback: the Lua function of the group, called with the
  ** topmost budgeted thread and the thread that spent the budget, on a
  ** thread of its own (so that the thread that spent it keeps its stack and
  ** C calls as they are). An error in it destroys the chain (see
  ** 'luaD_budget'): it goes on as an error of the spender, which nothing
  ** catches.
  */

  /*
  ** The thread the callbacks of the debug library run on (anchored in the
  ** registry): found, or made when it is not there yet (the first budget set,
  ** or a budget that came with a stream into a state that had none).
  */
  private static lua_State callbackthread(lua_State L)
  {
    lua_State B = lua_budgetthread(L);
    if (B != null)
      return B;
    if (lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._BUDGETTHREAD) == LUA_TTHREAD)
    {
      B = lua_tothread(L, -1);
      lua_pop(L, 1);
    }
    else
    {
      lua_pop(L, 1);
      B = lua_newthread(L);
      lua_sethook(B, null, 0, 0); /* (no hooks inherited) */
      lua_setbudget(B, 0, null);  /* (and no budget) */
      lua_setfieldi(L, LUA_REGISTRYINDEX, Lt._BUDGETTHREAD);
    }
    lua_setbudgetthread(L, B);
    return B;
  }

  private static void budgetf(lua_State user_co, lua_State L)
  {
    lua_State B = lua_budgetthread(L); /* (made when a budget is set or comes in: no room is made in 'L') */
    lua_pushbudgetvalue(B, L, true); /* the function of the group in effect */
    lua_pushthreadi(B, user_co);
    lua_pushthreadi(B, L);
    if (lua_pcall(B, 2, 0, 0) != LUA_OK)
    {
      luaL_checkstacki(L, 1, null);
      lua_xmove(B, L, 1);
      lua_error(L);
    }
  }

  private static final lua_Budget budgetf = Ldblib::budgetf;

  /* a budget of this callback comes into a state (see 'lua_preparebudget') */
  private static void budgetprepare(lua_State L)
  {
    callbackthread(L);
  }

  private static final Consumer<lua_State> budgetprepare = Ldblib::budgetprepare;

  private static final BudgetNamer bnamer_budgetf = new BudgetNamer("debug.budgetf", budgetf, budgetprepare);

  private static int db_setbudget(lua_State L)
  {
    lua_State co = checkco(L, 1); /* (the thread is required: the kernel sets it on another) */
    final int arg = 1;
    long n = luaL_checkinteger(L, arg + 1);
    luaL_argchecki(L, n >= 0, arg + 1, Lt.out_of_range);
    if (n == 0 || lua_isnoneornil(L, arg + 2))
    { /* no budget */
      lua_setbudget(co, 0, null);
      return 0;
    }
    luaL_checktype(L, arg + 2, LUA_TFUNCTION);
    callbackthread(L); /* (the thread the callback runs on) */
    int status = lua_setbudget(co, n, budgetf);
    if (status == LUA_ERRMEM)
    { /* (raised here, on the caller) */
      lua_pushistringref(L, Lt.not_enough_memory);
      return lua_error(L);
    }
    if (status != LUA_OK)
      return luaL_errori(L, Lt.too_many_budget_groups);
    lua_pushvalue(L, arg + 2);
    lua_setbudgetvalue(L, co); /* the function goes into the group */
    return 0;
  }

  private static final lua_CFunction db_setbudget = Ldblib::db_setbudget;

  private static int db_getbudget(lua_State L)
  {
    lua_State co = checkco(L, 1);
    long n = lua_getbudget(co);
    if (n == 0)
    {
      luaL_pushfail(L);
      return 1;
    }
    lua_pushinteger(L, n);
    lua_pushbudgetvalue(L, co, false);
    lua_pushinteger(L, lua_budgetleft(co));
    lua_State root = lua_budgetroot(co);
    if (root != null)
      lua_pushthreadi(L, root);
    else
      lua_pushnil(L);
    return 4;
  }

  private static final lua_CFunction db_getbudget = Ldblib::db_getbudget;

  private static lua_State checkco(lua_State L, int arg)
  {
    lua_State co = lua_tothread(L, arg);
    luaL_argexpectedi(L, co != null, arg, Lt.thread);
    return co;
  }

  private static int db_freeze(lua_State L)
  {
    throw lua_freeze(L);
  }

  private static final lua_CFunction db_freeze = Ldblib::db_freeze;

  /* debug.canfreeze(): true, or false and why (a few reasons) */
  private static int db_canfreeze(lua_State L)
  {
    int why = lua_nofreeze(L);
    if (why == 0)
    {
      lua_pushboolean(L, true);
      return 1;
    }
    lua_pushboolean(L, false);
    lua_pushistringref(L, why == LUA_NOFREEZE_BUDGET ? Lt.no_budget_in_effect
                           : why == LUA_NOFREEZE_CALL ? Lt.a_call_that_cannot_freeze_is_in_the_way
                                                      : Lt.a_resume_that_cannot_freeze_is_in_the_way);
    return 2;
  }

  private static final lua_CFunction db_canfreeze = Ldblib::db_canfreeze;

  /*
  ** debug.persistable(v [, permanents]): true if a persist of the state can
  ** write all that 'v' holds (a kernel asks it of a user program, to find the
  ** one that keeps what cannot be persisted); otherwise false and why, with
  ** the path of what cannot be written. 'permanents' is the kernel's table
  ** of permanents (object -> name), as the host gives it to its persist.
  */
  private static int db_persistable(lua_State L)
  {
    luaL_checkany(L, 1);
    int perms = 0;
    if (!lua_isnoneornil(L, 2))
    {
      luaL_checktype(L, 2, LUA_TTABLE);
      perms = 2;
    }
    luaL_StyxResult res = lua_styxcheck(L, 1, perms);
    if (res.status == LUA_OK)
    {
      lua_pushboolean(L, true);
      return 1;
    }
    lua_pushboolean(L, false);
    ByteView m = res.msg.to_utf8();
    lua_pushlstring(L, m, 0, m.size());
    return 2;
  }

  private static final lua_CFunction db_persistable = Ldblib::db_persistable;

  private static int db_isfrozen(lua_State L)
  {
    lua_pushboolean(L, lua_isfrozen(checkco(L, 1)));
    return 1;
  }

  private static final lua_CFunction db_isfrozen = Ldblib::db_isfrozen;

  /* the results of a thaw of 'co', as 'coroutine.resume' gives them */
  private static int thaw_end(lua_State L, lua_State co, lua_ResumeResult rr)
  {
    if (rr.status == LUA_OK || rr.status == LUA_YIELD)
    {
      if (!lua_checkstack(L, rr.nres + 1))
      {
        lua_pop(co, rr.nres); /* remove results anyway */
        luaL_pushfail(L);
        lua_pushistringref(L, Lt.too_many_results_to_resume);
        return 2;
      }
      lua_pushboolean(L, true);
      lua_xmove(co, L, rr.nres);
      return rr.nres + 1;
    }
    else
    {
      lua_pushboolean(L, false);
      if (co == L)
        lua_insert(L, -2); /* (a thread thawing itself: its message is on its top) */
      else
        lua_xmove(co, L, 1); /* move error message */
      return 2;
    }
  }

  private static int thaw_cont(lua_State L, int status, long ctx)
  {
    lua_State co = lua_tothread(L, 1);
    return thaw_end(L, co, new lua_ResumeResult(status, lua_resumenres(co, status)));
  }

  private static final lua_KFunction thaw_cont = Ldblib::thaw_cont;

  private static final KNamer knamer_thaw_cont = new KNamer("debug.thaw_cont", thaw_cont);

  private static int db_thaw(lua_State L)
  {
    lua_State co = checkco(L, 1);
    if (lua_budgeted(L))
      return luaL_error(L, new HostString(Lt.cannot_thaw_from_budgeted_code));
    lua_settop(L, 1);
    return thaw_end(L, co, lua_thawk(co, L, 0, thaw_cont));
  }

  private static final lua_CFunction db_thaw = Ldblib::db_thaw;

  /* }====================================================== */

  private static int db_traceback(lua_State L)
  {
    ThreadArg ta = getthread(L);
    lua_State L1 = ta.L1;
    int arg = ta.arg;
    if (!lua_isstring(L, arg + 1) && !lua_isnoneornil(L, arg + 1)) /* non-string 'msg'? */
      lua_pushvalue(L, arg + 1); /* return it untouched */
    else
    {
      int level = (int) luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
      if (lua_isnoneornil(L, arg + 1))
        luaL_traceback(L, L1, null, level);
      else
      {
        luaL_traceback(L, L1, lua_tostring(L, arg + 1), level);
      }
    }
    return 1;
  }

  private static final lua_CFunction db_traceback = Ldblib::db_traceback;

  private static final CFuncInfo cfi_db_debug = new CFuncInfo(db_debug, "debug.db_debug");
  private static final CFuncInfo cfi_db_getuservalue = new CFuncInfo(db_getuservalue, "debug.db_getuservalue");
  private static final CFuncInfo cfi_db_gethook = new CFuncInfo(db_gethook, "debug.db_gethook");
  private static final CFuncInfo cfi_db_getinfo = new CFuncInfo(db_getinfo, "debug.db_getinfo");
  private static final CFuncInfo cfi_db_getlocal = new CFuncInfo(db_getlocal, "debug.db_getlocal");
  private static final CFuncInfo cfi_db_getregistry = new CFuncInfo(db_getregistry, "debug.db_getregistry");
  private static final CFuncInfo cfi_db_getmetatable = new CFuncInfo(db_getmetatable, "debug.db_getmetatable");
  private static final CFuncInfo cfi_db_getupvalue = new CFuncInfo(db_getupvalue, "debug.db_getupvalue");
  private static final CFuncInfo cfi_db_upvaluejoin = new CFuncInfo(db_upvaluejoin, "debug.db_upvaluejoin");
  private static final CFuncInfo cfi_db_upvalueid = new CFuncInfo(db_upvalueid, "debug.db_upvalueid");
  private static final CFuncInfo cfi_db_setuservalue = new CFuncInfo(db_setuservalue, "debug.db_setuservalue");
  private static final CFuncInfo cfi_db_sethook = new CFuncInfo(db_sethook, "debug.db_sethook");
  private static final CFuncInfo cfi_db_setlocal = new CFuncInfo(db_setlocal, "debug.db_setlocal");
  private static final CFuncInfo cfi_db_setmetatable = new CFuncInfo(db_setmetatable, "debug.db_setmetatable");
  private static final CFuncInfo cfi_db_setupvalue = new CFuncInfo(db_setupvalue, "debug.db_setupvalue");
  private static final CFuncInfo cfi_db_traceback = new CFuncInfo(db_traceback, "debug.db_traceback");
  private static final CFuncInfo cfi_db_setbudget = new CFuncInfo(db_setbudget, "debug.db_setbudget");
  private static final CFuncInfo cfi_db_getbudget = new CFuncInfo(db_getbudget, "debug.db_getbudget");
  private static final CFuncInfo cfi_db_freeze = new CFuncInfo(db_freeze, "debug.db_freeze");
  private static final CFuncInfo cfi_db_persistable = new CFuncInfo(db_persistable, "debug.db_persistable");
  private static final CFuncInfo cfi_db_isfrozen = new CFuncInfo(db_isfrozen, "debug.db_isfrozen");
  private static final CFuncInfo cfi_db_canfreeze = new CFuncInfo(db_canfreeze, "debug.db_canfreeze");
  private static final CFuncInfo cfi_db_thaw = new CFuncInfo(db_thaw, "debug.db_thaw");

  private static final luaL_RegCFI[] dblib = {
    new luaL_RegCFI(Lt.debug, cfi_db_debug),
    new luaL_RegCFI(Lt.getuservalue, cfi_db_getuservalue),
    new luaL_RegCFI(Lt.gethook, cfi_db_gethook),
    new luaL_RegCFI(Lt.getinfo, cfi_db_getinfo),
    new luaL_RegCFI(Lt.getlocal, cfi_db_getlocal),
    new luaL_RegCFI(Lt.getregistry, cfi_db_getregistry),
    new luaL_RegCFI(Lt.getmetatable, cfi_db_getmetatable),
    new luaL_RegCFI(Lt.getupvalue, cfi_db_getupvalue),
    new luaL_RegCFI(Lt.upvaluejoin, cfi_db_upvaluejoin),
    new luaL_RegCFI(Lt.upvalueid, cfi_db_upvalueid),
    new luaL_RegCFI(Lt.setuservalue, cfi_db_setuservalue),
    new luaL_RegCFI(Lt.sethook, cfi_db_sethook),
    new luaL_RegCFI(Lt.setlocal, cfi_db_setlocal),
    new luaL_RegCFI(Lt.setmetatable, cfi_db_setmetatable),
    new luaL_RegCFI(Lt.setupvalue, cfi_db_setupvalue),
    new luaL_RegCFI(Lt.traceback, cfi_db_traceback),
    new luaL_RegCFI(Lt.setbudget, cfi_db_setbudget),
    new luaL_RegCFI(Lt.getbudget, cfi_db_getbudget),
    new luaL_RegCFI(Lt.freeze, cfi_db_freeze),
    new luaL_RegCFI(Lt.isfrozen, cfi_db_isfrozen),
    new luaL_RegCFI(Lt.persistable, cfi_db_persistable),
    new luaL_RegCFI(Lt.canfreeze, cfi_db_canfreeze),
    new luaL_RegCFI(Lt.thaw, cfi_db_thaw),
  };

  public static int luaopen_debug(lua_State L)
  {
    luaL_newlibcfi(L, dblib);
    return 1;
  }

  public static final lua_CFunction luaopen_debug = Ldblib::luaopen_debug;

  public static lua_Hook luaL_debughook()
  {
    return hookf;
  }
}
