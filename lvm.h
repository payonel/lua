/*
** $Id: lvm.h $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#pragma once

#include "ldo.h"
#include "lobject.h"
#include "ltm.h"

static inline int cvt2str(const TValue* o)
{
  return (o->tt_ & NOVARIANT_MASK) == LUA_TNUMBER;
}

static inline int cvt2str(lua_State* L, StkId idx)
{
  TValue v = getslot(L, idx);
  return cvt2str(&v);
}

static inline int cvt2num(const TValue* o)
{
  return (o->tt_ & NOVARIANT_MASK) == LUA_TSTRING;
}

/*
** Rounding modes for float->integer coercion
*/
typedef enum
{
  F2Ieq,    /* no rounding; accepts only integral values */
  F2Ifloor, /* takes the floor of the number */
  F2Iceil   /* takes the ceiling of the number */
} F2Imod;

constexpr F2Imod LUA_FLOORN2I = F2Ieq;

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
LUAI_FUNC int luaV_equalobj(lua_State* L, StkId t1, StkId t2);
LUAI_FUNC int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_tonumber_(const TValue* obj, NumResult* n);
LUAI_FUNC int luaV_tointeger(const TValue* obj, IntResult* p, F2Imod mode);
LUAI_FUNC int luaV_tointegerns(const TValue* obj, IntResult* p,
                               F2Imod mode);
LUAI_FUNC int luaV_flttointeger(lua_Number n, IntResult* p, F2Imod mode);
LUAI_FUNC lu_byte luaV_finishget(lua_State* L, const TValue* t, TValue* key,
                                 StkId val, lu_byte tag, StkId tsrc);
LUAI_FUNC void luaV_finishset(lua_State* L, const TValue* t, TValue* key,
                              TValue* val, int aux, StkId tsrc);
LUAI_FUNC void luaV_finishOp(lua_State* L);
LUAI_FUNC void luaV_execute(lua_State* L, CallInfo* ci);
LUAI_FUNC void luaV_concat(lua_State* L, int total);
LUAI_FUNC lua_Integer luaV_idiv(lua_State* L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Integer luaV_mod(lua_State* L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Number luaV_modf(lua_State* L, lua_Number x, lua_Number y);
LUAI_FUNC lua_Integer luaV_shiftl(lua_Integer x, lua_Integer y);
LUAI_FUNC void luaV_objlen(lua_State* L, StkId ra, const TValue* rb, StkId src);
LUAI_FUNC void luaV_objlen(lua_State* L, StkId ra, StkId rb);

static l_forceinline int tonumber(const TValue* o, NumResult* nout)
{
  if (o->tt_ == LUA_VNUMFLT)
  {
    nout->v = fltvalue(o);
    return 1;
  }
  return luaV_tonumber_(o, nout);
}

static l_forceinline int tonumberns(const TValue* o, NumResult* nout)
{
  if (o->tt_ == LUA_VNUMFLT)
  {
    nout->v = fltvalue(o);
    return 1;
  }
  if (o->tt_ == LUA_VNUMINT)
  {
    nout->v = (lua_Number)ivalue(o);
    return 1;
  }
  return 0;
}

static l_forceinline int tointeger(const TValue* o, IntResult* iout)
{
  if (l_likely(o->tt_ == LUA_VNUMINT))
  {
    iout->v = ivalue(o);
    return 1;
  }
  return luaV_tointeger(o, iout, LUA_FLOORN2I);
}

static l_forceinline int tointegerns(const TValue* o, IntResult* iout)
{
  if (l_likely(o->tt_ == LUA_VNUMINT))
  {
    iout->v = ivalue(o);
    return 1;
  }
  return luaV_tointegerns(o, iout, LUA_FLOORN2I);
}

static l_forceinline int tonumber(lua_State* L, StkId idx, NumResult* nout)
{
  TValue v = getslot(L, idx);
  return tonumber(&v, nout);
}

static l_forceinline int tonumberns(lua_State* L, StkId idx, NumResult* nout)
{
  TValue v = getslot(L, idx);
  return tonumberns(&v, nout);
}

static l_forceinline int tointegerns(lua_State* L, StkId idx, IntResult* iout)
{
  TValue v = getslot(L, idx);
  return tointegerns(&v, iout);
}

static inline int luaV_rawequalobj(const TValue* t1, const TValue* t2)
{
  return luaV_equalobj(nullptr, t1, t2);
}

static inline int luaV_rawequalobj(lua_State* L, StkId t1, const TValue* t2)
{
  TValue v1 = getslot(L, t1);
  return luaV_equalobj(nullptr, &v1, t2);
}

static inline void luaV_finishfastset(lua_State* L, const TValue* t, const TValue* v)
{
  assert(t->tt_ & BIT_ISCOLLECTABLE);
  luaC_barrierback(L, gcvalue(t), v);
}

static inline void luaV_finishfastset(lua_State* L, StkId t, const TValue* v)
{
  TValue tv = getslot(L, t);
  luaV_finishfastset(L, &tv, v);
}

static inline lua_Integer luaV_shiftr(lua_Integer x, lua_Integer y)
{
  return luaV_shiftl(x, (lua_Integer)(0u - (lua_Unsigned)y));
}
