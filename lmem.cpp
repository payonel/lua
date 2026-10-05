/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/


#include "lmem.h"
#include "lstate.h"
#include "literals.h"
#include "lgc.h"
#include "ldo.h"
#include "ldebug.h"
#include "lparser.h"

#include "lopcodes.h"
#include "lfunc.h"

namespace lua::runtime
{

/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
static inline bool cantryagain(global_State* g)
{
  return completestate(g) && !g->gcstopem;
}

static bool luaM_permit_(global_State* g, lua_State* L, size_t osize, size_t nsize, int tag)
{
  if (osize == 0 && nsize == 0)
    return 1;
  if (l_unlikely(!g->frealloc->permit(osize, nsize, tag)))
  {
    if (!L)
      return false;
    if (!cantryagain(g))
      return false;
    luaC_fullgc(L, 1); /* try to free some memory... */
    if (!g->frealloc->permit(osize, nsize, tag))
      return false;
  }
  return true;
}

static inline bool _alloc_span_request(global_State* g, lua_State* L, size_t oldCount, size_t newCount, size_t esize, bool bthrow)
{
  if (newCount + 1 > MAX_SIZET / esize)
    luaM_toobig(L);
  size_t osz = oldCount * esize;
  size_t nsz = newCount * esize;
  if (luaM_permit_(g, L, osz, nsz, 0) || newCount == 0) // always allow release
  {
    if (L)
      g->GCdebt -= static_cast<l_mem>(nsz) - static_cast<l_mem>(osz);
  }
  else
  {
    if (bthrow && L)
    {
      luaD_throw(L, LUA_ERRMEM);
    }
    return false;
  }
  return true;
}

byte_owner luaM_alloc_char_span(global_State* g, lua_State* L, byte_span block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Byte, bthrow))) return {};
  byte_owner nb;
  if (count > 0)
  {
    nb = byte_owner(count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  return nb;
}

static inline void free_debt(lua_State* L, size_t size, int tag)
{
  global_State* g = L->l_G;
  g->frealloc->permit(size, 0, tag);
  g->GCdebt += (l_mem)size;
}

static inline void take_debt(lua_State* L, size_t size, int tag)
{
  global_State* g = L->l_G;
  if (l_unlikely(!luaM_permit_(g, L, 0, size, tag)))
    luaD_throw(L, LUA_ERRMEM);
  g->GCdebt -= (l_mem)size;
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
constexpr size_t MINSIZEARRAY = 4;

static inline size_t luaM_growsize_(lua_State* L, size_t size, size_t nelems, size_t limit, byte_view what)
{
  if (nelems + 1 <= size)
    return size;
  if (size >= limit / 2)
  {                                /* cannot double it? */
    if (l_unlikely(size >= limit)) /* cannot grow even a little? */
      luaG_runerror(L, lt::too_many_s_limit_is_d, FmtArgs().str(what).num((int)limit));
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

/* }================================================================== */

[[noreturn]] void luaM_toobig(lua_State* L)
{
  luaG_runerror(L, lt::memory_allocation_error_block_too_big);
}

void luaM_takeobj(lua_State* L, size_t size)
{
  take_debt(L, size, novariant(LUA_VUSERDATA));
}

void luaM_freeobj(lua_State* L, size_t size)
{
  free_debt(L, size, novariant(LUA_VUSERDATA));
}

// type allocators
std::span<TValue> luaM_alloc_TValue_span(global_State* g, lua_State* L, std::span<TValue> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::TValue, bthrow))) return {};
  std::span<TValue> nb{};
  if (count > 0)
  {
    nb = std::span<TValue>(new TValue[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        copyobj(&nb[ci], &block[ci]);
  }
  delete [] block.data();
  return nb;
}

std::span<Value> luaM_alloc_Value_span(global_State* g, lua_State* L, std::span<Value> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Value, bthrow))) return {};
  std::span<Value> nb{};
  if (count > 0)
  {
    nb = std::span<Value>(new Value[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<UpVal*> luaM_alloc_UpVal_ptr_span(global_State* g, lua_State* L, std::span<UpVal*> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Ref, bthrow))) return {};
  std::span<UpVal*> nb{};
  if (count > 0)
  {
    nb = std::span<UpVal*>(new UpVal*[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<lu_byte> luaM_alloc_lu_byte_span(global_State* g, lua_State* L, std::span<lu_byte> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Byte, bthrow))) return {};
  std::span<lu_byte> nb{};
  if (count > 0)
  {
    nb = std::span<lu_byte>(new lu_byte[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

CallInfo* luaM_alloc_CallInfo(lua_State* L)
{
  global_State* g = L->l_G;
  if (l_unlikely(!luaM_permit_(g, L, 0, cost::CallInfo, 0)))
    return nullptr; /* denied; do not update 'GCdebt' */
  g->GCdebt -= cost::CallInfo;
  return new CallInfo;
}

// typed grow/shrink span allocators
std::span<LocVar> luaM_alloc_LocVar_span(global_State* g, lua_State* L, std::span<LocVar> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::LocVar, bthrow))) return {};
  std::span<LocVar> nb{};
  if (count > 0)
  {
    nb = std::span<LocVar>(new LocVar[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<LocVar> luaM_safegrow_LocVar_span(lua_State* L, std::span<LocVar> block, size_t nelems)
{
  constexpr size_t limit = (SHRT_MAX <= MAX_SIZET / cost::LocVar) ? SHRT_MAX : (MAX_SIZET / cost::LocVar);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::local_variables);
  if (newsize == block.size())
    return block;
  return luaM_alloc_LocVar_span(L->l_G, L, block, newsize, true);
}

std::span<Vardesc> luaM_alloc_Vardesc_span(global_State* g, lua_State* L, std::span<Vardesc> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Vardesc, bthrow))) return {};
  std::span<Vardesc> nb{};
  if (count > 0)
  {
    nb = std::span<Vardesc>(new Vardesc[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = std::move(block[ci]);
  }
  delete [] block.data();
  return nb;
}

std::span<Vardesc> luaM_safegrow_Vardesc_span(lua_State* L, std::span<Vardesc> block, size_t nelems)
{
  constexpr size_t limit = (SHRT_MAX <= MAX_SIZET / cost::Vardesc) ? SHRT_MAX : (MAX_SIZET / cost::Vardesc);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::variable_declarations);
  if (newsize == block.size())
    return block;
  return luaM_alloc_Vardesc_span(L->l_G, L, block, newsize, true);
}

std::span<Upvaldesc> luaM_alloc_Upvaldesc_span(global_State* g, lua_State* L, std::span<Upvaldesc> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Upvaldesc, bthrow))) return {};
  std::span<Upvaldesc> nb{};
  if (count > 0)
  {
    nb = std::span<Upvaldesc>(new Upvaldesc[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<Upvaldesc> luaM_safegrow_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, size_t nelems)
{
  constexpr size_t limit = (MAXUPVAL <= MAX_SIZET / cost::Upvaldesc) ? MAXUPVAL : (MAX_SIZET / cost::Upvaldesc);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::upvalues);
  if (newsize == block.size())
    return block;
  return luaM_alloc_Upvaldesc_span(L->l_G, L, block, newsize, true);
}

std::span<Labeldesc> luaM_alloc_Labeldesc_span(global_State* g, lua_State* L, std::span<Labeldesc> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Labeldesc, bthrow))) return {};
  std::span<Labeldesc> nb{};
  if (count > 0)
  {
    nb = std::span<Labeldesc>(new Labeldesc[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<Labeldesc> luaM_safegrow_Labeldesc_span(lua_State* L, std::span<Labeldesc> block, size_t nelems)
{
  constexpr size_t limit = (SHRT_MAX <= MAX_SIZET / cost::Labeldesc) ? SHRT_MAX : (MAX_SIZET / cost::Labeldesc);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::labels_gotos);
  if (newsize == block.size())
    return block;
  return luaM_alloc_Labeldesc_span(L->l_G, L, block, newsize, true);
}

std::span<Proto*> luaM_alloc_Proto_ptr_span(global_State* g, lua_State* L, std::span<Proto*> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Ref, bthrow))) return {};
  std::span<Proto*> nb{};
  if (count > 0)
  {
    nb = std::span<Proto*>(new Proto*[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<Proto*> luaM_safegrow_Proto_ptr_span(lua_State* L, std::span<Proto*> block, size_t nelems)
{
  constexpr size_t limit = (MAXARG_Bx <= MAX_SIZET / cost::Ref) ? MAXARG_Bx : (MAX_SIZET / cost::Ref);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::functions);
  if (newsize == block.size())
    return block;
  return luaM_alloc_Proto_ptr_span(L->l_G, L, block, newsize, true);
}

std::span<AbsLineInfo> luaM_alloc_AbsLineInfo_span(global_State* g, lua_State* L, std::span<AbsLineInfo> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::AbsLineInfo, bthrow))) return {};
  std::span<AbsLineInfo> nb{};
  if (count > 0)
  {
    nb = std::span<AbsLineInfo>(new AbsLineInfo[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<AbsLineInfo> luaM_safegrow_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, size_t nelems)
{
  constexpr size_t limit = (INT_MAX <= MAX_SIZET / cost::AbsLineInfo) ? INT_MAX : (MAX_SIZET / cost::AbsLineInfo);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::lines);
  if (newsize == block.size())
    return block;
  return luaM_alloc_AbsLineInfo_span(L->l_G, L, block, newsize, true);
}

std::span<ls_byte> luaM_alloc_ls_byte_span(global_State* g, lua_State* L, std::span<ls_byte> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Byte, bthrow))) return {};
  std::span<ls_byte> nb{};
  if (count > 0)
  {
    nb = std::span<ls_byte>(new ls_byte[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<ls_byte> luaM_safegrow_ls_byte_span(lua_State* L, std::span<ls_byte> block, size_t nelems)
{
  constexpr size_t limit = (INT_MAX <= MAX_SIZET / cost::Byte) ? INT_MAX : (MAX_SIZET / cost::Byte);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::opcodes);
  if (newsize == block.size())
    return block;
  return luaM_alloc_ls_byte_span(L->l_G, L, block, newsize, true);
}

std::span<Instruction> luaM_alloc_Instruction_span(global_State* g, lua_State* L, std::span<Instruction> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Instruction, bthrow))) return {};
  std::span<Instruction> nb{};
  if (count > 0)
  {
    nb = std::span<Instruction>(new Instruction[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<Instruction> luaM_safegrow_Instruction_span(lua_State* L, std::span<Instruction> block, size_t nelems)
{
  constexpr size_t limit = (INT_MAX <= MAX_SIZET / cost::Instruction) ? INT_MAX : (MAX_SIZET / cost::Instruction);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::opcodes);
  if (newsize == block.size())
    return block;
  return luaM_alloc_Instruction_span(L->l_G, L, block, newsize, true);
}

std::span<TValue> luaM_safegrow_TValue_span(lua_State* L, std::span<TValue> block, size_t nelems)
{
  constexpr size_t limit = (MAXARG_Ax <= MAX_SIZET / cost::TValue) ? MAXARG_Ax : (MAX_SIZET / cost::TValue);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::constants);
  if (newsize == block.size())
    return block;
  return luaM_alloc_TValue_span(L->l_G, L, block, newsize, true);
}

std::span<StkId> luaM_alloc_StkId_span(global_State* g, lua_State* L, std::span<StkId> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Ref, bthrow))) return {};
  std::span<StkId> nb{};
  if (count > 0)
  {
    nb = std::span<StkId>(new StkId[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        nb[ci] = block[ci];
  }
  delete [] block.data();
  return nb;
}

std::span<StkId> luaM_safegrow_StkId_span(lua_State* L, std::span<StkId> block, size_t nelems)
{
  constexpr size_t limit = (INT_MAX <= MAX_SIZET / cost::Ref) ? INT_MAX : (MAX_SIZET / cost::Ref);
  size_t newsize = luaM_growsize_(L, block.size(), nelems, limit, lt::to_be_closed_variables);
  if (newsize == block.size())
    return block;
  return luaM_alloc_StkId_span(L->l_G, L, block, newsize, true);
}

// typed fresh (throwing) allocators
std::span<Node> luaM_alloc_Node_span(global_State* g, lua_State* L, std::span<Node> block, size_t count, bool bthrow)
{
  if (l_unlikely(!_alloc_span_request(g, L, block.size(), count, cost::Node, bthrow))) return {};
  std::span<Node> nb{};
  if (count > 0)
  {
    nb = std::span<Node>(new Node[count], count);
    if (!block.empty())
      for (size_t ci = 0; ci < std::min<size_t>(count, block.size()); ci++)
        copynode(&nb[ci], &block[ci]);
  }
  delete [] block.data();
  return nb;
}

TString* luaM_malloc_TString(lua_State* L, size_t bytes)
{
  take_debt(L, cost::TString + bytes, novariant(LUA_VSTRING));
  return new TString;
}

Udata* luaM_malloc_Udata(lua_State* L)
{
  take_debt(L, cost::Udata, novariant(LUA_VUSERDATA));
  return new Udata;
}

Table* luaM_malloc_Table(lua_State* L)
{
  take_debt(L, cost::Table, novariant(LUA_VTABLE));
  return new Table;
}

CClosure* luaM_malloc_CClosure(lua_State* L)
{
  take_debt(L, cost::CClosure, novariant(LUA_VCCL));
  return new CClosure;
}

LClosure* luaM_malloc_LClosure(lua_State* L)
{
  take_debt(L, cost::LClosure, novariant(LUA_VLCL));
  return new LClosure;
}

CFuncInfo* luaM_malloc_CFuncInfo(lua_State* L)
{
  take_debt(L, cost::CFuncInfo, novariant(LUA_VLCF));
  return new CFuncInfo;
}

UpVal* luaM_malloc_UpVal(lua_State* L)
{
  take_debt(L, cost::UpVal, novariant(LUA_VUPVAL));
  return new UpVal;
}

Proto* luaM_malloc_Proto(lua_State* L)
{
  take_debt(L, cost::Proto, novariant(LUA_VPROTO));
  return new Proto;
}

lua_State* luaM_malloc_lua_State(lua_State* L)
{
  take_debt(L, cost::Thread, novariant(LUA_TTHREAD));
  return new lua_State;
}

void luaM_free_UpVal(lua_State* L, UpVal* p)
{
  if (p != nullptr) free_debt(L, cost::UpVal, 0);
  delete p;
}

void luaM_free_LClosure(lua_State* L, LClosure* p)
{
  if (p != nullptr) free_debt(L, cost::LClosure, novariant(LUA_VLCL));
  delete p;
}

void luaM_free_CClosure(lua_State* L, CClosure* p)
{
  if (p != nullptr) free_debt(L, cost::CClosure, novariant(LUA_VCCL));
  delete p;
}

void luaM_free_Udata(lua_State* L, Udata* p)
{
  if (p != nullptr) free_debt(L, cost::Udata, novariant(LUA_VUSERDATA));
  delete p;
}

void luaM_free_TString(lua_State* L, TString* p, size_t bytes)
{
  if (p != nullptr) free_debt(L, cost::TString + bytes, novariant(LUA_VSTRING));
  delete p;
}

void luaM_free_CFuncInfo(lua_State* L, CFuncInfo* p)
{
  if (p != nullptr) free_debt(L, cost::CFuncInfo, novariant(LUA_VLCF));
  delete p;
}

void luaM_free_Proto(lua_State* L, Proto* p)
{
  if (p != nullptr) free_debt(L, cost::Proto, 0);
  delete p;
}

void luaM_free_Table(lua_State* L, Table* p)
{
  if (p != nullptr) free_debt(L, cost::Table, novariant(LUA_VTABLE));
  delete p;
}

void luaM_free_lua_State(lua_State* L, lua_State* p)
{
  if (p != nullptr) free_debt(L, cost::Thread, LUA_TTHREAD);
  delete p;
}

void luaM_free_CallInfo(lua_State* L, CallInfo* p)
{
  if (p != nullptr) free_debt(L, cost::CallInfo, 0);
  delete p;
}

}
