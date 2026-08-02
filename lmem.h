/*
** $Id: lmem.h $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#pragma once

#include <stddef.h>

#include "llimits.h"
#include "lua.h"

/*
** This macro tests whether it is safe to multiply 'n' by the size of
** type 't' without overflows. Because 'e' is always constant, it avoids
** the runtime division MAX_SIZET/(e).
** (The macro is somewhat complex to avoid warnings:  The 'sizeof'
** comparison avoids a runtime comparison when overflow cannot occur.
** The compiler should be able to optimize the real test by itself, but
** when it does it, it may give a warning about "comparison is always
** false due to limited range of data type"; the +1 tricks the compiler,
** avoiding this warning but also this optimization.)
*/

/*
** Computes the minimum between 'n' and 'MAX_SIZET/sizeof(t)', so that
** the result is not larger than 'n' and cannot overflow a 'size_t'
** when multiplied by the size of type 't'. (Assumes that 'n' is an
** 'int' and that 'int' is not larger than 'size_t'.)
*/

/*
** Arrays of chars do not need any test
*/

LUAI_FUNC l_noret luaM_toobig(lua_State* L);

/* not to be called directly */
LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void* luaM_saferealloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void luaM_free_(lua_State* L, void* block, size_t osize);
LUAI_FUNC void* luaM_growaux_(lua_State* L, void* block, int nelems, int* size, unsigned size_elem, int limit, std::string_view what);
LUAI_FUNC void* luaM_shrinkvector_(lua_State* L, void* block, int* nelem, int final_n, unsigned size_elem);
LUAI_FUNC void* luaM_malloc_(lua_State* L, size_t size, int tag);
LUAI_FUNC int luaM_reserve(lua_State* L, size_t size);
LUAI_FUNC void luaM_release(lua_State* L, size_t size);
