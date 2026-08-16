/*
** $Id: lmem.h $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#pragma once

#include <stddef.h>
#include <span>

#include "llimits.h"
#include "lua.h"

LUAI_FUNC l_noret luaM_toobig(lua_State* L);

struct global_State;
LUAI_FUNC void* luaM_rawrealloc(global_State* g, void* block, size_t osize, size_t nsize, int tag);

/* not to be called directly */
LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void* luaM_saferealloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void luaM_free_(lua_State* L, void* block, size_t osize, int tag = 0);
LUAI_FUNC void* luaM_growaux_(lua_State* L, void* block, int nelems, int* size, unsigned size_elem, int limit, byte_view what);
LUAI_FUNC void* luaM_shrinkvector_(lua_State* L, void* block, int* nelem, int final_n, unsigned size_elem);
LUAI_FUNC void* luaM_malloc_(lua_State* L, size_t size, int tag);
LUAI_FUNC int luaM_reserve(lua_State* L, size_t size);
LUAI_FUNC void luaM_release(lua_State* L, size_t size);

// type allocators
struct TValue;
struct UpVal;
struct CallInfo;
LUAI_FUNC std::span<TValue> luaM_alloc_TValue_span(lua_State* L, size_t count);
LUAI_FUNC std::span<UpVal*> luaM_alloc_UpVal_ptr_span(lua_State* L, size_t count);
LUAI_FUNC std::span<char> luaM_alloc_char_span(lua_State* L, size_t count);
LUAI_FUNC CallInfo* luaM_alloc_CallInfo(lua_State* L);

