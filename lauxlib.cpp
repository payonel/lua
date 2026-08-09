/*
** $Id: lauxlib.c $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/



#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
** This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#include "lua.h"

#include "lua_internal.h"
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "util_cast.h"
#include "lstate.h"

/*
** {======================================================
** Traceback
** =======================================================
*/

constexpr int LEVELS1 = 10; /* size of the first part of the stack */
constexpr int LEVELS2 = 11; /* size of the second part of the stack */

/*
** Search for 'objidx' in table at index -1. ('objidx' must be an
** absolute index.) Return 1 + string at top if it found a good name.
*/
static int findfield(lua_State* L, int objidx, int level)
{
  if (level == 0 || !lua_istable(L, -1))
    return 0;     /* not found */
  lua_pushnil(L); /* start 'next' loop */
  while (lua_next(L, -2))
  { /* for each pair in table */
    if (lua_type(L, -2) == LUA_TSTRING)
    { /* ignore non-string keys */
      if (lua_rawequal(L, objidx, -1))
      {                /* found object? */
        lua_pop(L, 1); /* remove value (but keep name) */
        return 1;
      }
      else if (findfield(L, objidx, level - 1))
      { /* try recursively */
        /* stack: lib_name, lib_table, field_name (top) */
        lua_pushistringref(L, lit<".">()->view()); /* place '.' between the two names */
        lua_replace(L, -3);      /* (in the slot occupied by table) */
        lua_concat(L, 3);        /* lib_name.field_name */
        return 1;
      }
    }
    lua_pop(L, 1); /* remove value */
  }
  return 0; /* not found */
}

/*
** Search for a name for a function in all loaded modules
*/
static int pushglobalfuncname(lua_State* L, InternalDebug* ar)
{
  int top = lua_gettop(L);
  lua_getinfoi(L, lit<"f">()->view(), *ar);
  lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  luaL_checkstacki(L, 6, lit<"not enough stack">()->view()); /* slots for 'findfield' */
  if (findfield(L, top + 1, 2))
  {
    std::string_view name = *lua_tolstring(L, -1);
    if (name.starts_with(lit<"_G.">()->view()))
    {                                                    /* name start with '_G.'? */
      lua_pushlstring(L, name, 3, name.size() - 3);      /* push name without prefix */
      lua_remove(L, -2);                                 /* remove original name */
    }
    lua_copy(L, -1, top + 1); /* copy name to proper place */
    lua_settop(L, top + 1);   /* remove table "loaded" and name copy */
    return 1;
  }
  else
  {
    lua_settop(L, top); /* remove function and global table */
    return 0;
  }
}

static void pushfuncname(lua_State* L, InternalDebug* ar)
{
  if (ar->namewhat.data() != nullptr)
  {
    lua_pushlstring(L, ar->namewhat, 0, ar->namewhat.size());
    lua_pushlstring(L, ar->name, 0, ar->name.size());
    lua_pushfstringi(L, lit<"%s '%s'">()->view()); /* use it */
  }
  else if (ar->what[0] == 'm')                             /* main? */
    lua_pushistringref(L, lit<"main chunk">()->view());
  else if (pushglobalfuncname(L, ar))
  {                       /* try a global name */
    lua_pushvalue(L, -1);
    lua_pushfstringi(L, lit<"function '%s'">()->view());
    lua_remove(L, -2); /* remove name */
  }
  else if (ar->what[0] != 'C')
  { /* for Lua functions, use <file:line> */
    std::string_view ss{ar->short_src};
    lua_pushlstring(L, ss, 0, ss.size());
    lua_pushinteger(L, ar->linedefined);
    lua_pushfstringi(L, lit<"function <%s:%d>">()->view());
  }
  else /* nothing left... */
    lua_pushistringref(L, lit<"?">()->view());
}

static int lastlevel(lua_State* L)
{
  int li = 1, le = 1;
  /* find an upper bound */
  while (lua_getstacki(L, le))
  {
    li = le;
    le *= 2;
  }
  /* do a binary search */
  while (li < le)
  {
    int m = (li + le) / 2;
    if (lua_getstacki(L, m))
      li = m + 1;
    else
      le = m;
  }
  return le - 1;
}

LUALIB_API void luaL_traceback(lua_State* L, lua_State* L1,
                               const std::optional<HostString>& msg, int level)
{
  luaL_tracebacki(L, L1, msg ? msg->to_utf8() : std::string_view{}, level);
}

void luaL_tracebacki(lua_State* L, lua_State* L1, std::string_view msg, int level)
{
  luaL_Buffer b;
  std::optional<InternalDebug> oar;
  int last = lastlevel(L1);
  int limit2show = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  luaL_buffinit(L, &b);
  if (msg.data() != nullptr)
  {
    luaL_addlstring(&b, msg, 0, msg.size());
    luaL_addchar(&b, '\n');
  }
  luaL_addlstring(&b, lit<"stack traceback:">()->view(), 0, lit<"stack traceback:">()->size());
  while ((oar = lua_getstacki(L1, level++)))
  {
    if (limit2show-- == 0)
    {                                     /* too many levels? */
      int n = last - level - LEVELS2 + 1; /* number of levels to skip */
      lua_pushinteger(L, n);
      lua_pushfstringi(L, lit<"\n\t...\t(skipping %d levels)">()->view());
      luaL_addvalue(&b); /* add warning about skip */
      level += n;        /* and skip to last levels */
    }
    else
    {
      InternalDebug ar = *lua_getinfoi(L1, lit<"Slnt">()->view(), *oar);
      std::string_view ss{ar.short_src};
      if (ar.currentline <= 0)
      {
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushfstringi(L, lit<"\n\t%s: in ">()->view());
      }
      else
      {
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushinteger(L, ar.currentline);
        lua_pushfstringi(L, lit<"\n\t%s:%d: in ">()->view());
      }
      luaL_addvalue(&b);
      pushfuncname(L, &ar);
      luaL_addvalue(&b);
      if (ar.istailcall)
        luaL_addlstring(&b, lit<"\n\t(...tail calls...)">()->view(), 0, lit<"\n\t(...tail calls...)">()->size());
    }
  }
  luaL_pushresult(&b);
}

/* }====================================================== */

/*
** {======================================================
** Error-report functions
** =======================================================
*/

LUALIB_API int luaL_argerror(lua_State* L, int arg, const HostString& extramsg)
{
  return luaL_argerrori(L, arg, extramsg.to_utf8());
}

int luaL_argerrori(lua_State* L, int arg, std::string_view extramsg)
{
  const InternalString* argword;
  std::optional<InternalDebug> oar = lua_getstacki(L, 0);
  if (!oar)
  { /* no stack frame? */
    lua_pushinteger(L, arg);
    lua_pushlstring(L, extramsg, 0, extramsg.size());
    return luaL_errori(L, lit<"bad argument #%d (%s)">()->view());
  }
  InternalDebug ar = *lua_getinfoi(L, lit<"nt">()->view(), *oar);
  if (arg <= ar.extraargs) /* error in an extra argument? */
    argword = lit<"extra argument">();
  else
  {
    arg -= ar.extraargs; /* do not count extra arguments */
    if (ar.namewhat == lit<"method">()->view())
    {               /* colon syntax? */
      arg--;        /* do not count (extra) self argument */
      if (arg == 0)
      { /* error in self argument? */
        lua_pushlstring(L, ar.name, 0, ar.name.size());
        lua_pushlstring(L, extramsg, 0, extramsg.size());
        return luaL_errori(L, lit<"calling '%s' on bad self (%s)">()->view());
      }
      /* else go through; error in a regular argument */
    }
    argword = lit<"argument">();
  }
  if (ar.name.data() == nullptr)
    ar.name = (pushglobalfuncname(L, &ar)) ? *lua_tolstring(L, -1)
                                           : lit<"?">()->view();
  lua_pushistringref(L, argword->view());
  lua_pushinteger(L, arg);
  lua_pushlstring(L, ar.name, 0, ar.name.size());
  lua_pushlstring(L, extramsg, 0, extramsg.size());
  return luaL_errori(L, lit<"bad %s #%d to '%s' (%s)">()->view());
}

LUALIB_API int luaL_typeerror(lua_State* L, int arg, const HostString& tname)
{
  return luaL_typeerrori(L, arg, tname.to_utf8());
}

int luaL_typeerrori(lua_State* L, int arg, std::string_view tname)
{
  std::string_view typearg; /* name for the type of the actual argument */
  if (luaL_getmetafieldi(L, arg, lit<"__name">()->view()) == LUA_TSTRING)
    typearg = lua_toistring(L, -1)->view(); /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = lit<"light userdata">()->view(); /* special name for messages */
  else
    typearg = lua_typenamei(L, lua_type(L, arg))->view(); /* standard name */
  lua_pushlstring(L, tname, 0, tname.size());
  lua_pushlstring(L, typearg, 0, typearg.size());
  const InternalString* msg = lua_pushfstringi(L, lit<"%s expected, got %s">()->view());
  return luaL_argerrori(L, arg, msg->view());
}

static void tag_error(lua_State* L, int arg, int tag)
{
  luaL_typeerrori(L, arg, lua_typenamei(L, tag)->view());
}

/*
** The use of 'lua_pushfstring' ensures this function does not
** need reserved stack space when called.
*/
LUALIB_API void luaL_where(lua_State* L, int level)
{
  std::optional<InternalDebug> oar = lua_getstacki(L, level);
  if (oar)
  {                                                /* check function at level */
    InternalDebug ar = *lua_getinfoi(L, lit<"Sl">()->view(), *oar); /* get info about it */
    if (ar.currentline > 0)
    { /* is there info? */
      std::string_view ss{ar.short_src};
      lua_pushlstring(L, ss, 0, ss.size());
      lua_pushinteger(L, ar.currentline);
      lua_pushfstringi(L, lit<"%s:%d: ">()->view());
      return;
    }
  }
  lua_pushfstringi(L, lit<"">()->view()); /* else, no information available... */
}

LUALIB_API int luaL_error(lua_State* L, const HostString& fmt)
{
  return luaL_errori(L, fmt.to_utf8());
}

int luaL_errori(lua_State* L, std::string_view fmt)
{
  lua_pushfstringi(L, fmt);
  luaL_where(L, 1);
  lua_rotate(L, -2, 1);
  lua_concat(L, 2);
  return lua_error(L);
}

int luaL_fileresulti(lua_State* L, int stat, std::string_view fname, int en)
{
  if (stat)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    luaL_pushfail(L);
    InternalString errstr = (en != 0) ? os_strerror(en) : InternalString(lit<"(no extra info)">()->view());
    if (fname.data() != nullptr)
    {
      lua_pushlstring(L, fname, 0, fname.size());
      lua_pushlstring(L, errstr.view(), 0, errstr.size());
      lua_pushfstringi(L, lit<"%s: %s">()->view());
    }
    else
      lua_pushlstring(L, errstr.view(), 0, errstr.size());
    lua_pushinteger(L, en);
    return 3;
  }
}

LUALIB_API int luaL_fileresult(lua_State* L, int stat, const std::optional<HostString>& fname)
{
  return luaL_fileresulti(L, stat, fname ? fname->to_utf8() : std::string_view{}, errno);
}

LUALIB_API int luaL_execresult(lua_State* L, int stat)
{
  return luaL_execresulti(L, stat, errno);
}

int luaL_execresulti(lua_State* L, int stat, int en)
{
  if (stat != 0 && en != 0) /* error with an 'errno'? */
    return luaL_fileresulti(L, 0, {}, en);
  else
  {
    OsExecStatus es = os_decode_exec(stat);
    const InternalString* what = es.exited ? lit<"exit">() : lit<"signal">();
    if (es.exited && es.code == 0) /* successful termination? */
      lua_pushboolean(L, 1);
    else
      luaL_pushfail(L);
    lua_pushistringref(L, what->view());
    lua_pushinteger(L, es.code);
    return 3; /* return true/fail,what,code */
  }
}

/* }====================================================== */

/*
** {======================================================
** Userdata's metatable manipulation
** =======================================================
*/

LUALIB_API int luaL_newmetatable(lua_State* L, const HostString& tname)
{
  return luaL_newmetatablei(L, tname.to_utf8());
}

int luaL_newmetatablei(lua_State* L, std::string_view tname)
{
  if (luaL_getmetatablei(L, tname) != LUA_TNIL) /* name already in use? */
    return 0;                                  /* leave previous value on top, but return 0 */
  lua_pop(L, 1);
  lua_createtable(L, 0, 4);
  lua_pushlstring(L, tname, 0, tname.size());
  lua_setfieldi(L, -2, lit<"__name">()->view()); /* metatable.__name = tname */
  lua_pushvalue(L, -1);
  lua_setfieldi(L, LUA_REGISTRYINDEX, tname); /* registry.name = metatable */
  return 1;
}

LUALIB_API void luaL_setmetatable(lua_State* L, const HostString& tname)
{
  luaL_setmetatablei(L, tname.to_utf8());
}

void luaL_setmetatablei(lua_State* L, std::string_view tname)
{
  luaL_getmetatablei(L, tname);
  lua_setmetatable(L, -2);
}

LUALIB_API void* luaL_testudata(lua_State* L, int ud, const HostString& tname)
{
  return luaL_testudatai(L, ud, tname.to_utf8());
}

void* luaL_testudatai(lua_State* L, int ud, std::string_view tname)
{
  void* p = lua_touserdata(L, ud);
  if (p != nullptr)
  { /* value is a userdata? */
    if (lua_getmetatable(L, ud))
    {                               /* does it have a metatable? */
      luaL_getmetatablei(L, tname);  /* get correct metatable */
      if (!lua_rawequal(L, -1, -2)) /* not the same? */
        p = nullptr;                   /* value is a userdata with wrong metatable */
      lua_pop(L, 2);                /* remove both metatables */
      return p;
    }
  }
  return nullptr; /* value is not a userdata with a metatable */
}

LUALIB_API void* luaL_checkudata(lua_State* L, int ud, const HostString& tname)
{
  return luaL_checkudatai(L, ud, tname.to_utf8());
}

void* luaL_checkudatai(lua_State* L, int ud, std::string_view tname)
{
  void* p = luaL_testudatai(L, ud, tname);
  luaL_argexpectedi(L, p != nullptr, ud, tname);
  return p;
}

/* }====================================================== */

/*
** {======================================================
** Argument check functions
** =======================================================
*/

int luaL_checkoptioni(lua_State* L, int arg,
                      std::string_view def,
                      std::span<const std::string_view> lst)
{
  std::string_view name = (def.data() != nullptr)
                              ? luaL_optlstring(L, arg, def)
                              : luaL_checklstring(L, arg);
  for (size_t i = 0; i < lst.size(); i++)
    if (name == lst[i])
      return ((int)(i));
  lua_pushlstring(L, name, 0, name.size());
  const InternalString* msg = lua_pushfstringi(L, lit<"invalid option '%s'">()->view());
  return luaL_argerrori(L, arg, msg->view());
}

LUALIB_API int luaL_checkoption(lua_State* L, int arg,
                                const std::optional<HostString>& def,
                                const std::span<const HostString>& lst)
{
  std::string_view name = def
                              ? luaL_optlstring(L, arg, def->to_utf8())
                              : luaL_checklstring(L, arg);
  for (size_t i = 0; i < lst.size(); i++)
    if (name == lst[i].to_utf8())
      return ((int)(i));
  lua_pushlstring(L, name, 0, name.size());
  const InternalString* msg = lua_pushfstringi(L, lit<"invalid option '%s'">()->view());
  return luaL_argerrori(L, arg, msg->view());
}

/*
** Ensures the stack has at least 'space' extra slots, raising an error
** if it cannot fulfill the request. (The error handling needs a few
** extra slots to format the error message. In case of an error without
** this extra space, Lua will generate the same 'stack overflow' error,
** but without 'msg'.)
*/
LUALIB_API void luaL_checkstack(lua_State* L, int space, const std::optional<HostString>& msg)
{
  luaL_checkstacki(L, space, msg ? msg->to_utf8() : std::string_view{});
}

void luaL_checkstacki(lua_State* L, int space, std::string_view msg)
{
  if (l_unlikely(!lua_checkstack(L, space)))
  {
    if (msg.data() != nullptr && lua_checkstack(L, 1))
    {
      lua_pushlstring(L, msg, 0, msg.size());
      luaL_errori(L, lit<"stack overflow (%s)">()->view());
    }
    else
      luaL_errori(L, lit<"stack overflow">()->view());
  }
}

LUALIB_API void luaL_checktype(lua_State* L, int arg, int t)
{
  if (l_unlikely(lua_type(L, arg) != t))
    tag_error(L, arg, t);
}

LUALIB_API void luaL_checkany(lua_State* L, int arg)
{
  if (l_unlikely(lua_type(L, arg) == LUA_TNONE))
    luaL_argerrori(L, arg, lit<"value expected">()->view());
}

LUALIB_API std::string_view luaL_checklstring(lua_State* L, int arg)
{
  auto s = lua_tolstring(L, arg);
  if (l_unlikely(!s))
    tag_error(L, arg, LUA_TSTRING);
  return *s;
}

LUALIB_API std::string_view luaL_optlstring(
    lua_State* L, int arg, std::string_view def)
{
  if (lua_isnoneornil(L, arg))
    return def;
  else
    return luaL_checklstring(L, arg);
}

const InternalString* luaL_checkistring(lua_State* L, int arg)
{
  const InternalString* s = lua_toistring(L, arg);
  if (l_unlikely(!s))
    tag_error(L, arg, LUA_TSTRING);
  return s;
}

const InternalString* luaL_optistring(lua_State* L, int arg,
                                      const InternalString* def)
{
  if (lua_isnoneornil(L, arg))
    return def;
  else
    return luaL_checkistring(L, arg);
}

LUALIB_API lua_Number luaL_checknumber(lua_State* L, int arg)
{
  std::optional<lua_Number> d = lua_tonumberx(L, arg);
  if (l_unlikely(!d))
    tag_error(L, arg, LUA_TNUMBER);
  return *d;
}

LUALIB_API lua_Number luaL_optnumber(lua_State* L, int arg, lua_Number def)
{
  return (lua_isnoneornil(L, (arg)) ? (def) : luaL_checknumber(L, (arg)));
}

static void interror(lua_State* L, int arg)
{
  if (lua_isnumber(L, arg))
    luaL_argerrori(L, arg, lit<"number has no integer representation">()->view());
  else
    tag_error(L, arg, LUA_TNUMBER);
}

LUALIB_API lua_Integer luaL_checkinteger(lua_State* L, int arg)
{
  std::optional<lua_Integer> d = lua_tointegerx(L, arg);
  if (l_unlikely(!d))
  {
    interror(L, arg);
  }
  return *d;
}

LUALIB_API lua_Integer luaL_optinteger(lua_State* L, int arg,
                                       lua_Integer def)
{
  return (lua_isnoneornil(L, (arg)) ? (def) : luaL_checkinteger(L, (arg)));
}

/* }====================================================== */

/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

luaL_Buffer::~luaL_Buffer()
{
  if (reserved != 0 && L != nullptr)
    lua_releasebytes(L, reserved);
}

static size_t newbuffsize(luaL_Buffer* B, size_t sz)
{
  size_t newsize = B->bldr.capacity();
  size_t len = B->bldr.size();
  if (l_unlikely(sz >= MAX_SIZE - len))
    return ((size_t)(luaL_errori(B->L, lit<"resulting string too large">()->view())));
  if (newsize <= MAX_SIZE / 3 * 2) /* no overflow? */
    newsize += (newsize >> 1);     /* new size *= 1.5 */
  if (newsize < len + sz)          /* not big enough? */
    newsize = len + sz;
  if (newsize < LUAL_BUFFERSIZE)
    newsize = LUAL_BUFFERSIZE;
  return newsize;
}

static size_t prepbuff(luaL_Buffer* B, size_t sz)
{
  if (sz > B->bldr.capacity() - B->bldr.size())
  {
    size_t newsize = newbuffsize(B, sz);
    assert(newsize >= B->reserved);
    if (l_unlikely(!lua_reservebytes(B->L, newsize - B->reserved)))
    {
      lua_pushistringref(B->L, lit<"not enough memory">()->view());
      lua_error(B->L); /* raise a memory error */
    }
    B->reserved = newsize;
    B->bldr.growTo(newsize); /* raises a memory error on failure */
  }
  return B->bldr.prep(sz);
}

LUALIB_API size_t luaL_prepbuffsize(luaL_Buffer* B, size_t sz)
{
  return prepbuff(B, sz);
}

LUALIB_API void luaL_addlstring(luaL_Buffer* B, std::string_view s, size_t off, size_t l)
{
  if (l > 0)
  {
    prepbuff(B, l);
    B->bldr.append(s, off, l);
  }
}

LUALIB_API void luaL_addstring(luaL_Buffer* B, const HostString& s)
{
  std::string_view sv = s.to_utf8();
  luaL_addlstring(B, sv, 0, sv.size());
}

LUALIB_API void luaL_pushresult(luaL_Buffer* B)
{
  lua_State* L = B->L;
  size_t len = B->bldr.size();
  size_t reserved = B->reserved;
  B->reserved = 0;
  if (reserved != 0)
    lua_releasebytes(L, reserved);
  /* mirror stock 5.5: a buffer that outgrew the static-buffer size would
     have moved to a heap box there, and its result is adopted as an
     external string whose payload is outside GC accounting. Content that
     fits LUAL_BUFFERSIZE becomes a regular (accounted) string. */
  if (reserved > LUAL_BUFFERSIZE)
    lua_pushistringext(L, InternalString::take(B->bldr.release()));
  else
    lua_pushistring(L, InternalString::take(B->bldr.release()));
  lua_gc(L, LUA_GCSTEP, len, 0, 0);
}

LUALIB_API void luaL_pushresultsize(luaL_Buffer* B, size_t sz)
{
  luaL_addsize(B, sz);
  luaL_pushresult(B);
}

LUALIB_API void luaL_addvalue(luaL_Buffer* B)
{
  lua_State* L = B->L;
  std::string_view s = *lua_tolstring(L, -1);
  prepbuff(B, s.size());
  B->bldr.append(s, 0, s.size());
  lua_pop(L, 1); /* pop string */
}

LUALIB_API void luaL_buffinit(lua_State* L, luaL_Buffer* B)
{
  B->L = L;
  B->bldr.attach(L);
  B->bldr.reset();
}

LUALIB_API size_t luaL_buffinitsize(lua_State* L, luaL_Buffer* B, size_t sz)
{
  luaL_buffinit(L, B);
  return prepbuff(B, sz);
}

/* }====================================================== */

/*
** {======================================================
** Reference system
** =======================================================
*/

/*
** The previously freed references form a linked list: t[1] is the index
** of a first free index, t[t[1]] is the index of the second element,
** etc. A zero signals the end of the list.
*/
LUALIB_API int luaL_ref(lua_State* L, int t)
{
  int ref;
  if (lua_isnil(L, -1))
  {
    lua_pop(L, 1);     /* remove from stack */
    return LUA_REFNIL; /* 'nil' has a unique fixed reference */
  }
  t = lua_absindex(L, t);
  if (lua_rawgeti(L, t, 1) == LUA_TNUMBER) /* already initialized? */
    ref = (int) lua_tointeger(L, -1);      /* ref = t[1] */
  else
  {                                    /* first access */
    assert(!lua_toboolean(L, -1)); /* must be nil or false */
    ref = 0;                           /* list is empty */
    lua_pushinteger(L, 0);             /* initialize as an empty list */
    lua_rawseti(L, t, 1);              /* ref = t[1] = 0 */
  }
  lua_pop(L, 1); /* remove element from stack */
  if (ref != 0)
  {                         /* any free element? */
    lua_rawgeti(L, t, ref); /* remove it from list */
    lua_rawseti(L, t, 1);   /* (t[1] = t[ref]) */
  }
  else                                /* no free elements */
    ref = (int) lua_rawlen(L, t) + 1; /* get a new reference */
  lua_rawseti(L, t, ref);
  return ref;
}

LUALIB_API void luaL_unref(lua_State* L, int t, int ref)
{
  if (ref >= 0)
  {
    t = lua_absindex(L, t);
    lua_rawgeti(L, t, 1);
    assert(lua_isinteger(L, -1));
    lua_rawseti(L, t, ref); /* t[ref] = t[1] */
    lua_pushinteger(L, ref);
    lua_rawseti(L, t, 1); /* t[1] = ref */
  }
}

/* }====================================================== */

/*
** {======================================================
** Load functions
** =======================================================
*/

struct LoadF
{
  unsigned n;        /* number of pre-read characters */
  OsFile* f;         /* file being read */
  char buff[BUFSIZ]; /* area for reading file */
};

static std::string_view getF(lua_State*, void* ud)
{
  LoadF* lf = (LoadF*) ud;
  if (lf->n > 0)
  {                          /* are there pre-read characters to be read? */
    size_t n = lf->n;        /* return them (chars already in buffer) */
    lf->n = 0;               /* no more pre-read characters */
    return std::string_view{lf->buff, n};
  }
  else
  { /* read a block from file */
    /* 'fread' can return > 0 *and* set the EOF flag. If next call to
       'getF' called 'fread', it might still wait for user input.
       The next check avoids this problem. */
    if (os_feof(lf->f))
      return {};
    size_t n = os_fread(lf->f, std::span<char>{lf->buff, sizeof(lf->buff)}); /* read block */
    return std::string_view{lf->buff, n};
  }
}

static int errfile(lua_State* L, std::string_view what, int fnameindex, int err)
{
  std::string_view filename = lua_tolstring(L, fnameindex)->substr(1);
  if (err != 0)
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    InternalString errmsg = os_strerror(err);
    lua_pushlstring(L, errmsg.view(), 0, errmsg.size());
    lua_pushfstringi(L, lit<"cannot %s %s: %s">()->view());
  }
  else
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    lua_pushfstringi(L, lit<"cannot %s %s">()->view());
  }
  lua_remove(L, fnameindex);
  return LUA_ERRFILE;
}

/*
** Skip an optional BOM at the start of a stream. If there is an
** incomplete BOM (the first character is correct but the rest is
** not), returns the first character anyway to force an error
** (as no chunk can start with 0xEF).
*/
static int skipBOM(OsFile* f)
{
  int c = os_getbyte(f);                                     /* read first character */
  if (c == 0xEF && os_getbyte(f) == 0xBB && os_getbyte(f) == 0xBF) /* correct BOM? */
    return os_getbyte(f);                                    /* ignore BOM and return next char */
  else                                                 /* no (valid) BOM */
    return c;                                          /* return first character */
}

/*
** reads the first character of file 'f' and skips an optional BOM mark
** in its beginning plus its first line if it starts with '#'. Returns
** true if it skipped the first line.  In any case, '*cp' has the
** first "valid" character of the file (after the optional BOM and
** a first-line comment).
*/
static int skipcomment(OsFile* f, int* cp)
{
  int c = *cp = skipBOM(f);
  if (c == '#')
  { /* first line is a comment (Unix exec. file)? */
    do
    { /* skip first line */
      c = os_getbyte(f);
    } while (c != OS_EOF && c != '\n');
    *cp = os_getbyte(f); /* next character after comment, if present */
    return 1;      /* there was a comment */
  }
  else
    return 0; /* no comment */
}

int luaL_loadfilei(lua_State* L, std::string_view filename, std::string_view mode)
{
  LoadF lf;
  int status, readstatus;
  int c;
  int fnameindex = lua_gettop(L) + 1; /* index of filename on the stack */
  const bool bNullname = filename.data() == nullptr;
  if (bNullname)
  {
    lua_pushistringref(L, lit<"=stdin">()->view());
    lf.f = os_stdin();
  }
  else
  {
    lua_pushistringref(L, filename);
    lua_pushfstringi(L, lit<"@%s">()->view());
    OsOpenResult o = os_fopen(filename, lit<"r">()->view());
    lf.f = o.f;
    if (lf.f == nullptr)
      return errfile(L, lit<"open">()->view(), fnameindex, o.err);
  }
  lf.n = 0;
  if (skipcomment(lf.f, &c)) /* read initial portion */
    lf.buff[lf.n++] = '\n';  /* add newline to correct line numbers */
  if (c == LUA_SIGNATURE[0])
  {           /* binary file? */
    lf.n = 0; /* remove possible newline */
    if (!bNullname)
    { /* "real" file? */
      int rerr = os_freopen(lf.f, filename, lit<"rb">()->view()); /* reopen in binary mode */
      if (rerr != 0)
        return errfile(L, lit<"reopen">()->view(), fnameindex, rerr);
      skipcomment(lf.f, &c); /* re-read initial portion */
    }
  }
  if (c != OS_EOF)
    lf.buff[lf.n++] = ((char)(c)); /* 'c' is the first character */
  status = lua_loadi(L, getF, &lf, lua_toistring(L, -1)->view(), mode);
  readstatus = os_ferror(lf.f) ? 1 : 0;
  if (!bNullname)
    os_fclose(lf.f); /* close file (even in case of errors) */
  if (readstatus)
  {
    lua_settop(L, fnameindex); /* ignore results from 'lua_load' */
    return errfile(L, lit<"read">()->view(), fnameindex, 0);
  }
  lua_remove(L, fnameindex);
  return status;
}

LUALIB_API int luaL_loadfilex(lua_State* L, const std::optional<HostString>& filename, const std::optional<HostString>& mode)
{
  std::string_view fn = filename.has_value() ? filename->to_utf8() : std::string_view(nullptr, 0);
  std::string_view m = mode.has_value() ? mode->to_utf8() : std::string_view(nullptr, 0);
  return luaL_loadfilei(L, fn, m);
}

struct LoadS
{
  std::string_view chunk;
};

static std::string_view getS(lua_State*, void* ud)
{
  LoadS* ls = (LoadS*) ud;
  std::string_view chunk = ls->chunk;
  ls->chunk = {};
  return chunk;
}

int luaL_loadbufferi(lua_State* L, std::string_view buff, std::string_view name, std::string_view mode)
{
  LoadS ls;
  ls.chunk = buff;
  return lua_loadi(L, getS, &ls, name, mode);
}

LUALIB_API int luaL_loadbufferx(lua_State* L, std::string_view buff, const std::optional<HostString>& name, const std::optional<HostString>& mode)
{
  std::string_view nullsv(nullptr, 0);
  return luaL_loadbufferi(L, buff, name.has_value() ? name->to_utf8() : nullsv, mode.has_value() ? mode->to_utf8() : nullsv);
}

LUALIB_API int luaL_loadstring(lua_State* L, const HostString& s)
{
  return luaL_loadbufferi(L, s.to_utf8(), s.to_utf8(), lit<"t">()->view());
}

/* }====================================================== */

LUALIB_API int luaL_getmetafield(lua_State* L, int obj, const HostString& event)
{
  return luaL_getmetafieldi(L, obj, event.to_utf8());
}

int luaL_getmetafieldi(lua_State* L, int obj, std::string_view event)
{
  if (!lua_getmetatable(L, obj)) /* no metatable? */
    return LUA_TNIL;
  else
  {
    int tt;
    lua_pushlstring(L, event, 0, event.size());
    tt = lua_rawget(L, -2);
    if (tt == LUA_TNIL) /* is metafield nil? */
      lua_pop(L, 2);    /* remove metatable and metafield */
    else
      lua_remove(L, -2); /* remove only metatable */
    return tt;           /* return metafield type */
  }
}

LUALIB_API int luaL_callmeta(lua_State* L, int obj, const HostString& event)
{
  return luaL_callmetai(L, obj, event.to_utf8());
}

int luaL_callmetai(lua_State* L, int obj, std::string_view event)
{
  obj = lua_absindex(L, obj);
  if (luaL_getmetafieldi(L, obj, event) == LUA_TNIL) /* no metafield? */
    return 0;
  lua_pushvalue(L, obj);
  lua_call(L, 1, 1);
  return 1;
}

LUALIB_API lua_Integer luaL_len(lua_State* L, int idx)
{
  lua_len(L, idx);
  std::optional<lua_Integer> l = lua_tointegerx(L, -1);
  if (l_unlikely(!l))
    luaL_errori(L, lit<"object length is not an integer">()->view());
  lua_pop(L, 1); /* remove object */
  return *l;
}

const InternalString* luaL_toistring(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  if (luaL_callmetai(L, idx, lit<"__tostring">()->view()))
  { /* metafield? */
    if (!lua_isstring(L, -1))
      luaL_errori(L, lit<"'__tostring' must return a string">()->view());
  }
  else
  {
    switch (lua_type(L, idx))
    {
    case LUA_TNUMBER:
    {
      char nbuff[LUA_N2SBUFFSZ];
      std::string_view n = lua_numbertocstringi(L, idx, nbuff);
      lua_pushlstring(L, n, 0, n.size());
      break;
    }
    case LUA_TSTRING:
      lua_pushvalue(L, idx);
      break;
    case LUA_TBOOLEAN:
      lua_pushistringref(L, lua_toboolean(L, idx) ? lit<"true">()->view() : lit<"false">()->view());
      break;
    case LUA_TNIL:
      lua_pushistringref(L, lit<"nil">()->view());
      break;
    default:
    {
      int tt = luaL_getmetafieldi(L, idx, lit<"__name">()->view()); /* try name */
      std::string_view kind = (tt == LUA_TSTRING)
                                  ? *lua_tolstring(L, -1)
                                  : lua_typenamei(L, lua_type(L, idx))->view();
      if (lua_islightcfunction(L, idx))
      {
        InternalString fa = lua_funcaddressi(L, idx);
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushlstring(L, fa.view(), 0, fa.size());
        lua_pushfstringi(L, lit<"%s: %s">()->view());
      }
      else
      {
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushvalue(L, idx);
        lua_pushfstringi(L, lit<"%s: %p">()->view());
      }
      if (tt != LUA_TNIL)
        lua_remove(L, -2); /* remove '__name' */
      break;
    }
    }
  }
  return lua_toistring(L, -1);
}

LUALIB_API std::string_view luaL_tolstring(lua_State* L, int idx)
{
  return luaL_toistring(L, idx)->view();
}

/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
void luaL_setfuncsi(lua_State* L, std::span<const luaL_Regi> funcs, int nup)
{
  luaL_checkstacki(L, nup, lit<"too many upvalues">()->view());
  for (const auto& reg : funcs)
  {
    if (reg.func == nullptr) /* placeholder? */
      lua_pushboolean(L, 0);
    else
    {
      for (int i = 0; i < nup; i++) /* copy upvalues to the top */
        lua_pushvalue(L, -nup);
      lua_pushcclosure(L, reg.func, nup); /* closure with those upvalues */
    }
    lua_setfieldi(L, -(nup + 2), reg.name);
  }
  lua_pop(L, nup); /* remove upvalues */
}

void luaL_newlibi(lua_State* L, std::span<const luaL_Regi> funcs)
{
  luaL_checkversion(L);
  lua_createtable(L, 0, (int)funcs.size());
  luaL_setfuncsi(L, funcs, 0);
}

LUALIB_API void luaL_setfuncs(lua_State* L, const std::span<const luaL_Reg>& funcs, int nup)
{
  luaL_checkstacki(L, nup, lit<"too many upvalues">()->view());
  for (const auto& reg : funcs)
  {
    if (reg.func == nullptr) /* placeholder? */
      lua_pushboolean(L, 0);
    else
    {
      for (int i = 0; i < nup; i++) /* copy upvalues to the top */
        lua_pushvalue(L, -nup);
      lua_pushcclosure(L, reg.func, nup); /* closure with those upvalues */
    }
    lua_setfieldi(L, -(nup + 2), reg.name.to_utf8());
  }
  lua_pop(L, nup); /* remove upvalues */
}

LUALIB_API void luaL_newlib(lua_State* L, const std::span<const luaL_Reg>& funcs)
{
  luaL_checkversion(L);
  lua_createtable(L, 0, (int)funcs.size());
  luaL_setfuncs(L, funcs, 0);
}

/*
** ensure that stack[idx][fname] has a table and push that table
** into the stack
*/
LUALIB_API int luaL_getsubtable(lua_State* L, int idx, const HostString& fname)
{
  return luaL_getsubtablei(L, idx, fname.to_utf8());
}

int luaL_getsubtablei(lua_State* L, int idx, std::string_view fname)
{
  if (lua_getfieldi(L, idx, fname) == LUA_TTABLE)
    return 1; /* table already there */
  else
  {
    lua_pop(L, 1); /* remove previous result */
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1);        /* copy to be left at top */
    lua_setfieldi(L, idx, fname); /* assign new table to field */
    return 0;                    /* false, because did not find table there */
  }
}

/*
** Stripped-down 'require': After checking "loaded" table, calls 'openf'
** to open a module, registers the result in 'package.loaded' table and,
** if 'glb' is true, also registers the result in the global table.
** Leaves resulting module on the top.
*/
LUALIB_API void luaL_requiref(lua_State* L, const HostString& modname,
                              lua_CFunction openf, int glb)
{
  luaL_requirefi(L, modname.to_utf8(), openf, glb);
}

void luaL_requirefi(lua_State* L, std::string_view modname,
                    lua_CFunction openf, int glb)
{
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_getfieldi(L, -1, modname); /* LOADED[modname] */
  if (!lua_toboolean(L, -1))
  {                /* package not already loaded? */
    lua_pop(L, 1); /* remove field */
    lua_pushcfunction(L, openf);
    lua_pushlstring(L, modname, 0, modname.size());   /* argument to open function */
    lua_call(L, 1, 1);            /* call 'openf' to open module */
    lua_pushvalue(L, -1);         /* make copy of module (call result) */
    lua_setfieldi(L, -3, modname); /* LOADED[modname] = module */
  }
  lua_remove(L, -2); /* remove LOADED table */
  if (glb)
  {
    lua_pushvalue(L, -1);      /* copy of module */
    lua_setglobali(L, modname); /* _G[modname] = module */
  }
}

void luaL_addgsubi(luaL_Buffer* b, std::string_view s,
                   std::string_view p, std::string_view r)
{
  size_t pos = 0;
  size_t wild;
  while ((wild = s.find(p, pos)) != std::string_view::npos)
  {
    luaL_addlstring(b, s, pos, wild - pos); /* push prefix */
    luaL_addlstring(b, r, 0, r.size()); /* push replacement in place of pattern */
    pos = wild + p.size();              /* continue after 'p' */
  }
  luaL_addlstring(b, s, pos, s.size() - pos); /* push last suffix */
}

LUALIB_API void luaL_addgsub(luaL_Buffer* b, const HostString& s, const HostString& p, const HostString& r)
{
  luaL_addgsubi(b, s.to_utf8(), p.to_utf8(), r.to_utf8());
}

std::string_view luaL_gsubi(lua_State* L, std::string_view s, std::string_view p, std::string_view r)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addgsubi(&b, s, p, r);
  luaL_pushresult(&b);
  return *lua_tolstring(L, -1);
}

LUALIB_API HostString luaL_gsub(lua_State* L, const HostString& s, const HostString& p, const HostString& r)
{
  return HostString(luaL_gsubi(L, s.to_utf8(), p.to_utf8(), r.to_utf8()));
}

static void* luaL_alloc(void*, void* ptr, size_t, size_t nsize)
{
  if (nsize == 0)
  {
    free(ptr);
    return nullptr;
  }
  else
    return realloc(ptr, nsize);
}

/*
** Standard panic function just prints an error message. The test
** with 'lua_type' avoids possible memory errors in 'lua_tostring'.
*/
static int panic(lua_State* L)
{
  const InternalString* msg = (lua_type(L, -1) == LUA_TSTRING)
                                  ? lua_toistring(L, -1)
                                  : lit<"error object is not a string">();
  os_print_stderr(lit<"PANIC: unprotected error in call to Lua API (">()->view());
  os_print_stderr(msg->view());
  os_print_stderr(lit<")\n">()->view());
  os_flush_stderr();
  return 0; /* return to Lua to abort */
}

/*
** Warning functions:
** warnfoff: warning system is off
** warnfon: ready to start a new message
** warnfcont: previous message is to be continued
*/
static void warnfoff(void* ud, const HostString& message, int tocont);
static void warnfon(void* ud, const HostString& message, int tocont);
static void warnfcont(void* ud, const HostString& message, int tocont);

/*
** Check whether message is a control message. If so, execute the
** control or ignore it if unknown.
*/
static int checkcontrol(lua_State* L, const HostString& message, int tocont)
{
  std::string_view m = message.to_utf8();
  if (tocont || m.empty() || m[0] != '@') /* not a control message? */
    return 0;
  else
  {
    if (m.substr(1) == lit<"off">()->view())
      lua_setwarnf(L, warnfoff, L); /* turn warnings off */
    else if (m.substr(1) == lit<"on">()->view())
      lua_setwarnf(L, warnfon, L); /* turn warnings on */
    return 1;                      /* it was a control message */
  }
}

static void warnfoff(void* ud, const HostString& message, int tocont)
{
  checkcontrol((lua_State*) ud, message, tocont);
}

/*
** Writes the message and handle 'tocont', finishing the message
** if needed and setting the next warn function.
*/
static void warnfcont(void* ud, const HostString& message, int tocont)
{
  lua_State* L = (lua_State*) ud;
  os_print_stderr(message.to_utf8());
  os_flush_stderr();
  if (tocont)                          /* not the last part? */
    lua_setwarnf(L, warnfcont, L);     /* to be continued */
  else
  {                                /* last part */
    os_print_stderr(lit<"\n">()->view()); /* finish message with end-of-line */
    os_flush_stderr();
    lua_setwarnf(L, warnfon, L);   /* next call is a new message */
  }
}

static void warnfon(void* ud, const HostString& message, int tocont)
{
  if (checkcontrol((lua_State*) ud, message, tocont)) /* control message? */
    return;                                           /* nothing else to be done */
  os_print_stderr(lit<"Lua warning: ">()->view()); /* start a new warning */
  os_flush_stderr();
  warnfcont(ud, message, tocont);         /* finish processing */
}

/*
** A function to compute an unsigned int with some level of
** randomness. Rely on Address Space Layout Randomization (if present)
** and the current time.
*/
#include <time.h>

/* Size for the buffer, in bytes */
constexpr size_t BUFSEEDB = (sizeof(void*) + sizeof(time_t));

/* Size for the buffer in int's, rounded up */
constexpr size_t BUFSEED = ((BUFSEEDB + sizeof(int) - 1) / sizeof(int));

/*
** Copy the contents of variable 'v' into the buffer pointed by 'b'.
** (The '&b[0]' disguises 'b' to fix an absurd warning from clang.)
*/

static unsigned int luai_makeseed(void)
{
  unsigned int buff[BUFSEED];
  unsigned int res;
  unsigned int i;
  time_t t = time(nullptr);
  char* b = (char*) buff;
  (memcpy(&(b)[0], &(b), sizeof(b)), (b) += sizeof(b)); /* local variable's address */
  (memcpy(&(b)[0], &(t), sizeof(t)), (b) += sizeof(t)); /* time */
  /* fill (rare but possible) remain of the buffer with zeros */
  memset(b, 0, sizeof(buff) - BUFSEEDB);
  res = buff[0];
  for (i = 1; i < BUFSEED; i++)
    res ^= (res >> 3) + (res << 7) + buff[i];
  return res;
}

LUALIB_API unsigned int luaL_makeseed(lua_State*)
{
  return luai_makeseed();
}

/*
** Use the name with parentheses so that headers can redefine it
** as a macro.
*/
LUALIB_API lua_State*(luaL_newstate) (void)
{
  lua_State* L = lua_newstate(luaL_alloc, nullptr, luaL_makeseed(nullptr));
  if (l_likely(L))
  {
    lua_atpanic(L, &panic);
    lua_setwarnf(L, warnfon, L);
  }
  return L;
}

LUALIB_API void luaL_checkversion_(lua_State* L, lua_Number ver, size_t sz)
{
  lua_Number v = lua_version(L);
  if (sz != LUAL_NUMSIZES) /* check numeric types */
    luaL_errori(L, lit<"core and library have incompatible numeric types">()->view());
  else if (v != ver)
  {
    lua_pushnumber(L, ver);
    lua_pushnumber(L, v);
    luaL_errori(L, lit<"version mismatch: app. needs %f, Lua core provides %f">()->view());
  }
}


LUALIB_API void luaL_checkversion(lua_State* L)
{
  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

LUALIB_API int luaL_loadfile(lua_State* L, const std::optional<HostString>& filename)
{
  std::string_view nullview(nullptr, 0);
  if (!filename)
    return luaL_loadfilei(L, nullview, nullview);
  std::string_view sv = filename->to_utf8();
  return luaL_loadfilei(L, sv, nullview);
}

LUALIB_API void luaL_argcheck(lua_State* L, bool cond, int arg, const HostString& extramsg)
{
  if (luai_unlikely(!cond))
    luaL_argerrori(L, arg, extramsg.to_utf8());
}

void luaL_argchecki(lua_State* L, bool cond, int arg, std::string_view extramsg)
{
  if (luai_unlikely(!cond))
    luaL_argerrori(L, arg, extramsg);
}

LUALIB_API void luaL_argexpected(lua_State* L, bool cond, int arg, const HostString& tname)
{
  if (luai_unlikely(!cond))
    luaL_typeerrori(L, arg, tname.to_utf8());
}

void luaL_argexpectedi(lua_State* L, bool cond, int arg, std::string_view tname)
{
  if (luai_unlikely(!cond))
    luaL_typeerrori(L, arg, tname);
}

LUALIB_API HostString luaL_checkstring(lua_State* L, int arg)
{
  return HostString(luaL_checklstring(L, arg));
}

LUALIB_API std::optional<HostString> luaL_optstring(lua_State* L, int arg,
                                                    const std::optional<HostString>& d)
{
  if (lua_isnoneornil(L, arg))
    return d;
  return luaL_checkstring(L, arg);
}

LUALIB_API HostString luaL_typename(lua_State* L, int idx)
{
  return lua_typename(L, lua_type(L, idx));
}

LUALIB_API int luaL_dofile(lua_State* L, const std::optional<HostString>& filename)
{
  int status = luaL_loadfile(L, filename);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return status;
}

LUALIB_API int luaL_dostring(lua_State* L, const HostString& str)
{
  int status = luaL_loadstring(L, str);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return status;
}

LUALIB_API int luaL_getmetatable(lua_State* L, const HostString& tname)
{
  return luaL_getmetatablei(L, tname.to_utf8());
}

int luaL_getmetatablei(lua_State* L, std::string_view tname)
{
  return lua_getfieldi(L, LUA_REGISTRYINDEX, tname);
}

LUALIB_API int luaL_loadbuffer(lua_State* L, std::string_view buff, const std::optional<HostString>& name)
{
  std::string_view nullsv(nullptr, 0);
  return luaL_loadbufferi(L, buff, name.has_value() ? name->to_utf8() : nullsv, nullsv);
}

LUALIB_API void luaL_pushfail(lua_State* L)
{
  lua_pushnil(L);
}

LUALIB_API size_t luaL_bufflen(const luaL_Buffer* bf)
{
  return bf->bldr.size();
}

LUALIB_API std::span<char> luaL_buffarea(luaL_Buffer* bf)
{
  return bf->bldr.area();
}

LUALIB_API void luaL_addchar(luaL_Buffer* B, char c)
{
  size_t off = luaL_prepbuffsize(B, 1);
  B->bldr.area()[off] = c;
  luaL_addsize(B, 1);
}

LUALIB_API void luaL_addsize(luaL_Buffer* B, size_t s)
{
  B->bldr.commit(s);
}

LUALIB_API void luaL_buffsub(luaL_Buffer* B, size_t s)
{
  B->bldr.drop(s);
}

LUALIB_API size_t luaL_prepbuffer(luaL_Buffer* B)
{
  return luaL_prepbuffsize(B, LUAL_BUFFERSIZE);
}

/* }====================================================================== */

void ByteBuilder::attach(lua_State* L)
{
  assert(bytes_.empty() || L == L_);
  L_ = L;
  lua_Allocator a = lua_getallocf(L);
  this->allocf_ = a.f;
  this->allocud_ = a.ud;
}

ByteBuilder& ByteBuilder::operator=(ByteBuilder&& o) noexcept
{
  if (this != &o)
  {
    dealloc();
    bytes_ = o.bytes_;
    len_ = o.len_;
    L_ = o.L_;
    allocf_ = o.allocf_;
    allocud_ = o.allocud_;
    o.bytes_ = {};
    o.len_ = 0;
  }
  return *this;
}
ByteBuilder::~ByteBuilder()
{
  dealloc();
}

void ByteBuilder::append(char c)
{
  ensure(1);
  bytes_[len_] = c;
  commit(1);
}
void ByteBuilder::append(std::string_view b)
{
  append(b, 0, b.size());
}

void ByteBuilder::append(std::string_view b, size_t off, size_t n)
{
  if (n > 0)
  {
    ensure(n);
    memcpy(&bytes_[len_], b.data() + off, n);
    commit(n);
  }
}

void ByteBuilder::reset() noexcept { len_ = 0; }

void ByteBuilder::drop(size_t k) { len_ -= k; }

size_t ByteBuilder::size() const noexcept { return len_; }

std::string_view ByteBuilder::view() const noexcept { return {bytes_.data(), len_}; }

std::span<char> ByteBuilder::area() noexcept { return bytes_; }

/* reserve sz bytes after current content; capacity must already suffice */
size_t ByteBuilder::prep(size_t sz) noexcept
{
  assert(sz <= bytes_.size() - len_);
  (void) sz;
  return len_;
}
void ByteBuilder::commit(size_t sz) noexcept { len_ += sz; }

size_t ByteBuilder::capacity() const noexcept { return bytes_.size(); }

/*
** Resize through the state's allocator with realloc semantics,
** mirroring 'resizebox' in stock lauxlib.c: on allocation failure,
** raise a Lua memory error. Callers never check.
*/
void ByteBuilder::growTo(size_t cap)
{
  if (cap == bytes_.size())
    return;
  assert(allocf_ != nullptr);
  void* nb = allocf_(allocud_, bytes_.data(), bytes_.size(), cap);
  if (l_unlikely(nb == nullptr && cap > 0))
  {
    lua_pushistringref(L_, lit<"not enough memory">()->view());
    lua_error(L_); /* raise a memory error */
  }
  bytes_ = {static_cast<char*>(nb), cap};
}

std::string ByteBuilder::release()
{
  std::string out;
  if (len_ != 0)
    out.assign(bytes_.data(), len_);
  dealloc();
  len_ = 0;
  return out;
}

void ByteBuilder::ensure(size_t sz)
{
  if (sz > bytes_.size() - len_)
    growTo(len_ + sz);
}

void ByteBuilder::dealloc() noexcept
{
  if (bytes_.data() != nullptr)
  {
    allocf_(allocud_, bytes_.data(), bytes_.size(), 0);
    bytes_ = {};
  }
}
