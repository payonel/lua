/*
** $Id: lua.h $
** Lua - A Scripting Language
** Lua.org, PUC-Rio, Brazil (www.lua.org)
** See Copyright Notice at the end of this file
*/

#pragma once

#include <stddef.h>
#include <optional>
#include <span>
#include <string>
#include "luaconf.h"
#include "host_string.h"

inline constexpr int LUA_VERSION_MAJOR_N = 5;
inline constexpr int LUA_VERSION_MINOR_N = 5;
inline constexpr int LUA_VERSION_RELEASE_N = 1;

inline constexpr int LUA_VERSION_NUM = LUA_VERSION_MAJOR_N * 100 + LUA_VERSION_MINOR_N;
inline constexpr int LUA_VERSION_RELEASE_NUM = LUA_VERSION_NUM * 100 + LUA_VERSION_RELEASE_N;

inline constexpr byte_view LUA_VERSION_MAJOR("5");
inline constexpr byte_view LUA_VERSION_MINOR("5");
inline constexpr byte_view LUA_VERSION_RELEASE("1");
inline constexpr byte_view LUA_VERSION("Lua 5.5");
inline constexpr byte_view LUA_RELEASE("Lua 5.5.1");
inline constexpr byte_view LUA_COPYRIGHT("Lua 5.5.1  Copyright (C) 1994-2026 Lua.org, PUC-Rio");
inline constexpr byte_view LUA_AUTHORS("R. Ierusalimschy, L. H. de Figueiredo, W. Celes");
static_assert(LUA_VERSION_MAJOR_N == 5 && LUA_VERSION_MINOR_N == 5 && LUA_VERSION_RELEASE_N == 1, "update the version strings above (and LUA_VDIR in luaconf.h) to match the new numbers");

/* mark for precompiled code ('<esc>Lua') */
inline constexpr byte_view LUA_SIGNATURE("\x1bLua");

/* option for multiple returns in 'lua_pcall' and 'lua_call' */
constexpr int LUA_MULTRET = (-1);

/*
** Pseudo-indices
** (The stack size is limited to INT_MAX/2; we keep some free empty
** space after that to help overflow detection.)
*/
constexpr int LUA_REGISTRYINDEX = (-(INT_MAX / 2 + 1000));
LUA_API int lua_upvalueindex(int i);

/* thread status */
constexpr int LUA_OK = 0;
constexpr int LUA_YIELD = 1;
constexpr int LUA_ERRRUN = 2;
constexpr int LUA_ERRSYNTAX = 3;
constexpr int LUA_ERRMEM = 4;
constexpr int LUA_ERRERR = 5;

struct lua_State;
struct CFuncInfo;

/*
** basic types
*/
constexpr int LUA_TNONE = (-1);

constexpr int LUA_TNIL = 0;
constexpr int LUA_TBOOLEAN = 1;
constexpr int LUA_TLIGHTUSERDATA = 2;
constexpr int LUA_TNUMBER = 3;
constexpr int LUA_TSTRING = 4;
constexpr int LUA_TTABLE = 5;
constexpr int LUA_TFUNCTION = 6;
constexpr int LUA_TUSERDATA = 7;
constexpr int LUA_TTHREAD = 8;

constexpr int LUA_NUMTYPES = 9;

/* minimum Lua stack available to a C function */
constexpr int LUA_MINSTACK = 20;

/* predefined values in the registry */
/* index 1 is reserved for the reference mechanism */
constexpr int LUA_RIDX_GLOBALS = 2;
constexpr int LUA_RIDX_MAINTHREAD = 3;
constexpr int LUA_RIDX_LAST = 3;

typedef double lua_Number;
typedef long long lua_Integer;
typedef unsigned long long lua_Unsigned;
typedef ptrdiff_t lua_KContext;

/*
** Type for C functions registered with Lua
*/
typedef int (*lua_CFunction)(lua_State* L);

/*
** Type for continuation functions
*/
typedef int (*lua_KFunction)(lua_State* L, int status, lua_KContext ctx);

typedef std::optional<byte_view> (*lua_Reader)(lua_State* L, void* ud);

typedef int (*lua_Writer)(lua_State* L, std::optional<byte_view> b, void* ud);

/*
** Type for memory-allocation functions
*/
typedef void* (*lua_Alloc)(void* ud, void* ptr, size_t osize, size_t nsize);

typedef void (*lua_WarnFunction)(void* ud, const HostString& msg, int tocont);

/*
** Type used by the debug API to collect debug information
*/
struct lua_Debug;

/*
** Functions to be called by the debugger in specific events
*/
typedef void (*lua_Hook)(lua_State* L, lua_Debug* ar);

/*
** RCS ident string
*/
extern const byte_view lua_ident;

/*
** state manipulation
*/
LUA_API lua_State*(lua_newstate) (lua_Alloc f, void* ud, unsigned seed);
LUA_API void(lua_close)(lua_State* L);
LUA_API lua_State*(lua_newthread) (lua_State* L);
LUA_API int(lua_closethread)(lua_State* L, lua_State* from);

LUA_API lua_CFunction(lua_atpanic)(lua_State* L, lua_CFunction panicf);

LUA_API lua_Number(lua_version)(lua_State* L);

/*
** basic stack manipulation
*/
LUA_API int(lua_absindex)(lua_State* L, int idx);
LUA_API int(lua_gettop)(lua_State* L);
LUA_API void(lua_settop)(lua_State* L, int idx);
LUA_API void(lua_pushvalue)(lua_State* L, int idx);
LUA_API void(lua_rotate)(lua_State* L, int idx, int n);
LUA_API void(lua_copy)(lua_State* L, int fromidx, int toidx);
LUA_API int(lua_checkstack)(lua_State* L, int n);

LUA_API void(lua_xmove)(lua_State* from, lua_State* to, int n);

/*
** access functions (stack -> C)
*/

LUA_API int(lua_isnumber)(lua_State* L, int idx);
LUA_API int(lua_isstring)(lua_State* L, int idx);
LUA_API int(lua_iscfunction)(lua_State* L, int idx);
LUA_API int(lua_isinteger)(lua_State* L, int idx);
LUA_API int(lua_isuserdata)(lua_State* L, int idx);
LUA_API int(lua_type)(lua_State* L, int idx);
LUA_API HostString(lua_typename) (lua_State* L, int tp);

LUA_API std::optional<lua_Number>(lua_tonumberx)(lua_State* L, int idx);
LUA_API std::optional<lua_Integer>(lua_tointegerx)(lua_State* L, int idx);
LUA_API int(lua_toboolean)(lua_State* L, int idx);

LUA_API std::optional<byte_view>(lua_tolstring) (lua_State* L, int idx);

LUA_API lua_Unsigned(lua_rawlen)(lua_State* L, int idx);
LUA_API lua_CFunction(lua_tocfunction)(lua_State* L, int idx);
LUA_API void*(lua_touserdata) (lua_State* L, int idx);
LUA_API lua_State*(lua_tothread) (lua_State* L, int idx);
LUA_API const void*(lua_topointer) (lua_State* L, int idx);

/*
** Comparison and arithmetic functions
*/

constexpr int LUA_OPADD = 0; /* ORDER TM, ORDER OP */
constexpr int LUA_OPSUB = 1;
constexpr int LUA_OPMUL = 2;
constexpr int LUA_OPMOD = 3;
constexpr int LUA_OPPOW = 4;
constexpr int LUA_OPDIV = 5;
constexpr int LUA_OPIDIV = 6;
constexpr int LUA_OPBAND = 7;
constexpr int LUA_OPBOR = 8;
constexpr int LUA_OPBXOR = 9;
constexpr int LUA_OPSHL = 10;
constexpr int LUA_OPSHR = 11;
constexpr int LUA_OPUNM = 12;
constexpr int LUA_OPBNOT = 13;

LUA_API void(lua_arith)(lua_State* L, int op);

constexpr int LUA_OPEQ = 0;
constexpr int LUA_OPLT = 1;
constexpr int LUA_OPLE = 2;

LUA_API int(lua_rawequal)(lua_State* L, int idx1, int idx2);
LUA_API int(lua_compare)(lua_State* L, int idx1, int idx2, int op);

/*
** push functions (C -> stack)
*/
LUA_API void(lua_pushnil)(lua_State* L);
LUA_API void(lua_pushnumber)(lua_State* L, lua_Number n);
LUA_API void(lua_pushinteger)(lua_State* L, lua_Integer n);
LUA_API byte_view(lua_pushlstring) (lua_State* L, byte_view s, size_t off, size_t len);
LUA_API void(lua_pushstring) (lua_State* L, const std::optional<HostString>& s);
LUA_API HostString(lua_pushfstring) (lua_State* L, const HostString& fmt);
LUA_API void(lua_pushcclosure)(lua_State* L, lua_CFunction fn, int n);
LUA_API CFuncInfo*(lua_createcfuncinfo)(lua_State* L, lua_CFunction fn, std::optional<HostString> name = std::nullopt);
LUA_API void(lua_pushcfuncinfo)(lua_State* L, CFuncInfo* cfi);
LUA_API void(lua_pushboolean)(lua_State* L, int b);
LUA_API void(lua_pushlightuserdata)(lua_State* L, void* p);
LUA_API int(lua_pushthread)(lua_State* L);

/*
** get functions (Lua -> stack)
*/
LUA_API int(lua_getglobal)(lua_State* L, const HostString& name);
LUA_API int(lua_gettable)(lua_State* L, int idx);
LUA_API int(lua_getfield)(lua_State* L, int idx, const HostString& k);
LUA_API int(lua_geti)(lua_State* L, int idx, lua_Integer n);
LUA_API int(lua_rawget)(lua_State* L, int idx);
LUA_API int(lua_rawgeti)(lua_State* L, int idx, lua_Integer n);
LUA_API int(lua_rawgetp)(lua_State* L, int idx, const void* p);

LUA_API void(lua_createtable)(lua_State* L, int narr, int nrec);
LUA_API void*(lua_newuserdatauv) (lua_State* L, size_t sz, int nuvalue);
LUA_API int(lua_getmetatable)(lua_State* L, int objindex);
LUA_API int(lua_getiuservalue)(lua_State* L, int idx, int n);

/*
** set functions (stack -> Lua)
*/
LUA_API void(lua_setglobal)(lua_State* L, const HostString& name);
LUA_API void(lua_settable)(lua_State* L, int idx);
LUA_API void(lua_setfield)(lua_State* L, int idx, const HostString& k);
LUA_API void(lua_seti)(lua_State* L, int idx, lua_Integer n);
LUA_API void(lua_rawset)(lua_State* L, int idx);
LUA_API void(lua_rawseti)(lua_State* L, int idx, lua_Integer n);
LUA_API void(lua_rawsetp)(lua_State* L, int idx, const void* p);
LUA_API int(lua_setmetatable)(lua_State* L, int objindex);
LUA_API int(lua_setiuservalue)(lua_State* L, int idx, int n);

/*
** 'load' and 'call' functions (load and run Lua code)
*/
LUA_API void(lua_callk)(lua_State* L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k);
LUA_API void lua_call(lua_State* L, int nargs, int nresults);

LUA_API int(lua_pcallk)(lua_State* L, int nargs, int nresults, int errfunc,
                        lua_KContext ctx, lua_KFunction k);
LUA_API int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc);

LUA_API int(lua_load)(lua_State* L, lua_Reader reader, void* dt,
                      const std::optional<HostString>& chunkname,
                      const std::optional<HostString>& mode);

LUA_API int(lua_dump)(lua_State* L, lua_Writer writer, void* data, int strip);

/*
** coroutine functions
*/
LUA_API int(lua_yieldk)(lua_State* L, int nresults, lua_KContext ctx,
                        lua_KFunction k);
struct lua_ResumeResult
{
  int status;
  int nres;
};
LUA_API lua_ResumeResult(lua_resume)(lua_State* L, lua_State* from, int narg);
LUA_API int(lua_status)(lua_State* L);
LUA_API int(lua_isyieldable)(lua_State* L);

LUA_API int lua_yield(lua_State* L, int nresults);

/*
** Warning-related functions
*/
LUA_API void(lua_setwarnf)(lua_State* L, lua_WarnFunction f, void* ud);
LUA_API void(lua_warning)(lua_State* L, const HostString& msg, int tocont);

/*
** garbage-collection options
*/

constexpr int LUA_GCSTOP = 0;
constexpr int LUA_GCRESTART = 1;
constexpr int LUA_GCCOLLECT = 2;
constexpr int LUA_GCCOUNT = 3;
constexpr int LUA_GCCOUNTB = 4;
constexpr int LUA_GCSTEP = 5;
constexpr int LUA_GCISRUNNING = 6;
constexpr int LUA_GCGEN = 7;
constexpr int LUA_GCINC = 8;
constexpr int LUA_GCPARAM = 9;

/*
** garbage-collection parameters
*/
/* parameters for generational mode */
constexpr int LUA_GCPMINORMUL = 0; /* control minor collections */
constexpr int LUA_GCPMAJORMINOR = 1; /* control shift major->minor */
constexpr int LUA_GCPMINORMAJOR = 2; /* control shift minor->major */

/* parameters for incremental mode */
constexpr int LUA_GCPPAUSE = 3; /* size of pause between successive GCs */
constexpr int LUA_GCPSTEPMUL = 4; /* GC "speed" */
constexpr int LUA_GCPSTEPSIZE = 5; /* GC granularity */

/* number of parameters */
constexpr int LUA_GCPN = 6;

LUA_API int(lua_gc)(lua_State* L, int what, size_t stepsize,
                    int param, int value);

/*
** miscellaneous functions
*/

LUA_API int(lua_error)(lua_State* L);

LUA_API int(lua_next)(lua_State* L, int idx);

LUA_API void(lua_concat)(lua_State* L, int n);
LUA_API void(lua_len)(lua_State* L, int idx);

constexpr int LUA_N2SBUFFSZ = 64;
LUA_API std::optional<HostString>(lua_numbertocstring)(lua_State* L, int idx);
LUA_API size_t(lua_stringtonumber)(lua_State* L, const HostString& s);

struct lua_Allocator
{
  lua_Alloc f;
  void* ud;
};
LUA_API lua_Allocator(lua_getallocf)(lua_State* L);
LUA_API void(lua_setallocf)(lua_State* L, lua_Alloc f, void* ud);

LUA_API void(lua_toclose)(lua_State* L, int idx);
LUA_API void(lua_closeslot)(lua_State* L, int idx);

/*
** {==============================================================
** some useful macros
** ===============================================================
*/

LUA_API void* lua_getextraspace(lua_State* L);

LUA_API lua_Number lua_tonumber(lua_State* L, int idx);
LUA_API lua_Integer lua_tointeger(lua_State* L, int idx);

LUA_API void lua_pop(lua_State* L, int n);

LUA_API void lua_newtable(lua_State* L);

LUA_API void lua_register(lua_State* L, const HostString& name, lua_CFunction f);

LUA_API void lua_pushcfunction(lua_State* L, lua_CFunction f);

LUA_API int lua_isfunction(lua_State* L, int idx);
LUA_API int lua_istable(lua_State* L, int idx);
LUA_API int lua_islightuserdata(lua_State* L, int idx);
LUA_API int lua_isnil(lua_State* L, int idx);
LUA_API int lua_isboolean(lua_State* L, int idx);
LUA_API int lua_isthread(lua_State* L, int idx);
LUA_API int lua_isnone(lua_State* L, int idx);
LUA_API int lua_isnoneornil(lua_State* L, int idx);

LUA_API void lua_pushliteral(lua_State* L, const HostString& s);

LUA_API void lua_pushglobaltable(lua_State* L);

LUA_API std::optional<HostString> lua_tostring(lua_State* L, int idx);

LUA_API void lua_insert(lua_State* L, int idx);

LUA_API void lua_remove(lua_State* L, int idx);

LUA_API void lua_replace(lua_State* L, int idx);

/* }============================================================== */

/*
** {======================================================================
** Debug API
** =======================================================================
*/

/*
** Event codes
*/
constexpr int LUA_HOOKCALL = 0;
constexpr int LUA_HOOKRET = 1;
constexpr int LUA_HOOKLINE = 2;
constexpr int LUA_HOOKCOUNT = 3;
constexpr int LUA_HOOKTAILCALL = 4;

/*
** Event masks
*/
constexpr int LUA_MASKCALL = (1 << LUA_HOOKCALL);
constexpr int LUA_MASKRET = (1 << LUA_HOOKRET);
constexpr int LUA_MASKLINE = (1 << LUA_HOOKLINE);
constexpr int LUA_MASKCOUNT = (1 << LUA_HOOKCOUNT);

struct CallInfo;
struct lua_Debug
{
  int event;
  HostString name;
  HostString namewhat;
  HostString what;            /* (S) 'Lua', 'C', 'main', 'tail' */
  byte_view source;    /* (S) */
  int currentline;            /* (l) */
  int linedefined;            /* (S) */
  int lastlinedefined;        /* (S) */
  unsigned char nups;         /* (u) number of upvalues */
  unsigned char nparams;      /* (u) number of parameters */
  char isvararg;              /* (u) */
  unsigned char extraargs;    /* (t) number of extra arguments */
  char istailcall;            /* (t) */
  int ftransfer;              /* (r) index of first value transferred */
  int ntransfer;              /* (r) number of transferred values */
  HostString short_src;       /* (S) */
  /* private part */
  CallInfo* i_ci; /* active function */
};

LUA_API std::optional<lua_Debug>(lua_getstack)(lua_State* L, int level);
LUA_API std::optional<lua_Debug>(lua_getinfo)(lua_State* L, const HostString& what,
                                              const lua_Debug& ar);
LUA_API std::optional<HostString>(lua_getlocal) (lua_State* L, const lua_Debug* ar, int n);
LUA_API std::optional<HostString>(lua_setlocal) (lua_State* L, const lua_Debug& ar, int n);
LUA_API std::optional<HostString>(lua_getupvalue) (lua_State* L, int funcindex, int n);
LUA_API std::optional<HostString>(lua_setupvalue) (lua_State* L, int funcindex, int n);

LUA_API void*(lua_upvalueid) (lua_State* L, int fidx, int n);
LUA_API void(lua_upvaluejoin)(lua_State* L, int fidx1, int n1,
                              int fidx2, int n2);

LUA_API void(lua_sethook)(lua_State* L, lua_Hook func, int mask, int count);
LUA_API lua_Hook(lua_gethook)(lua_State* L);
LUA_API int(lua_gethookmask)(lua_State* L);
LUA_API int(lua_gethookcount)(lua_State* L);

/* }====================================================================== */

/******************************************************************************
 * Copyright (C) 1994-2026 Lua.org, PUC-Rio.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
