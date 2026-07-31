/*
** lua_internal.h
** Internal-only API functions — port-specific extensions whose names
** are NOT part of the official Lua 5.5 specification (see
** https://www.lua.org/manual/5.5/manual.html). They were formerly
** declared LUA_API in lua.h; they are engine/library-layer plumbing,
** not host-facing surface, so they carry no export/linkage property
** (plain C++ linkage — the symbols mangle).
**
** INCLUSION RULE: this header must only be included from .cpp files.
** It must never be included from another header.
*/

#pragma once

#include "lua.h"

class InternalString;

/* distinguishes light C functions (port-specific type refinement) */
int lua_islightcfunction(lua_State* L, int idx);

/*
** Engine-side string accessor: the value as an engine string
** (InternalString — see internal_string.h), or nullptr if the value
** is neither a string nor a number (numbers are converted in place,
** as in 'lua_tolstring'). The returned pointer is BORROWED from the
** value on the stack: valid while the value stays there. This is the
** accessor the standard libraries use (in the Java port they receive
** the engine string object itself); the host-facing text API converts
** to HostString instead.
*/
const InternalString* lua_toistring(lua_State* L, int idx);

/*
** Engine-string form of lua_load: chunkname/mode are already engine
** bytes (the G.load path — Lua-string arguments never round-trip
** through host text); nullptr mode = default "bt".
*/
int lua_loadi(lua_State* L, lua_Reader reader, void* dt,
              const InternalString* chunkname,
              const InternalString* mode);

/*
** Byte-budget reservation (reserve-before-growth accounting):
** engine/library buffers whose growth is script-driven reserve
** capacity BEFORE allocating it, so the state's memory limit can
** veto (returns 0). Released bytes must have been reserved.
*/
int lua_reservebytes(lua_State* L, size_t sz);
void lua_releasebytes(lua_State* L, size_t sz);

/*
** Push by ADOPTION of an engine-owned byte buffer (zero copy; the
** complement of the deep-copying pushes for buffers built inside the
** engine/library layer, e.g. luaL_Buffer results). Never external
** bytes — those go through the copying pushes.
*/
void lua_pushistring(lua_State* L, InternalString&& s);
/* push a BORROWED engine string (deep copy/intern; never retained) —
   the push for lit<> constants and other InternalString* references */
void lua_pushistringref(lua_State* L, const InternalString* s);

/* per-state memory limit (port-specific) */
void lua_setmemlimit(lua_State* L, size_t limit);
size_t lua_getmemlimit(lua_State* L);

/*
** Deprecated official compat functions, kept ONLY because ltests.cpp
** exercises them (policy: no deprecated API is exported; unused ones
** are deleted — lua_getuservalue/lua_setuservalue are gone).
*/
void* lua_newuserdata(lua_State* L, size_t sz);
int lua_resetthread(lua_State* L);

/*
** ===============================================================
** Internal auxiliary-library functions (formerly LUALIB_API in
** lauxlib.h) — engine-string channels for the standard libraries.
** ===============================================================
*/

/*
** Engine-side string accessors (see lua_toistring): borrowed
** InternalString* views of stack values, for the standard libraries.
*/
const InternalString* luaL_checkistring(lua_State* L, int arg);
const InternalString* luaL_optistring(lua_State* L, int arg,
                                      const InternalString* def);
/* 'luaL_tolstring' companion: converts (honoring '__tostring'), PUSHES
   the result, and returns its payload (borrowed from the pushed value) */
const InternalString* luaL_toistring(lua_State* L, int idx);

/* engine-string form of buffer load: name/mode already engine bytes
   (nullable mode = default "bt") */
int luaL_loadbufferi(lua_State* L, std::string_view buff,
                     const InternalString* name,
                     const InternalString* mode);
