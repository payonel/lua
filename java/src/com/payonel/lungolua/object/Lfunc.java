package com.payonel.lungolua.object;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.runtime.Ltm.*;
import static com.payonel.lungolua.vm.Ldebug.*;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.runtime.lua_State;

public final class Lfunc
{
  private Lfunc()
  {
  }

  /* test whether thread is in 'twups' list */
  public static int isintwups(lua_State L)
  {
    return (L.twups != L) ? 1 : 0;
  }

  /*
  ** maximum number of upvalues in a closure (both C and Lua). (Value
  ** must fit in a VM register.)
  */
  public static final int MAXUPVAL = 255;

  public static int upisopen(UpVal up)
  {
    return (up.owner != null) ? 1 : 0;
  }

  public static int uplevel(UpVal up)
  {
    assert upisopen(up) != 0;
    return up.stkid;
  }

  public static TValue upvalgetval(UpVal uv)
  {
    return upisopen(uv) != 0 ? getslot(uv.owner, uv.stkid) : getobj(uv.value);
  }

  public static void upvalsetval(lua_State L, UpVal uv, TValue v)
  {
    if (upisopen(uv) != 0)
      setslot(uv.owner, uv.stkid, v);
    else
    {
      setobj(L, uv.value, v);
    }
  }

  /*
  ** maximum number of misses before giving up the cache of closures
  ** in prototypes
  */
  public static final int MAXMISS = 10;

  /* special status to close upvalues preserving the top of the stack */
  public static final int CLOSEKTOP = LUA_ERRERR + 1;

  /*
  ** Call closing method for object 'obj' with error object 'err'. 'yy'
  ** controls whether the call is yieldable (1), cannot yield but can
  ** freeze (2: the caller can recover from a freeze, and it is not under
  ** a resume that would catch what the method's own protected calls do),
  ** or neither (0).
  ** (This function assumes EXTRA_STACK.)
  */
  private static void callclosemethod(lua_State L, TValue obj, TValue err, int yy)
  {
    int top = L.top;
    int func = top;
    TValue tm = luaT_gettmbyobj(L, obj, TM_CLOSE);
    setslot(L, top++, tm);    /* will call metamethod... */
    setslot(L, top++, obj);   /* with 'self' as the 1st argument */
    if (err != null)           /* if there was an error... */
      setslot(L, top++, err); /* then error object will be 2nd argument */
    L.top = top;            /* add function and arguments */
    if (yy == 1)
      luaD_call(L, func, 0);
    else if (yy == 2)
      luaD_callnoyield(L, func, 0);
    else
    { /* (cannot freeze) */
      L.nfz++;
      luaD_callnoyield(L, func, 0);
      L.nfz--;
    }
  }

  /*
  ** Check whether object at given level has a close metamethod and raise
  ** an error if not.
  */
  private static void checkclosemth(lua_State L, int level)
  {
    TValue tm = luaT_gettmbyobj(L, level, TM_CLOSE);
    if (ttisnil(tm))
    {                                            /* no metamethod? */
      int idx = (int) (level - L.ci.func); /* variable index */
      InternalString vname_i = luaG_findlocal(L, L.ci, idx).name;
      ByteView vname = vname_i != null ? vname_i.view() : Lt.qmark;
      luaG_runerror(L, Lt.variable_s_got_a_non_closable_value, new FmtArgs().str(vname));
    }
  }

  /*
  ** Prepare and call a closing method.
  ** If status is CLOSEKTOP, the call to the closing method will be pushed
  ** at the top of the stack. Otherwise, values can be pushed right after
  ** the 'level' of the upvalue being closed, as everything after that
  ** won't be used again.
  */
  private static void prepcallclosemth(lua_State L, int level, byte status,
                                       int yy)
  {
    int uv = level; /* value being closed */
    TValue errobj = new TValue();
    int haserr;
    switch (status)
    {
    case LUA_OK:
      L.top = level + 1; /* call will be at this level */
      /* FALLTHROUGH */
    case CLOSEKTOP:  /* don't need to change top */
      haserr = 0; /* no error object */
      break;
    default:                                  /* 'luaD_seterrorobj' will set top to level + 2 */
      luaD_seterrorobj(L, status, level + 1);
      errobj = getslot(L, level + 1);
      haserr = 1;
      break;
    }
    TValue uvv = getslot(L, uv);
    callclosemethod(L, uvv, haserr != 0 ? errobj : null, yy);
  }

  /*
  ** Insert a variable in the list of to-be-closed variables.
  */
  public static void luaF_newtbcupval(lua_State L, int level)
  {
    assert level > L.tbclist;
    if (tagisfalse(slottag(L, level)))
      return;                /* false doesn't need to be closed */
    checkclosemth(L, level); /* value must have a close method */
    {
      L.tbc = luaM_safegrow_StkId_span(L, L.tbc, L.ntbc);
    }
    L.tbc[L.ntbc++] = level;
    L.tbclist = level;
  }

  /*
  ** Close all upvalues up to the given stack level.
  */
  public static void luaF_closeupval(lua_State L, int level)
  {
    UpVal uv;
    while ((uv = L.openupval) != null && uplevel(uv) >= level)
    {
      TValue slot = uv.value; /* new position for value */
      assert uplevel(uv) < L.top;
      uv.luaF_unlinkupval();       /* remove upvalue from 'openupval' list */
      slot2obj(L, slot, uv.stkid); /* move value to upvalue slot */
      uv.prevopen = null;
      uv.opennext = null;
      uv.owner = null;
      if (iswhite(uv) == 0)
      {               /* neither white nor dead? */
        nw2black(uv); /* closed upvalues cannot be gray */
        luaC_barrier(L, uv, slot);
      }
    }
  }

  private static void poptbclist(lua_State L)
  {
    assert L.ntbc > 0 && L.tbc[L.ntbc - 1] == L.tbclist;
    L.ntbc--;
    L.tbclist = (L.ntbc > 0) ? L.tbc[L.ntbc - 1] : 0;
  }

  /*
  ** Close all upvalues and to-be-closed variables up to the given stack
  ** level. Return restored 'level'.
  */
  public static int luaF_close(lua_State L, int level, byte status, int yy)
  {
    int levelrel = level;
    luaF_closeupval(L, level); /* first, close the upvalues */
    while (L.tbclist >= level)
    {                                       /* traverse tbc's down to that level */
      int tbc = L.tbclist;             /* get variable index */
      poptbclist(L);                        /* remove it from list */
      prepcallclosemth(L, tbc, status, yy); /* close variable */
      level = levelrel;
    }
    return level;
  }
}
