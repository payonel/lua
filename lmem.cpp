/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/


#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "literals.h"

/*
** Macro to call the allocation function.
*/
void* luaM_rawrealloc(global_State* g, void* block, size_t os, size_t ns, int tag)
{
  if (ns == 0)
  {
    if (block != nullptr)
    {
      (*g->frealloc)(g->ud, os, 0, tag);
      free(block);
    }
    return nullptr;
  }
  if (l_unlikely(!(*g->frealloc)(g->ud, os, ns, tag)))
    return nullptr;
  return realloc(block, ns);
}

/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
static inline int cantryagain(global_State* g)
{
  return completestate(g) && !g->gcstopem;
}

static inline void* firsttry(global_State* g, void* block, size_t os, size_t ns, int tag)
{
  return luaM_rawrealloc(g, block, os, ns, tag);
}

/*
** {==================================================================
** Functions to allocate/deallocate arrays for the Parser
** ===================================================================
*/

/*
** Minimum size for arrays during parsing, to avoid overhead of
** reallocating to size 1, then 2, and then 4. All these arrays
** will be reallocated to exact sizes or erased when parsing ends.
*/
constexpr int MINSIZEARRAY = 4;

void* luaM_growaux_(lua_State* L, void* block, int nelems, int* psize, unsigned size_elems, int limit, byte_view what)
{
  void* newblock;
  int size = *psize;
  if (nelems + 1 <= size) /* does one extra element still fit? */
    return block;         /* nothing to be done */
  if (size >= limit / 2)
  {                                /* cannot double it? */
    if (l_unlikely(size >= limit)) /* cannot grow even a little? */
      luaG_runerror(L, lt::too_many_s_limit_is_d, FmtArgs().str(what).num(limit));
    size = limit; /* still have at least one free place */
  }
  else
  {
    size *= 2;
    if (size < MINSIZEARRAY)
      size = MINSIZEARRAY; /* minimum size */
  }
  assert(nelems + 1 <= size && size <= limit);
  /* 'limit' ensures that multiplication will not overflow */
  newblock = luaM_saferealloc_(L, block, ((size_t)(*psize)) * size_elems,
                               ((size_t)(size)) * size_elems);
  *psize = size; /* update only when everything else is OK */
  return newblock;
}

/*
** In prototypes, the size of the array is also its number of
** elements (to save memory). So, if it cannot shrink an array
** to its number of elements, the only option is to raise an
** error.
*/
void* luaM_shrinkvector_(lua_State* L, void* block, int* size,
                         int final_n, unsigned size_elem)
{
  void* newblock;
  size_t oldsize = ((size_t)(*size)) * size_elem;
  size_t newsize = ((size_t)(final_n)) * size_elem;
  assert(newsize <= oldsize);
  newblock = luaM_saferealloc_(L, block, oldsize, newsize);
  *size = final_n;
  return newblock;
}

/* }================================================================== */

l_noret luaM_toobig(lua_State* L)
{
  luaG_runerror(L, lt::memory_allocation_error_block_too_big);
}

/*
** Free memory
*/
void luaM_free_(lua_State* L, void* block, size_t osize, int tag)
{
  global_State* g = L->l_G;
  assert((osize == 0) == (block == nullptr));
  luaM_rawrealloc(g, block, osize, 0, tag);
  g->GCdebt += ((l_mem)(osize));
}

/*
** In case of allocation fail, this function will do an emergency
** collection to free some memory and then try the allocation again.
*/
static void* tryagain(lua_State* L, void* block,
                      size_t osize, size_t nsize, int tag)
{
  global_State* g = L->l_G;
  if (cantryagain(g))
  {
    luaC_fullgc(L, 1);                                     /* try to free some memory... */
    return luaM_rawrealloc(g, block, osize, nsize, tag); /* try again */
  }
  else
    return nullptr; /* cannot run an emergency collection */
}

/*
** Generic allocation routine.
*/
void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize)
{
  void* newblock;
  global_State* g = L->l_G;
  assert((osize == 0) == (block == nullptr));
  newblock = firsttry(g, block, osize, nsize, 0);
  if (l_unlikely(newblock == nullptr && nsize > 0))
  {
    newblock = tryagain(L, block, osize, nsize, 0);
    if (newblock == nullptr) /* still no memory? */
      return nullptr;        /* do not update 'GCdebt' */
  }
  assert((nsize == 0) == (newblock == nullptr));
  g->GCdebt -= ((l_mem)(nsize)) - ((l_mem)(osize));
  return newblock;
}

void* luaM_saferealloc_(lua_State* L, void* block, size_t osize,
                        size_t nsize)
{
  void* newblock = luaM_realloc_(L, block, osize, nsize);
  if (l_unlikely(newblock == nullptr && nsize > 0)) /* allocation failed? */
    luaD_throw(L, LUA_ERRMEM);
  return newblock;
}

void* luaM_malloc_(lua_State* L, size_t size, int tag)
{
  if (size == 0)
    return nullptr; /* that's all */
  else
  {
    global_State* g = L->l_G;
    void* newblock = firsttry(g, nullptr, 0, size, tag);
    if (l_unlikely(newblock == nullptr))
    {
      newblock = tryagain(L, nullptr, 0, size, tag);
      if (newblock == nullptr)
        luaD_throw(L, LUA_ERRMEM);
    }
    g->GCdebt -= ((l_mem)(size));
    return newblock;
  }
}

int luaM_reserve(lua_State* L, size_t size)
{
  global_State* g = L->l_G;
  if (g->memlimit > 0 &&
      ((size_t)(gettotalbytes(g))) + size > g->memlimit)
  {
    return 0;
  }
  g->GCdebt -= ((l_mem)(size));
  return 1;
}

void luaM_release(lua_State* L, size_t size)
{
  global_State* g = L->l_G;
  g->GCdebt += ((l_mem)(size));
}
