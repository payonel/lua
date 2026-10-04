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

namespace cost
{
constexpr int Byte = 1;
constexpr int Ref = 8;
constexpr int Instruction = 4;
constexpr int Value = 16;
constexpr int TValue = 24;
constexpr int Node = 48;
constexpr int LocVar = 16;
constexpr int Vardesc = 48;
constexpr int Upvaldesc = 24;
constexpr int Labeldesc = 24;
constexpr int AbsLineInfo = 8;
constexpr int CallInfo = 88;
constexpr int TString = 64;
constexpr int Udata = 72;
constexpr int Table = 96;
constexpr int CClosure = 48;
constexpr int LClosure = 48;
constexpr int CFuncInfo = 64;
constexpr int UpVal = 72;
constexpr int Proto = 168;
constexpr int Thread = 280;
constexpr int Global = 1024;
}

LUAI_FUNC l_noret luaM_toobig(lua_State* L);

struct global_State;

LUAI_FUNC bool luaM_reserve(lua_State* L, size_t size);
LUAI_FUNC void luaM_takeobj(lua_State* L, size_t size);
LUAI_FUNC void luaM_freeobj(lua_State* L, size_t size);
LUAI_FUNC void luaM_release(lua_State* L, size_t size);

// type allocators
namespace lua::object
{
struct TValue;
struct Value;
struct UpVal;
struct LocVar;
struct Upvaldesc;
struct Proto;
struct AbsLineInfo;
typedef ptrdiff_t StkId;
struct Node;
struct GCObject;
struct TString;
struct Udata;
struct Table;
struct CClosure;
struct LClosure;
struct CFuncInfo;
}
struct CallInfo;
struct Vardesc;
struct Labeldesc;
typedef l_uint32 Instruction;
LUAI_FUNC byte_owner luaM_alloc_char_span(global_State* g, lua_State* L, byte_span block, size_t count, bool bthrow);
LUAI_FUNC std::span<TValue> luaM_alloc_TValue_span(global_State* g, lua_State* L, std::span<TValue> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Value> luaM_alloc_Value_span(global_State* g, lua_State* L, std::span<Value> block, size_t count, bool bthrow);
LUAI_FUNC std::span<UpVal*> luaM_alloc_UpVal_ptr_span(global_State* g, lua_State* L, std::span<UpVal*> block, size_t count, bool bthrow);
LUAI_FUNC std::span<lu_byte> luaM_alloc_lu_byte_span(global_State* g, lua_State* L, std::span<lu_byte> block, size_t count, bool bthrow);
LUAI_FUNC std::span<StkId> luaM_alloc_StkId_span(global_State* g, lua_State* L, std::span<StkId> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Instruction> luaM_alloc_Instruction_span(global_State* g, lua_State* L, std::span<Instruction> block, size_t count, bool bthrow);
LUAI_FUNC std::span<ls_byte> luaM_alloc_ls_byte_span(global_State* g, lua_State* L, std::span<ls_byte> block, size_t count, bool bthrow);
LUAI_FUNC std::span<AbsLineInfo> luaM_alloc_AbsLineInfo_span(global_State* g, lua_State* L, std::span<AbsLineInfo> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Vardesc> luaM_alloc_Vardesc_span(global_State* g, lua_State* L, std::span<Vardesc> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Labeldesc> luaM_alloc_Labeldesc_span(global_State* g, lua_State* L, std::span<Labeldesc> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Proto*> luaM_alloc_Proto_ptr_span(global_State* g, lua_State* L, std::span<Proto*> block, size_t count, bool bthrow);
LUAI_FUNC std::span<LocVar> luaM_alloc_LocVar_span(global_State* g, lua_State* L, std::span<LocVar> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Upvaldesc> luaM_alloc_Upvaldesc_span(global_State* g, lua_State* L, std::span<Upvaldesc> block, size_t count, bool bthrow);
LUAI_FUNC std::span<Node> luaM_alloc_Node_span(global_State* g, lua_State* L, std::span<Node> block, size_t count, bool bthrow);
LUAI_FUNC std::span<LocVar> luaM_safegrow_LocVar_span(lua_State* L, std::span<LocVar> block, size_t nelems);
LUAI_FUNC std::span<Vardesc> luaM_safegrow_Vardesc_span(lua_State* L, std::span<Vardesc> block, size_t nelems);
LUAI_FUNC std::span<Upvaldesc> luaM_safegrow_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, size_t nelems);
LUAI_FUNC std::span<Labeldesc> luaM_safegrow_Labeldesc_span(lua_State* L, std::span<Labeldesc> block, size_t nelems);
LUAI_FUNC std::span<Proto*> luaM_safegrow_Proto_ptr_span(lua_State* L, std::span<Proto*> block, size_t nelems);
LUAI_FUNC std::span<AbsLineInfo> luaM_safegrow_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, size_t nelems);
LUAI_FUNC std::span<ls_byte> luaM_safegrow_ls_byte_span(lua_State* L, std::span<ls_byte> block, size_t nelems);
LUAI_FUNC std::span<Instruction> luaM_safegrow_Instruction_span(lua_State* L, std::span<Instruction> block, size_t nelems);
LUAI_FUNC std::span<TValue> luaM_safegrow_TValue_span(lua_State* L, std::span<TValue> block, size_t nelems);
LUAI_FUNC std::span<StkId> luaM_safegrow_StkId_span(lua_State* L, std::span<StkId> block, size_t nelems);

// typed GC-object allocators (throwing, fixed-size per type)
LUAI_FUNC TString* luaM_malloc_TString(lua_State* L);
LUAI_FUNC Udata* luaM_malloc_Udata(lua_State* L);
LUAI_FUNC Table* luaM_malloc_Table(lua_State* L);
LUAI_FUNC CClosure* luaM_malloc_CClosure(lua_State* L);
LUAI_FUNC LClosure* luaM_malloc_LClosure(lua_State* L);
LUAI_FUNC CFuncInfo* luaM_malloc_CFuncInfo(lua_State* L);
LUAI_FUNC UpVal* luaM_malloc_UpVal(lua_State* L);
LUAI_FUNC Proto* luaM_malloc_Proto(lua_State* L);
LUAI_FUNC lua_State* luaM_malloc_lua_State(lua_State* L);
LUAI_FUNC CallInfo* luaM_alloc_CallInfo(lua_State* L);

LUAI_FUNC void luaM_free_UpVal(lua_State* L, UpVal* p);
LUAI_FUNC void luaM_free_LClosure(lua_State* L, LClosure* p);
LUAI_FUNC void luaM_free_CClosure(lua_State* L, CClosure* p);
LUAI_FUNC void luaM_free_Udata(lua_State* L, Udata* p);
LUAI_FUNC void luaM_free_TString(lua_State* L, TString* p);
LUAI_FUNC void luaM_free_CFuncInfo(lua_State* L, CFuncInfo* p);
LUAI_FUNC void luaM_free_Proto(lua_State* L, Proto* p);
LUAI_FUNC void luaM_free_Table(lua_State* L, Table* p);
LUAI_FUNC void luaM_free_lua_State(lua_State* L, lua_State* p);
LUAI_FUNC void luaM_free_CallInfo(lua_State* L, CallInfo* p);
