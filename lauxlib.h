/*
** $Id: lauxlib.h $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/

#pragma once

#include <stddef.h>
#include <cstddef>
#include <stdio.h>
#include <cassert>
#include <string.h>

#include "lua.h"

namespace lua::platform
{
struct OsFile;
}
#include "luaconf.h"

/* global table */
inline constexpr byte_view LUA_GNAME = "_G";

/* extra error code for 'luaL_loadfilex' */
constexpr int LUA_ERRFILE = LUA_ERRERR + 1;

/* key, in the registry, for table of loaded modules */
inline constexpr byte_view LUA_LOADED_TABLE = "_LOADED";

/* key, in the registry, for table of preloaded loaders */
inline constexpr byte_view LUA_PRELOAD_TABLE = "_PRELOAD";

struct luaL_Reg
{
  HostString name;
  lua_CFunction func;
};

constexpr size_t LUAL_NUMSIZES = 8 * 16 + 8;
static_assert(LUAL_NUMSIZES == sizeof(lua_Integer) * 16 + sizeof(lua_Number));

void(luaL_checkversion_)(lua_State* L, lua_Number ver, size_t sz);
void luaL_checkversion(lua_State* L);

int(luaL_getmetafield)(lua_State* L, int obj, const HostString& e);
int(luaL_callmeta)(lua_State* L, int obj, const HostString& e);
byte_view(luaL_tolstring) (lua_State* L, int idx);
/*
** Freeze-ready forms of the helpers that can run user code. Like the
** 'k' calls of lua.h, they return as the plain helper does, unless what
** they call freezes (or yields): then 'k' is called when it thaws, with
** the stack as the helper leaves it, and finishes the job as the notes
** say.
**  - luaL_callmetak: as luaL_callmeta; 'k' runs only if the metamethod
**    was called, with its result on the top.
**  - luaL_tolstringk: pushes the string; 'k' gets the value of
**    __tostring on the top and calls luaL_tolstringresult for the string
**    (it checks that it is one).
**  - luaL_lenk: 'k' gets the length on the top and calls luaL_lenresult
**    for the integer (it checks and pops it).
**  - luaL_getsubtablek: as luaL_getsubtable, but it leaves the table and
**    then a boolean (true if the table was already there) and returns
**    that boolean; 'k' finds the same two values.
**  - luaL_requirefk: as luaL_requiref; 'k' finds the module on the top.
**  - luaL_dofilek, luaL_dostringk: as luaL_dofile and luaL_dostring;
**    their pcall has the continuation, and 'k' gets the status as for
**    lua_pcallk.
*/
int(luaL_callmetak)(lua_State* L, int obj, const HostString& e, lua_KContext ctx, lua_KFunction k);
byte_view(luaL_tolstringk)(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k);
byte_view(luaL_tolstringresult)(lua_State* L);
lua_Integer(luaL_lenk)(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k);
lua_Integer(luaL_lenresult)(lua_State* L);
int(luaL_getsubtablek)(lua_State* L, int idx, const HostString& fname, lua_KContext ctx, lua_KFunction k);
void(luaL_requirefk)(lua_State* L, const HostString& modname, lua_CFunction openf, int glb, lua_KContext ctx, lua_KFunction k);
int(luaL_dofilek)(lua_State* L, const std::optional<HostString>& filename, lua_KContext ctx, lua_KFunction k);
int(luaL_dostringk)(lua_State* L, const HostString& str, lua_KContext ctx, lua_KFunction k);
int(luaL_argerror)(lua_State* L, int arg, const HostString& extramsg);
int(luaL_typeerror)(lua_State* L, int arg, const HostString& tname);
byte_view(luaL_checklstring) (lua_State* L, int arg);
byte_view(luaL_optlstring) (lua_State* L, int arg, byte_view def);
lua_Number(luaL_checknumber)(lua_State* L, int arg);
lua_Number(luaL_optnumber)(lua_State* L, int arg, lua_Number def);
lua_Integer(luaL_checkinteger)(lua_State* L, int arg);
lua_Integer(luaL_optinteger)(lua_State* L, int arg, lua_Integer def);
void(luaL_checkstack)(lua_State* L, int sz, const std::optional<HostString>& msg);
void(luaL_checktype)(lua_State* L, int arg, int t);
void(luaL_checkany)(lua_State* L, int arg);
int(luaL_newmetatable)(lua_State* L, const HostString& tname);
void(luaL_setmetatable)(lua_State* L, const HostString& tname);
void*(luaL_testudata) (lua_State* L, int ud, const HostString& tname);
void*(luaL_checkudata) (lua_State* L, int ud, const HostString& tname);
lua_UserObject*(luaL_testuserobject) (lua_State* L, int ud, const HostString& tname);
lua_UserObject*(luaL_checkuserobject) (lua_State* L, int ud, const HostString& tname);
void(luaL_where)(lua_State* L, int lvl);
int(luaL_error)(lua_State* L, const HostString& fmt);
int(luaL_checkoption)(lua_State* L, int arg, const std::optional<HostString>& def, const std::span<const HostString>& lst);
int(luaL_fileresult)(lua_State* L, int stat, const std::optional<HostString>& fname);
int(luaL_execresult)(lua_State* L, int stat);

/* predefined references */
constexpr int LUA_NOREF = -2;
constexpr int LUA_REFNIL = -1;

int(luaL_ref)(lua_State* L, int t);
void(luaL_unref)(lua_State* L, int t, int ref);
int(luaL_loadfilex)(lua_State* L, const std::optional<HostString>& filename, const std::optional<HostString>& mode);
int luaL_loadfile(lua_State* L, const std::optional<HostString>& filename);
int(luaL_loadbufferx)(lua_State* L, byte_view buff, const std::optional<HostString>& name, const std::optional<HostString>& mode);
int(luaL_loadstring)(lua_State* L, const HostString& s);
lua_State*(luaL_newstate) (void);
unsigned(luaL_makeseed)(lua_State* L);
lua_Integer(luaL_len)(lua_State* L, int idx);
HostString(luaL_gsub) (lua_State* L, const HostString& s, const HostString& p, const HostString& r);
void(luaL_setfuncs)(lua_State* L, const std::span<const luaL_Reg>& funcs, int nup);
void luaL_newlibtable(lua_State* L, const std::span<const luaL_Reg>& funcs);
void luaL_newlib(lua_State* L, const std::span<const luaL_Reg>& funcs);

template <typename F, typename D>
auto luaL_opt(lua_State* L, F f, int n, D d)
{
  return lua_isnoneornil(L, n) ? static_cast<decltype(f(L, n))>(d) : f(L, n);
}

int(luaL_getsubtable)(lua_State* L, int idx, const HostString& fname);
void(luaL_traceback)(lua_State* L, lua_State* L1, const std::optional<HostString>& msg, int level);
void(luaL_requiref)(lua_State* L, const HostString& modname, lua_CFunction openf, int glb);

/*
** ===============================================================
** some useful macros
** ===============================================================
*/

void luaL_argcheck(lua_State* L, bool cond, int arg, const HostString& extramsg);
void luaL_argexpected(lua_State* L, bool cond, int arg, const HostString& tname);
HostString luaL_checkstring(lua_State* L, int arg);
std::optional<HostString> luaL_optstring(lua_State* L, int arg, const std::optional<HostString>& d);
HostString luaL_typename(lua_State* L, int idx);
int luaL_dofile(lua_State* L, const std::optional<HostString>& filename);
int luaL_dostring(lua_State* L, const HostString& str);
int luaL_getmetatable(lua_State* L, const HostString& tname);
int luaL_loadbuffer(lua_State* L, byte_view buff, const std::optional<HostString>& name);

/*
** Perform arithmetic operations on lua_Integer values with wrap-around
** semantics, as the Lua core does.
*/


/* push the value used to represent failure/error */
void luaL_pushfail(lua_State* L);

/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

struct luaL_Buffer
{
  byte_owner bytes = {};
  size_t used = 0;
  lua_State* L = nullptr;

  ~luaL_Buffer();
  luaL_Buffer() = default;
  luaL_Buffer(const luaL_Buffer&) = delete;
  luaL_Buffer& operator=(const luaL_Buffer&) = delete;
};

size_t luaL_bufflen(const luaL_Buffer* bf);
byte_span luaL_buffaddr(luaL_Buffer* bf);
void luaL_addsize(luaL_Buffer* B, size_t s);
void luaL_buffsub(luaL_Buffer* B, size_t s);
void(luaL_buffinit)(lua_State* L, luaL_Buffer* B);
size_t(luaL_prepbuffsize) (luaL_Buffer* B, size_t sz);
void(luaL_addlstring)(luaL_Buffer* B, byte_view s, size_t off, size_t l);
void(luaL_addstring)(luaL_Buffer* B, const HostString& s);
void(luaL_addvalue)(luaL_Buffer* B);
void(luaL_pushresult)(luaL_Buffer* B);
void(luaL_pushresultsize)(luaL_Buffer* B, size_t sz);
size_t(luaL_buffinitsize) (lua_State* L, luaL_Buffer* B, size_t sz);
size_t luaL_prepbuffer(luaL_Buffer* B);
void(luaL_addgsub)(luaL_Buffer* b, const HostString& s, const HostString& p, const HostString& r);

inline void luaL_addchar(luaL_Buffer* B, char c)
{
  if (B->used >= B->bytes.size()) [[unlikely]]
    (void)luaL_prepbuffsize(B, 1);
  B->bytes[B->used++] = c;
}

/* }====================================================== */

/*
** {======================================================
** File handles for IO library
** =======================================================
*/

/*
** A file handle is a userdata with metatable 'LUA_FILEHANDLE' and
** initial structure 'luaL_Stream' (it may contain other fields
** after that initial structure).
*/

inline constexpr byte_view LUA_FILEHANDLE = "FILE*";

struct luaL_Stream : lua_UserObject
{
  OsFile* f;
  lua_CFunction closef;
  void freed(lua_State* L) override; /* a handle freed without its '__gc' does not leak its file */
};

/* }====================================================== */

/*
** {============================================================
** Compatibility with deprecated conversions
** =============================================================
*/
/* }============================================================ */
