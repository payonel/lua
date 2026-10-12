package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.api.LuaConf.*;
import static lua.object.Llimits.*;
import static lua.object.Lnames.*;
import static lua.runtime.Lstate.*;

import lua.api.KNamer;
import lua.api.luaL_Box;
import lua.api.luaL_Buffer;
import lua.api.luaL_RegCFI;
import lua.api.lua_CFunction;
import lua.api.lua_KFunction;
import lua.api.lua_ObjectFactory;
import lua.api.lua_UserObject;
import lua.bytes.ByteOwner;
import lua.bytes.ByteView;
import lua.bytes.Lt;
import lua.object.CFuncInfo;
import lua.runtime.lua_State;

public final class Ltablib
{
  private Ltablib()
  {
  }

  /*
  ** Operations that an object must define to mimic a table
  ** (some functions only need some of them)
  */
  private static final int TAB_R = 1; /* read */
  private static final int TAB_W = 2; /* write */
  private static final int TAB_L = 4; /* length */
  private static final int TAB_RW = TAB_R | TAB_W; /* read/write */


  private static int checkfield(lua_State L, ByteView key, int n)
  {
    lua_pushlstring(L, key, 0, key.size());
    return (lua_rawget(L, -n) != LUA_TNIL) ? 1 : 0;
  }

  /*
  ** Check that 'arg' either is a table or can behave like one (that is,
  ** has a metatable with the required metamethods).
  */
  private static void checktab(lua_State L, int arg, int what)
  {
    int tp = lua_type(L, arg);
    if (tp != LUA_TTABLE)
    {                                 /* is it not a table? */
      int n = 1;                      /* number of elements to pop */
      if (lua_getmetatable(L, arg) != 0 && /* must have metatable */
          ((what & TAB_R) == 0 || checkfield(L, Lt.mm_index, ++n) != 0) &&
          ((what & TAB_W) == 0 || checkfield(L, Lt.mm_newindex, ++n) != 0) &&
          ((what & TAB_L) == 0 || /* strings don't need '__len' to have a length */
           tp == LUA_TSTRING || checkfield(L, Lt.mm_len, ++n) != 0))
      {
        lua_pop(L, n); /* pop metatable and tested metamethods */
      }
      else
        luaL_checktype(L, arg, LUA_TTABLE); /* force an error */
    }
  }

  /*
  ** The length of the object at 'n', its '__len' called with continuation
  ** 'k' (which gets the length with 'luaL_lenresult').
  */
  private static long aux_getnk(lua_State L, int n, int w, long ctx, lua_KFunction k)
  {
    if (lua_isplaintable(L, n) != 0) /* (no '__len') */
      return (long) lua_rawlen(L, n);
    checktab(L, n, w | TAB_L);
    return luaL_lenk(L, n, ctx, k);
  }

  /*
  ** The functions here keep the yieldability of stock Lua: what they call
  ** back cannot yield, but can freeze ('lua_freezeonly'). Their loops are
  ** resumable: each place where a metamethod may run, and each check point
  ** of the budget (an element is a step), has a continuation that goes on
  ** from there. 'at' says where in an element a loop goes on.
  */

  private static final int
      AT_LOOP = 0, /* at the start of an element */
      AT_SET = 1,  /* the element was read (it is on the top) */
      AT_NEXT = 2;  /* the element is done */

  private static int tcreate(lua_State L)
  {
    long sizeseq = luaL_checkinteger(L, 1);
    long sizerest = luaL_optinteger(L, 2, 0);
    luaL_argchecki(L, 0 <= sizeseq && sizeseq <= Integer.MAX_VALUE, 1, Lt.out_of_range);
    luaL_argchecki(L, 0 <= sizerest && sizerest <= Integer.MAX_VALUE, 2, Lt.out_of_range);
    lua_createtable(L, (int) sizeseq, (int) sizerest);
    return 1;
  }

  private static final lua_CFunction tcreate = Ltablib::tcreate;

  /*
  ** 'insert': moves up t[i - 1] .. t[pos] (t[k] = t[k - 1], from 'k' = 'i'
  ** down), then sets t[pos] = v.
  */

  private static int tinsert_move(lua_State L, long i, long pos, int at)
  {
    for (;;)
    {
      switch (at)
      {
      case AT_LOOP:
        if (i <= pos)
        {
          lua_setik(L, 1, pos, 0, tinsertk_done); /* t[pos] = v */
          return 0;
        }
        if (lua_isplaintable(L, 1) != 0)
        { /* (reads and writes call nothing) */
          i -= lua_plainmove(L, 1, pos, 1, pos + 1, i - pos, 0);
          if (luaE_budgetout(L))
            lua_checkpointk(L, (long) i, tinsertk_loop);
          break;
        }
        lua_getik(L, 1, i - 1, (long) i, tinsertk_get);
      case AT_SET:
        lua_setik(L, 1, i, (long) i, tinsertk_set); /* t[i] = t[i - 1] */
      default: /* AT_NEXT */
        i--;
        at = AT_LOOP;
        if (luaE_spent(L, 1))
          lua_checkpointk(L, (long) i, tinsertk_loop);
      }
    }
  }

  private static int tinsertk_get(lua_State L, int status, long ctx)
  {
    return tinsert_move(L, (long) ctx, luaL_checkinteger(L, 2), AT_SET);
  }

  private static final lua_KFunction tinsertk_get = Ltablib::tinsertk_get;

  private static final KNamer knamer_tinsertk_get = new KNamer("table.tinsertk_get", tinsertk_get);

  private static int tinsertk_set(lua_State L, int status, long ctx)
  {
    return tinsert_move(L, (long) ctx, luaL_checkinteger(L, 2), AT_NEXT);
  }

  private static final lua_KFunction tinsertk_set = Ltablib::tinsertk_set;

  private static final KNamer knamer_tinsertk_set = new KNamer("table.tinsertk_set", tinsertk_set);

  private static int tinsertk_loop(lua_State L, int status, long ctx)
  {
    return tinsert_move(L, (long) ctx, luaL_checkinteger(L, 2), AT_LOOP);
  }

  private static final lua_KFunction tinsertk_loop = Ltablib::tinsertk_loop;

  private static final KNamer knamer_tinsertk_loop = new KNamer("table.tinsertk_loop", tinsertk_loop);

  private static int tinsertk_done(lua_State L, int status, long ctx)
  {
    return 0;
  }

  private static final lua_KFunction tinsertk_done = Ltablib::tinsertk_done;

  private static final KNamer knamer_tinsertk_done = new KNamer("table.tinsertk_done", tinsertk_done);

  /* 'insert' with the length 'e' */
  private static int tinsert_len(lua_State L, long e)
  {
    long pos; /* where to insert new element */
    e = l_intadd(e, 1); /* first empty element */
    switch (lua_gettop(L))
    {
    case 2:
    {          /* called with only 2 arguments */
      pos = e; /* insert new element at the end */
      break;
    }
    case 3:
    {
      pos = luaL_checkinteger(L, 2); /* 2nd argument is the position */
      /* check whether 'pos' is in [1, e] */
      luaL_argchecki(L, l_ult(l_intsub(pos, 1), e), 2, Lt.position_out_of_bounds);
      return tinsert_move(L, e, pos, AT_LOOP);
    }
    default:
    {
      return luaL_errori(L, Lt.wrong_number_of_arguments_to_insert);
    }
    }
    lua_setik(L, 1, pos, 0, tinsertk_done); /* t[pos] = v */
    return 0;
  }

  private static int tinsertk_len(lua_State L, int status, long ctx)
  {
    return tinsert_len(L, luaL_lenresult(L));
  }

  private static final lua_KFunction tinsertk_len = Ltablib::tinsertk_len;

  private static final KNamer knamer_tinsertk_len = new KNamer("table.tinsertk_len", tinsertk_len);

  private static int tinsert(lua_State L)
  {
    luaE_freezeonly(L);
    return tinsert_len(L, aux_getnk(L, 1, TAB_RW, 0, tinsertk_len));
  }

  private static final lua_CFunction tinsert = Ltablib::tinsert;

  /*
  ** 'remove' (arguments set to 2; the size at 3, the result at 4): moves
  ** down t[pos + 1] .. t[size] (t[k] = t[k + 1], from 'k' = 'pos' up), then
  ** removes t[size].
  */

  private static int tremove_move(lua_State L, long pos, int at)
  {
    long size = lua_tointeger(L, 3);
    for (;;)
    {
      switch (at)
      {
      case AT_LOOP:
        if (pos >= size)
        {
          lua_pushnil(L);
          lua_setik(L, 1, pos, 0, tremovek_done); /* remove entry t[pos] */
          return 1;
        }
        if (lua_isplaintable(L, 1) != 0)
        { /* (reads and writes call nothing) */
          pos += lua_plainmove(L, 1, pos + 1, 1, pos, size - pos, 1);
          if (luaE_budgetout(L))
            lua_checkpointk(L, (long) pos, tremovek_loop);
          break;
        }
        lua_getik(L, 1, pos + 1, (long) pos, tremovek_get);
      case AT_SET:
        lua_setik(L, 1, pos, (long) pos, tremovek_set); /* t[pos] = t[pos + 1] */
      default: /* AT_NEXT */
        pos++;
        at = AT_LOOP;
        if (luaE_spent(L, 1))
          lua_checkpointk(L, (long) pos, tremovek_loop);
      }
    }
  }

  private static int tremovek_get(lua_State L, int status, long ctx)
  {
    return tremove_move(L, (long) ctx, AT_SET);
  }

  private static final lua_KFunction tremovek_get = Ltablib::tremovek_get;

  private static final KNamer knamer_tremovek_get = new KNamer("table.tremovek_get", tremovek_get);

  private static int tremovek_set(lua_State L, int status, long ctx)
  {
    return tremove_move(L, (long) ctx, AT_NEXT);
  }

  private static final lua_KFunction tremovek_set = Ltablib::tremovek_set;

  private static final KNamer knamer_tremovek_set = new KNamer("table.tremovek_set", tremovek_set);

  private static int tremovek_loop(lua_State L, int status, long ctx)
  {
    return tremove_move(L, (long) ctx, AT_LOOP);
  }

  private static final lua_KFunction tremovek_loop = Ltablib::tremovek_loop;

  private static final KNamer knamer_tremovek_loop = new KNamer("table.tremovek_loop", tremovek_loop);

  private static int tremovek_first(lua_State L, int status, long ctx)
  {
    return tremove_move(L, (long) ctx, AT_LOOP);
  }

  private static final lua_KFunction tremovek_first = Ltablib::tremovek_first;

  private static final KNamer knamer_tremovek_first = new KNamer("table.tremovek_first", tremovek_first);

  private static int tremovek_done(lua_State L, int status, long ctx)
  {
    return 1;
  }

  private static final lua_KFunction tremovek_done = Ltablib::tremovek_done;

  private static final KNamer knamer_tremovek_done = new KNamer("table.tremovek_done", tremovek_done);

  /* 'remove' with the length 'size' */
  private static int tremove_len(lua_State L, long size)
  {
    long pos = luaL_optinteger(L, 2, size);
    if (pos != size) /* validate 'pos' if given */
      /* check whether 'pos' is in [1, size + 1] */
      luaL_argchecki(L, !l_ult(size, l_intsub(pos, 1)), 2, Lt.position_out_of_bounds);
    lua_pushinteger(L, size);
    lua_getik(L, 1, pos, (long) pos, tremovek_first); /* result = t[pos] */
    return tremove_move(L, pos, AT_LOOP);
  }

  private static int tremovek_len(lua_State L, int status, long ctx)
  {
    return tremove_len(L, luaL_lenresult(L));
  }

  private static final lua_KFunction tremovek_len = Ltablib::tremovek_len;

  private static final KNamer knamer_tremovek_len = new KNamer("table.tremovek_len", tremovek_len);

  private static int tremove(lua_State L)
  {
    lua_settop(L, 2);
    luaE_freezeonly(L);
    return tremove_len(L, aux_getnk(L, 1, TAB_RW, 0, tremovek_len));
  }

  private static final lua_CFunction tremove = Ltablib::tremove;

  /*
  ** Copy elements (1[f], ..., 1[e]) into (tt[t], tt[t+1], ...). Whenever
  ** possible, copy in increasing order, which is better for rehashing.
  ** "possible" means destination after original range, or smaller
  ** than origin, or copying to another table. (Arguments set to 5; 'i'
  ** counts from 0 up, or from 'n' - 1 down: 'dir'.)
  */
  /* (continuations: going up or down, at a place in the element 'ctx') */
  private static int tmovek_upset(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, 1, AT_SET);
  }

  private static final lua_KFunction tmovek_upset = Ltablib::tmovek_upset;

  private static int tmovek_upnext(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, 1, AT_NEXT);
  }

  private static final lua_KFunction tmovek_upnext = Ltablib::tmovek_upnext;

  private static int tmovek_uploop(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, 1, AT_LOOP);
  }

  private static final lua_KFunction tmovek_uploop = Ltablib::tmovek_uploop;

  private static int tmovek_downset(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, -1, AT_SET);
  }

  private static final lua_KFunction tmovek_downset = Ltablib::tmovek_downset;

  private static int tmovek_downnext(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, -1, AT_NEXT);
  }

  private static final lua_KFunction tmovek_downnext = Ltablib::tmovek_downnext;

  private static int tmovek_downloop(lua_State L, int status, long ctx)
  {
    return tmove_loop(L, (long) ctx, -1, AT_LOOP);
  }

  private static final lua_KFunction tmovek_downloop = Ltablib::tmovek_downloop;

  private static final KNamer knamer_tmovek_upset = new KNamer("table.tmovek_upset", tmovek_upset);
  private static final KNamer knamer_tmovek_upnext = new KNamer("table.tmovek_upnext", tmovek_upnext);
  private static final KNamer knamer_tmovek_uploop = new KNamer("table.tmovek_uploop", tmovek_uploop);
  private static final KNamer knamer_tmovek_downset = new KNamer("table.tmovek_downset", tmovek_downset);
  private static final KNamer knamer_tmovek_downnext = new KNamer("table.tmovek_downnext", tmovek_downnext);
  private static final KNamer knamer_tmovek_downloop = new KNamer("table.tmovek_downloop", tmovek_downloop);

  private static int tmove_loop(lua_State L, long i, int dir, int at)
  {
    long f = luaL_checkinteger(L, 2);
    long e = luaL_checkinteger(L, 3);
    long t = luaL_checkinteger(L, 4);
    int tt = lua_isnil(L, 5) == 0 ? 5 : 1; /* destination table */
    long n = e - f + 1;         /* number of elements to move */
    boolean up = (dir > 0);
    for (;;)
    {
      switch (at)
      {
      case AT_LOOP:
        if (up ? i >= n : i < 0)
        {
          lua_pushvalue(L, tt); /* return destination table */
          return 1;
        }
        if (lua_isplaintable(L, 1) != 0 && lua_isplaintable(L, tt) != 0)
        { /* (reads and writes call nothing) */
          if (up)
            i += lua_plainmove(L, 1, f + i, tt, t + i, n - i, 1);
          else
            i -= lua_plainmove(L, 1, f, tt, t, i + 1, 0);
          if (luaE_budgetout(L))
            lua_checkpointk(L, (long) i, up ? tmovek_uploop : tmovek_downloop);
          break;
        }
        lua_getik(L, 1, f + i, (long) i, up ? tmovek_upset : tmovek_downset);
      case AT_SET:
        lua_setik(L, tt, t + i, (long) i, up ? tmovek_upnext : tmovek_downnext);
      default: /* AT_NEXT */
        i += dir;
        at = AT_LOOP;
        if (luaE_spent(L, 1))
          lua_checkpointk(L, (long) i, up ? tmovek_uploop : tmovek_downloop);
      }
    }
  }

  /* 'move' after comparing the tables: they are not the same, go up */
  private static int tmove_dir(lua_State L, int up)
  {
    long n = luaL_checkinteger(L, 3) - luaL_checkinteger(L, 2) + 1;
    return up != 0 ? tmove_loop(L, 0, 1, AT_LOOP) : tmove_loop(L, n - 1, -1, AT_LOOP);
  }

  private static int tmovek_eq(lua_State L, int status, long ctx)
  {
    int eq = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return tmove_dir(L, (eq == 0) ? 1 : 0);
  }

  private static final lua_KFunction tmovek_eq = Ltablib::tmovek_eq;

  private static final KNamer knamer_tmovek_eq = new KNamer("table.tmovek_eq", tmovek_eq);

  private static int tmove(lua_State L)
  {
    long f = luaL_checkinteger(L, 2);
    long e = luaL_checkinteger(L, 3);
    long t = luaL_checkinteger(L, 4);
    int tt = lua_isnoneornil(L, 5) == 0 ? 5 : 1; /* destination table */
    checktab(L, 1, TAB_R);
    checktab(L, tt, TAB_W);
    lua_settop(L, 5);
    luaE_freezeonly(L);
    if (e >= f)
    { /* otherwise, nothing to move */
      luaL_argchecki(L, f > 0 || e < LUA_MAXINTEGER + f, 3, Lt.too_many_elements_to_move);
      long n = e - f + 1; /* number of elements to move */
      luaL_argchecki(L, t <= LUA_MAXINTEGER - n + 1, 4, Lt.destination_wrap_around);
      if (t > e || t <= f)
        return tmove_dir(L, 1);
      else if (tt == 1)
        return tmove_dir(L, 0);
      int eq = lua_comparek(L, 1, tt, LUA_OPEQ, 0, tmovek_eq);
      lua_pop(L, 1);
      return tmove_dir(L, (eq == 0) ? 1 : 0);
    }
    lua_pushvalue(L, tt); /* return destination table */
    return 1;
  }

  private static final lua_CFunction tmove = Ltablib::tmove;

  /*
  ** 'concat' (arguments set to 4, the last index at 4; a box at 5, made
  ** where something may freeze): adds t[i], a separator, ..., t[last].
  */
  private static final int CONCATCHUNK = 64; /* elements read at a time from a plain table */

  private static int tconcat_loop(lua_State L, luaL_Buffer b, luaL_Box box, long i, int at)
  {
    long last = lua_tointeger(L, 4);
    ByteView sep = lua_tolstring(L, 2);
    if (at == AT_LOOP && i > last) /* empty interval? */
    {
      luaL_pushresult(b);
      return 1;
    }
    for (;;)
    {
      if (at == AT_LOOP && lua_isplaintable(L, 1) != 0)
      { /* (reads call nothing): a chunk at a time */
        long left = last - i; /* elements after 'i' */
        int chunk = lua_checkstack(L, CONCATCHUNK + LUA_MINSTACK) != 0 ? CONCATCHUNK : 1;
        int got = lua_plaingetrange(L, 1, i, (left < chunk) ? (int) left + 1 : chunk);
        int first = lua_gettop(L) - got + 1;
        for (int k = 0; k < got; k++, i++)
        {
          ByteView v = lua_tolstring(L, first + k);
          if (v == null)
          {
            lua_pushistringref(L, lua_typenamei(L, lua_type(L, first + k)));
            lua_pushinteger(L, i);
            luaL_errori(L, Lt.invalid_value_s_at_index_I_in_table_for_concat);
          }
          luaL_addlstring(b, v, 0, v.size());
          if (i == last)
          {
            lua_pop(L, got);
            luaL_pushresult(b);
            return 1;
          }
          luaL_addlstring(b, sep, 0, sep.size());
        }
        lua_pop(L, got);
        if (luaE_budgetout(L))
        {
          if (box == null)
          {
            assert lua_gettop(L) == 4;
            box = luaL_newbox(L);
          }
          luaL_buffsave(b, box);
          lua_checkpointk(L, (long) i, tconcatk_loop);
          luaL_buffload(b, box);
        }
        continue;
      }
      if (at == AT_LOOP)
      {
        if (box != null)
          luaL_buffsave(b, box);
        lua_getik(L, 1, i, (long) i, tconcatk_get);
        if (box != null)
          luaL_buffload(b, box);
      }
      if (lua_isstring(L, -1) == 0)
      {
        lua_pushistringref(L, lua_typenamei(L, lua_type(L, -1)));
        lua_pushinteger(L, i);
        luaL_errori(L, Lt.invalid_value_s_at_index_I_in_table_for_concat);
      }
      luaL_addvalue(b);
      if (i == last)
        break;
      luaL_addlstring(b, sep, 0, sep.size());
      i++;
      at = AT_LOOP;
      if (luaE_spent(L, 1))
      {
        if (box == null)
        {
          assert lua_gettop(L) == 4;
          box = luaL_newbox(L);
        }
        luaL_buffsave(b, box);
        lua_checkpointk(L, (long) i, tconcatk_loop);
        luaL_buffload(b, box);
      }
    }
    luaL_pushresult(b);
    return 1;
  }

  private static int tconcat_resume(lua_State L, long i, int at)
  {
    luaL_Box box = luaL_tobox(L, 5);
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      luaL_buffload(b, box);
      return tconcat_loop(L, b, box, i, at);
    }
    finally
    {
      b.release();
    }
  }

  private static int tconcatk_get(lua_State L, int status, long ctx)
  {
    return tconcat_resume(L, (long) ctx, AT_SET);
  }

  private static final lua_KFunction tconcatk_get = Ltablib::tconcatk_get;

  private static final KNamer knamer_tconcatk_get = new KNamer("table.tconcatk_get", tconcatk_get);

  private static int tconcatk_loop(lua_State L, int status, long ctx)
  {
    return tconcat_resume(L, (long) ctx, AT_LOOP);
  }

  private static final lua_KFunction tconcatk_loop = Ltablib::tconcatk_loop;

  private static final KNamer knamer_tconcatk_loop = new KNamer("table.tconcatk_loop", tconcatk_loop);

  /* 'concat' with the length 'last' */
  private static int tconcat_len(lua_State L, long last)
  {
    luaL_optlstring(L, 2, Lt.empty_str);
    long i = luaL_optinteger(L, 3, 1);
    last = luaL_optinteger(L, 4, last);
    if (lua_isnil(L, 2) != 0)
    {
      lua_pushistringref(L, Lt.empty_str);
      lua_replace(L, 2);
    }
    lua_pushinteger(L, last);
    lua_replace(L, 4);
    luaL_Box box = null;
    if (lua_type(L, 1) != LUA_TTABLE || lua_getmetatable(L, 1) != 0)
    { /* reading may call back */
      lua_settop(L, 4);
      box = luaL_newbox(L);
    }
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      return tconcat_loop(L, b, box, i, AT_LOOP);
    }
    finally
    {
      b.release();
    }
  }

  private static int tconcatk_len(lua_State L, int status, long ctx)
  {
    return tconcat_len(L, luaL_lenresult(L));
  }

  private static final lua_KFunction tconcatk_len = Ltablib::tconcatk_len;

  private static final KNamer knamer_tconcatk_len = new KNamer("table.tconcatk_len", tconcatk_len);

  private static int tconcat(lua_State L)
  {
    lua_settop(L, 4);
    luaE_freezeonly(L);
    return tconcat_len(L, aux_getnk(L, 1, TAB_R, 0, tconcatk_len));
  }

  private static final lua_CFunction tconcat = Ltablib::tconcat;

  /*
  ** {======================================================
  ** Pack/unpack
  ** =======================================================
  */

  private static int tpack(lua_State L)
  {
    int i;
    int n = lua_gettop(L);    /* number of elements to pack */
    lua_createtable(L, n, 1); /* create result table */
    lua_insert(L, 1);         /* put it at index 1 */
    for (i = n; i >= 1; i--)  /* assign elements */
      lua_seti(L, 1, i);
    lua_pushinteger(L, n);
    lua_setfieldi(L, 1, Lt.n_char); /* t.n = number of elements */
    return 1;                /* return table */
  }

  private static final lua_CFunction tpack = Ltablib::tpack;

  /*
  ** 'unpack' (arguments set to 3, the last index at 3): pushes t[i] ..
  ** t[e] from 4 on; how many it pushed says where it goes on.
  */

  private static int tunpack_loop(lua_State L, long i, int n, int count, int at)
  {
    for (;;)
    {
      if (at == AT_LOOP)
      {
        if (lua_isplaintable(L, 1) != 0)
        { /* (reads call nothing) */
          count += lua_plaingetrange(L, 1, i + count, n - count);
          if (count == n)
            return n;
          lua_pushinteger(L, i + n - 1); /* the last index, for after a freeze */
          lua_replace(L, 3);
          lua_checkpointk(L, 0, tunpackk_loop); /* (the budget is spent) */
          continue;
        }
        lua_getik(L, 1, i + count, 0, tunpackk_got);
        count++;
      }
      if (count == n)
        return n;
      at = AT_LOOP;
      if (luaE_spent(L, 1))
        lua_checkpointk(L, 0, tunpackk_loop);
    }
  }

  /* (after a freeze: the first index at 2, the last at 3, the elements
     pushed from 4 on) */
  private static int tunpack_resume(lua_State L, int at)
  {
    long i = luaL_optinteger(L, 2, 1);
    int n = (int) (lua_tointeger(L, 3) - i + 1); /* number of elements */
    return tunpack_loop(L, i, n, lua_gettop(L) - 3, at);
  }

  private static int tunpackk_got(lua_State L, int status, long ctx)
  {
    return tunpack_resume(L, AT_NEXT);
  }

  private static final lua_KFunction tunpackk_got = Ltablib::tunpackk_got;

  private static final KNamer knamer_tunpackk_got = new KNamer("table.tunpackk_got", tunpackk_got);

  private static int tunpackk_loop(lua_State L, int status, long ctx)
  {
    return tunpack_resume(L, AT_LOOP);
  }

  private static final lua_KFunction tunpackk_loop = Ltablib::tunpackk_loop;

  private static final KNamer knamer_tunpackk_loop = new KNamer("table.tunpackk_loop", tunpackk_loop);

  /* 'unpack' with the length 'len' */
  private static int tunpack_len(lua_State L, long len)
  {
    long n;
    long i = luaL_optinteger(L, 2, 1);
    long e = (lua_isnoneornil(L, 3) != 0 ? len : luaL_checkinteger(L, 3));
    if (i > e)
      return 0;                      /* empty range */
    n = l_intsub(e, i); /* number of elements minus 1 */
    if (!l_ult(n, Integer.MAX_VALUE) ||
        lua_checkstack(L, (int) (++n)) == 0)
      return luaL_errori(L, Lt.too_many_results_to_unpack);
    if (lua_isplaintable(L, 1) == 0)
    { /* reads may call back: the last index, for after a freeze */
      lua_pushinteger(L, e);
      lua_replace(L, 3);
    }
    return tunpack_loop(L, i, (int) n, 0, AT_LOOP);
  }

  private static int tunpackk_len(lua_State L, int status, long ctx)
  {
    return tunpack_len(L, luaL_lenresult(L));
  }

  private static final lua_KFunction tunpackk_len = Ltablib::tunpackk_len;

  private static final KNamer knamer_tunpackk_len = new KNamer("table.tunpackk_len", tunpackk_len);

  private static int tunpack(lua_State L)
  {
    lua_settop(L, 3);
    luaE_freezeonly(L);
    return tunpack_len(L, aux_getnk(L, 1, TAB_R, 0, tunpackk_len));
  }

  private static final lua_CFunction tunpack = Ltablib::tunpack;

  /* }====================================================== */

  /*
  ** {======================================================
  ** Quicksort
  ** (based on 'Algorithms in MODULA-3', Robert Sedgewick;
  **  Addison-Wesley, 1993.)
  ** =======================================================
  */

  /*
  ** Type for array indices. These indices are always limited by INT_MAX,
  ** so it is safe to cast them to lua_Integer even for Lua 32 bits.
  */

  /* Versions of lua_seti/lua_geti specialized for IdxT */

  /*
  ** Produce a "random" 'unsigned int' to randomize pivot choice. This
  ** macro is used only when 'sort' detects a big imbalance in the result
  ** of a partition. (If you don't want/need this "randomness", ~0 is a
  ** good choice.)
  */

  /* arrays larger than 'RANLIMIT' may use randomized pivots */
  private static final int RANLIMIT = 100;

  /*
  ** The sort runs the quicksort of stock Lua, with its recursion on the
  ** smaller interval made a stack of pending intervals (the larger one,
  ** where the caller goes on afterwards, with its randomization). It can
  ** stop in the middle: before every step that may call back (a read or a
  ** write through a metamethod, a comparison by a function or '__lt') and
  ** at every check point, it saves its variables and where it goes on
  ** ('SO_PC', a case label) into its state, for 'sortk' to go on there
  ** after a freeze. The state is in C (on the C stack) until something may
  ** call back or freeze: then it moves into a userdata at 3 (arguments set
  ** to 3), to stay there. The values it works on are on the stack from 4 on.
  */
  private static final int
      SO_PC = 0,                /* where it goes on */
      SO_LO = 1, SO_UP = 2, SO_RND = 3, /* the interval being sorted, its randomization */
      SO_P = 4,                 /* the pivot */
      SO_I = 5, SO_J = 6,           /* the partition's indices */
      SO_R = 7,                 /* result of the last comparison (-1: on the stack) */
      SO_FN = 8,                /* is there a comparison function? */
      SO_PLAIN = 9,             /* is the table plain (reads and writes call nothing)? */
      SO_BOXED = 10,             /* is the state in the userdata? */
      SO_NINTS = 11,             /* size of the state */
      SO_N = 12,                 /* number of pending intervals */
      SO_PENDING = 13;            /* pending intervals: lo, up, n, rnd */

  /* the pending intervals: one for each level of recursion of stock Lua,
     which recurs on intervals at most half as big (n < INT_MAX) */
  private static final int SORT_MAXDEPTH = 33;

  /*
  ** Where it goes on: after a read or a write, or after a comparison (these
  ** are kept outside the process); and the heads of its loops and branches,
  ** where it is never kept.
  */
  private static final int
      S_START = 0, S_LO1 = 1, S_LO2 = 2, S_LO3 = 3, S_LO4 = 4, S_P1 = 5, S_P2 = 6, S_P3 = 7, S_P4 = 8, S_P5 = 9,
      S_P6 = 10, S_P7 = 11, S_P8 = 12, S_P9 = 13, S_P10 = 14, S_P11 = 15, S_I1 = 16, S_J1 = 17, S_SW1 = 18,
      S_SW2 = 19, S_SW3 = 20, S_SW4 = 21,
      S_LT1 = 22, S_LT2 = 23, S_LT3 = 24, S_LT4 = 25, S_LT5 = 26, /* (comparisons last) */
      S_NKEPT = 27,
      S_WHILE = S_NKEPT, /* the head of the loop over an interval */
      S_PIVOT = 28,           /* after 'lo' and 'up' are in order */
      S_ELSEP = 29,           /* a[p] >= a[lo]: compare a[up] with a[p] */
      S_PIVOTED = 30,         /* 'lo', 'p' and 'up' are in order */
      S_ILOOP = 31,           /* repeat ++i while a[i] < P */
      S_JLOOP = 32,           /* repeat --j while P < a[j] */
      S_PARTED = 33,          /* no elements out of place */
      S_NEXT = 34;             /* an interval is sorted */

  /* the most integers of a sort's state */
  private static final int SORT_NINTS = SO_PENDING + 4 * SORT_MAXDEPTH;
  private static final int SORTBOX_SIZE = 8 * SORT_NINTS + 16; /* (its accounted size) */

  /*
  ** The state of a sort that can call back, in its own userdata (at 3); kept
  ** outside the process as the bytes of its integers, little-endian
  */
  private static final class SortBox implements lua_UserObject
  {
    long[] s = new long[SORT_NINTS];
    ByteOwner image = new ByteOwner(8 * SORT_NINTS);

    @Override
    public String persistkind()
    {
      return "table.sort";
    }

    @Override
    public ByteView persistbytes()
    {
      int n = (int) s[SO_NINTS];
      for (int k = 0; k < n; k++)
        image.write_int64(s[k], 8 * k);
      return image.view().substr(0, 8 * n);
    }
  }

  /* a sort box from its state, checked to be one */
  private static lua_UserObject makesortbox(lua_State L, long[] ints, int nints, ByteView bytes)
  {
    if (nints != 0 || bytes.size() % 8 != 0 || bytes.size() < 8 * (SO_PENDING + 4) ||
        bytes.size() > 8 * SORT_NINTS)
      return null;
    SortBox box = new SortBox();
    for (int k = 0; k < bytes.size() / 8; k++)
      box.s[k] = bytes.toInt64(8 * k);
    long[] v = box.s;
    if (v[SO_NINTS] != (long) (bytes.size() / 8) || v[SO_BOXED] != 1 || v[SO_PC] < 0 ||
        v[SO_PC] >= S_NKEPT || v[SO_N] < 0 || v[SO_N] >= SORT_MAXDEPTH ||
        SO_PENDING + 4 * v[SO_N] > v[SO_NINTS] ||
        (v[SO_FN] != 0 && v[SO_FN] != 1) || (v[SO_PLAIN] != 0 && v[SO_PLAIN] != 1) || v[SO_R] < -1 ||
        v[SO_R] > 1)
      return null;
    return box;
  }

  private static final lua_ObjectFactory makesortbox = Ltablib::makesortbox;

  private static final class SortBoxKind
  {
    SortBoxKind()
    {
      lua_registerobjectkind("table.sort", makesortbox);
    }
  }

  private static final SortBoxKind sortboxkind = new SortBoxKind();

  /* the state moved into a new userdata at 3 (the first time it can call back) */
  private static long[] sort_box(lua_State L, long[] s)
  {
    if (s[SO_BOXED] != 0)
      return s;
    SortBox o = new SortBox();
    SortBox box = o;
    lua_newuserobject(L, o, SORTBOX_SIZE, 0);
    lua_replace(L, 3);
    for (long k = 0; k < s[SO_NINTS]; k++)
      box.s[(int) k] = s[(int) k];
    box.s[SO_BOXED] = 1;
    return box.s;
  }

  /*
  ** Whether the value at stack index 'a' is less than the value at index
  ** 'b' (according to the order of the sort), by the function or '__lt'.
  */
  private static int sort_call(lua_State L, int fn, int a, int b)
  {
    if (fn == 0) /* no function? */
      lua_comparek(L, a, b, LUA_OPLT, 0, sortk); /* a < b */
    else
    { /* function */
      lua_pushvalue(L, 2);     /* push function */
      lua_pushvalue(L, a - 1); /* -1 to compensate function */
      lua_pushvalue(L, b - 2); /* -2 to compensate function and 'a' */
      lua_callk(L, 2, 1, 0, sortk); /* call function */
    }
    int res = lua_toboolean(L, -1); /* get result */
    lua_pop(L, 1);                  /* pop result */
    return res;
  }

  /* the result of the comparison it goes on after */
  private static int sort_result(lua_State L, long[] s)
  {
    if (s[SO_R] >= 0)
      return (int) s[SO_R];
    int res = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return res;
  }

  /*
  ** Choose an element in the middle (2nd-3th quarters) of [lo,up]
  ** "randomized" by 'rnd'
  */
  private static int choosePivot(int lo, int up, int rnd)
  {
    int r4 = (up - lo) / 4; /* range/4 */
    int p = (int) l_umod(Integer.toUnsignedLong(rnd ^ lo ^ up), r4 * 2) + (lo + r4);
    assert lo + r4 <= p && p <= up - r4;
    return p;
  }

  /* the variables kept, to go on at 'pc' (the state in its userdata) */
  private static long[] sort_save(lua_State L, long[] s, int pc, long lo, long up,
                                  long rnd, long p, long i, long j)
  {
    s = sort_box(L, s);
    s[SO_PC] = pc;
    s[SO_LO] = lo;
    s[SO_UP] = up;
    s[SO_RND] = rnd;
    s[SO_P] = p;
    s[SO_I] = i;
    s[SO_J] = j;
    return s;
  }

  /*
  ** Stock Lua's 'auxsort', as steps: each read, write or comparison that can
  ** call back keeps the variables first, and the continuation goes on at the
  ** step after it (a comparison by '__lt' of plain values, or of numbers or
  ** strings, calls nothing). Each step falls through to the next; a loop or
  ** a branch goes on at the step it names.
  */
  private static int sort_run(lua_State L, long[] s)
  {
    int fn = (int) s[SO_FN];
    boolean plain = (s[SO_PLAIN] != 0);
    long lo = s[SO_LO], up = s[SO_UP], p = s[SO_P], i = s[SO_I], j = s[SO_J];
    long rnd = s[SO_RND];
    int res = 0;
    int pc = (int) s[SO_PC];
    if (pc >= S_LT1)
      res = sort_result(L, s);
    for (;;)
    {
      switch (pc)
      {
      case S_START:
      case S_WHILE: /* loop for tail recursion */
        if (lo >= up)
        {
          pc = S_NEXT;
          continue;
        }
        /* sort elements 'lo', 'p', and 'up' */
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_LO1, lo, up, rnd, p, i, j);
        lua_getik(L, 1, lo, 0, sortk);
      case S_LO1:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_LO2, lo, up, rnd, p, i, j);
        lua_getik(L, 1, up, 0, sortk);
      case S_LO2:
        if (fn != 0 || (res = lua_rawlessthan(L, -1, -2)) < 0) /* a[up] < a[lo]? */
        { /* it calls back */
          s = sort_save(L, s, S_LT1, lo, up, rnd, p, i, j);
          s[SO_R] = -1; /* (after a freeze, the result is on the stack) */
          res = sort_call(L, fn, -1, -2);
        }
        if (luaE_spent(L, 1))
        {
          s = sort_save(L, s, S_LT1, lo, up, rnd, p, i, j);
          s[SO_R] = res;
          lua_checkpointk(L, 0, sortk);
        }
      case S_LT1:
        if (res == 0)
        {
          lua_pop(L, 2); /* remove both values */
          pc = S_PIVOT;
          continue;
        }
        if (s[SO_BOXED] != 0 || !plain) /* swap a[lo] - a[up] */
          s = sort_save(L, s, S_LO3, lo, up, rnd, p, i, j);
        lua_setik(L, 1, lo, 0, sortk);
      case S_LO3:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_LO4, lo, up, rnd, p, i, j);
        lua_setik(L, 1, up, 0, sortk);
      case S_LO4:
      case S_PIVOT:
        if (up - lo == 1) /* only 2 elements? */
        {               /* already sorted */
          pc = S_NEXT;
          continue;
        }
        if (up - lo < RANLIMIT || rnd == 0) /* small interval or no randomize? */
          p = lo + (up - lo) / 2; /* middle element is a good pivot */
        else /* for larger intervals, it is worth a random pivot */
          p = choosePivot((int) lo, (int) up, (int) rnd);
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P1, lo, up, rnd, p, i, j);
        lua_getik(L, 1, p, 0, sortk);
      case S_P1:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P2, lo, up, rnd, p, i, j);
        lua_getik(L, 1, lo, 0, sortk);
      case S_P2:
        if (fn != 0 || (res = lua_rawlessthan(L, -2, -1)) < 0) /* a[p] < a[lo]? */
        {
          s = sort_save(L, s, S_LT2, lo, up, rnd, p, i, j);
          s[SO_R] = -1;
          res = sort_call(L, fn, -2, -1);
        }
        if (luaE_spent(L, 1))
        {
          s = sort_save(L, s, S_LT2, lo, up, rnd, p, i, j);
          s[SO_R] = res;
          lua_checkpointk(L, 0, sortk);
        }
      case S_LT2:
        if (res == 0)
        {
          lua_pop(L, 1); /* remove a[lo] */
          pc = S_ELSEP;
          continue;
        }
        if (s[SO_BOXED] != 0 || !plain) /* swap a[p] - a[lo] */
          s = sort_save(L, s, S_P3, lo, up, rnd, p, i, j);
        lua_setik(L, 1, p, 0, sortk);
      case S_P3:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P4, lo, up, rnd, p, i, j);
        lua_setik(L, 1, lo, 0, sortk);
      case S_P4:
        pc = S_PIVOTED;
        continue;
      case S_ELSEP:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P5, lo, up, rnd, p, i, j);
        lua_getik(L, 1, up, 0, sortk);
      case S_P5:
        if (fn != 0 || (res = lua_rawlessthan(L, -1, -2)) < 0) /* a[up] < a[p]? */
        {
          s = sort_save(L, s, S_LT3, lo, up, rnd, p, i, j);
          s[SO_R] = -1;
          res = sort_call(L, fn, -1, -2);
        }
        if (luaE_spent(L, 1))
        {
          s = sort_save(L, s, S_LT3, lo, up, rnd, p, i, j);
          s[SO_R] = res;
          lua_checkpointk(L, 0, sortk);
        }
      case S_LT3:
        if (res == 0)
        {
          lua_pop(L, 2);
          pc = S_PIVOTED;
          continue;
        }
        if (s[SO_BOXED] != 0 || !plain) /* swap a[up] - a[p] */
          s = sort_save(L, s, S_P6, lo, up, rnd, p, i, j);
        lua_setik(L, 1, p, 0, sortk);
      case S_P6:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P7, lo, up, rnd, p, i, j);
        lua_setik(L, 1, up, 0, sortk);
      case S_P7:
      case S_PIVOTED:
        if (up - lo == 2) /* only 3 elements? */
        {               /* already sorted */
          pc = S_NEXT;
          continue;
        }
        if (s[SO_BOXED] != 0 || !plain) /* get middle element (Pivot) */
          s = sort_save(L, s, S_P8, lo, up, rnd, p, i, j);
        lua_getik(L, 1, p, 0, sortk);
      case S_P8:
        lua_pushvalue(L, -1); /* push Pivot */
        if (s[SO_BOXED] != 0 || !plain) /* push a[up - 1] */
          s = sort_save(L, s, S_P9, lo, up, rnd, p, i, j);
        lua_getik(L, 1, up - 1, 0, sortk);
      case S_P9:
        if (s[SO_BOXED] != 0 || !plain) /* swap Pivot (a[p]) with a[up - 1] */
          s = sort_save(L, s, S_P10, lo, up, rnd, p, i, j);
        lua_setik(L, 1, p, 0, sortk);
      case S_P10:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_P11, lo, up, rnd, p, i, j);
        lua_setik(L, 1, up - 1, 0, sortk);
      case S_P11:
        /* the partition: Pivot P is at the top of the stack.
           precondition: a[lo] <= P == a[up-1] <= a[up],
           so it only needs to do the partition from lo + 1 to up - 2.
           Pos-condition: a[lo .. i - 1] <= a[i] == P <= a[i + 1 .. up];
           loop invariant: a[lo .. i] <= P <= a[j .. up] */
        i = lo;     /* will be incremented before first use */
        j = up - 1; /* will be decremented before first use */
      case S_ILOOP: /* repeat ++i while a[i] < P */
        i++;
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_I1, lo, up, rnd, p, i, j);
        lua_getik(L, 1, i, 0, sortk);
      case S_I1:
        if (fn != 0 || (res = lua_rawlessthan(L, -1, -2)) < 0)
        {
          s = sort_save(L, s, S_LT4, lo, up, rnd, p, i, j);
          s[SO_R] = -1;
          res = sort_call(L, fn, -1, -2);
        }
        if (luaE_spent(L, 1))
        {
          s = sort_save(L, s, S_LT4, lo, up, rnd, p, i, j);
          s[SO_R] = res;
          lua_checkpointk(L, 0, sortk);
        }
      case S_LT4:
        if (res != 0)
        {
          if (i == up - 1) /* a[up - 1] < P == a[up - 1] */
            luaL_errori(L, Lt.invalid_order_function_for_sorting);
          lua_pop(L, 1); /* remove a[i] */
          pc = S_ILOOP;
          continue;
        }
        /* after the loop, a[i] >= P and a[lo .. i - 1] < P  (a) */
      case S_JLOOP: /* repeat --j while P < a[j] */
        j--;
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_J1, lo, up, rnd, p, i, j);
        lua_getik(L, 1, j, 0, sortk);
      case S_J1:
        if (fn != 0 || (res = lua_rawlessthan(L, -3, -1)) < 0)
        {
          s = sort_save(L, s, S_LT5, lo, up, rnd, p, i, j);
          s[SO_R] = -1;
          res = sort_call(L, fn, -3, -1);
        }
        if (luaE_spent(L, 1))
        {
          s = sort_save(L, s, S_LT5, lo, up, rnd, p, i, j);
          s[SO_R] = res;
          lua_checkpointk(L, 0, sortk);
        }
      case S_LT5:
        if (res != 0)
        {
          if (j < i) /* j <= i - 1 and a[j] > P, contradicts (a) */
            luaL_errori(L, Lt.invalid_order_function_for_sorting);
          lua_pop(L, 1); /* remove a[j] */
          pc = S_JLOOP;
          continue;
        }
        /* after the loop, a[j] <= P and a[j + 1 .. up] >= P */
        if (j < i)
        { /* no elements out of place? */
          pc = S_PARTED;
          continue;
        }
        /* otherwise, swap a[i] - a[j] to restore invariant and repeat */
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_SW3, lo, up, rnd, p, i, j);
        lua_setik(L, 1, i, 0, sortk);
      case S_SW3:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_SW4, lo, up, rnd, p, i, j);
        lua_setik(L, 1, j, 0, sortk);
      case S_SW4:
        pc = S_ILOOP;
        continue;
      case S_PARTED:
        /* a[lo .. i - 1] <= P <= a[j + 1 .. i .. up] */
        lua_pop(L, 1); /* pop a[j] */
        /* swap pivot (a[up - 1]) with a[i] to satisfy pos-condition */
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_SW1, lo, up, rnd, p, i, j);
        lua_setik(L, 1, up - 1, 0, sortk);
      case S_SW1:
        if (s[SO_BOXED] != 0 || !plain)
          s = sort_save(L, s, S_SW2, lo, up, rnd, p, i, j);
        lua_setik(L, 1, i, 0, sortk);
      case S_SW2:
      {
        p = i; /* a[lo .. p - 1] <= a[p] == P <= a[p + 1 .. up] */
        int pend = (int) (SO_PENDING + 4 * s[SO_N]++);
        if (p - lo < up - p)
        { /* lower interval is smaller? sort it first, then [p + 1 .. up] */
          s[pend] = p + 1;
          s[pend + 1] = up;
          s[pend + 2] = p - lo; /* size of smaller interval */
          up = p - 1;
        }
        else
        { /* sort [p + 1 .. up] first, then [lo .. p - 1] */
          s[pend] = lo;
          s[pend + 1] = p - 1;
          s[pend + 2] = up - p; /* size of smaller interval */
          lo = p + 1;
        }
        s[pend + 3] = rnd;
        pc = S_WHILE;
        continue;
      }
      default: /* S_NEXT: an interval is sorted: go on with the one pending */
      {
        if (s[SO_N] == 0)
          return 0;
        int pend = (int) (SO_PENDING + 4 * --s[SO_N]);
        lo = s[pend];
        up = s[pend + 1];
        rnd = s[pend + 3];
        if ((up - lo) / 128 > s[pend + 2]) /* partition too imbalanced? */
          rnd = Integer.toUnsignedLong(luaL_makeseed(L));           /* try a new randomization */
        pc = S_WHILE;
        continue;
      }
      }
    }
  }

  private static int sortk(lua_State L, int status, long ctx)
  {
    SortBox box = (lua_touserobject(L, 3) instanceof SortBox sb) ? sb : null;
    if (box == null)
      luaL_errori(L, Lt.invalid_sort_state);
    return sort_run(L, box.s);
  }

  private static final lua_KFunction sortk = Ltablib::sortk;

  private static final KNamer knamer_sortk = new KNamer("table.sortk", sortk);

  /* 'sort' with the length 'n' */
  private static int sort_len(lua_State L, long n)
  {
    if (n > 1)
    { /* non-trivial interval? */
      luaL_argchecki(L, n < Integer.MAX_VALUE, 1, Lt.array_too_big);
      if (lua_isnoneornil(L, 2) == 0)            /* is there a 2nd argument? */
        luaL_checktype(L, 2, LUA_TFUNCTION); /* must be a function */
      lua_settop(L, 3);                      /* make sure there are two arguments (and room for the state) */
      int depth = (64 - Long.numberOfLeadingZeros(n)) + 1;
      assert depth <= SORT_MAXDEPTH;
      long[] state = new long[SORT_NINTS];
      long[] s = state;
      s[SO_PC] = S_START;
      s[SO_LO] = 1;
      s[SO_UP] = n;
      s[SO_RND] = 0;
      s[SO_FN] = (lua_isnil(L, 2) == 0) ? 1 : 0;
      s[SO_PLAIN] = lua_isplaintable(L, 1);
      s[SO_P] = s[SO_I] = s[SO_J] = 0;
      s[SO_R] = 0;
      s[SO_BOXED] = 0;
      s[SO_NINTS] = SO_PENDING + 4 * depth;
      s[SO_N] = 0;
      if (s[SO_FN] != 0 || s[SO_PLAIN] == 0) /* will it call back? */
        s = sort_box(L, s);
      return sort_run(L, s);
    }
    return 0;
  }

  private static int sortk_len(lua_State L, int status, long ctx)
  {
    return sort_len(L, luaL_lenresult(L));
  }

  private static final lua_KFunction sortk_len = Ltablib::sortk_len;

  private static final KNamer knamer_sortk_len = new KNamer("table.sortk_len", sortk_len);

  private static int sort(lua_State L)
  {
    luaE_freezeonly(L);
    return sort_len(L, aux_getnk(L, 1, TAB_RW, 0, sortk_len));
  }

  private static final lua_CFunction sort = Ltablib::sort;

  /* }====================================================== */

  private static final CFuncInfo cfi_tconcat = new CFuncInfo(tconcat, "table.tconcat");
  private static final CFuncInfo cfi_tcreate = new CFuncInfo(tcreate, "table.tcreate");
  private static final CFuncInfo cfi_tinsert = new CFuncInfo(tinsert, "table.tinsert");
  private static final CFuncInfo cfi_tpack = new CFuncInfo(tpack, "table.tpack");
  private static final CFuncInfo cfi_tunpack = new CFuncInfo(tunpack, "table.tunpack");
  private static final CFuncInfo cfi_tremove = new CFuncInfo(tremove, "table.tremove");
  private static final CFuncInfo cfi_tmove = new CFuncInfo(tmove, "table.tmove");
  private static final CFuncInfo cfi_sort = new CFuncInfo(sort, "table.sort");

  private static final luaL_RegCFI[] tab_funcs = {
    new luaL_RegCFI(Lt.concat, cfi_tconcat),
    new luaL_RegCFI(Lt.create, cfi_tcreate),
    new luaL_RegCFI(Lt.insert, cfi_tinsert),
    new luaL_RegCFI(Lt.pack, cfi_tpack),
    new luaL_RegCFI(Lt.unpack, cfi_tunpack),
    new luaL_RegCFI(Lt.remove_kw, cfi_tremove),
    new luaL_RegCFI(Lt.move, cfi_tmove),
    new luaL_RegCFI(Lt.sort, cfi_sort),
  };

  public static int luaopen_table(lua_State L)
  {
    luaL_newlibcfi(L, tab_funcs);
    return 1;
  }

  public static final lua_CFunction luaopen_table = Ltablib::luaopen_table;
}
