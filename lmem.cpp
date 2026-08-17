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

// type allocators
LUAI_FUNC std::span<TValue> luaM_alloc_TValue_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  TValue* stk = (TValue*)luaM_realloc_(L, nullptr, 0, count * sizeof(TValue));
  return std::span<TValue>(stk, stk == nullptr ? 0 : count);
}

LUAI_FUNC std::span<Value> luaM_alloc_Value_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  Value* p = (Value*)luaM_realloc_(L, nullptr, 0, count * sizeof(Value));
  return std::span<Value>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<UpVal*> luaM_alloc_UpVal_ptr_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  UpVal** p = (UpVal**)luaM_realloc_(L, nullptr, 0, count * sizeof(UpVal*));
  return std::span<UpVal*>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<char> luaM_alloc_char_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  char* p = (char*)luaM_realloc_(L, nullptr, 0, count);
  return std::span<char>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<char> luaM_saferealloc_char_span(lua_State* L, std::span<char> block, size_t count)
{
  char* p = (char*)luaM_saferealloc_(L, block.data(), block.size(), count);
  return std::span<char>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC CallInfo* luaM_alloc_CallInfo(lua_State* L)
{
  return (CallInfo*)luaM_realloc_(L, nullptr, 0, sizeof(CallInfo));
}

// typed grow/shrink span allocators
LUAI_FUNC std::span<LocVar> luaM_safegrow_LocVar_span(lua_State* L, std::span<LocVar> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  LocVar* p = (LocVar*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(LocVar), limit, what);
  return std::span<LocVar>(p, (size_t)size);
}

LUAI_FUNC std::span<Vardesc> luaM_safegrow_Vardesc_span(lua_State* L, std::span<Vardesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  Vardesc* p = (Vardesc*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(Vardesc), limit, what);
  return std::span<Vardesc>(p, (size_t)size);
}

LUAI_FUNC std::span<Upvaldesc> luaM_safegrow_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  Upvaldesc* p = (Upvaldesc*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(Upvaldesc), limit, what);
  return std::span<Upvaldesc>(p, (size_t)size);
}

LUAI_FUNC std::span<Labeldesc> luaM_safegrow_Labeldesc_span(lua_State* L, std::span<Labeldesc> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  Labeldesc* p = (Labeldesc*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(Labeldesc), limit, what);
  return std::span<Labeldesc>(p, (size_t)size);
}

LUAI_FUNC std::span<Proto*> luaM_safegrow_Proto_ptr_span(lua_State* L, std::span<Proto*> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  Proto** p = (Proto**)luaM_growaux_(L, block.data(), nelems, &size, sizeof(Proto*), limit, what);
  return std::span<Proto*>(p, (size_t)size);
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

LUAI_FUNC std::span<StkId> luaM_safegrow_StkId_span(lua_State* L, std::span<StkId> block, int nelems, int limit, byte_view what)
{
  int size = (int)block.size();
  StkId* p = (StkId*)luaM_growaux_(L, block.data(), nelems, &size, sizeof(StkId), limit, what);
  return std::span<StkId>(p, (size_t)size);
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
  int size = (int)block.size();
  Proto** p = (Proto**)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(Proto*));
  return std::span<Proto*>(p, (size_t)size);
}

LUAI_FUNC std::span<LocVar> luaM_safeshrink_LocVar_span(lua_State* L, std::span<LocVar> block, int final_n)
{
  int size = (int)block.size();
  LocVar* p = (LocVar*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(LocVar));
  return std::span<LocVar>(p, (size_t)size);
}

LUAI_FUNC std::span<Upvaldesc> luaM_safeshrink_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int final_n)
{
  int size = (int)block.size();
  Upvaldesc* p = (Upvaldesc*)luaM_shrinkvector_(L, block.data(), &size, final_n, sizeof(Upvaldesc));
  return std::span<Upvaldesc>(p, (size_t)size);
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

LUAI_FUNC std::span<Proto*> luaM_malloc_Proto_ptr_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(Proto*))
    luaM_toobig(L);
  Proto** p = (Proto**)luaM_malloc_(L, count * sizeof(Proto*), 0);
  return std::span<Proto*>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<Upvaldesc> luaM_malloc_Upvaldesc_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(Upvaldesc))
    luaM_toobig(L);
  Upvaldesc* p = (Upvaldesc*)luaM_malloc_(L, count * sizeof(Upvaldesc), 0);
  return std::span<Upvaldesc>(p, p == nullptr ? 0 : count);
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

LUAI_FUNC std::span<LocVar> luaM_malloc_LocVar_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(LocVar))
    luaM_toobig(L);
  LocVar* p = (LocVar*)luaM_malloc_(L, count * sizeof(LocVar), 0);
  return std::span<LocVar>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<char> luaM_malloc_char_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(char))
    luaM_toobig(L);
  char* p = (char*)luaM_malloc_(L, count * sizeof(char), 0);
  return std::span<char>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC std::span<Node> luaM_malloc_Node_span(lua_State* L, size_t count)
{
  if (count == 0)
    return {};
  if (count + 1 > MAX_SIZET / sizeof(Node))
    luaM_toobig(L);
  Node* p = (Node*)luaM_malloc_(L, count * sizeof(Node), 0);
  return std::span<Node>(p, p == nullptr ? 0 : count);
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

// typed raw (g-based) char realloc
LUAI_FUNC std::span<char> luaM_rawrealloc_char_span(global_State* g, std::span<char> block, size_t count)
{
  char* p = (char*)luaM_rawrealloc(g, block.data(), block.size(), count, 0);
  return std::span<char>(p, p == nullptr ? 0 : count);
}

LUAI_FUNC void luaM_rawfree_char_span(global_State* g, std::span<char> block)
{
  if (block.data() != nullptr)
    luaM_rawrealloc(g, block.data(), block.size(), 0, 0);
}

// typed span frees
LUAI_FUNC void luaM_free_TValue_span(lua_State* L, std::span<TValue> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(TValue), 0);
}

LUAI_FUNC void luaM_free_Value_span(lua_State* L, std::span<Value> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Value), 0);
}

LUAI_FUNC void luaM_free_UpVal_ptr_span(lua_State* L, std::span<UpVal*> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(UpVal*), 0);
}

LUAI_FUNC void luaM_free_char_span(lua_State* L, std::span<char> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(char), 0);
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

LUAI_FUNC void luaM_free_Proto_ptr_span(lua_State* L, std::span<Proto*> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Proto*), 0);
}

LUAI_FUNC void luaM_free_LocVar_span(lua_State* L, std::span<LocVar> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(LocVar), 0);
}

LUAI_FUNC void luaM_free_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Upvaldesc), 0);
}

LUAI_FUNC void luaM_free_Node_span(lua_State* L, std::span<Node> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Node), 0);
}

LUAI_FUNC void luaM_free_StkId_span(lua_State* L, std::span<StkId> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(StkId), 0);
}

LUAI_FUNC void luaM_free_Vardesc_span(lua_State* L, std::span<Vardesc> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Vardesc), 0);
}

LUAI_FUNC void luaM_free_Labeldesc_span(lua_State* L, std::span<Labeldesc> block)
{
  luaM_free_(L, block.data(), block.size() * sizeof(Labeldesc), 0);
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
