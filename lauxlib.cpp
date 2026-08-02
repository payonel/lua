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

#include "lauxlib.h"
#include "llimits.h"
#include "util_cast.h"

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
        lua_pushliteral(L, HostString(lit<".">()->view())); /* place '.' between the two names */
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
  lua_getfield(L, LUA_REGISTRYINDEX, HostString(LUA_LOADED_TABLE->view()));
  luaL_checkstack(L, 6, HostString(lit<"not enough stack">()->view())); /* slots for 'findfield' */
  if (findfield(L, top + 1, 2))
  {
    std::string_view name = lua_tolstring(L, -1);
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
    lua_pushfstring(L, HostString(lit<"%s '%s'">()->view())); /* use it */
  }
  else if (ar->what[0] == 'm')                             /* main? */
    lua_pushliteral(L, HostString(lit<"main chunk">()->view()));
  else if (pushglobalfuncname(L, ar))
  {                       /* try a global name */
    lua_pushvalue(L, -1);
    lua_pushfstring(L, HostString(lit<"function '%s'">()->view()));
    lua_remove(L, -2); /* remove name */
  }
  else if (ar->what[0] != 'C')
  { /* for Lua functions, use <file:line> */
    std::string_view ss{ar->short_src};
    lua_pushlstring(L, ss, 0, ss.size());
    lua_pushinteger(L, ar->linedefined);
    lua_pushfstring(L, HostString(lit<"function <%s:%d>">()->view()));
  }
  else /* nothing left... */
    lua_pushliteral(L, HostString(lit<"?">()->view()));
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
  luaL_Buffer b;
  std::optional<InternalDebug> oar;
  int last = lastlevel(L1);
  int limit2show = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  luaL_buffinit(L, &b);
  if (msg)
  {
    luaL_addstring(&b, *msg);
    luaL_addchar(&b, '\n');
  }
  luaL_addstring(&b, HostString(lit<"stack traceback:">()->view()));
  while ((oar = lua_getstacki(L1, level++)))
  {
    if (limit2show-- == 0)
    {                                     /* too many levels? */
      int n = last - level - LEVELS2 + 1; /* number of levels to skip */
      lua_pushinteger(L, n);
      lua_pushfstring(L, HostString(lit<"\n\t...\t(skipping %d levels)">()->view()));
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
        lua_pushfstring(L, HostString(lit<"\n\t%s: in ">()->view()));
      }
      else
      {
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushinteger(L, ar.currentline);
        lua_pushfstring(L, HostString(lit<"\n\t%s:%d: in ">()->view()));
      }
      luaL_addvalue(&b);
      pushfuncname(L, &ar);
      luaL_addvalue(&b);
      if (ar.istailcall)
        luaL_addstring(&b, HostString(lit<"\n\t(...tail calls...)">()->view()));
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
  const InternalString* argword;
  std::optional<InternalDebug> oar = lua_getstacki(L, 0);
  if (!oar)
  { /* no stack frame? */
    lua_pushinteger(L, arg);
    lua_pushstring(L, extramsg);
    return luaL_error(L, HostString(lit<"bad argument #%d (%s)">()->view()));
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
        lua_pushstring(L, extramsg);
        return luaL_error(L, HostString(lit<"calling '%s' on bad self (%s)">()->view()));
      }
      /* else go through; error in a regular argument */
    }
    argword = lit<"argument">();
  }
  if (ar.name.data() == nullptr)
    ar.name = (pushglobalfuncname(L, &ar)) ? lua_tolstring(L, -1)
                                           : lit<"?">()->view();
  lua_pushstring(L, HostString(argword->view()));
  lua_pushinteger(L, arg);
  lua_pushlstring(L, ar.name, 0, ar.name.size());
  lua_pushstring(L, extramsg);
  return luaL_error(L, HostString(lit<"bad %s #%d to '%s' (%s)">()->view()));
}

LUALIB_API int luaL_typeerror(lua_State* L, int arg, const HostString& tname)
{
  HostString typearg; /* name for the type of the actual argument */
  if (luaL_getmetafield(L, arg, HostString(lit<"__name">()->view())) == LUA_TSTRING)
    typearg = *lua_tostring(L, -1); /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = HostString(lit<"light userdata">()->view()); /* special name for messages */
  else
    typearg = luaL_typename(L, arg); /* standard name */
  lua_pushstring(L, tname);
  lua_pushstring(L, typearg);
  const HostString msg = lua_pushfstring(L, HostString(lit<"%s expected, got %s">()->view()));
  return luaL_argerror(L, arg, msg);
}

static void tag_error(lua_State* L, int arg, int tag)
{
  luaL_typeerror(L, arg, lua_typename(L, tag));
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
      lua_pushfstring(L, HostString(lit<"%s:%d: ">()->view()));
      return;
    }
  }
  lua_pushfstring(L, HostString(lit<"">()->view())); /* else, no information available... */
}

LUALIB_API int luaL_error(lua_State* L, const HostString& fmt)
{
  lua_pushfstring(L, fmt);
  luaL_where(L, 1);
  lua_rotate(L, -2, 1);
  lua_concat(L, 2);
  return lua_error(L);
}

int luaL_fileresulti(lua_State* L, int stat, std::string_view fname)
{
  int en = errno; /* calls to Lua API may change this value */
  if (stat)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    luaL_pushfail(L);
    std::string_view msg = (en != 0) ? std::string_view{strerror(en)}
                                     : lit<"(no extra info)">()->view();
    if (fname.data() != nullptr)
    {
      lua_pushlstring(L, fname, 0, fname.size());
      lua_pushlstring(L, msg, 0, msg.size());
      lua_pushfstring(L, HostString(lit<"%s: %s">()->view()));
    }
    else
      lua_pushlstring(L, msg, 0, msg.size());
    lua_pushinteger(L, en);
    return 3;
  }
}

LUALIB_API int luaL_fileresult(lua_State* L, int stat, const std::optional<HostString>& fname)
{
  return luaL_fileresulti(L, stat, fname ? fname->view() : std::string_view{});
}

#include <sys/wait.h> /* waitpid macros to interpret 'pclose' return status */

LUALIB_API int luaL_execresult(lua_State* L, int stat)
{
  if (stat != 0 && errno != 0) /* error with an 'errno'? */
    return luaL_fileresult(L, 0, std::nullopt);
  else
  {
    const InternalString* what = lit<"exit">(); /* type of termination */
    /* interpret result */
    if (WIFEXITED(stat))
    {
      stat = WEXITSTATUS(stat);
    }
    else if (WIFSIGNALED(stat))
    {
      stat = WTERMSIG(stat);
      what = lit<"signal">();
    }
    if (what == lit<"exit">() && stat == 0) /* successful termination? */
      lua_pushboolean(L, 1);
    else
      luaL_pushfail(L);
    lua_pushstring(L, HostString(what->view()));
    lua_pushinteger(L, stat);
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
  if (luaL_getmetatable(L, tname) != LUA_TNIL) /* name already in use? */
    return 0;                                  /* leave previous value on top, but return 0 */
  lua_pop(L, 1);
  lua_createtable(L, 0, 4);
  lua_pushstring(L, tname);
  lua_setfield(L, -2, HostString(lit<"__name">()->view())); /* metatable.__name = tname */
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, tname); /* registry.name = metatable */
  return 1;
}

LUALIB_API void luaL_setmetatable(lua_State* L, const HostString& tname)
{
  luaL_getmetatable(L, tname);
  lua_setmetatable(L, -2);
}

LUALIB_API void* luaL_testudata(lua_State* L, int ud, const HostString& tname)
{
  void* p = lua_touserdata(L, ud);
  if (p != nullptr)
  { /* value is a userdata? */
    if (lua_getmetatable(L, ud))
    {                               /* does it have a metatable? */
      luaL_getmetatable(L, tname);  /* get correct metatable */
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
  void* p = luaL_testudata(L, ud, tname);
  luaL_argexpected(L, p != nullptr, ud, tname);
  return p;
}

/* }====================================================== */

/*
** {======================================================
** Argument check functions
** =======================================================
*/

LUALIB_API int luaL_checkoption(lua_State* L, int arg,
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
  const HostString msg = lua_pushfstring(L, HostString(lit<"invalid option '%s'">()->view()));
  return luaL_argerror(L, arg, msg);
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
  if (l_unlikely(!lua_checkstack(L, space)))
  {
    if (msg && lua_checkstack(L, 1))
    {
      lua_pushstring(L, *msg);
      luaL_error(L, HostString(lit<"stack overflow (%s)">()->view()));
    }
    else
      luaL_error(L, HostString(lit<"stack overflow">()->view()));
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
    luaL_argerror(L, arg, HostString(lit<"value expected">()->view()));
}

LUALIB_API std::string_view luaL_checklstring(lua_State* L, int arg)
{
  std::string_view s = lua_tolstring(L, arg);
  if (l_unlikely(s.data() == nullptr))
    tag_error(L, arg, LUA_TSTRING);
  return s;
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
    luaL_argerror(L, arg, HostString(lit<"number has no integer representation">()->view()));
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
    return ((size_t)(luaL_error(B->L, HostString(lit<"resulting string too large">()->view()))));
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
  if (B->bldr.size() + sz > B->bldr.capacity())
  {
    size_t newsize = newbuffsize(B, sz);
    assert(newsize >= B->reserved);
    if (l_unlikely(!lua_reservebytes(B->L, newsize - B->reserved)))
    {
      lua_pushliteral(B->L, HostString(lit<"not enough memory">()->view()));
      lua_error(B->L); /* raise a memory error */
    }
    B->reserved = newsize;
    B->bldr.growTo(newsize);
  }
  return B->bldr.prep(sz);
}

LUALIB_API size_t luaL_prepbuffsize(luaL_Buffer* B, size_t sz)
{
  return prepbuff(B, sz);
}

LUALIB_API void luaL_addlstring(luaL_Buffer* B, std::string_view s,
                                size_t off, size_t l)
{
  if (l > 0)
  {
    size_t at = prepbuff(B, l);
    memcpy(B->bldr.area().data() + at, s.data() + off, l * sizeof(char));
    luaL_addsize(B, l);
  }
}

LUALIB_API void luaL_addstring(luaL_Buffer* B, const HostString& s)
{
  std::string_view sv = s.view();
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
  lua_pushistring(L, B->bldr.finish());
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
  std::string_view s = lua_tolstring(L, -1);
  size_t off = prepbuff(B, s.size());
  memcpy(B->bldr.area().data() + off, s.data(), s.size() * sizeof(char));
  luaL_addsize(B, s.size());
  lua_pop(L, 1); /* pop string */
}

LUALIB_API void luaL_buffinit(lua_State* L, luaL_Buffer* B)
{
  B->L = L;
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
  FILE* f;           /* file being read */
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
    if (feof(lf->f))
      return {};
    size_t n = fread(lf->buff, 1, sizeof(lf->buff), lf->f); /* read block */
    return std::string_view{lf->buff, n};
  }
}

static int errfile(lua_State* L, std::string_view what, int fnameindex)
{
  int err = errno;
  std::string_view filename = lua_tolstring(L, fnameindex).substr(1);
  if (err != 0)
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    lua_pushstring(L, strerror(err));
    lua_pushfstring(L, HostString(lit<"cannot %s %s: %s">()->view()));
  }
  else
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    lua_pushfstring(L, HostString(lit<"cannot %s %s">()->view()));
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
static int skipBOM(FILE* f)
{
  int c = getc(f);                                     /* read first character */
  if (c == 0xEF && getc(f) == 0xBB && getc(f) == 0xBF) /* correct BOM? */
    return getc(f);                                    /* ignore BOM and return next char */
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
static int skipcomment(FILE* f, int* cp)
{
  int c = *cp = skipBOM(f);
  if (c == '#')
  { /* first line is a comment (Unix exec. file)? */
    do
    { /* skip first line */
      c = getc(f);
    } while (c != EOF && c != '\n');
    *cp = getc(f); /* next character after comment, if present */
    return 1;      /* there was a comment */
  }
  else
    return 0; /* no comment */
}

int luaL_loadfilei(lua_State* L, const InternalString* filename,
                   const InternalString* mode)
{
  LoadF lf;
  int status, readstatus;
  int c;
  int fnameindex = lua_gettop(L) + 1; /* index of filename on the stack */
  if (filename == nullptr)
  {
    lua_pushliteral(L, HostString(lit<"=stdin">()->view()));
    lf.f = stdin;
  }
  else
  {
    lua_pushistringref(L, filename);
    lua_pushfstring(L, HostString(lit<"@%s">()->view()));
    errno = 0;
    lf.f = fopen(filename->c_str(), "r");
    if (lf.f == nullptr)
      return errfile(L, lit<"open">()->view(), fnameindex);
  }
  lf.n = 0;
  if (skipcomment(lf.f, &c)) /* read initial portion */
    lf.buff[lf.n++] = '\n';  /* add newline to correct line numbers */
  if (c == LUA_SIGNATURE[0])
  {           /* binary file? */
    lf.n = 0; /* remove possible newline */
    if (filename != nullptr)
    { /* "real" file? */
      errno = 0;
      lf.f = freopen(filename->c_str(), "rb", lf.f); /* reopen in binary mode */
      if (lf.f == nullptr)
        return errfile(L, lit<"reopen">()->view(), fnameindex);
      skipcomment(lf.f, &c); /* re-read initial portion */
    }
  }
  if (c != EOF)
    lf.buff[lf.n++] = ((char)(c)); /* 'c' is the first character */
  status = lua_loadi(L, getF, &lf, lua_toistring(L, -1), mode);
  readstatus = ferror(lf.f);
  errno = 0; /* no useful error number until here */
  if (filename)
    fclose(lf.f); /* close file (even in case of errors) */
  if (readstatus)
  {
    lua_settop(L, fnameindex); /* ignore results from 'lua_load' */
    return errfile(L, lit<"read">()->view(), fnameindex);
  }
  lua_remove(L, fnameindex);
  return status;
}

LUALIB_API int luaL_loadfilex(lua_State* L,
                              const std::optional<HostString>& filename,
                              const std::optional<HostString>& mode)
{
  InternalString fn{filename ? InternalString(*filename) : InternalString()};
  InternalString m{mode ? InternalString(*mode) : InternalString()};
  return luaL_loadfilei(L, filename ? &fn : nullptr, mode ? &m : nullptr);
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

int luaL_loadbufferi(lua_State* L, std::string_view buff,
                     const InternalString* name,
                     const InternalString* mode)
{
  LoadS ls;
  ls.chunk = buff;
  return lua_loadi(L, getS, &ls, name, mode);
}

LUALIB_API int luaL_loadbufferx(lua_State* L, std::string_view buff,
                                const std::optional<HostString>& name,
                                const std::optional<HostString>& mode)
{
  InternalString n{name ? *name : HostString("?")};
  InternalString m{mode ? *mode : HostString()};
  return luaL_loadbufferi(L, buff, &n, mode ? &m : nullptr);
}

LUALIB_API int luaL_loadstring(lua_State* L, const HostString& s)
{
  return luaL_loadbufferx(L, s.view(), s, HostString(lit<"t">()->view()));
}

/* }====================================================== */

LUALIB_API int luaL_getmetafield(lua_State* L, int obj, const HostString& event)
{
  if (!lua_getmetatable(L, obj)) /* no metatable? */
    return LUA_TNIL;
  else
  {
    int tt;
    lua_pushstring(L, event);
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
  obj = lua_absindex(L, obj);
  if (luaL_getmetafield(L, obj, event) == LUA_TNIL) /* no metafield? */
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
    luaL_error(L, HostString(lit<"object length is not an integer">()->view()));
  lua_pop(L, 1); /* remove object */
  return *l;
}

const InternalString* luaL_toistring(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  if (luaL_callmeta(L, idx, HostString(lit<"__tostring">()->view())))
  { /* metafield? */
    if (!lua_isstring(L, -1))
      luaL_error(L, HostString(lit<"'__tostring' must return a string">()->view()));
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
      lua_pushstring(L, (lua_toboolean(L, idx)
                             ? HostString(lit<"true">()->view())
                             : HostString(lit<"false">()->view())));
      break;
    case LUA_TNIL:
      lua_pushliteral(L, HostString(lit<"nil">()->view()));
      break;
    default:
    {
      int tt = luaL_getmetafield(L, idx, HostString(lit<"__name">()->view())); /* try name */
      std::string_view kind = (tt == LUA_TSTRING)
                                  ? lua_tolstring(L, -1)
                                  : lua_typenamei(L, lua_type(L, idx))->view();
      if (lua_islightcfunction(L, idx))
      {
        std::string fa = lua_funcaddressi(L, idx);
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushlstring(L, fa, 0, fa.size());
        lua_pushfstring(L, "%s: %s");
      }
      else
      {
        lua_pushlstring(L, kind, 0, kind.size());
        lua_pushvalue(L, idx);
        lua_pushfstring(L, HostString(lit<"%s: %p">()->view()));
      }
      if (tt != LUA_TNIL)
        lua_remove(L, -2); /* remove '__name' */
      break;
    }
    }
  }
  return lua_toistring(L, -1);
}

LUALIB_API HostString luaL_tolstring(lua_State* L, int idx)
{
  return HostString(luaL_toistring(L, idx)->view());
}

/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
LUALIB_API void luaL_setfuncs(lua_State* L, const luaL_Reg* l, int nup)
{
  luaL_checkstack(L, nup, HostString(lit<"too many upvalues">()->view()));
  for (; l->name.data() != nullptr; l++)
  {                      /* fill the table with given functions */
    if (l->func == nullptr) /* placeholder? */
      lua_pushboolean(L, 0);
    else
    {
      int i;
      for (i = 0; i < nup; i++) /* copy upvalues to the top */
        lua_pushvalue(L, -nup);
      lua_pushcclosure(L, l->func, nup); /* closure with those upvalues */
    }
    lua_setfield(L, -(nup + 2), HostString{l->name});
  }
  lua_pop(L, nup); /* remove upvalues */
}

/*
** ensure that stack[idx][fname] has a table and push that table
** into the stack
*/
LUALIB_API int luaL_getsubtable(lua_State* L, int idx, const HostString& fname)
{
  if (lua_getfield(L, idx, fname) == LUA_TTABLE)
    return 1; /* table already there */
  else
  {
    lua_pop(L, 1); /* remove previous result */
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1);        /* copy to be left at top */
    lua_setfield(L, idx, fname); /* assign new table to field */
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
  luaL_getsubtable(L, LUA_REGISTRYINDEX, HostString(LUA_LOADED_TABLE->view()));
  lua_getfield(L, -1, modname); /* LOADED[modname] */
  if (!lua_toboolean(L, -1))
  {                /* package not already loaded? */
    lua_pop(L, 1); /* remove field */
    lua_pushcfunction(L, openf);
    lua_pushstring(L, modname);   /* argument to open function */
    lua_call(L, 1, 1);            /* call 'openf' to open module */
    lua_pushvalue(L, -1);         /* make copy of module (call result) */
    lua_setfield(L, -3, modname); /* LOADED[modname] = module */
  }
  lua_remove(L, -2); /* remove LOADED table */
  if (glb)
  {
    lua_pushvalue(L, -1);      /* copy of module */
    lua_setglobal(L, modname); /* _G[modname] = module */
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

LUALIB_API void luaL_addgsub(luaL_Buffer* b, const HostString& s,
                             const HostString& p, const HostString& r)
{
  luaL_addgsubi(b, s.view(), p.view(), r.view());
}

std::string_view luaL_gsubi(lua_State* L, std::string_view s,
                            std::string_view p, std::string_view r)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addgsubi(&b, s, p, r);
  luaL_pushresult(&b);
  return lua_tolstring(L, -1);
}

LUALIB_API HostString luaL_gsub(lua_State* L, const HostString& s,
                                const HostString& p, const HostString& r)
{
  return HostString(luaL_gsubi(L, s.view(), p.view(), r.view()));
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
  const char* msg = (lua_type(L, -1) == LUA_TSTRING)
                        ? lua_toistring(L, -1)->c_str()
                        : "error object is not a string";
  fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n"_os, msg);
  fflush(stderr);
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
  std::string_view m = message.view();
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
  std::string msg{message.view()};
  fprintf(stderr, "%s", msg.c_str());
  fflush(stderr);
  if (tocont)                          /* not the last part? */
    lua_setwarnf(L, warnfcont, L);     /* to be continued */
  else
  {                                /* last part */
    fprintf(stderr, "%s", "\n");   /* finish message with end-of-line */
    fflush(stderr);
    lua_setwarnf(L, warnfon, L);   /* next call is a new message */
  }
}

static void warnfon(void* ud, const HostString& message, int tocont)
{
  if (checkcontrol((lua_State*) ud, message, tocont)) /* control message? */
    return;                                           /* nothing else to be done */
  fprintf(stderr, "%s", "Lua warning: "); /* start a new warning */
  fflush(stderr);
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
    luaL_error(L, HostString(lit<"core and library have incompatible numeric types">()->view()));
  else if (v != ver)
  {
    lua_pushnumber(L, ver);
    lua_pushnumber(L, v);
    luaL_error(L, HostString(lit<"version mismatch: app. needs %f, Lua core provides %f">()->view()));
  }
}


LUALIB_API void luaL_checkversion(lua_State* L)
{
  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

LUALIB_API int luaL_loadfile(lua_State* L, const std::optional<HostString>& filename)
{
  return luaL_loadfilex(L, filename, std::nullopt);
}

LUALIB_API void luaL_argcheck(lua_State* L, bool cond, int arg, const HostString& extramsg)
{
  if (luai_unlikely(!cond))
    luaL_argerror(L, arg, extramsg);
}

LUALIB_API void luaL_argexpected(lua_State* L, bool cond, int arg, const HostString& tname)
{
  if (luai_unlikely(!cond))
    luaL_typeerror(L, arg, tname);
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
  return lua_getfield(L, LUA_REGISTRYINDEX, tname);
}

LUALIB_API int luaL_loadbuffer(lua_State* L, std::string_view buff,
                               const std::optional<HostString>& name)
{
  return luaL_loadbufferx(L, buff, name, std::nullopt);
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
