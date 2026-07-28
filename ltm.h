/*
** $Id: ltm.h $
** Tag methods
** See Copyright Notice in lua.h
*/

#pragma once

#include "lobject.h"

/*
 * WARNING: if you change the order of this enumeration,
 * grep "ORDER TM" and "ORDER OP"
 */
typedef enum
{
  TM_INDEX,
  TM_NEWINDEX,
  TM_GC,
  TM_MODE,
  TM_LEN,
  TM_EQ, /* last tag method with fast access */
  TM_ADD,
  TM_SUB,
  TM_MUL,
  TM_MOD,
  TM_POW,
  TM_DIV,
  TM_IDIV,
  TM_BAND,
  TM_BOR,
  TM_BXOR,
  TM_SHL,
  TM_SHR,
  TM_UNM,
  TM_BNOT,
  TM_LT,
  TM_LE,
  TM_CONCAT,
  TM_CALL,
  TM_CLOSE,
  TM_N /* number of elements in the enum */
} TMS;

/*
** Mask with 1 in all fast-access methods. A 1 in any of these bits
** in the flag of a (meta)table means the metatable does not have the
** corresponding metamethod field. (Bit 6 of the flag indicates that
** the table is using the dummy node.)
*/
constexpr lu_byte maskflags = ((lu_byte)(~(~0u << (TM_EQ + 1))));

/*
** Test whether there is no tagmethod.
** (Because tagmethods use raw accesses, the result may be an "empty" nil.)
*/
static l_forceinline int notm(const TValue* tm)
{
  return (tm->tt_ & NOVARIANT_MASK) == LUA_TNIL;
}

static l_forceinline int checknoTM(const Table* mt, TMS e)
{
  return mt == nullptr || (mt->flags & (1u << e));
}


LUAI_DDEC(const char* const luaT_typenames_[LUA_TOTALTYPES];)

static l_forceinline const char* ttypename(int x)
{
  return luaT_typenames_[x + 1];
}

LUAI_FUNC const char* luaT_objtypename(lua_State* L, const TValue* o);

LUAI_FUNC const TValue* luaT_gettm(Table* events, TMS event, TString* ename);
LUAI_FUNC const TValue* luaT_gettmbyobj(lua_State* L, const TValue* o,
                                        TMS event);
LUAI_FUNC void luaT_init(lua_State* L);

LUAI_FUNC void luaT_callTM(lua_State* L, const TValue* f, const TValue* p1,
                           const TValue* p2, const TValue* p3);
LUAI_FUNC lu_byte luaT_callTMres(lua_State* L, const TValue* f,
                                 const TValue* p1, const TValue* p2, StkId p3);
/*
** 'src1'/'src2' are operand origins for error attribution: the stack
** index of the operand when it lives in the current frame, -1 otherwise
** (constants, immediates). See varinfo in ldebug.cpp.
*/
LUAI_FUNC void luaT_trybinTM(lua_State* L, const TValue* p1, const TValue* p2,
                             StkId res, TMS event,
                             StkId src1, StkId src2);
LUAI_FUNC void luaT_tryconcatTM(lua_State* L);
LUAI_FUNC void luaT_trybinassocTM(lua_State* L, const TValue* p1,
                                  const TValue* p2, int inv, StkId res, TMS event,
                                  StkId src1);
LUAI_FUNC void luaT_trybiniTM(lua_State* L, const TValue* p1, lua_Integer i2,
                              int inv, StkId res, TMS event,
                              StkId src1);
LUAI_FUNC int luaT_callorderTM(lua_State* L, const TValue* p1,
                               const TValue* p2, TMS event);
LUAI_FUNC int luaT_callorderiTM(lua_State* L, const TValue* p1, int v2,
                                int inv, int isfloat, TMS event);

LUAI_FUNC void luaT_adjustvarargs(lua_State* L, struct CallInfo* ci,
                                  const Proto* p);
LUAI_FUNC void luaT_getvararg(lua_State* L, CallInfo* ci, StkId ra, TValue* rc);
LUAI_FUNC void luaT_getvarargs(lua_State* L, struct CallInfo* ci, StkId where,
                               int wanted, int vatab);
