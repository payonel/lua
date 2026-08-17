/*
** $Id: ltable.h $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#pragma once

#include "lobject.h"

static l_forceinline Node* gnode(const Table* t, size_t i)
{
  return &t->node[i];
}

/*
** Clear all bits of fast-access metamethods, which means that the table
** may have any of these metamethods. (First access that fails after the
** clearing will set the bit again.)
*/
static l_forceinline void invalidateTMcache(Table* t)
{
  t->flags = (lu_byte)(t->flags & ~maskflags);
}

/*
** Bit BITDUMMY set in 'flags' means the table is using the dummy node
** for its hash part.
*/

constexpr int BITDUMMY = (1 << 6);
constexpr lu_byte NOTBITDUMMY = ((lu_byte)(~BITDUMMY));
static l_forceinline int isdummy(const Table* t)
{
  return t->flags & BITDUMMY;
}

static l_forceinline void setnodummy(Table* t)
{
  t->flags = (lu_byte)(t->flags & NOTBITDUMMY);
}
static l_forceinline void setdummy(Table* t)
{
  t->flags = (lu_byte)(t->flags | BITDUMMY);
}

/* allocated size for hash nodes */
static l_forceinline unsigned int allocsizenode(const Table* t)
{
  return isdummy(t) ? 0 : sizenode(t);
}

/* results from pset */
constexpr int HOK = 0;
constexpr int HNOTFOUND = 1;
constexpr int HNOTATABLE = 2;
constexpr int HFIRSTNODE = 3;

/*
** 'luaH_get*' operations set 'res', unless the value is absent, and
** return the tag of the result.
** The 'luaH_pset*' (pre-set) operations set the given value and return
** HOK, unless the original value is absent. In that case, if the key
** is really absent, they return HNOTFOUND. Otherwise, if there is a
** slot with that key but with no value, 'luaH_pset*' return an encoding
** of where the key is (usually called 'hres'). (pset cannot set that
** value because there might be a metamethod.) If the slot is in the
** hash part, the encoding is (HFIRSTNODE + hash index); if the slot is
** in the array part, the encoding is (~array index), a negative value.
** The value HNOTATABLE is used by the fast macros to signal that the
** value being indexed is not a table.
** (The size for the array part is limited by the maximum power of two
** that fits in an unsigned integer; that is INT_MAX+1. So, the C-index
** ranges from 0, which encodes to -1, to INT_MAX, which encodes to
** INT_MIN. The size of the hash part is limited by the maximum power of
** two that fits in a signed integer; that is (INT_MAX+1)/2. So, it is
** safe to add HFIRSTNODE to any index there.)
*/

/*
** The array part of a table is two separate, forward-indexed arrays: a
** 'Value' array ('t->array') and a parallel tag array ('t->tags'). The
** length hint for #t lives in the 'lenhint' field of the Table.
** All accesses to the arrays should be through the macros 'getArrTag'
** and 'getArrVal'.
*/

/* Computes the address of the tag for the abstract C-index 'k' */
static l_forceinline lu_byte* getArrTag(const Table* t, lua_Unsigned k)
{
  return t->tags.data() + k;
}

/* Computes the address of the value for the abstract C-index 'k' */
static l_forceinline Value* getArrVal(const Table* t, lua_Unsigned k)
{
  return t->array.data() + k;
}

/*
** Move TValues to/from arrays, using C indices
*/
static l_forceinline void arr2obj(const Table* h, lua_Unsigned k, TValue* val)
{
  val->tt_ = *getArrTag(h, k);
  val->value_ = *getArrVal(h, k);
}

static l_forceinline void obj2arr(const Table* h, lua_Unsigned k, const TValue* val)
{
  *getArrTag(h, k) = val->tt_;
  *getArrVal(h, k) = val->value_;
}

/*
** Often, we need to check the tag of a value before moving it. The
** following macros also move TValues to/from arrays, but receive the
** precomputed tag value or address as an extra argument.
*/
static l_forceinline void farr2val(const Table* h, lua_Unsigned k, lu_byte tag, TValue* res)
{
  res->tt_ = tag;
  res->value_ = *getArrVal(h, k);
}

static l_forceinline void fval2arr(const Table* h, lua_Unsigned k, lu_byte* tag, const TValue* val)
{
  *tag = val->tt_;
  *getArrVal(h, k) = val->value_;
}

LUAI_FUNC lu_byte luaH_get(Table* t, const TValue* key, TValue* res);
LUAI_FUNC lu_byte luaH_getshortstr(Table* t, TString* key, TValue* res);
LUAI_FUNC lu_byte luaH_getstr(Table* t, TString* key, TValue* res);
LUAI_FUNC lu_byte luaH_getint(Table* t, lua_Integer key, TValue* res);

/* Special get for metamethods */
LUAI_FUNC const TValue* luaH_Hgetshortstr(Table* t, TString* key);

LUAI_FUNC int luaH_psetint(Table* t, lua_Integer key, TValue* val);
LUAI_FUNC int luaH_psetshortstr(Table* t, TString* key, TValue* val);
LUAI_FUNC int luaH_psetstr(Table* t, TString* key, TValue* val);
LUAI_FUNC int luaH_pset(Table* t, const TValue* key, TValue* val);

LUAI_FUNC void luaH_setint(lua_State* L, Table* t, lua_Integer key,
                           TValue* value);
LUAI_FUNC void luaH_set(lua_State* L, Table* t, const TValue* key,
                        TValue* value);

LUAI_FUNC void luaH_finishset(lua_State* L, Table* t, const TValue* key,
                              TValue* value, int hres);
LUAI_FUNC Table* luaH_new(lua_State* L);
LUAI_FUNC void luaH_resize(lua_State* L, Table* t, unsigned nasize,
                           unsigned nhsize);
LUAI_FUNC void luaH_resizearray(lua_State* L, Table* t, unsigned nasize);
LUAI_FUNC lu_mem luaH_size(Table* t);
LUAI_FUNC void luaH_free(lua_State* L, Table* t);
LUAI_FUNC int luaH_next(lua_State* L, Table* t, StkId key);
LUAI_FUNC lua_Unsigned luaH_getn(lua_State* L, Table* t);

#if defined(LUA_DEBUG)
LUAI_FUNC int luaH_mainposition(const Table* t, const TValue* key);
#endif
