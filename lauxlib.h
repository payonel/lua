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

#include "lua.h"

struct OsFile;
#include "luaconf.h"

#include "internal_string.h"

/* global table */
inline const InternalString* const LUA_GNAME = lit<"_G">();

struct luaL_Buffer;

/* extra error code for 'luaL_loadfilex' */
constexpr int LUA_ERRFILE = (LUA_ERRERR + 1);

/* key, in the registry, for table of loaded modules */
inline const InternalString* const LUA_LOADED_TABLE = lit<"_LOADED">();

/* key, in the registry, for table of preloaded loaders */
inline const InternalString* const LUA_PRELOAD_TABLE = lit<"_PRELOAD">();

struct luaL_Reg
{
  HostString name;
  lua_CFunction func;
};

constexpr size_t LUAL_NUMSIZES = (sizeof(lua_Integer) * 16 + sizeof(lua_Number));

LUALIB_API void(luaL_checkversion_)(lua_State* L, lua_Number ver, size_t sz);
LUALIB_API void luaL_checkversion(lua_State* L);

LUALIB_API int(luaL_getmetafield)(lua_State* L, int obj, const HostString& e);
LUALIB_API int(luaL_callmeta)(lua_State* L, int obj, const HostString& e);
LUALIB_API std::string_view(luaL_tolstring) (lua_State* L, int idx);
LUALIB_API int(luaL_argerror)(lua_State* L, int arg, const HostString& extramsg);
LUALIB_API int(luaL_typeerror)(lua_State* L, int arg, const HostString& tname);
LUALIB_API std::string_view(luaL_checklstring) (lua_State* L, int arg);
LUALIB_API std::string_view(luaL_optlstring) (
    lua_State* L, int arg, std::string_view def);
LUALIB_API lua_Number(luaL_checknumber)(lua_State* L, int arg);
LUALIB_API lua_Number(luaL_optnumber)(lua_State* L, int arg, lua_Number def);

LUALIB_API lua_Integer(luaL_checkinteger)(lua_State* L, int arg);
LUALIB_API lua_Integer(luaL_optinteger)(lua_State* L, int arg,
                                        lua_Integer def);

LUALIB_API void(luaL_checkstack)(lua_State* L, int sz,
                                 const std::optional<HostString>& msg);
LUALIB_API void(luaL_checktype)(lua_State* L, int arg, int t);
LUALIB_API void(luaL_checkany)(lua_State* L, int arg);

LUALIB_API int(luaL_newmetatable)(lua_State* L, const HostString& tname);
LUALIB_API void(luaL_setmetatable)(lua_State* L, const HostString& tname);
LUALIB_API void*(luaL_testudata) (lua_State* L, int ud, const HostString& tname);
LUALIB_API void*(luaL_checkudata) (lua_State* L, int ud, const HostString& tname);

LUALIB_API void(luaL_where)(lua_State* L, int lvl);
LUALIB_API int(luaL_error)(lua_State* L, const HostString& fmt);

LUALIB_API int(luaL_checkoption)(lua_State* L, int arg,
                                 const std::optional<HostString>& def,
                                 const std::span<const HostString>& lst);

LUALIB_API int(luaL_fileresult)(lua_State* L, int stat,
                                const std::optional<HostString>& fname);
LUALIB_API int(luaL_execresult)(lua_State* L, int stat);

/* predefined references */
constexpr int LUA_NOREF = (-2);
constexpr int LUA_REFNIL = (-1);

LUALIB_API int(luaL_ref)(lua_State* L, int t);
LUALIB_API void(luaL_unref)(lua_State* L, int t, int ref);

LUALIB_API int(luaL_loadfilex)(lua_State* L,
                               const std::optional<HostString>& filename,
                               const std::optional<HostString>& mode);

LUALIB_API int luaL_loadfile(lua_State* L,
                             const std::optional<HostString>& filename);

LUALIB_API int(luaL_loadbufferx)(lua_State* L, std::string_view buff,
                                 const std::optional<HostString>& name,
                                 const std::optional<HostString>& mode);
LUALIB_API int(luaL_loadstring)(lua_State* L, const HostString& s);

LUALIB_API lua_State*(luaL_newstate) (void);

LUALIB_API unsigned(luaL_makeseed)(lua_State* L);

LUALIB_API lua_Integer(luaL_len)(lua_State* L, int idx);

LUALIB_API void(luaL_addgsub)(luaL_Buffer* b, const HostString& s,
                              const HostString& p, const HostString& r);
LUALIB_API HostString(luaL_gsub) (lua_State* L, const HostString& s,
                                  const HostString& p, const HostString& r);

LUALIB_API void(luaL_setfuncs)(lua_State* L, const std::span<const luaL_Reg>& funcs, int nup);

LUALIB_API void luaL_newlib(lua_State* L, const std::span<const luaL_Reg>& funcs);

template <typename F, typename D>
auto luaL_opt(lua_State* L, F f, int n, D d)
{
  return lua_isnoneornil(L, n) ? static_cast<decltype(f(L, n))>(d) : f(L, n);
}


LUALIB_API int(luaL_getsubtable)(lua_State* L, int idx, const HostString& fname);

LUALIB_API void(luaL_traceback)(lua_State* L, lua_State* L1,
                                const std::optional<HostString>& msg, int level);

LUALIB_API void(luaL_requiref)(lua_State* L, const HostString& modname,
                               lua_CFunction openf, int glb);

/*
** ===============================================================
** some useful macros
** ===============================================================
*/





LUALIB_API void luaL_argcheck(lua_State* L, bool cond, int arg, const HostString& extramsg);

LUALIB_API void luaL_argexpected(lua_State* L, bool cond, int arg, const HostString& tname);

LUALIB_API HostString luaL_checkstring(lua_State* L, int arg);
LUALIB_API std::optional<HostString> luaL_optstring(lua_State* L, int arg,
                                                    const std::optional<HostString>& d);

LUALIB_API HostString luaL_typename(lua_State* L, int idx);

LUALIB_API int luaL_dofile(lua_State* L, const std::optional<HostString>& filename);

LUALIB_API int luaL_dostring(lua_State* L, const HostString& str);

LUALIB_API int luaL_getmetatable(lua_State* L, const HostString& tname);



LUALIB_API int luaL_loadbuffer(lua_State* L, std::string_view buff,
                               const std::optional<HostString>& name);

/*
** Perform arithmetic operations on lua_Integer values with wrap-around
** semantics, as the Lua core does.
*/


/* push the value used to represent failure/error */
LUALIB_API void luaL_pushfail(lua_State* L);

/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

struct luaL_Buffer
{
  ByteBuilder bldr;
  size_t reserved = 0;
  lua_State* L = nullptr;

  ~luaL_Buffer();
  luaL_Buffer() = default;
  luaL_Buffer(const luaL_Buffer&) = delete;
  luaL_Buffer& operator=(const luaL_Buffer&) = delete;
};

LUALIB_API size_t luaL_bufflen(const luaL_Buffer* bf);
LUALIB_API std::span<char> luaL_buffarea(luaL_Buffer* bf);

LUALIB_API void luaL_addchar(luaL_Buffer* B, char c);

LUALIB_API void luaL_addsize(luaL_Buffer* B, size_t s);

LUALIB_API void luaL_buffsub(luaL_Buffer* B, size_t s);

LUALIB_API void(luaL_buffinit)(lua_State* L, luaL_Buffer* B);
LUALIB_API size_t(luaL_prepbuffsize) (luaL_Buffer* B, size_t sz);
LUALIB_API void(luaL_addlstring)(luaL_Buffer* B, std::string_view s,
                                 size_t off, size_t l);
LUALIB_API void(luaL_addstring)(luaL_Buffer* B, const HostString& s);
LUALIB_API void(luaL_addvalue)(luaL_Buffer* B);
LUALIB_API void(luaL_pushresult)(luaL_Buffer* B);
LUALIB_API void(luaL_pushresultsize)(luaL_Buffer* B, size_t sz);
LUALIB_API size_t(luaL_buffinitsize) (lua_State* L, luaL_Buffer* B, size_t sz);

LUALIB_API size_t luaL_prepbuffer(luaL_Buffer* B);

struct luaL_BuffSpan
{
  std::span<char> area;
  size_t off;
};

inline luaL_BuffSpan luaL_prepbuffspan(luaL_Buffer* B, size_t sz)
{
  size_t off = luaL_prepbuffsize(B, sz);
  return {luaL_buffarea(B), off};
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

inline const InternalString* const LUA_FILEHANDLE = lit<"FILE*">();

struct luaL_Stream
{
  OsFile* f;            /* stream (nullptr for incompletely created streams) */
  lua_CFunction closef; /* to close stream (nullptr for closed streams) */
};

/* }====================================================== */

/*
** {============================================================
** Compatibility with deprecated conversions
** =============================================================
*/
/* }============================================================ */
