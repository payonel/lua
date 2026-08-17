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
#include "lparser.h"
#include "lstate.h"
#include "literals.h"

LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void* luaM_saferealloc_(lua_State* L, void* block, size_t oldsize, size_t size);
LUAI_FUNC void* luaM_growaux_(lua_State* L, void* block, int nelems, int* size, unsigned size_elem, int limit, byte_view what);
LUAI_FUNC void* luaM_shrinkvector_(lua_State* L, void* block, int* nelem, int final_n, unsigned size_elem);
LUAI_FUNC void* luaM_rawrealloc(global_State* g, void* block, size_t osize, size_t nsize, int tag);
LUAI_FUNC void* luaM_malloc_(lua_State* L, size_t size, int tag);
LUAI_FUNC void luaM_free_(lua_State* L, void* block, size_t osize, int tag);

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

static int luaM_permit_(lua_State* L, size_t osize, size_t nsize, int tag)
{
  global_State* g = L->l_G;
  if (osize == 0 && nsize == 0)
    return 1;
  if (l_unlikely(!(*g->frealloc)(g->ud, osize, nsize, tag)))
  {
    if (!cantryagain(g))
      return 0;
    luaC_fullgc(L, 1); /* try to free some memory... */
    if (!(*g->frealloc)(g->ud, osize, nsize, tag))
      return 0;
  }
  return 1;
}

static int luaM_permit_(global_State* g, lua_State* L, size_t osize, size_t nsize, int tag)
{
  if (osize == 0 && nsize == 0)
    return 1;
  if (l_unlikely(!(*g->frealloc)(g->ud, osize, nsize, tag)))
  {
    if (!L)
      return 0; /* raw (g-only) path cannot run emergency GC: deny, like luaM_rawrealloc */
    if (!cantryagain(g))
      return 0;
    luaC_fullgc(L, 1); /* try to free some memory... */
    if (!(*g->frealloc)(g->ud, osize, nsize, tag))
      return 0;
  }
  return 1;
}

LUAI_FUNC std::span<char> luaM_alloc_char_span(global_State* g, lua_State* L, std::span<char> block, size_t count, bool bthrow)
{
  std::span<char> nb{};
  if (luaM_permit_(g, L, block.size(), count, 0))
  {
    if (count > 0)
    {
      nb = std::span<char>(new char[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(count) - static_cast<l_mem>(block.size());
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
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

LUAI_FUNC int luaM_growsize_(lua_State* L, int size, int nelems, int limit, byte_view what)
{
  if (nelems + 1 <= size)
    return size;
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
  return size;
}

void* luaM_growaux_(lua_State* L, void* block, int nelems, int* psize, unsigned size_elems, int limit, byte_view what)
{
  int size = luaM_growsize_(L, *psize, nelems, limit, what);
  if (size == *psize) /* nothing to be done? */
    return block;
  /* 'limit' ensures that multiplication will not overflow */
  void* newblock = luaM_saferealloc_(L, block, ((size_t)(*psize)) * size_elems,
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
** Generic allocation routine.
*/
void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize)
{
  global_State* g = L->l_G;
  assert((osize == 0) == (block == nullptr));
  if (l_unlikely(!luaM_permit_(L, osize, nsize, 0)))
    return nullptr; /* denied; do not update 'GCdebt' */
  void* newblock;
  if (nsize == 0)
  {
    free(block);
    newblock = nullptr;
  }
  else
  {
    newblock = realloc(block, nsize);
    if (l_unlikely(newblock == nullptr))
    { /* physical allocation failed after being permitted */
      if (cantryagain(g))
      {
        luaC_fullgc(L, 1);
        newblock = realloc(block, nsize);
      }
      if (newblock == nullptr)
        return nullptr; /* do not update 'GCdebt' */
    }
  }
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
    if (l_unlikely(!luaM_permit_(L, 0, size, tag)))
      luaD_throw(L, LUA_ERRMEM);
    void* newblock = realloc(nullptr, size);
    if (l_unlikely(newblock == nullptr))
    { /* physical allocation failed after being permitted */
      if (cantryagain(g))
      {
        luaC_fullgc(L, 1);
        newblock = realloc(nullptr, size);
      }
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

// type allocators
LUAI_FUNC std::span<TValue> luaM_alloc_TValue_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  TValue* stk = (TValue*)luaM_realloc_(L, nullptr, 0, count * sizeof(TValue));
  return std::span<TValue>(stk, stk == nullptr ? 0 : count);
}

LUAI_FUNC std::span<Value> luaM_alloc_Value_span(global_State* g, lua_State* L, std::span<Value> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Value))
    luaM_toobig(L);
  std::span<Value> nb{};
  size_t osz = block.size() * sizeof(Value);
  size_t nsz = count * sizeof(Value);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Value>(new Value[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Value));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<UpVal*> luaM_alloc_UpVal_ptr_span(global_State* g, lua_State* L, std::span<UpVal*> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(UpVal*))
    luaM_toobig(L);
  std::span<UpVal*> nb{};
  size_t osz = block.size() * sizeof(UpVal*);
  size_t nsz = count * sizeof(UpVal*);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<UpVal*>(new UpVal*[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(UpVal*));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<lu_byte> luaM_alloc_lu_byte_span(global_State* g, lua_State* L, std::span<lu_byte> block, size_t count, bool bthrow)
{
  std::span<lu_byte> nb{};
  if (luaM_permit_(g, L, block.size(), count, 0))
  {
    if (count > 0)
    {
      nb = std::span<lu_byte>(new lu_byte[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(count) - static_cast<l_mem>(block.size());
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC CallInfo* luaM_alloc_CallInfo(lua_State* L)
{
  return (CallInfo*)luaM_realloc_(L, nullptr, 0, sizeof(CallInfo));
}

// typed grow/shrink span allocators
LUAI_FUNC std::span<LocVar> luaM_alloc_LocVar_span(global_State* g, lua_State* L, std::span<LocVar> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(LocVar))
    luaM_toobig(L);
  std::span<LocVar> nb{};
  size_t osz = block.size() * sizeof(LocVar);
  size_t nsz = count * sizeof(LocVar);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<LocVar>(new LocVar[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(LocVar));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<LocVar> luaM_safegrow_LocVar_span(lua_State* L, std::span<LocVar> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_LocVar_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<Vardesc> luaM_alloc_Vardesc_span(global_State* g, lua_State* L, std::span<Vardesc> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Vardesc))
    luaM_toobig(L);
  std::span<Vardesc> nb{};
  size_t osz = block.size() * sizeof(Vardesc);
  size_t nsz = count * sizeof(Vardesc);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Vardesc>(new Vardesc[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Vardesc));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<Vardesc> luaM_safegrow_Vardesc_span(lua_State* L, std::span<Vardesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_Vardesc_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<Upvaldesc> luaM_alloc_Upvaldesc_span(global_State* g, lua_State* L, std::span<Upvaldesc> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Upvaldesc))
    luaM_toobig(L);
  std::span<Upvaldesc> nb{};
  size_t osz = block.size() * sizeof(Upvaldesc);
  size_t nsz = count * sizeof(Upvaldesc);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Upvaldesc>(new Upvaldesc[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Upvaldesc));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<Upvaldesc> luaM_safegrow_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_Upvaldesc_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<Labeldesc> luaM_alloc_Labeldesc_span(global_State* g, lua_State* L, std::span<Labeldesc> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Labeldesc))
    luaM_toobig(L);
  std::span<Labeldesc> nb{};
  size_t osz = block.size() * sizeof(Labeldesc);
  size_t nsz = count * sizeof(Labeldesc);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Labeldesc>(new Labeldesc[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Labeldesc));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<Labeldesc> luaM_safegrow_Labeldesc_span(lua_State* L, std::span<Labeldesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_Labeldesc_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<Proto*> luaM_alloc_Proto_ptr_span(global_State* g, lua_State* L, std::span<Proto*> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Proto*))
    luaM_toobig(L);
  std::span<Proto*> nb{};
  size_t osz = block.size() * sizeof(Proto*);
  size_t nsz = count * sizeof(Proto*);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Proto*>(new Proto*[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Proto*));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<Proto*> luaM_safegrow_Proto_ptr_span(lua_State* L, std::span<Proto*> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_Proto_ptr_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<AbsLineInfo> luaM_safegrow_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  AbsLineInfo* p = (AbsLineInfo*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(AbsLineInfo), limit, what);
  return std::span<AbsLineInfo>(p, (size_t)size);
}

LUAI_FUNC std::span<ls_byte> luaM_safegrow_ls_byte_span(lua_State* L, std::span<ls_byte> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  ls_byte* p = (ls_byte*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(ls_byte), limit, what);
  return std::span<ls_byte>(p, (size_t)size);
}

LUAI_FUNC std::span<Instruction> luaM_safegrow_Instruction_span(lua_State* L, std::span<Instruction> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  Instruction* p = (Instruction*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(Instruction), limit, what);
  return std::span<Instruction>(p, (size_t)size);
}

LUAI_FUNC std::span<TValue> luaM_safegrow_TValue_span(lua_State* L, std::span<TValue> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  TValue* p = (TValue*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(TValue), limit, what);
  return std::span<TValue>(p, (size_t)size);
}

LUAI_FUNC std::span<StkId> luaM_alloc_StkId_span(global_State* g, lua_State* L, std::span<StkId> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(StkId))
    luaM_toobig(L);
  std::span<StkId> nb{};
  size_t osz = block.size() * sizeof(StkId);
  size_t nsz = count * sizeof(StkId);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<StkId>(new StkId[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(StkId));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC std::span<StkId> luaM_safegrow_StkId_span(lua_State* L, std::span<StkId> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  int newsize = luaM_growsize_(L, size, nelems, limit, what);
  if (newsize == size)
    return block;
  return luaM_alloc_StkId_span(L->l_G, L, block, (size_t)newsize, true);
}

LUAI_FUNC std::span<Instruction> luaM_safeshrink_Instruction_span(lua_State* L, std::span<Instruction> block, int final_n)
{
  int size = (int)block.size();
  Instruction* p = (Instruction*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(Instruction));
  return std::span<Instruction>(p, (size_t)size);
}

LUAI_FUNC std::span<ls_byte> luaM_safeshrink_ls_byte_span(lua_State* L, std::span<ls_byte> block, int final_n)
{
  int size = (int)block.size();
  ls_byte* p = (ls_byte*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(ls_byte));
  return std::span<ls_byte>(p, (size_t)size);
}

LUAI_FUNC std::span<AbsLineInfo> luaM_safeshrink_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, int final_n)
{
  int size = (int)block.size();
  AbsLineInfo* p = (AbsLineInfo*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(AbsLineInfo));
  return std::span<AbsLineInfo>(p, (size_t)size);
}

LUAI_FUNC std::span<TValue> luaM_safeshrink_TValue_span(lua_State* L, std::span<TValue> block, int final_n)
{
  int size = (int)block.size();
  TValue* p = (TValue*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(TValue));
  return std::span<TValue>(p, (size_t)size);
}

LUAI_FUNC std::span<Proto*> luaM_safeshrink_Proto_ptr_span(lua_State* L, std::span<Proto*> block, int final_n)
{
  return luaM_alloc_Proto_ptr_span(L->l_G, L, block, (size_t)final_n, true);
}

LUAI_FUNC std::span<LocVar> luaM_safeshrink_LocVar_span(lua_State* L, std::span<LocVar> block, int final_n)
{
  return luaM_alloc_LocVar_span(L->l_G, L, block, (size_t)final_n, true);
}

LUAI_FUNC std::span<Upvaldesc> luaM_safeshrink_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int final_n)
{
  return luaM_alloc_Upvaldesc_span(L->l_G, L, block, (size_t)final_n, true);
}

// typed fresh (throwing) allocators
LUAI_FUNC std::span<Instruction> luaM_malloc_Instruction_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(Instruction))
    luaM_toobig(L);
  Instruction* p = (Instruction*)luaM_malloc_(L, count * sizeof(Instruction), 0);
  return std::span<Instruction>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<TValue> luaM_malloc_TValue_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(TValue))
    luaM_toobig(L);
  TValue* p = (TValue*)luaM_malloc_(L, count * sizeof(TValue), 0);
  return std::span<TValue>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<ls_byte> luaM_malloc_ls_byte_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(ls_byte))
    luaM_toobig(L);
  ls_byte* p = (ls_byte*)luaM_malloc_(L, count * sizeof(ls_byte), 0);
  return std::span<ls_byte>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<AbsLineInfo> luaM_malloc_AbsLineInfo_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(AbsLineInfo))
    luaM_toobig(L);
  AbsLineInfo* p = (AbsLineInfo*)luaM_malloc_(L, count * sizeof(AbsLineInfo), 0);
  return std::span<AbsLineInfo>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<Node> luaM_alloc_Node_span(global_State* g, lua_State* L, std::span<Node> block, size_t count, bool bthrow)
{
  if (count > 0 && count + 1 > MAX_SIZET / sizeof(Node))
    luaM_toobig(L);
  std::span<Node> nb{};
  size_t osz = block.size() * sizeof(Node);
  size_t nsz = count * sizeof(Node);
  if (luaM_permit_(g, L, osz, nsz, 0))
  {
    if (count > 0)
    {
      nb = std::span<Node>(new Node[count], count); // throws bad_alloc, never null
      if (!block.empty())
        ::memcpy(nb.data(), block.data(), std::min<size_t>(count, block.size()) * sizeof(Node));
    }
    delete [] block.data(); // free old block on success (no-op when null)
    if (L) // raw (g-only) path must not touch GCdebt
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else if (l_unlikely(count > 0 && bthrow && L))
  {
    luaD_throw(L, LUA_ERRMEM);
  }
  return nb;
}

LUAI_FUNC TString* luaM_malloc_TString(lua_State* L)
{
  return (TString*)luaM_malloc_(L, sizeof(TString), LUA_VSTRING & NOVARIANT_MASK);
}

LUAI_FUNC Udata* luaM_malloc_Udata(lua_State* L)
{
  return (Udata*)luaM_malloc_(L, sizeof(Udata), LUA_VUSERDATA & NOVARIANT_MASK);
}

LUAI_FUNC Table* luaM_malloc_Table(lua_State* L)
{
  return (Table*)luaM_malloc_(L, sizeof(Table), LUA_VTABLE & NOVARIANT_MASK);
}

LUAI_FUNC CClosure* luaM_malloc_CClosure(lua_State* L)
{
  return (CClosure*)luaM_malloc_(L, sizeof(CClosure), LUA_VCCL & NOVARIANT_MASK);
}

LUAI_FUNC LClosure* luaM_malloc_LClosure(lua_State* L)
{
  return (LClosure*)luaM_malloc_(L, sizeof(LClosure), LUA_VLCL & NOVARIANT_MASK);
}

LUAI_FUNC CFuncInfo* luaM_malloc_CFuncInfo(lua_State* L)
{
  return (CFuncInfo*)luaM_malloc_(L, sizeof(CFuncInfo), LUA_VLCF & NOVARIANT_MASK);
}

LUAI_FUNC UpVal* luaM_malloc_UpVal(lua_State* L)
{
  return (UpVal*)luaM_malloc_(L, sizeof(UpVal), LUA_VUPVAL & NOVARIANT_MASK);
}

LUAI_FUNC Proto* luaM_malloc_Proto(lua_State* L)
{
  return (Proto*)luaM_malloc_(L, sizeof(Proto), LUA_VPROTO & NOVARIANT_MASK);
}

LUAI_FUNC LX* luaM_malloc_LX(lua_State* L)
{
  return (LX*)luaM_malloc_(L, sizeof(LX), LUA_TTHREAD & NOVARIANT_MASK);
}

// typed span frees
LUAI_FUNC void luaM_free_TValue_span(lua_State* L, std::span<TValue> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(TValue), 0);
}

LUAI_FUNC void luaM_free_Instruction_span(lua_State* L, std::span<Instruction> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Instruction), 0);
}

LUAI_FUNC void luaM_free_ls_byte_span(lua_State* L, std::span<ls_byte> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(ls_byte), 0);
}

LUAI_FUNC void luaM_free_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(AbsLineInfo), 0);
}

// typed object frees
LUAI_FUNC void luaM_free_UpVal(lua_State* L, UpVal* p)
{
  luaM_free_(L, p, sizeof(UpVal), 0);
}

LUAI_FUNC void luaM_free_LClosure(lua_State* L, LClosure* p)
{
  luaM_free_(L, p, sizeof(LClosure), LUA_VLCL & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_CClosure(lua_State* L, CClosure* p)
{
  luaM_free_(L, p, sizeof(CClosure), LUA_VCCL & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_Udata(lua_State* L, Udata* p)
{
  luaM_free_(L, p, sizeof(Udata), LUA_VUSERDATA & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_TString(lua_State* L, TString* p)
{
  luaM_free_(L, p, sizeof(TString), LUA_VSTRING & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_CFuncInfo(lua_State* L, CFuncInfo* p)
{
  luaM_free_(L, p, sizeof(CFuncInfo), LUA_VLCF & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_Proto(lua_State* L, Proto* p)
{
  luaM_free_(L, p, sizeof(Proto), 0);
}

LUAI_FUNC void luaM_free_Table(lua_State* L, Table* p)
{
  luaM_free_(L, p, sizeof(Table), LUA_VTABLE & NOVARIANT_MASK);
}

LUAI_FUNC void luaM_free_LX(lua_State* L, LX* p)
{
  luaM_free_(L, p, sizeof(LX), LUA_TTHREAD);
}

LUAI_FUNC void luaM_free_CallInfo(lua_State* L, CallInfo* p)
{
  luaM_free_(L, p, sizeof(CallInfo), 0);
}
