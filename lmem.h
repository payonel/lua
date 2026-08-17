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

/* not to be called directly */
LUAI_FUNC int luaM_reserve(lua_State* L, size_t size);
LUAI_FUNC void luaM_release(lua_State* L, size_t size);

LUAI_FUNC int luaM_growsize_(lua_State* L, int size, int nelems, int limit, byte_view what);

LUAI_FUNC std::span<char> luaM_alloc_char_span(global_State* g, lua_State* L, std::span<char> block, size_t count, bool bthrow);

// type allocators
struct TValue;
struct Value;
struct UpVal;
struct CallInfo;
LUAI_FUNC std::span<TValue> luaM_alloc_TValue_span(lua_State* L, size_t count);
LUAI_FUNC std::span<Value> luaM_alloc_Value_span(lua_State* L, size_t count);
LUAI_FUNC std::span<UpVal*> luaM_alloc_UpVal_ptr_span(lua_State* L, size_t count);
LUAI_FUNC std::span<lu_byte> luaM_alloc_lu_byte_span(global_State* g, lua_State* L, std::span<lu_byte> block, size_t count, bool bthrow);
LUAI_FUNC CallInfo* luaM_alloc_CallInfo(lua_State* L);

// typed grow/shrink span allocators
struct LocVar;
struct Vardesc;
struct Upvaldesc;
struct Labeldesc;
struct Proto;
struct AbsLineInfo;
typedef l_uint32 Instruction;
typedef size_t StkId;

LUAI_FUNC std::span<LocVar> luaM_safegrow_LocVar_span(lua_State* L, std::span<LocVar> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<Vardesc> luaM_safegrow_Vardesc_span(lua_State* L, std::span<Vardesc> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<Upvaldesc> luaM_safegrow_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<Labeldesc> luaM_safegrow_Labeldesc_span(lua_State* L, std::span<Labeldesc> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<Proto*> luaM_safegrow_Proto_ptr_span(lua_State* L, std::span<Proto*> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<AbsLineInfo> luaM_safegrow_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<ls_byte> luaM_safegrow_ls_byte_span(lua_State* L, std::span<ls_byte> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<Instruction> luaM_safegrow_Instruction_span(lua_State* L, std::span<Instruction> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<TValue> luaM_safegrow_TValue_span(lua_State* L, std::span<TValue> block, int nelems, int limit, byte_view what);
LUAI_FUNC std::span<StkId> luaM_safegrow_StkId_span(lua_State* L, std::span<StkId> block, int nelems, int limit, byte_view what);

LUAI_FUNC std::span<Instruction> luaM_safeshrink_Instruction_span(lua_State* L, std::span<Instruction> block, int final_n);
LUAI_FUNC std::span<ls_byte> luaM_safeshrink_ls_byte_span(lua_State* L, std::span<ls_byte> block, int final_n);
LUAI_FUNC std::span<AbsLineInfo> luaM_safeshrink_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block, int final_n);
LUAI_FUNC std::span<TValue> luaM_safeshrink_TValue_span(lua_State* L, std::span<TValue> block, int final_n);
LUAI_FUNC std::span<Proto*> luaM_safeshrink_Proto_ptr_span(lua_State* L, std::span<Proto*> block, int final_n);
LUAI_FUNC std::span<LocVar> luaM_safeshrink_LocVar_span(lua_State* L, std::span<LocVar> block, int final_n);
LUAI_FUNC std::span<Upvaldesc> luaM_safeshrink_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block, int final_n);

// typed fresh (throwing) allocators
struct Node;
struct GCObject;
LUAI_FUNC std::span<Instruction> luaM_malloc_Instruction_span(lua_State* L, size_t count);
LUAI_FUNC std::span<TValue> luaM_malloc_TValue_span(lua_State* L, size_t count);
LUAI_FUNC std::span<Proto*> luaM_malloc_Proto_ptr_span(lua_State* L, size_t count);
LUAI_FUNC std::span<Upvaldesc> luaM_malloc_Upvaldesc_span(lua_State* L, size_t count);
LUAI_FUNC std::span<ls_byte> luaM_malloc_ls_byte_span(lua_State* L, size_t count);
LUAI_FUNC std::span<AbsLineInfo> luaM_malloc_AbsLineInfo_span(lua_State* L, size_t count);
LUAI_FUNC std::span<LocVar> luaM_malloc_LocVar_span(lua_State* L, size_t count);
LUAI_FUNC std::span<Node> luaM_malloc_Node_span(lua_State* L, size_t count);

// typed GC-object allocators (throwing, fixed-size per type)
struct TString;
struct Udata;
struct Table;
struct CClosure;
struct LClosure;
struct CFuncInfo;
struct LX;
LUAI_FUNC TString* luaM_malloc_TString(lua_State* L);
LUAI_FUNC Udata* luaM_malloc_Udata(lua_State* L);
LUAI_FUNC Table* luaM_malloc_Table(lua_State* L);
LUAI_FUNC CClosure* luaM_malloc_CClosure(lua_State* L);
LUAI_FUNC LClosure* luaM_malloc_LClosure(lua_State* L);
LUAI_FUNC CFuncInfo* luaM_malloc_CFuncInfo(lua_State* L);
LUAI_FUNC UpVal* luaM_malloc_UpVal(lua_State* L);
LUAI_FUNC Proto* luaM_malloc_Proto(lua_State* L);
LUAI_FUNC LX* luaM_malloc_LX(lua_State* L);

// typed span frees
LUAI_FUNC void luaM_free_TValue_span(lua_State* L, std::span<TValue> block);
LUAI_FUNC void luaM_free_Value_span(lua_State* L, std::span<Value> block);
LUAI_FUNC void luaM_free_UpVal_ptr_span(lua_State* L, std::span<UpVal*> block);
LUAI_FUNC void luaM_free_Instruction_span(lua_State* L, std::span<Instruction> block);
LUAI_FUNC void luaM_free_ls_byte_span(lua_State* L, std::span<ls_byte> block);
LUAI_FUNC void luaM_free_AbsLineInfo_span(lua_State* L, std::span<AbsLineInfo> block);
LUAI_FUNC void luaM_free_Proto_ptr_span(lua_State* L, std::span<Proto*> block);
LUAI_FUNC void luaM_free_LocVar_span(lua_State* L, std::span<LocVar> block);
LUAI_FUNC void luaM_free_Upvaldesc_span(lua_State* L, std::span<Upvaldesc> block);
LUAI_FUNC void luaM_free_Node_span(lua_State* L, std::span<Node> block);
LUAI_FUNC void luaM_free_StkId_span(lua_State* L, std::span<StkId> block);
LUAI_FUNC void luaM_free_Vardesc_span(lua_State* L, std::span<Vardesc> block);
LUAI_FUNC void luaM_free_Labeldesc_span(lua_State* L, std::span<Labeldesc> block);

// typed object frees
LUAI_FUNC void luaM_free_UpVal(lua_State* L, UpVal* p);
LUAI_FUNC void luaM_free_LClosure(lua_State* L, LClosure* p);
LUAI_FUNC void luaM_free_CClosure(lua_State* L, CClosure* p);
LUAI_FUNC void luaM_free_Udata(lua_State* L, Udata* p);
LUAI_FUNC void luaM_free_TString(lua_State* L, TString* p);
LUAI_FUNC void luaM_free_CFuncInfo(lua_State* L, CFuncInfo* p);
LUAI_FUNC void luaM_free_Proto(lua_State* L, Proto* p);
LUAI_FUNC void luaM_free_Table(lua_State* L, Table* p);
LUAI_FUNC void luaM_free_LX(lua_State* L, LX* p);
LUAI_FUNC void luaM_free_CallInfo(lua_State* L, CallInfo* p);
