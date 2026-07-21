/*
** $Id: lvm.h $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#pragma once

#include "ldo.h"
#include "lobject.h"
#include "ltm.h"

#if !defined(LUA_NOCVTN2S)
static inline int cvt2str(const TValue* o)
{
  return (o->tt_ & NOVARIANT_MASK) == LUA_TNUMBER;
}
#else
static inline int cvt2str(const TValue* o)
{
  (void) o;
  return 0; /* no conversion from numbers to strings */
}
#endif

static inline int cvt2num(const TValue* o)
{
  return (o->tt_ & NOVARIANT_MASK) == LUA_TSTRING;
}

/*
** You can define LUA_FLOORN2I if you want to convert floats to integers
** by flooring them (instead of raising an error if they are not
** integral values)
*/
#if !defined(LUA_FLOORN2I)
#define LUA_FLOORN2I F2Ieq
#endif

/*
** Rounding modes for float->integer coercion
*/
typedef enum
{
  F2Ieq,    /* no rounding; accepts only integral values */
  F2Ifloor, /* takes the floor of the number */
  F2Iceil   /* takes the ceiling of the number */
} F2Imod;

/* convert an object to a float (including string coercion) */

/* convert an object to a float (without string coercion) */

/* convert an object to an integer (including string coercion) */

/* convert an object to an integer (without string coercion) */

/*
** fast track for 'gettable'
*/

/*
** Special case of 'luaV_fastget' for integers, inlining the fast case
** of 'luaH_getint'.
*/

/*
** Finish a fast set operation (when fast set succeeds).
*/

/*
** Shift right is the same as shift left with a negative 'y'
*/

LUAI_FUNC int luaV_equalobj(lua_State* L, const TValue* t1, const TValue* t2);
LUAI_FUNC int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_tonumber_(const TValue* obj, lua_Number* n);
LUAI_FUNC int luaV_tointeger(const TValue* obj, lua_Integer* p, F2Imod mode);
LUAI_FUNC int luaV_tointegerns(const TValue* obj, lua_Integer* p,
                               F2Imod mode);
LUAI_FUNC int luaV_flttointeger(lua_Number n, lua_Integer* p, F2Imod mode);
LUAI_FUNC lu_byte luaV_finishget(lua_State* L, const TValue* t, TValue* key,
                                 StkId val, lu_byte tag);
LUAI_FUNC void luaV_finishset(lua_State* L, const TValue* t, TValue* key,
                              TValue* val, int aux);
LUAI_FUNC void luaV_finishOp(lua_State* L);
LUAI_FUNC void luaV_execute(lua_State* L, CallInfo* ci);
LUAI_FUNC void luaV_concat(lua_State* L, int total);
LUAI_FUNC lua_Integer luaV_idiv(lua_State* L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Integer luaV_mod(lua_State* L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Number luaV_modf(lua_State* L, lua_Number x, lua_Number y);
LUAI_FUNC lua_Integer luaV_shiftl(lua_Integer x, lua_Integer y);
LUAI_FUNC void luaV_objlen(lua_State* L, StkId ra, const TValue* rb);

static l_forceinline int tonumber(const TValue* o, lua_Number* nout)
{
  if (o->tt_ == LUA_VNUMFLT)
  {
    *nout = o->value_.n;
    return 1;
  }
  return luaV_tonumber_(o, nout);
}

static l_forceinline int tointeger(const TValue* o, lua_Integer* iout)
{
  if (l_likely(o->tt_ == LUA_VNUMINT))
  {
    *iout = o->value_.i;
    return 1;
  }
  return luaV_tointeger(o, iout, LUA_FLOORN2I);
}

static l_forceinline int tointegerns(const TValue* o, lua_Integer* iout)
{
  if (l_likely(o->tt_ == LUA_VNUMINT))
  {
    *iout = o->value_.i;
    return 1;
  }
  return luaV_tointegerns(o, iout, LUA_FLOORN2I);
}

static inline int luaV_rawequalobj(const TValue* t1, const TValue* t2)
{
  return luaV_equalobj(NULL, t1, t2);
}

static inline void luaV_finishfastset(lua_State* L, const TValue* t, const TValue* v)
{
  lua_assert(t->tt_ & BIT_ISCOLLECTABLE);
  luaC_barrierback(L, t->value_.gc, v);
}

static inline lua_Integer luaV_shiftr(lua_Integer x, lua_Integer y)
{
  return luaV_shiftl(x, (lua_Integer)(0u - (lua_Unsigned)y));
}
