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

/* engine-string form of file load: nullptr filename = stdin,
   nullptr mode = default "bt" */
int luaL_loadfilei(lua_State* L, const InternalString* filename,
                   const InternalString* mode);

/* engine-byte field access (keys are engine bytes, not host text) */
int lua_getfieldi(lua_State* L, int idx, std::string_view k);
void lua_setfieldi(lua_State* L, int idx, std::string_view k);

/* engine-byte form of lua_funcaddress (std::string as byte container) */
std::string lua_funcaddressi(lua_State* L, int idx);

/* internal form of lua_typename: the static engine string */
const InternalString* lua_typenamei(lua_State* L, int tp);

/* number->text into a caller buffer (>= LUA_N2SBUFFSZ bytes; the
   official out-param shape). Null view = value is not a number. */
std::string_view lua_numbertocstringi(lua_State* L, int idx, std::span<char> buff);

/* byte form of luaL_fileresult: null-view fname = no file name */
int luaL_fileresulti(lua_State* L, int stat, std::string_view fname);

/* byte forms of the gsub helpers; luaL_gsubi pushes the result and
   returns a BORROWED view (valid while the result stays on the stack) */
struct luaL_Buffer;
void luaL_addgsubi(luaL_Buffer* b, std::string_view s,
                   std::string_view p, std::string_view r);
std::string_view luaL_gsubi(lua_State* L, std::string_view s,
                            std::string_view p, std::string_view r);

/*
** ===============================================================
** Internal debug interface (RULED 2026-07-31: lua_Debug split).
** lua_Debug is the CONSUMER-facing record — HostString fields, the
** host "out param". InternalDebug is the engine-side twin, shaped
** like the original upstream struct: string_view fields pointing at
** internal data (static literals, Proto sources, engine strings —
** borrowed, valid for the duration of the debug call) and a value
** buffer for short_src. null view = absent (the original char* NULL
** convention; the consumer record uses empty-HostString instead).
** Internal code (ldblib, lauxlib traceback/funcname/argerror) must
** use these i-forms; the public HostString forms are thin wrappers
** that convert at the boundary.
** ===============================================================
*/
struct InternalDebug
{
  int event = 0;
  std::string_view name;     /* (n) null = no name available */
  std::string_view namewhat; /* (n) 'global'|'local'|'field'|'method'; null = no info */
  std::string_view what;     /* (S) 'Lua', 'C', 'main', 'tail' */
  std::string_view source;   /* (S) engine/static bytes */
  int currentline = 0;       /* (l) */
  int linedefined = 0;       /* (S) */
  int lastlinedefined = 0;   /* (S) */
  unsigned char nups = 0;    /* (u) */
  unsigned char nparams = 0; /* (u) */
  char isvararg = 0;         /* (u) */
  unsigned char extraargs = 0; /* (t) */
  char istailcall = 0;         /* (t) */
  int ftransfer = 0;           /* (r) */
  int ntransfer = 0;           /* (r) */
  char short_src[LUA_IDSIZE] = {0}; /* (S) value buffer (luaO_chunkid) */
  struct CallInfo* i_ci = nullptr;  /* active function */
};

std::optional<InternalDebug> lua_getstacki(lua_State* L, int level);
std::optional<InternalDebug> lua_getinfoi(lua_State* L, std::string_view options,
                                          const InternalDebug& ar);
/* local names: borrowed engine strings, nullptr = no such local */
const InternalString* lua_getlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_setlocali(lua_State* L, const InternalDebug* ar, int n);
/* upvalue names: borrowed engine strings, nullptr = no such upvalue */
const InternalString* lua_getupvaluei(lua_State* L, int funcindex, int n);
const InternalString* lua_setupvaluei(lua_State* L, int funcindex, int n);
