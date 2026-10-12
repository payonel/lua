/*
** $Id: ltablib.c $
** Library for Table Manipulation
** See Copyright Notice in lua.h
*/



#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <bit>
#include <optional>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "lua_internal.h"
#include "lobject.h"
#include "lstate.h"
#include "literals.h"

namespace lua::lib
{

/*
** Operations that an object must define to mimic a table
** (some functions only need some of them)
*/
constexpr int TAB_R = 1; /* read */
constexpr int TAB_W = 2; /* write */
constexpr int TAB_L = 4; /* length */
constexpr int TAB_RW = TAB_R | TAB_W; /* read/write */


static int checkfield(lua_State* L, byte_view key, int n)
{
  lua_pushlstring(L, key, 0, key.size());
  return (lua_rawget(L, -n) != LUA_TNIL);
}

/*
** Check that 'arg' either is a table or can behave like one (that is,
** has a metatable with the required metamethods).
*/
static void checktab(lua_State* L, int arg, int what)
{
  int tp = lua_type(L, arg);
  if (tp != LUA_TTABLE)
  {                                 /* is it not a table? */
    int n = 1;                      /* number of elements to pop */
    if (lua_getmetatable(L, arg) && /* must have metatable */
        (!(what & TAB_R) || checkfield(L, lt::mm_index, ++n)) &&
        (!(what & TAB_W) || checkfield(L, lt::mm_newindex, ++n)) &&
        (!(what & TAB_L) || /* strings don't need '__len' to have a length */
         tp == LUA_TSTRING || checkfield(L, lt::mm_len, ++n)))
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
static inline lua_Integer aux_getnk(lua_State* L, int n, int w, lua_KContext ctx, lua_KFunction k)
{
  if (lua_isplaintable(L, n)) /* (no '__len') */
    return (lua_Integer)lua_rawlen(L, n);
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
static_assert(sizeof(lua_KContext) >= sizeof(lua_Integer));

enum
{
  AT_LOOP, /* at the start of an element */
  AT_SET,  /* the element was read (it is on the top) */
  AT_NEXT  /* the element is done */
};

static int tcreate(lua_State* L)
{
  lua_Integer sizeseq = luaL_checkinteger(L, 1);
  lua_Integer sizerest = luaL_optinteger(L, 2, 0);
  luaL_argchecki(L, 0 <= sizeseq && sizeseq <= INT_MAX, 1, lt::out_of_range);
  luaL_argchecki(L, 0 <= sizerest && sizerest <= INT_MAX, 2, lt::out_of_range);
  lua_createtable(L, (int)sizeseq, (int)sizerest);
  return 1;
}

/*
** 'insert': moves up t[i - 1] .. t[pos] (t[k] = t[k - 1], from 'k' = 'i'
** down), then sets t[pos] = v.
*/
static int tinsertk_get(lua_State* L, int status, lua_KContext ctx);
static int tinsertk_set(lua_State* L, int status, lua_KContext ctx);
static int tinsertk_loop(lua_State* L, int status, lua_KContext ctx);
static int tinsertk_done(lua_State* L, int status, lua_KContext ctx);

static int tinsert_move(lua_State* L, lua_Integer i, lua_Integer pos, int at)
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
      if (lua_isplaintable(L, 1))
      { /* (reads and writes call nothing) */
        i -= lua_plainmove(L, 1, pos, 1, pos + 1, i - pos, 0);
        if (luaE_budgetout(L))
          lua_checkpointk(L, (lua_KContext)i, tinsertk_loop);
        break;
      }
      lua_getik(L, 1, i - 1, (lua_KContext)i, tinsertk_get);
      [[fallthrough]];
    case AT_SET:
      lua_setik(L, 1, i, (lua_KContext)i, tinsertk_set); /* t[i] = t[i - 1] */
      [[fallthrough]];
    default: /* AT_NEXT */
      i--;
      at = AT_LOOP;
      if (luaE_spent(L, 1))
        lua_checkpointk(L, (lua_KContext)i, tinsertk_loop);
    }
  }
}

static int tinsertk_get(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tinsert_move(L, (lua_Integer)ctx, luaL_checkinteger(L, 2), AT_SET);
}

LUA_KNAME("table", tinsertk_get);

static int tinsertk_set(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tinsert_move(L, (lua_Integer)ctx, luaL_checkinteger(L, 2), AT_NEXT);
}

LUA_KNAME("table", tinsertk_set);

static int tinsertk_loop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tinsert_move(L, (lua_Integer)ctx, luaL_checkinteger(L, 2), AT_LOOP);
}

LUA_KNAME("table", tinsertk_loop);

static int tinsertk_done(lua_State* L, int status, lua_KContext ctx)
{
  (void)L;
  (void)status;
  (void)ctx;
  return 0;
}

LUA_KNAME("table", tinsertk_done);

/* 'insert' with the length 'e' */
static int tinsert_len(lua_State* L, lua_Integer e)
{
  lua_Integer pos; /* where to insert new element */
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
    luaL_argchecki(L, l_ult(l_intsub(pos, 1), e), 2, lt::position_out_of_bounds);
    return tinsert_move(L, e, pos, AT_LOOP);
  }
  default:
  {
    return luaL_errori(L, lt::wrong_number_of_arguments_to_insert);
  }
  }
  lua_setik(L, 1, pos, 0, tinsertk_done); /* t[pos] = v */
  return 0;
}

static int tinsertk_len(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tinsert_len(L, luaL_lenresult(L));
}

LUA_KNAME("table", tinsertk_len);

static int tinsert(lua_State* L)
{
  luaE_freezeonly(L);
  return tinsert_len(L, aux_getnk(L, 1, TAB_RW, 0, tinsertk_len));
}

/*
** 'remove' (arguments set to 2; the size at 3, the result at 4): moves
** down t[pos + 1] .. t[size] (t[k] = t[k + 1], from 'k' = 'pos' up), then
** removes t[size].
*/
static int tremovek_get(lua_State* L, int status, lua_KContext ctx);
static int tremovek_set(lua_State* L, int status, lua_KContext ctx);
static int tremovek_loop(lua_State* L, int status, lua_KContext ctx);
static int tremovek_done(lua_State* L, int status, lua_KContext ctx);

static int tremove_move(lua_State* L, lua_Integer pos, int at)
{
  lua_Integer size = lua_tointeger(L, 3);
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
      if (lua_isplaintable(L, 1))
      { /* (reads and writes call nothing) */
        pos += lua_plainmove(L, 1, pos + 1, 1, pos, size - pos, 1);
        if (luaE_budgetout(L))
          lua_checkpointk(L, (lua_KContext)pos, tremovek_loop);
        break;
      }
      lua_getik(L, 1, pos + 1, (lua_KContext)pos, tremovek_get);
      [[fallthrough]];
    case AT_SET:
      lua_setik(L, 1, pos, (lua_KContext)pos, tremovek_set); /* t[pos] = t[pos + 1] */
      [[fallthrough]];
    default: /* AT_NEXT */
      pos++;
      at = AT_LOOP;
      if (luaE_spent(L, 1))
        lua_checkpointk(L, (lua_KContext)pos, tremovek_loop);
    }
  }
}

static int tremovek_get(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tremove_move(L, (lua_Integer)ctx, AT_SET);
}

LUA_KNAME("table", tremovek_get);

static int tremovek_set(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tremove_move(L, (lua_Integer)ctx, AT_NEXT);
}

LUA_KNAME("table", tremovek_set);

static int tremovek_loop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tremove_move(L, (lua_Integer)ctx, AT_LOOP);
}

LUA_KNAME("table", tremovek_loop);

static int tremovek_first(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tremove_move(L, (lua_Integer)ctx, AT_LOOP);
}

LUA_KNAME("table", tremovek_first);

static int tremovek_done(lua_State* L, int status, lua_KContext ctx)
{
  (void)L;
  (void)status;
  (void)ctx;
  return 1;
}

LUA_KNAME("table", tremovek_done);

/* 'remove' with the length 'size' */
static int tremove_len(lua_State* L, lua_Integer size)
{
  lua_Integer pos = luaL_optinteger(L, 2, size);
  if (pos != size) /* validate 'pos' if given */
    /* check whether 'pos' is in [1, size + 1] */
    luaL_argchecki(L, !l_ult(size, l_intsub(pos, 1)), 2, lt::position_out_of_bounds);
  lua_pushinteger(L, size);
  lua_getik(L, 1, pos, (lua_KContext)pos, tremovek_first); /* result = t[pos] */
  return tremove_move(L, pos, AT_LOOP);
}

static int tremovek_len(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tremove_len(L, luaL_lenresult(L));
}

LUA_KNAME("table", tremovek_len);

static int tremove(lua_State* L)
{
  lua_settop(L, 2);
  luaE_freezeonly(L);
  return tremove_len(L, aux_getnk(L, 1, TAB_RW, 0, tremovek_len));
}

/*
** Copy elements (1[f], ..., 1[e]) into (tt[t], tt[t+1], ...). Whenever
** possible, copy in increasing order, which is better for rehashing.
** "possible" means destination after original range, or smaller
** than origin, or copying to another table. (Arguments set to 5; 'i'
** counts from 0 up, or from 'n' - 1 down: 'dir'.)
*/
/* (continuations: going up or down, at a place in the element 'ctx') */
static int tmove_loop(lua_State* L, lua_Integer i, int dir, int at);
static int tmovek_upset(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, 1, AT_SET);
}

static int tmovek_upnext(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, 1, AT_NEXT);
}

static int tmovek_uploop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, 1, AT_LOOP);
}

static int tmovek_downset(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, -1, AT_SET);
}

static int tmovek_downnext(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, -1, AT_NEXT);
}

static int tmovek_downloop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tmove_loop(L, (lua_Integer)ctx, -1, AT_LOOP);
}

LUA_KNAME("table", tmovek_upset);
LUA_KNAME("table", tmovek_upnext);
LUA_KNAME("table", tmovek_uploop);
LUA_KNAME("table", tmovek_downset);
LUA_KNAME("table", tmovek_downnext);
LUA_KNAME("table", tmovek_downloop);

static int tmove_loop(lua_State* L, lua_Integer i, int dir, int at)
{
  lua_Integer f = luaL_checkinteger(L, 2);
  lua_Integer e = luaL_checkinteger(L, 3);
  lua_Integer t = luaL_checkinteger(L, 4);
  int tt = !lua_isnil(L, 5) ? 5 : 1; /* destination table */
  lua_Integer n = e - f + 1;         /* number of elements to move */
  bool up = (dir > 0);
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
      if (lua_isplaintable(L, 1) && lua_isplaintable(L, tt))
      { /* (reads and writes call nothing) */
        if (up)
          i += lua_plainmove(L, 1, f + i, tt, t + i, n - i, 1);
        else
          i -= lua_plainmove(L, 1, f, tt, t, i + 1, 0);
        if (luaE_budgetout(L))
          lua_checkpointk(L, (lua_KContext)i, up ? tmovek_uploop : tmovek_downloop);
        break;
      }
      lua_getik(L, 1, f + i, (lua_KContext)i, up ? tmovek_upset : tmovek_downset);
      [[fallthrough]];
    case AT_SET:
      lua_setik(L, tt, t + i, (lua_KContext)i, up ? tmovek_upnext : tmovek_downnext);
      [[fallthrough]];
    default: /* AT_NEXT */
      i += dir;
      at = AT_LOOP;
      if (luaE_spent(L, 1))
        lua_checkpointk(L, (lua_KContext)i, up ? tmovek_uploop : tmovek_downloop);
    }
  }
}

/* 'move' after comparing the tables: they are not the same, go up */
static int tmove_dir(lua_State* L, int up)
{
  lua_Integer n = luaL_checkinteger(L, 3) - luaL_checkinteger(L, 2) + 1;
  return up ? tmove_loop(L, 0, 1, AT_LOOP) : tmove_loop(L, n - 1, -1, AT_LOOP);
}

static int tmovek_eq(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  int eq = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return tmove_dir(L, !eq);
}

LUA_KNAME("table", tmovek_eq);

static int tmove(lua_State* L)
{
  lua_Integer f = luaL_checkinteger(L, 2);
  lua_Integer e = luaL_checkinteger(L, 3);
  lua_Integer t = luaL_checkinteger(L, 4);
  int tt = !lua_isnoneornil(L, 5) ? 5 : 1; /* destination table */
  checktab(L, 1, TAB_R);
  checktab(L, tt, TAB_W);
  lua_settop(L, 5);
  luaE_freezeonly(L);
  if (e >= f)
  { /* otherwise, nothing to move */
    luaL_argchecki(L, f > 0 || e < LUA_MAXINTEGER + f, 3, lt::too_many_elements_to_move);
    lua_Integer n = e - f + 1; /* number of elements to move */
    luaL_argchecki(L, t <= LUA_MAXINTEGER - n + 1, 4, lt::destination_wrap_around);
    if (t > e || t <= f)
      return tmove_dir(L, 1);
    else if (tt == 1)
      return tmove_dir(L, 0);
    int eq = lua_comparek(L, 1, tt, LUA_OPEQ, 0, tmovek_eq);
    lua_pop(L, 1);
    return tmove_dir(L, !eq);
  }
  lua_pushvalue(L, tt); /* return destination table */
  return 1;
}

/*
** 'concat' (arguments set to 4, the last index at 4; a box at 5, made
** where something may freeze): adds t[i], a separator, ..., t[last].
*/
constexpr int CONCATCHUNK = 64; /* elements read at a time from a plain table */
static int tconcatk_get(lua_State* L, int status, lua_KContext ctx);
static int tconcatk_loop(lua_State* L, int status, lua_KContext ctx);

static int tconcat_loop(lua_State* L, luaL_Buffer* b, luaL_Box* box, lua_Integer i, int at)
{
  lua_Integer last = lua_tointeger(L, 4);
  byte_view sep = *lua_tolstring(L, 2);
  if (at == AT_LOOP && i > last) /* empty interval? */
  {
    luaL_pushresult(b);
    return 1;
  }
  for (;;)
  {
    if (at == AT_LOOP && lua_isplaintable(L, 1))
    { /* (reads call nothing): a chunk at a time */
      lua_Integer left = last - i; /* elements after 'i' */
      int chunk = lua_checkstack(L, CONCATCHUNK + LUA_MINSTACK) ? CONCATCHUNK : 1;
      int got = lua_plaingetrange(L, 1, i, (left < chunk) ? (int)left + 1 : chunk);
      int first = lua_gettop(L) - got + 1;
      for (int k = 0; k < got; k++, i++)
      {
        std::optional<byte_view> v = lua_tolstring(L, first + k);
        if (l_unlikely(!v))
        {
          lua_pushistringref(L, lua_typenamei(L, lua_type(L, first + k)));
          lua_pushinteger(L, i);
          luaL_errori(L, lt::invalid_value_s_at_index_I_in_table_for_concat);
        }
        luaL_addlstring(b, *v, 0, v->size());
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
        if (box == nullptr)
        {
          assert(lua_gettop(L) == 4);
          box = luaL_newbox(L);
        }
        luaL_buffsave(b, box);
        lua_checkpointk(L, (lua_KContext)i, tconcatk_loop);
        luaL_buffload(b, box);
      }
      continue;
    }
    if (at == AT_LOOP)
    {
      if (box != nullptr)
        luaL_buffsave(b, box);
      lua_getik(L, 1, i, (lua_KContext)i, tconcatk_get);
      if (box != nullptr)
        luaL_buffload(b, box);
    }
    if (l_unlikely(!lua_isstring(L, -1)))
    {
      lua_pushistringref(L, lua_typenamei(L, lua_type(L, -1)));
      lua_pushinteger(L, i);
      luaL_errori(L, lt::invalid_value_s_at_index_I_in_table_for_concat);
    }
    luaL_addvalue(b);
    if (i == last)
      break;
    luaL_addlstring(b, sep, 0, sep.size());
    i++;
    at = AT_LOOP;
    if (luaE_spent(L, 1))
    {
      if (box == nullptr)
      {
        assert(lua_gettop(L) == 4);
        box = luaL_newbox(L);
      }
      luaL_buffsave(b, box);
      lua_checkpointk(L, (lua_KContext)i, tconcatk_loop);
      luaL_buffload(b, box);
    }
  }
  luaL_pushresult(b);
  return 1;
}

static int tconcat_resume(lua_State* L, lua_Integer i, int at)
{
  luaL_Box* box = luaL_tobox(L, 5);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_buffload(&b, box);
  return tconcat_loop(L, &b, box, i, at);
}

static int tconcatk_get(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tconcat_resume(L, (lua_Integer)ctx, AT_SET);
}

LUA_KNAME("table", tconcatk_get);

static int tconcatk_loop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return tconcat_resume(L, (lua_Integer)ctx, AT_LOOP);
}

LUA_KNAME("table", tconcatk_loop);

/* 'concat' with the length 'last' */
static int tconcat_len(lua_State* L, lua_Integer last)
{
  luaL_optlstring(L, 2, lt::empty_str);
  lua_Integer i = luaL_optinteger(L, 3, 1);
  last = luaL_optinteger(L, 4, last);
  if (lua_isnil(L, 2))
  {
    lua_pushistringref(L, lt::empty_str);
    lua_replace(L, 2);
  }
  lua_pushinteger(L, last);
  lua_replace(L, 4);
  luaL_Box* box = nullptr;
  if (lua_type(L, 1) != LUA_TTABLE || lua_getmetatable(L, 1))
  { /* reading may call back */
    lua_settop(L, 4);
    box = luaL_newbox(L);
  }
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  return tconcat_loop(L, &b, box, i, AT_LOOP);
}

static int tconcatk_len(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tconcat_len(L, luaL_lenresult(L));
}

LUA_KNAME("table", tconcatk_len);

static int tconcat(lua_State* L)
{
  lua_settop(L, 4);
  luaE_freezeonly(L);
  return tconcat_len(L, aux_getnk(L, 1, TAB_R, 0, tconcatk_len));
}

/*
** {======================================================
** Pack/unpack
** =======================================================
*/

static int tpack(lua_State* L)
{
  int i;
  int n = lua_gettop(L);    /* number of elements to pack */
  lua_createtable(L, n, 1); /* create result table */
  lua_insert(L, 1);         /* put it at index 1 */
  for (i = n; i >= 1; i--)  /* assign elements */
    lua_seti(L, 1, i);
  lua_pushinteger(L, n);
  lua_setfieldi(L, 1, lt::n_char); /* t.n = number of elements */
  return 1;                /* return table */
}

/*
** 'unpack' (arguments set to 3, the last index at 3): pushes t[i] ..
** t[e] from 4 on; how many it pushed says where it goes on.
*/
static int tunpackk_got(lua_State* L, int status, lua_KContext ctx);
static int tunpackk_loop(lua_State* L, int status, lua_KContext ctx);

static int tunpack_loop(lua_State* L, lua_Integer i, int n, int count, int at)
{
  for (;;)
  {
    if (at == AT_LOOP)
    {
      if (lua_isplaintable(L, 1))
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
static int tunpack_resume(lua_State* L, int at)
{
  lua_Integer i = luaL_optinteger(L, 2, 1);
  int n = (int)(lua_tointeger(L, 3) - i + 1); /* number of elements */
  return tunpack_loop(L, i, n, lua_gettop(L) - 3, at);
}

static int tunpackk_got(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tunpack_resume(L, AT_NEXT);
}

LUA_KNAME("table", tunpackk_got);

static int tunpackk_loop(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tunpack_resume(L, AT_LOOP);
}

LUA_KNAME("table", tunpackk_loop);

/* 'unpack' with the length 'len' */
static int tunpack_len(lua_State* L, lua_Integer len)
{
  lua_Integer n;
  lua_Integer i = luaL_optinteger(L, 2, 1);
  lua_Integer e = (lua_isnoneornil(L, 3) ? len : luaL_checkinteger(L, 3));
  if (i > e)
    return 0;                      /* empty range */
  n = l_intsub(e, i); /* number of elements minus 1 */
  if (l_unlikely(!l_ult(n, INT_MAX) ||
                 !lua_checkstack(L, (int) (++n))))
    return luaL_errori(L, lt::too_many_results_to_unpack);
  if (!lua_isplaintable(L, 1))
  { /* reads may call back: the last index, for after a freeze */
    lua_pushinteger(L, e);
    lua_replace(L, 3);
  }
  return tunpack_loop(L, i, (int)n, 0, AT_LOOP);
}

static int tunpackk_len(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return tunpack_len(L, luaL_lenresult(L));
}

LUA_KNAME("table", tunpackk_len);

static int tunpack(lua_State* L)
{
  lua_settop(L, 3);
  luaE_freezeonly(L);
  return tunpack_len(L, aux_getnk(L, 1, TAB_R, 0, tunpackk_len));
}

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
typedef int IdxT;

/* Versions of lua_seti/lua_geti specialized for IdxT */

/*
** Produce a "random" 'unsigned int' to randomize pivot choice. This
** macro is used only when 'sort' detects a big imbalance in the result
** of a partition. (If you don't want/need this "randomness", ~0 is a
** good choice.)
*/

/* arrays larger than 'RANLIMIT' may use randomized pivots */
constexpr int RANLIMIT = 100;

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
enum
{
  SO_PC,                /* where it goes on */
  SO_LO, SO_UP, SO_RND, /* the interval being sorted, its randomization */
  SO_P,                 /* the pivot */
  SO_I, SO_J,           /* the partition's indices */
  SO_R,                 /* result of the last comparison (-1: on the stack) */
  SO_FN,                /* is there a comparison function? */
  SO_PLAIN,             /* is the table plain (reads and writes call nothing)? */
  SO_BOXED,             /* is the state in the userdata? */
  SO_NINTS,             /* size of the state */
  SO_N,                 /* number of pending intervals */
  SO_PENDING            /* pending intervals: lo, up, n, rnd */
};

/* the pending intervals: one for each level of recursion of stock Lua,
   which recurs on intervals at most half as big (n < INT_MAX) */
constexpr int SORT_MAXDEPTH = 33;

/*
** Where it goes on: after a read or a write, or after a comparison (these
** are kept outside the process); and the heads of its loops and branches,
** where it is never kept.
*/
enum
{
  S_START, S_LO1, S_LO2, S_LO3, S_LO4, S_P1, S_P2, S_P3, S_P4, S_P5, S_P6,
  S_P7, S_P8, S_P9, S_P10, S_P11, S_I1, S_J1, S_SW1, S_SW2, S_SW3, S_SW4,
  S_LT1, S_LT2, S_LT3, S_LT4, S_LT5, /* (comparisons last) */
  S_NKEPT,
  S_WHILE = S_NKEPT, /* the head of the loop over an interval */
  S_PIVOT,           /* after 'lo' and 'up' are in order */
  S_ELSEP,           /* a[p] >= a[lo]: compare a[up] with a[p] */
  S_PIVOTED,         /* 'lo', 'p' and 'up' are in order */
  S_ILOOP,           /* repeat ++i while a[i] < P */
  S_JLOOP,           /* repeat --j while P < a[j] */
  S_PARTED,          /* no elements out of place */
  S_NEXT             /* an interval is sorted */
};

/* the most integers of a sort's state */
constexpr int SORT_NINTS = SO_PENDING + 4 * SORT_MAXDEPTH;
constexpr size_t SORTBOX_SIZE = 8 * SORT_NINTS + 16; /* (its accounted size) */

/*
** The state of a sort that can call back, in its own userdata (at 3); kept
** outside the process as the bytes of its integers, little-endian
*/
struct SortBox : lua_UserObject
{
  lua_Integer s[SORT_NINTS]{};
  mutable byte_inline<8 * SORT_NINTS> image;
  const char* persistkind() const override { return "table.sort"; }
  byte_view persistbytes() const override
  {
    size_t n = static_cast<size_t>(s[SO_NINTS]);
    for (size_t k = 0; k < n; k++)
      image.write_int64(s[k], 8 * k);
    return image.view().substr(0, 8 * n);
  }
};

/* a sort box from its state, checked to be one */
static std::unique_ptr<lua_UserObject> makesortbox(lua_State*, const lua_Integer*, int nints, byte_view bytes)
{
  if (nints != 0 || bytes.size() % 8 != 0 || bytes.size() < 8 * (SO_PENDING + 4) ||
      bytes.size() > 8 * SORT_NINTS)
    return nullptr;
  auto box = std::make_unique<SortBox>();
  for (size_t k = 0; k < bytes.size() / 8; k++)
    box->s[k] = bytes.toInt64(8 * k);
  const lua_Integer* v = box->s;
  if (v[SO_NINTS] != static_cast<lua_Integer>(bytes.size() / 8) || v[SO_BOXED] != 1 || v[SO_PC] < 0 ||
      v[SO_PC] >= S_NKEPT || v[SO_N] < 0 || SO_PENDING + 4 * v[SO_N] > v[SO_NINTS] ||
      (v[SO_FN] != 0 && v[SO_FN] != 1) || (v[SO_PLAIN] != 0 && v[SO_PLAIN] != 1) || v[SO_R] < -1 ||
      v[SO_R] > 1)
    return nullptr;
  return box;
}

struct SortBoxKind
{
  SortBoxKind() { lua_registerobjectkind("table.sort", makesortbox); }
};
static SortBoxKind sortboxkind;

static int sortk(lua_State* L, int status, lua_KContext ctx);

/* the state moved into a new userdata at 3 (the first time it can call back) */
static lua_Integer* sort_box(lua_State* L, lua_Integer* s)
{
  if (s[SO_BOXED])
    return s;
  auto o = std::make_unique<SortBox>();
  SortBox* box = o.get();
  lua_newuserobject(L, std::move(o), SORTBOX_SIZE, 0);
  lua_replace(L, 3);
  for (lua_Integer k = 0; k < s[SO_NINTS]; k++)
    box->s[k] = s[k];
  box->s[SO_BOXED] = 1;
  return box->s;
}

/*
** Whether the value at stack index 'a' is less than the value at index
** 'b' (according to the order of the sort), by the function or '__lt'.
*/
static int sort_call(lua_State* L, int fn, int a, int b)
{
  if (!fn) /* no function? */
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
static int sort_result(lua_State* L, lua_Integer* s)
{
  if (s[SO_R] >= 0)
    return (int)s[SO_R];
  int res = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}

/*
** Choose an element in the middle (2nd-3th quarters) of [lo,up]
** "randomized" by 'rnd'
*/
static IdxT choosePivot(IdxT lo, IdxT up, unsigned int rnd)
{
  IdxT r4 = (up - lo) / 4; /* range/4 */
  IdxT p = (IdxT)l_umod(rnd ^ (unsigned int)lo ^ (unsigned int)up, r4 * 2) + (lo + r4);
  assert(lo + r4 <= p && p <= up - r4);
  return p;
}

/* the variables kept, to go on at 'pc' (the state in its userdata) */
static l_forceinline lua_Integer* sort_save(lua_State* L, lua_Integer* s, int pc, lua_Integer lo, lua_Integer up,
                                           lua_Integer rnd, lua_Integer p, lua_Integer i, lua_Integer j)
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
static int sort_run(lua_State* L, lua_Integer* s)
{
  int fn = (int)s[SO_FN];
  bool plain = (s[SO_PLAIN] != 0);
  lua_Integer lo = s[SO_LO], up = s[SO_UP], p = s[SO_P], i = s[SO_I], j = s[SO_J];
  lua_Integer rnd = s[SO_RND];
  int res = 0;
  int pc = (int)s[SO_PC];
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
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_LO1, lo, up, rnd, p, i, j);
      lua_getik(L, 1, lo, 0, sortk);
      [[fallthrough]];
    case S_LO1:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_LO2, lo, up, rnd, p, i, j);
      lua_getik(L, 1, up, 0, sortk);
      [[fallthrough]];
    case S_LO2:
      if (fn || (res = lua_rawlessthan(L, -1, -2)) < 0) /* a[up] < a[lo]? */
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
      [[fallthrough]];
    case S_LT1:
      if (!res)
      {
        lua_pop(L, 2); /* remove both values */
        pc = S_PIVOT;
        continue;
      }
      if (s[SO_BOXED] || !plain) /* swap a[lo] - a[up] */
        s = sort_save(L, s, S_LO3, lo, up, rnd, p, i, j);
      lua_setik(L, 1, lo, 0, sortk);
      [[fallthrough]];
    case S_LO3:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_LO4, lo, up, rnd, p, i, j);
      lua_setik(L, 1, up, 0, sortk);
      [[fallthrough]];
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
        p = choosePivot((IdxT)lo, (IdxT)up, (unsigned int)rnd);
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P1, lo, up, rnd, p, i, j);
      lua_getik(L, 1, p, 0, sortk);
      [[fallthrough]];
    case S_P1:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P2, lo, up, rnd, p, i, j);
      lua_getik(L, 1, lo, 0, sortk);
      [[fallthrough]];
    case S_P2:
      if (fn || (res = lua_rawlessthan(L, -2, -1)) < 0) /* a[p] < a[lo]? */
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
      [[fallthrough]];
    case S_LT2:
      if (!res)
      {
        lua_pop(L, 1); /* remove a[lo] */
        pc = S_ELSEP;
        continue;
      }
      if (s[SO_BOXED] || !plain) /* swap a[p] - a[lo] */
        s = sort_save(L, s, S_P3, lo, up, rnd, p, i, j);
      lua_setik(L, 1, p, 0, sortk);
      [[fallthrough]];
    case S_P3:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P4, lo, up, rnd, p, i, j);
      lua_setik(L, 1, lo, 0, sortk);
      [[fallthrough]];
    case S_P4:
      pc = S_PIVOTED;
      continue;
    case S_ELSEP:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P5, lo, up, rnd, p, i, j);
      lua_getik(L, 1, up, 0, sortk);
      [[fallthrough]];
    case S_P5:
      if (fn || (res = lua_rawlessthan(L, -1, -2)) < 0) /* a[up] < a[p]? */
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
      [[fallthrough]];
    case S_LT3:
      if (!res)
      {
        lua_pop(L, 2);
        pc = S_PIVOTED;
        continue;
      }
      if (s[SO_BOXED] || !plain) /* swap a[up] - a[p] */
        s = sort_save(L, s, S_P6, lo, up, rnd, p, i, j);
      lua_setik(L, 1, p, 0, sortk);
      [[fallthrough]];
    case S_P6:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P7, lo, up, rnd, p, i, j);
      lua_setik(L, 1, up, 0, sortk);
      [[fallthrough]];
    case S_P7:
    case S_PIVOTED:
      if (up - lo == 2) /* only 3 elements? */
      {               /* already sorted */
        pc = S_NEXT;
        continue;
      }
      if (s[SO_BOXED] || !plain) /* get middle element (Pivot) */
        s = sort_save(L, s, S_P8, lo, up, rnd, p, i, j);
      lua_getik(L, 1, p, 0, sortk);
      [[fallthrough]];
    case S_P8:
      lua_pushvalue(L, -1); /* push Pivot */
      if (s[SO_BOXED] || !plain) /* push a[up - 1] */
        s = sort_save(L, s, S_P9, lo, up, rnd, p, i, j);
      lua_getik(L, 1, up - 1, 0, sortk);
      [[fallthrough]];
    case S_P9:
      if (s[SO_BOXED] || !plain) /* swap Pivot (a[p]) with a[up - 1] */
        s = sort_save(L, s, S_P10, lo, up, rnd, p, i, j);
      lua_setik(L, 1, p, 0, sortk);
      [[fallthrough]];
    case S_P10:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_P11, lo, up, rnd, p, i, j);
      lua_setik(L, 1, up - 1, 0, sortk);
      [[fallthrough]];
    case S_P11:
      /* the partition: Pivot P is at the top of the stack.
         precondition: a[lo] <= P == a[up-1] <= a[up],
         so it only needs to do the partition from lo + 1 to up - 2.
         Pos-condition: a[lo .. i - 1] <= a[i] == P <= a[i + 1 .. up];
         loop invariant: a[lo .. i] <= P <= a[j .. up] */
      i = lo;     /* will be incremented before first use */
      j = up - 1; /* will be decremented before first use */
      [[fallthrough]];
    case S_ILOOP: /* repeat ++i while a[i] < P */
      i++;
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_I1, lo, up, rnd, p, i, j);
      lua_getik(L, 1, i, 0, sortk);
      [[fallthrough]];
    case S_I1:
      if (fn || (res = lua_rawlessthan(L, -1, -2)) < 0)
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
      [[fallthrough]];
    case S_LT4:
      if (res)
      {
        if (l_unlikely(i == up - 1)) /* a[up - 1] < P == a[up - 1] */
          luaL_errori(L, lt::invalid_order_function_for_sorting);
        lua_pop(L, 1); /* remove a[i] */
        pc = S_ILOOP;
        continue;
      }
      /* after the loop, a[i] >= P and a[lo .. i - 1] < P  (a) */
      [[fallthrough]];
    case S_JLOOP: /* repeat --j while P < a[j] */
      j--;
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_J1, lo, up, rnd, p, i, j);
      lua_getik(L, 1, j, 0, sortk);
      [[fallthrough]];
    case S_J1:
      if (fn || (res = lua_rawlessthan(L, -3, -1)) < 0)
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
      [[fallthrough]];
    case S_LT5:
      if (res)
      {
        if (l_unlikely(j < i)) /* j <= i - 1 and a[j] > P, contradicts (a) */
          luaL_errori(L, lt::invalid_order_function_for_sorting);
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
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_SW3, lo, up, rnd, p, i, j);
      lua_setik(L, 1, i, 0, sortk);
      [[fallthrough]];
    case S_SW3:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_SW4, lo, up, rnd, p, i, j);
      lua_setik(L, 1, j, 0, sortk);
      [[fallthrough]];
    case S_SW4:
      pc = S_ILOOP;
      continue;
    case S_PARTED:
      /* a[lo .. i - 1] <= P <= a[j + 1 .. i .. up] */
      lua_pop(L, 1); /* pop a[j] */
      /* swap pivot (a[up - 1]) with a[i] to satisfy pos-condition */
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_SW1, lo, up, rnd, p, i, j);
      lua_setik(L, 1, up - 1, 0, sortk);
      [[fallthrough]];
    case S_SW1:
      if (s[SO_BOXED] || !plain)
        s = sort_save(L, s, S_SW2, lo, up, rnd, p, i, j);
      lua_setik(L, 1, i, 0, sortk);
      [[fallthrough]];
    case S_SW2:
    {
      p = i; /* a[lo .. p - 1] <= a[p] == P <= a[p + 1 .. up] */
      size_t pend = static_cast<size_t>(SO_PENDING + 4 * s[SO_N]++);
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
      size_t pend = static_cast<size_t>(SO_PENDING + 4 * --s[SO_N]);
      lo = s[pend];
      up = s[pend + 1];
      rnd = s[pend + 3];
      if ((up - lo) / 128 > s[pend + 2]) /* partition too imbalanced? */
        rnd = luaL_makeseed(L);           /* try a new randomization */
      pc = S_WHILE;
      continue;
    }
    }
  }
}

static int sortk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  SortBox* box = dynamic_cast<SortBox*>(lua_touserobject(L, 3));
  if (box == nullptr)
    luaL_errori(L, lt::invalid_sort_state);
  return sort_run(L, box->s);
}

LUA_KNAME("table", sortk);

/* 'sort' with the length 'n' */
static int sort_len(lua_State* L, lua_Integer n)
{
  if (n > 1)
  { /* non-trivial interval? */
    luaL_argchecki(L, n < INT_MAX, 1, lt::array_too_big);
    if (!lua_isnoneornil(L, 2))            /* is there a 2nd argument? */
      luaL_checktype(L, 2, LUA_TFUNCTION); /* must be a function */
    lua_settop(L, 3);                      /* make sure there are two arguments (and room for the state) */
    int depth = (int)std::bit_width((lua_Unsigned)n) + 1;
    assert(depth <= SORT_MAXDEPTH);
    lua_Integer state[SORT_NINTS];
    lua_Integer* s = state;
    s[SO_PC] = S_START;
    s[SO_LO] = 1;
    s[SO_UP] = n;
    s[SO_RND] = 0;
    s[SO_FN] = !lua_isnil(L, 2);
    s[SO_PLAIN] = lua_isplaintable(L, 1);
    s[SO_P] = s[SO_I] = s[SO_J] = 0;
    s[SO_R] = 0;
    s[SO_BOXED] = 0;
    s[SO_NINTS] = SO_PENDING + 4 * depth;
    s[SO_N] = 0;
    if (s[SO_FN] || !s[SO_PLAIN]) /* will it call back? */
      s = sort_box(L, s);
    return sort_run(L, s);
  }
  return 0;
}

static int sortk_len(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  return sort_len(L, luaL_lenresult(L));
}

LUA_KNAME("table", sortk_len);

static int sort(lua_State* L)
{
  luaE_freezeonly(L);
  return sort_len(L, aux_getnk(L, 1, TAB_RW, 0, sortk_len));
}

/* }====================================================== */

static CFuncInfo cfi_tconcat(tconcat, "table.tconcat");
static CFuncInfo cfi_tcreate(tcreate, "table.tcreate");
static CFuncInfo cfi_tinsert(tinsert, "table.tinsert");
static CFuncInfo cfi_tpack(tpack, "table.tpack");
static CFuncInfo cfi_tunpack(tunpack, "table.tunpack");
static CFuncInfo cfi_tremove(tremove, "table.tremove");
static CFuncInfo cfi_tmove(tmove, "table.tmove");
static CFuncInfo cfi_sort(sort, "table.sort");

static const luaL_RegCFI tab_funcs[] = {
  {lt::concat, &cfi_tconcat},
  {lt::create, &cfi_tcreate},
  {lt::insert, &cfi_tinsert},
  {lt::pack, &cfi_tpack},
  {lt::unpack, &cfi_tunpack},
  {lt::remove_kw, &cfi_tremove},
  {lt::move, &cfi_tmove},
  {lt::sort, &cfi_sort},
};

}

LUAMOD_API int luaopen_table(lua_State* L)
{
  luaL_newlibcfi(L, tab_funcs);
  return 1;
}

