/*
** $Id: lauxlib.h $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/

#pragma once

#include <stddef.h>
#include <cstddef>
#include <stdio.h>

#include "lua.h"
#include "luaconf.h"

#include "internal_string.h"
#include "lhack.h"

/* global table */
constexpr char LUA_GNAME[] = "_G";

struct luaL_Buffer;

/* extra error code for 'luaL_loadfilex' */
constexpr int LUA_ERRFILE = (LUA_ERRERR + 1);

/* key, in the registry, for table of loaded modules */
constexpr char LUA_LOADED_TABLE[] = "_LOADED";

/* key, in the registry, for table of preloaded loaders */
constexpr char LUA_PRELOAD_TABLE[] = "_PRELOAD";

struct luaL_Reg
{
  std::string_view name; /* registration key (utf8 literal bytes,
                            interned into the state at luaL_setfuncs;
                            NOT host text); {} = sentinel */
  lua_CFunction func;
};

constexpr size_t LUAL_NUMSIZES = (sizeof(lua_Integer) * 16 + sizeof(lua_Number));

LUALIB_API void(luaL_checkversion_)(lua_State* L, lua_Number ver, size_t sz);
/* NB: as a compiled wrapper this bakes the library's own constants —
   the cross-ABI check is moot for this port */
LUALIB_API void luaL_checkversion(lua_State* L);

LUALIB_API int(luaL_getmetafield)(lua_State* L, int obj, const HostString& e);
LUALIB_API int(luaL_callmeta)(lua_State* L, int obj, const HostString& e);
/* the __tostring'd value as consumer TEXT (RULED 2026-07-31):
   converts (honoring '__tostring'), PUSHES the result, and returns
   it as a HostString COPY. Engine libraries use the internal twin
   luaL_toistring (lua_internal.h) instead. */
LUALIB_API HostString(luaL_tolstring) (lua_State* L, int idx);
LUALIB_API int(luaL_argerror)(lua_State* L, int arg, const HostString& extramsg);
LUALIB_API int(luaL_typeerror)(lua_State* L, int arg, const HostString& tname);
LUALIB_API std::string_view(luaL_checklstring) (lua_State* L, int arg);
/* a null-view def is allowed (string_view stores null natively; no
   optional wrapper, RULED 2026-07-31); the return is def when the
   argument is absent — so it has null data() exactly when def did */
LUALIB_API std::string_view(luaL_optlstring) (
    lua_State* L, int arg, std::string_view def);
LUALIB_API lua_Number(luaL_checknumber)(lua_State* L, int arg);
LUALIB_API lua_Number(luaL_optnumber)(lua_State* L, int arg, lua_Number def);

LUALIB_API lua_Integer(luaL_checkinteger)(lua_State* L, int arg);
LUALIB_API lua_Integer(luaL_optinteger)(lua_State* L, int arg,
                                        lua_Integer def);

/* nullopt msg = no additional text in the overflow error */
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

/* option names are byte views; null-view def = argument required
   (note "" can be a genuine option/default — null != empty) */
LUALIB_API int(luaL_checkoption)(lua_State* L, int arg,
                                 std::string_view def,
                                 std::span<const std::string_view> lst);

/* nullopt fname = no file name to report (Java: String or null) */
LUALIB_API int(luaL_fileresult)(lua_State* L, int stat,
                                const std::optional<HostString>& fname);
LUALIB_API int(luaL_execresult)(lua_State* L, int stat);

/* predefined references */
constexpr int LUA_NOREF = (-2);
constexpr int LUA_REFNIL = (-1);

LUALIB_API int(luaL_ref)(lua_State* L, int t);
LUALIB_API void(luaL_unref)(lua_State* L, int t, int ref);

/* filename is host text at the fopen boundary (nullopt = stdin);
   nullopt mode = default "bt" */
LUALIB_API int(luaL_loadfilex)(lua_State* L,
                               const std::optional<HostString>& filename,
                               const std::optional<HostString>& mode);

LUALIB_API int luaL_loadfile(lua_State* L,
                             const std::optional<HostString>& filename);

/* nullopt name = "?" (as in lua_load); nullopt mode = default "bt" */
LUALIB_API int(luaL_loadbufferx)(lua_State* L, std::string_view buff,
                                 const std::optional<HostString>& name,
                                 const std::optional<HostString>& mode);
LUALIB_API int(luaL_loadstring)(lua_State* L, const HostString& s);

LUALIB_API lua_State*(luaL_newstate) (void);

LUALIB_API unsigned(luaL_makeseed)(lua_State* L);

LUALIB_API lua_Integer(luaL_len)(lua_State* L, int idx);

LUALIB_API void(luaL_addgsub)(luaL_Buffer* b, const HostString& s,
                              const HostString& p, const HostString& r);
/* pushes the result and also returns it as a HostString COPY */
LUALIB_API HostString(luaL_gsub) (lua_State* L, const HostString& s,
                                  const HostString& p, const HostString& r);

LUALIB_API void(luaL_setfuncs)(lua_State* L, const luaL_Reg* l, int nup);

LUALIB_API int(luaL_getsubtable)(lua_State* L, int idx, const HostString& fname);

/* nullopt msg = no message prefix */
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

/* text accessors: HostString COPIES of the argument (see lua_tostring).
   luaL_optstring: nullopt d is allowed; the return is d when the
   argument is absent (so it is nullopt exactly when d was) */
LUALIB_API HostString luaL_checkstring(lua_State* L, int arg);
LUALIB_API std::optional<HostString> luaL_optstring(lua_State* L, int arg,
                                                    const std::optional<HostString>& d);

LUALIB_API HostString luaL_typename(lua_State* L, int idx);

/* nullopt filename = stdin */
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

/*
** A string builder over the engine's ByteBuilder, ACCOUNTED: capacity
** growth reserves bytes with the lua_State first (reserve-before-
** growth), so script-driven accumulation (io.read, string.rep,
** table.concat) is subject to the state's memory limit. The
** destructor releases the reservation on error unwind (errors are
** C++ exceptions here). 'pushresult' hands the bytes to the result
** string by adoption — no finish copy.
*/
struct luaL_Buffer
{
  ByteBuilder bldr;    /* accumulated bytes */
  size_t reserved = 0; /* capacity charged to L's byte budget */
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

constexpr char LUA_FILEHANDLE[] = "FILE*";

struct luaL_Stream
{
  FILE* f;              /* stream (nullptr for incompletely created streams) */
  lua_CFunction closef; /* to close stream (nullptr for closed streams) */
};

/* }====================================================== */

/*
** {============================================================
** Compatibility with deprecated conversions
** =============================================================
*/
/* }============================================================ */
