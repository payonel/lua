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
#define cvt2str(o) (((o)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER)
#else
#define cvt2str(o) 0 /* no conversion from numbers to strings */
#endif

#define cvt2num(o) (((o)->tt_ & NOVARIANT_MASK) == LUA_TSTRING)

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
#define tonumber(o, nout) (((o)->tt_ == LUA_VNUMFLT) ? (*(nout) = (lua_assert((o)->tt_ == LUA_VNUMFLT), ((o)->value_).n), 1) : luaV_tonumber_(o, nout))

/* convert an object to a float (without string coercion) */
#define tonumberns(o, nout) (((o)->tt_ == LUA_VNUMFLT) ? ((nout) = (lua_assert((o)->tt_ == LUA_VNUMFLT), ((o)->value_).n), 1) : (((o)->tt_ == LUA_VNUMINT) ? ((nout) = ((lua_Number)((lua_assert((o)->tt_ == LUA_VNUMINT), ((o)->value_).i))), 1) : 0))

/* convert an object to an integer (including string coercion) */
#define tointeger(o, iout) (l_likely(((o)->tt_ == LUA_VNUMINT)) ? (*(iout) = (lua_assert((o)->tt_ == LUA_VNUMINT), ((o)->value_).i), 1) : luaV_tointeger(o, iout, LUA_FLOORN2I))

/* convert an object to an integer (without string coercion) */
#define tointegerns(o, iout) (l_likely(((o)->tt_ == LUA_VNUMINT)) ? (*(iout) = (lua_assert((o)->tt_ == LUA_VNUMINT), ((o)->value_).i), 1) : luaV_tointegerns(o, iout, LUA_FLOORN2I))

#define intop(op, v1, v2) ((lua_Integer)(((lua_Unsigned)(v1)) op ((lua_Unsigned)(v2))))

#define luaV_rawequalobj(t1, t2) luaV_equalobj(NULL, t1, t2)

/*
** fast track for 'gettable'
*/
#define luaV_fastget(t, k, res, f, tag) (tag = (!((t)->tt_ == ctb(LUA_VTABLE)) ? LUA_VNOTABLE : f((lua_assert((t)->tt_ == ctb(LUA_VTABLE)), (lua_assert((((t)->value_).gc)->tt == LUA_VTABLE), static_cast<Table*>(((t)->value_).gc))), k, res)))

/*
** Special case of 'luaV_fastget' for integers, inlining the fast case
** of 'luaH_getint'.
*/
#define luaV_fastgeti(t, k, res, tag)      \
  if (!((t)->tt_ == ctb(LUA_VTABLE)))                       \
    tag = LUA_VNOTABLE;                    \
  else                                     \
  {                                        \
    luaH_fastgeti((lua_assert((t)->tt_ == ctb(LUA_VTABLE)), (lua_assert((((t)->value_).gc)->tt == LUA_VTABLE), static_cast<Table*>(((t)->value_).gc))), k, res, tag); \
  }

#define luaV_fastset(t, k, val, hres, f) (hres = (!((t)->tt_ == ctb(LUA_VTABLE)) ? HNOTATABLE : f((lua_assert((t)->tt_ == ctb(LUA_VTABLE)), (lua_assert((((t)->value_).gc)->tt == LUA_VTABLE), static_cast<Table*>(((t)->value_).gc))), k, val)))

#define luaV_fastseti(t, k, val, hres)      \
  if (!((t)->tt_ == ctb(LUA_VTABLE)))                        \
    hres = HNOTATABLE;                      \
  else                                      \
  {                                         \
    luaH_fastseti((lua_assert((t)->tt_ == ctb(LUA_VTABLE)), (lua_assert((((t)->value_).gc)->tt == LUA_VTABLE), static_cast<Table*>(((t)->value_).gc))), k, val, hres); \
  }

/*
** Finish a fast set operation (when fast set succeeds).
*/
#define luaV_finishfastset(L, t, v) luaC_barrierback(L, (lua_assert((t)->tt_ & BIT_ISCOLLECTABLE), ((t)->value_).gc), v)

/*
** Shift right is the same as shift left with a negative 'y'
*/
#define luaV_shiftr(x, y) luaV_shiftl(x, intop(-, 0, y))

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
