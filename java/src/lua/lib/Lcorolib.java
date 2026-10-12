package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lstate.*;
import static lua.vm.Ldebug.*;

import lua.api.CClosureNamer;
import lua.api.KNamer;
import lua.api.luaL_RegCFI;
import lua.api.lua_CClosureKind;
import lua.api.lua_CFunction;
import lua.api.lua_KFunction;
import lua.api.lua_ResumeResult;
import lua.bytes.ByteView;
import lua.bytes.Lt;
import lua.object.CFuncInfo;
import lua.runtime.lua_State;

public final class Lcorolib
{
  private Lcorolib()
  {
  }

  private static lua_State getco(lua_State L)
  {
    lua_State co = lua_tothread(L, 1);
    luaL_argexpectedi(L, co != null, 1, Lt.thread);
    return co;
  }

  /*
  ** The results of a resume of 'co' that ended with 'rr': the number of
  ** results for non-error cases or -1 for errors.
  */
  private static int auxresumed(lua_State L, lua_State co, lua_ResumeResult rr)
  {
    if (rr.status == LUA_OK || rr.status == LUA_YIELD)
    {
      int nres = rr.nres;
      if (lua_checkstack(L, nres + 1) == 0)
      {
        lua_pop(co, nres); /* remove results anyway */
        lua_pushistringref(L, Lt.too_many_results_to_resume);
        return -1; /* error flag */
      }
      lua_xmove(co, L, nres); /* move yielded values */
      return nres;
    }
    else
    {
      lua_xmove(co, L, 1); /* move error message */
      return -1;           /* error flag */
    }
  }

  /*
  ** Resumes a coroutine. Returns the number of results for non-error
  ** cases or -1 for errors. A freeze of a coroutine below passes through
  ** the resume; when it thaws, 'k' goes on with the result.
  */
  private static int auxresume(lua_State L, lua_State co, int narg, lua_KFunction k)
  {
    if (lua_checkstack(co, narg) == 0)
    {
      lua_pushistringref(L, Lt.too_many_arguments_to_resume);
      return -1; /* error flag */
    }
    lua_xmove(L, co, narg);
    return auxresumed(L, co, lua_resumek(co, L, narg, 0, k));
  }

  private static int coresume_end(lua_State L, int r)
  {
    if (r < 0)
    {
      lua_pushboolean(L, 0);
      lua_insert(L, -2);
      return 2; /* return false + error message */
    }
    else
    {
      lua_pushboolean(L, 1);
      lua_insert(L, -(r + 1));
      return r + 1; /* return true + 'resume' returns */
    }
  }

  private static int coresume_cont(lua_State L, int status, long ctx)
  {
    lua_State co = lua_tothread(L, 1);
    return coresume_end(L, auxresumed(L, co, new lua_ResumeResult(status, lua_resumenres(co, status))));
  }

  private static final lua_KFunction coresume_cont = Lcorolib::coresume_cont;

  private static final KNamer knamer_coresume_cont = new KNamer("coroutine.coresume_cont", coresume_cont);

  private static int luaB_coresume(lua_State L)
  {
    lua_State co = getco(L);
    return coresume_end(L, auxresume(L, co, lua_gettop(L) - 1, coresume_cont));
  }

  private static final lua_CFunction luaB_coresume = Lcorolib::luaB_coresume;

  /* the error of a wrapped coroutine, in the caller's stack: propagate it */
  private static int auxwrap_fail(lua_State L, int stat)
  {
    if (stat != LUA_ERRMEM && /* not a memory error and ... */
        lua_type(L, -1) == LUA_TSTRING)
    {                   /* ... error object is a string? */
      luaL_where(L, 1); /* add extra info, if available */
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    return lua_error(L); /* propagate error */
  }

  /* the variables of the coroutine are closed, after a thaw (a '__close' froze) */
  private static int auxwrap_closed(lua_State L, int status, long ctx)
  {
    lua_State co = lua_tothread(L, lua_upvalueindex(1));
    assert status != LUA_OK;
    lua_xmove(co, L, 1); /* move error message to the caller */
    return auxwrap_fail(L, status);
  }

  private static final lua_KFunction auxwrap_closed = Lcorolib::auxwrap_closed;

  private static final KNamer knamer_auxwrap_closed = new KNamer("coroutine.auxwrap_closed", auxwrap_closed);

  private static int auxwrap_end(lua_State L, lua_State co, int r)
  {
    if (r < 0)
    { /* error? */
      int stat = lua_status(co);
      if (stat != LUA_OK && stat != LUA_YIELD)
      { /* error in the coroutine? close its tbc variables (a '__close' can freeze) */
        stat = lua_closethreadk(co, L, 0, auxwrap_closed).status;
        if (stat == LUA_OK && lua_isfrozen(co) != 0)
          return 0; /* the close froze here, as a resume can: no values */
        assert stat != LUA_OK;
        lua_xmove(co, L, 1); /* move error message to the caller */
      }
      return auxwrap_fail(L, stat);
    }
    return r;
  }

  private static int auxwrap_cont(lua_State L, int status, long ctx)
  {
    lua_State co = lua_tothread(L, lua_upvalueindex(1));
    return auxwrap_end(L, co, auxresumed(L, co, new lua_ResumeResult(status, lua_resumenres(co, status))));
  }

  private static final lua_KFunction auxwrap_cont = Lcorolib::auxwrap_cont;

  private static final KNamer knamer_auxwrap_cont = new KNamer("coroutine.auxwrap_cont", auxwrap_cont);

  private static int luaB_auxwrap(lua_State L)
  {
    lua_State co = lua_tothread(L, lua_upvalueindex(1));
    return auxwrap_end(L, co, auxresume(L, co, lua_gettop(L), auxwrap_cont));
  }

  private static final lua_CFunction luaB_auxwrap = Lcorolib::luaB_auxwrap;

  private static final CClosureNamer auxwrap_kind = new CClosureNamer(new lua_CClosureKind("coroutine.auxwrap", luaB_auxwrap, 1, null, null));

  private static int luaB_cocreate(lua_State L)
  {
    lua_State NL;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    NL = lua_newthread(L);
    lua_pushvalue(L, 1); /* move function to top */
    lua_xmove(L, NL, 1); /* move function from L to NL */
    return 1;
  }

  private static final lua_CFunction luaB_cocreate = Lcorolib::luaB_cocreate;

  private static int luaB_cowrap(lua_State L)
  {
    luaB_cocreate(L);
    lua_pushcclosure(L, luaB_auxwrap, 1);
    return 1;
  }

  private static final lua_CFunction luaB_cowrap = Lcorolib::luaB_cowrap;

  private static int luaB_yield(lua_State L)
  {
    return lua_yield(L, lua_gettop(L));
  }

  private static final lua_CFunction luaB_yield = Lcorolib::luaB_yield;

  private static final int COS_RUN = 0;
  private static final int COS_DEAD = 1;
  private static final int COS_YIELD = 2;
  private static final int COS_NORM = 3;

  private static final ByteView[] statname = {Lt.running, Lt.dead, Lt.suspended, Lt.normal_2};

  private static int auxstatus(lua_State L, lua_State co)
  {
    if (L == co)
      return COS_RUN;
    else if (lua_isfrozen(co) != 0) /* as when it froze */
      return (lua_frozenchild(co) != null) ? COS_NORM : COS_RUN;
    else
    {
      switch (lua_status(co))
      {
      case LUA_YIELD:
        return COS_YIELD;
      case LUA_OK:
      {
        if (lua_getstack(co, 0) != null) /* does it have frames? */
          return COS_NORM;       /* it is running */
        else if (lua_gettop(co) == 0)
          return COS_DEAD;
        else
          return COS_YIELD; /* initial state */
      }
      default: /* some error occurred */
        return COS_DEAD;
      }
    }
  }

  private static int luaB_costatus(lua_State L)
  {
    lua_State co = getco(L);
    lua_pushistringref(L, statname[auxstatus(L, co)]);
    return 1;
  }

  private static final lua_CFunction luaB_costatus = Lcorolib::luaB_costatus;

  private static lua_State getoptco(lua_State L)
  {
    return ((lua_isnone(L, 1) != 0) ? L : getco(L));
  }

  private static int luaB_yieldable(lua_State L)
  {
    lua_State co = getoptco(L);
    lua_pushboolean(L, lua_isyieldable(co));
    return 1;
  }

  private static final lua_CFunction luaB_yieldable = Lcorolib::luaB_yieldable;

  private static int luaB_corunning(lua_State L)
  {
    int ismain = lua_pushthread(L);
    lua_pushboolean(L, ismain);
    return 2;
  }

  private static final lua_CFunction luaB_corunning = Lcorolib::luaB_corunning;

  private static int close_end(lua_State L, lua_State co, int status)
  {
    if (status == LUA_OK)
    {
      lua_pushboolean(L, 1);
      return 1;
    }
    else
    {
      lua_pushboolean(L, 0);
      lua_xmove(co, L, 1); /* move error message */
      return 2;
    }
  }

  /* after a thaw (a '__close' froze) */
  private static int close_cont(lua_State L, int status, long ctx)
  {
    return close_end(L, getoptco(L), status);
  }

  private static final lua_KFunction close_cont = Lcorolib::close_cont;

  private static final KNamer knamer_close_cont = new KNamer("coroutine.close_cont", close_cont);

  private static int luaB_close(lua_State L)
  {
    lua_State co = getoptco(L);
    int status = auxstatus(L, co);
    switch (status)
    {
    case COS_DEAD:
    case COS_YIELD:
      return close_end(L, co, lua_closethreadk(co, L, 0, close_cont).status);
    case COS_NORM:
      lua_pushistringref(L, statname[status]);
      return luaL_errori(L, Lt.cannot_close_a_s_coroutine);
    case COS_RUN:
      if (co != L)
      { /* frozen (running where it stopped) */
        lua_pushistringref(L, statname[status]);
        return luaL_errori(L, Lt.cannot_close_a_s_coroutine);
      }
      lua_geti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD); /* get main */
      if (lua_tothread(L, -1) == co)
        return luaL_errori(L, Lt.cannot_close_main_thread);
      lua_closeself(L);                   /* close itself (a '__close' can freeze) */
      /* previous call does not return */ /* FALLTHROUGH */
    default:
      assert false;
      return 0;
    }
  }

  private static final lua_CFunction luaB_close = Lcorolib::luaB_close;

  private static final CFuncInfo cfi_luaB_cocreate = new CFuncInfo(luaB_cocreate, "coroutine.luaB_cocreate");
  private static final CFuncInfo cfi_luaB_coresume = new CFuncInfo(luaB_coresume, "coroutine.luaB_coresume");
  private static final CFuncInfo cfi_luaB_corunning = new CFuncInfo(luaB_corunning, "coroutine.luaB_corunning");
  private static final CFuncInfo cfi_luaB_costatus = new CFuncInfo(luaB_costatus, "coroutine.luaB_costatus");
  private static final CFuncInfo cfi_luaB_cowrap = new CFuncInfo(luaB_cowrap, "coroutine.luaB_cowrap");
  private static final CFuncInfo cfi_luaB_yield = new CFuncInfo(luaB_yield, "coroutine.luaB_yield");
  private static final CFuncInfo cfi_luaB_yieldable = new CFuncInfo(luaB_yieldable, "coroutine.luaB_yieldable");
  private static final CFuncInfo cfi_luaB_close = new CFuncInfo(luaB_close, "coroutine.luaB_close");

  private static final luaL_RegCFI[] co_funcs = {
    new luaL_RegCFI(Lt.create, cfi_luaB_cocreate),
    new luaL_RegCFI(Lt.resume, cfi_luaB_coresume),
    new luaL_RegCFI(Lt.running, cfi_luaB_corunning),
    new luaL_RegCFI(Lt.status, cfi_luaB_costatus),
    new luaL_RegCFI(Lt.wrap, cfi_luaB_cowrap),
    new luaL_RegCFI(Lt.yield_kw, cfi_luaB_yield),
    new luaL_RegCFI(Lt.isyieldable, cfi_luaB_yieldable),
    new luaL_RegCFI(Lt.close_kw, cfi_luaB_close),
  };

  public static int luaopen_coroutine(lua_State L)
  {
    luaL_newlibcfi(L, co_funcs);
    return 1;
  }

  public static final lua_CFunction luaopen_coroutine = Lcorolib::luaopen_coroutine;
}
