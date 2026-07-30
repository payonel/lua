/*
** $Id: lauxlib.c $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/


#include "lprefix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
** This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#include "lua.h"

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
        lua_pushliteral(L, "."); /* place '.' between the two names */
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
static int pushglobalfuncname(lua_State* L, lua_Debug* ar)
{
  int top = lua_gettop(L);
  lua_getinfo(L, "f", ar); /* push function */
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  luaL_checkstack(L, 6, "not enough stack"); /* slots for 'findfield' */
  if (findfield(L, top + 1, 2))
  {
    HostString name = lua_tostring(L, -1);
    if (name.compare(0, 3, "_G.") == 0)
    {                                       /* name start with '_G.'? */
      lua_pushstring(L, name.substr(3));    /* push name without prefix */
      lua_remove(L, -2);                    /* remove original name */
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

static void pushfuncname(lua_State* L, lua_Debug* ar)
{
  if (!ar->namewhat.empty())
  { /* is there a name from code? */
    lua_pushstring(L, ar->namewhat);
    lua_pushstring(L, ar->name);
    lua_pushfstring(L, "%s '%s'"); /* use it */
  }
  else if (ar->what[0] == 'm')                             /* main? */
    lua_pushliteral(L, "main chunk");
  else if (pushglobalfuncname(L, ar))
  {                       /* try a global name */
    lua_pushvalue(L, -1); /* the name is the argument */
    lua_pushfstring(L, "function '%s'");
    lua_remove(L, -2); /* remove name */
  }
  else if (ar->what[0] != 'C')
  { /* for Lua functions, use <file:line> */
    lua_pushstring(L, ar->short_src);
    lua_pushinteger(L, ar->linedefined);
    lua_pushfstring(L, "function <%s:%d>");
  }
  else /* nothing left... */
    lua_pushliteral(L, "?");
}

static int lastlevel(lua_State* L)
{
  lua_Debug ar;
  int li = 1, le = 1;
  /* find an upper bound */
  while (lua_getstack(L, le, &ar))
  {
    li = le;
    le *= 2;
  }
  /* do a binary search */
  while (li < le)
  {
    int m = (li + le) / 2;
    if (lua_getstack(L, m, &ar))
      li = m + 1;
    else
      le = m;
  }
  return le - 1;
}

LUALIB_API void luaL_traceback(lua_State* L, lua_State* L1,
                               const char* msg, int level)
{
  luaL_Buffer b;
  lua_Debug ar;
  int last = lastlevel(L1);
  int limit2show = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  luaL_buffinit(L, &b);
  if (msg)
  {
    luaL_addstring(&b, msg);
    luaL_addchar(&b, '\n');
  }
  luaL_addstring(&b, "stack traceback:");
  while (lua_getstack(L1, level++, &ar))
  {
    if (limit2show-- == 0)
    {                                     /* too many levels? */
      int n = last - level - LEVELS2 + 1; /* number of levels to skip */
      lua_pushinteger(L, n);
      lua_pushfstring(L, "\n\t...\t(skipping %d levels)");
      luaL_addvalue(&b); /* add warning about skip */
      level += n;        /* and skip to last levels */
    }
    else
    {
      lua_getinfo(L1, "Slnt", &ar);
      if (ar.currentline <= 0)
      {
        lua_pushstring(L, ar.short_src);
        lua_pushfstring(L, "\n\t%s: in ");
      }
      else
      {
        lua_pushstring(L, ar.short_src);
        lua_pushinteger(L, ar.currentline);
        lua_pushfstring(L, "\n\t%s:%d: in ");
      }
      luaL_addvalue(&b);
      pushfuncname(L, &ar);
      luaL_addvalue(&b);
      if (ar.istailcall)
        luaL_addstring(&b, "\n\t(...tail calls...)");
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
  lua_Debug ar;
  const char* argword;
  if (!lua_getstack(L, 0, &ar))
  { /* no stack frame? */
    lua_pushinteger(L, arg);
    lua_pushstring(L, extramsg);
    return luaL_error(L, "bad argument #%d (%s)");
  }
  lua_getinfo(L, "nt", &ar);
  if (arg <= ar.extraargs) /* error in an extra argument? */
    argword = "extra argument";
  else
  {
    arg -= ar.extraargs; /* do not count extra arguments */
    if (ar.namewhat == "method")
    {               /* colon syntax? */
      arg--;        /* do not count (extra) self argument */
      if (arg == 0)
      { /* error in self argument? */
        lua_pushstring(L, ar.name);
        lua_pushstring(L, extramsg);
        return luaL_error(L, "calling '%s' on bad self (%s)");
      }
      /* else go through; error in a regular argument */
    }
    argword = "argument";
  }
  if (ar.name.empty())
    ar.name = (pushglobalfuncname(L, &ar)) ? lua_tostring(L, -1) : "?";
  lua_pushstring(L, argword);
  lua_pushinteger(L, arg);
  lua_pushstring(L, ar.name);
  lua_pushstring(L, extramsg);
  return luaL_error(L, "bad %s #%d to '%s' (%s)");
}

LUALIB_API int luaL_typeerror(lua_State* L, int arg, const HostString& tname)
{
  HostString typearg; /* name for the type of the actual argument */
  if (luaL_getmetafield(L, arg, "__name") == LUA_TSTRING)
    typearg = lua_tostring(L, -1); /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = "light userdata"; /* special name for messages */
  else
    typearg = luaL_typename(L, arg); /* standard name */
  lua_pushstring(L, tname);
  lua_pushstring(L, typearg);
  const InternalString* msg = lua_pushfstring(L, "%s expected, got %s");
  return luaL_argerror(L, arg, HostString(msg->bytes(), msg->size()));
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
  lua_Debug ar;
  if (lua_getstack(L, level, &ar))
  {                            /* check function at level */
    lua_getinfo(L, "Sl", &ar); /* get info about it */
    if (ar.currentline > 0)
    { /* is there info? */
      lua_pushstring(L, ar.short_src);
      lua_pushinteger(L, ar.currentline);
      lua_pushfstring(L, "%s:%d: ");
      return;
    }
  }
  lua_pushfstring(L, ""); /* else, no information available... */
}

/*
** The error message is formatted first (consuming its stack
** arguments, see 'lua_pushfstring'); the location is then pushed and
** rotated before the message for the concatenation.
*/
LUALIB_API int luaL_error(lua_State* L, const char* fmt)
{
  lua_pushfstring(L, fmt); /* format message, consuming its arguments */
  luaL_where(L, 1);
  lua_rotate(L, -2, 1); /* move location before message */
  lua_concat(L, 2);
  return lua_error(L);
}

LUALIB_API int luaL_fileresult(lua_State* L, int stat, const char* fname)
{
  int en = errno; /* calls to Lua API may change this value */
  if (stat)
  {
    lua_pushboolean(L, 1);
    return 1;
  }
  else
  {
    const char* msg;
    luaL_pushfail(L);
    msg = (en != 0) ? strerror(en) : "(no extra info)";
    if (fname)
    {
      lua_pushstring(L, fname);
      lua_pushstring(L, msg);
      lua_pushfstring(L, "%s: %s");
    }
    else
      lua_pushstring(L, msg);
    lua_pushinteger(L, en);
    return 3;
  }
}

#if !defined(l_inspectstat) /* { */

#include <sys/wait.h>

/*
** use appropriate macros to interpret 'pclose' return status
*/

#endif /* } */

LUALIB_API int luaL_execresult(lua_State* L, int stat)
{
  if (stat != 0 && errno != 0) /* error with an 'errno'? */
    return luaL_fileresult(L, 0, nullptr);
  else
  {
    const char* what = "exit";     /* type of termination */
    /* interpret result */
    if (WIFEXITED(stat))
    {
      stat = WEXITSTATUS(stat);
    }
    else if (WIFSIGNALED(stat))
    {
      stat = WTERMSIG(stat);
      what = "signal";
    }
    if (*what == 'e' && stat == 0) /* successful termination? */
      lua_pushboolean(L, 1);
    else
      luaL_pushfail(L);
    lua_pushstring(L, what);
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
  lua_createtable(L, 0, 4); /* create metatable (room for __name + methods) */
  lua_pushstring(L, tname);
  lua_setfield(L, -2, "__name"); /* metatable.__name = tname */
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
                                const InternalString* def,
                                std::span<const InternalString* const> lst)
{
  /* engine-native: the argument is an engine string; options are lit<>
     constants — no host text is involved */
  const InternalString* name =
      (def != nullptr) ? luaL_optistring(L, arg, def) : luaL_checkistring(L, arg);
  for (size_t i = 0; i < lst.size(); i++)
    if (name->view() == lst[i]->view())
      return ((int)(i));
  lua_pushistringref(L, name);
  const InternalString* msg = lua_pushfstring(L, "invalid option '%s'");
  return luaL_argerror(L, arg, HostString{msg->view()});
}

/*
** Ensures the stack has at least 'space' extra slots, raising an error
** if it cannot fulfill the request. (The error handling needs a few
** extra slots to format the error message. In case of an error without
** this extra space, Lua will generate the same 'stack overflow' error,
** but without 'msg'.)
*/
LUALIB_API void luaL_checkstack(lua_State* L, int space, const HostString& msg)
{
  if (l_unlikely(!lua_checkstack(L, space)))
  {
    /* the argument push needs a free slot; on a completely full stack,
       fall back to the plain message */
    if (!msg.empty() && lua_checkstack(L, 1))
    {
      lua_pushstring(L, msg);
      luaL_error(L, "stack overflow (%s)");
    }
    else
      luaL_error(L, "stack overflow");
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
    luaL_argerror(L, arg, "value expected");
}

LUALIB_API std::string_view luaL_checklstring(lua_State* L, int arg)
{
  std::string_view s = lua_tolstring(L, arg);
  if (l_unlikely(s.data() == nullptr))
    tag_error(L, arg, LUA_TSTRING);
  return s;
}

LUALIB_API std::string_view luaL_optlstring(lua_State* L, int arg,
                                            std::string_view def)
{
  if (lua_isnoneornil(L, arg))
    return def;
  else
    return luaL_checklstring(L, arg);
}

LUALIB_API const InternalString* luaL_checkistring(lua_State* L, int arg)
{
  const InternalString* s = lua_toistring(L, arg);
  if (l_unlikely(!s))
    tag_error(L, arg, LUA_TSTRING);
  return s;
}

LUALIB_API const InternalString* luaL_optistring(lua_State* L, int arg,
                                                 const InternalString* def)
{
  if (lua_isnoneornil(L, arg))
    return def;
  else
    return luaL_checkistring(L, arg);
}

LUALIB_API lua_Number luaL_checknumber(lua_State* L, int arg)
{
  int isnum;
  lua_Number d = lua_tonumberx(L, arg, &isnum);
  if (l_unlikely(!isnum))
    tag_error(L, arg, LUA_TNUMBER);
  return d;
}

LUALIB_API lua_Number luaL_optnumber(lua_State* L, int arg, lua_Number def)
{
  return (lua_isnoneornil(L, (arg)) ? (def) : luaL_checknumber(L, (arg)));
}

static void interror(lua_State* L, int arg)
{
  if (lua_isnumber(L, arg))
    luaL_argerror(L, arg, "number has no integer representation");
  else
    tag_error(L, arg, LUA_TNUMBER);
}

LUALIB_API lua_Integer luaL_checkinteger(lua_State* L, int arg)
{
  int isnum;
  lua_Integer d = lua_tointegerx(L, arg, &isnum);
  if (l_unlikely(!isnum))
  {
    interror(L, arg);
  }
  return d;
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

/*
** The buffer accumulates into an engine ByteBuilder embedded in the
** luaL_Buffer itself. Growth is ACCOUNTED: capacity is reserved with
** the lua_State BEFORE it is allocated (reserve-before-growth), so
** script-driven accumulation (io.read, string.rep, table.concat) is
** subject to the state's memory limit. 'B->reserved' tracks the
** capacity charged so far; the destructor releases it on error
** unwind, and 'pushresult' releases it after handing the bytes to
** the result string by adoption.
*/

luaL_Buffer::~luaL_Buffer()
{
  if (reserved != 0 && L != nullptr)
    lua_releasebytes(L, reserved);
}

/*
** Compute new buffer capacity, enough to accommodate 'sz' extra
** bytes on top of the current content. (Same growth policy as the
** old box buffer: at least 1.5x.)
*/
static size_t newbuffsize(luaL_Buffer* B, size_t sz)
{
  size_t newsize = B->bldr.capacity();
  size_t len = B->bldr.size();
  if (l_unlikely(sz >= MAX_SIZE - len))
    return ((size_t)(luaL_error(B->L, "resulting string too large")));
  if (newsize <= MAX_SIZE / 3 * 2) /* no overflow? */
    newsize += (newsize >> 1);     /* new size *= 1.5 */
  if (newsize < len + sz)          /* not big enough? */
    newsize = len + sz;
  if (newsize < LUAL_BUFFERSIZE)   /* keep a minimum useful capacity */
    newsize = LUAL_BUFFERSIZE;
  return newsize;
}

/*
** Ensures a writable area with at least 'sz' bytes past the current
** content, growing capacity through the accounted boundary if needed.
** Returns the write offset into the buffer's area.
*/
static size_t prepbuff(luaL_Buffer* B, size_t sz)
{
  if (B->bldr.size() + sz > B->bldr.capacity())
  {
    size_t newsize = newbuffsize(B, sz);
    /* reserve BEFORE allocating; the memory limit can veto */
    assert(newsize >= B->reserved);
    if (l_unlikely(!lua_reservebytes(B->L, newsize - B->reserved)))
    {
      lua_pushliteral(B->L, "not enough memory");
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
    /* Java: System.arraycopy(s, off, buf, at, l) */
    memcpy(B->bldr.area().data() + at, s.data() + off, l * sizeof(char));
    luaL_addsize(B, l);
  }
}

LUALIB_API void luaL_addstring(luaL_Buffer* B, const HostString& s)
{
  luaL_addlstring(B, s, 0, s.size());
}

LUALIB_API void luaL_pushresult(luaL_Buffer* B)
{
  lua_State* L = B->L;
  size_t len = B->bldr.size();
  /* hand our capacity charge back BEFORE adoption re-reserves the
     size: these bytes were already admitted for this buffer, so the
     handoff window is bounded — whereas holding both charges would
     double-count and could trip the memory limit on the very push
     that releases the buffer */
  size_t reserved = B->reserved;
  B->reserved = 0;
  if (reserved != 0)
    lua_releasebytes(L, reserved);
  /* adoption: the result string takes the built bytes (no copy);
     admission does a checked reserve of the size */
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
  /* Java: System.arraycopy(s, 0, buf, off, len) */
  memcpy(B->bldr.area().data() + off, s.data(), s.size() * sizeof(char));
  luaL_addsize(B, s.size());
  lua_pop(L, 1); /* pop string */
}

LUALIB_API void luaL_buffinit(lua_State* L, luaL_Buffer* B)
{
  B->L = L;
  B->bldr.reset();
  /* (no stack placeholder: the builder lives in the luaL_Buffer) */
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

static const char* getF(lua_State*, void* ud, size_t* size)
{
  LoadF* lf = (LoadF*) ud;
  if (lf->n > 0)
  {                /* are there pre-read characters to be read? */
    *size = lf->n; /* return them (chars already in buffer) */
    lf->n = 0;     /* no more pre-read characters */
  }
  else
  { /* read a block from file */
    /* 'fread' can return > 0 *and* set the EOF flag. If next call to
       'getF' called 'fread', it might still wait for user input.
       The next check avoids this problem. */
    if (feof(lf->f))
      return nullptr;
    *size = fread(lf->buff, 1, sizeof(lf->buff), lf->f); /* read block */
  }
  return lf->buff;
}

static int errfile(lua_State* L, const char* what, int fnameindex)
{
  int err = errno;
  HostString filename = lua_tostring(L, fnameindex).substr(1); /* skip '@'/'=' */
  if (err != 0)
  {
    lua_pushstring(L, what);
    lua_pushstring(L, filename);
    lua_pushstring(L, strerror(err));
    lua_pushfstring(L, "cannot %s %s: %s");
  }
  else
  {
    lua_pushstring(L, what);
    lua_pushstring(L, filename);
    lua_pushfstring(L, "cannot %s %s");
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

LUALIB_API int luaL_loadfilex(lua_State* L, const char* filename,
                              const InternalString* mode)
{
  LoadF lf;
  int status, readstatus;
  int c;
  int fnameindex = lua_gettop(L) + 1; /* index of filename on the stack */
  if (filename == nullptr)
  {
    lua_pushliteral(L, "=stdin");
    lf.f = stdin;
  }
  else
  {
    lua_pushstring(L, filename);
    lua_pushfstring(L, "@%s");
    errno = 0;
    lf.f = fopen(filename, "r");
    if (lf.f == nullptr)
      return errfile(L, "open", fnameindex);
  }
  lf.n = 0;
  if (skipcomment(lf.f, &c)) /* read initial portion */
    lf.buff[lf.n++] = '\n';  /* add newline to correct line numbers */
  if (c == LUA_SIGNATURE[0])
  {           /* binary file? */
    lf.n = 0; /* remove possible newline */
    if (filename)
    { /* "real" file? */
      errno = 0;
      lf.f = freopen(filename, "rb", lf.f); /* reopen in binary mode */
      if (lf.f == nullptr)
        return errfile(L, "reopen", fnameindex);
      skipcomment(lf.f, &c); /* re-read initial portion */
    }
  }
  if (c != EOF)
    lf.buff[lf.n++] = ((char)(c)); /* 'c' is the first character */
  /* chunkname was pushed above; borrow its engine string */
  status = lua_loadi(L, getF, &lf, lua_toistring(L, -1), mode);
  readstatus = ferror(lf.f);
  errno = 0; /* no useful error number until here */
  if (filename)
    fclose(lf.f); /* close file (even in case of errors) */
  if (readstatus)
  {
    lua_settop(L, fnameindex); /* ignore results from 'lua_load' */
    return errfile(L, "read", fnameindex);
  }
  lua_remove(L, fnameindex);
  return status;
}

struct LoadS
{
  const char* s;
  size_t size;
};

static const char* getS(lua_State*, void* ud, size_t* size)
{
  LoadS* ls = (LoadS*) ud;
  if (ls->size == 0)
    return nullptr;
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}

LUALIB_API int luaL_loadbufferi(lua_State* L, std::string_view buff,
                                const InternalString* name,
                                const InternalString* mode)
{
  LoadS ls;
  /* stream boundary: the reader holds the chunk as pointer+remaining
     (category 5; Java: byte[] + offset in the ZIO struct) */
  ls.s = buff.data();
  ls.size = buff.size();
  return lua_loadi(L, getS, &ls, name, mode);
}

LUALIB_API int luaL_loadbufferx(lua_State* L, std::string_view buff,
                                const HostString& name, const HostString& mode)
{
  /* boundary: convert once, delegate to the engine-string form */
  InternalString n{name};
  InternalString m{mode};
  return luaL_loadbufferi(L, buff, &n, mode.empty() ? nullptr : &m);
}

LUALIB_API int luaL_loadstring(lua_State* L, const char* s)
{
  return luaL_loadbufferx(L, std::string_view{s, strlen(s)}, s, "t");
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
  lua_Integer l;
  int isnum;
  lua_len(L, idx);
  l = lua_tointegerx(L, -1, &isnum);
  if (l_unlikely(!isnum))
    luaL_error(L, "object length is not an integer");
  lua_pop(L, 1); /* remove object */
  return l;
}

LUALIB_API std::string_view luaL_tolstring(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  if (luaL_callmeta(L, idx, "__tostring"))
  { /* metafield? */
    if (!lua_isstring(L, -1))
      luaL_error(L, "'__tostring' must return a string");
  }
  else
  {
    switch (lua_type(L, idx))
    {
    case LUA_TNUMBER:
    {
      char buff[LUA_N2SBUFFSZ];
      lua_numbertocstring(L, idx, buff);
      lua_pushstring(L, buff);
      break;
    }
    case LUA_TSTRING:
      lua_pushvalue(L, idx);
      break;
    case LUA_TBOOLEAN:
      lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
      break;
    case LUA_TNIL:
      lua_pushliteral(L, "nil");
      break;
    default:
    {
      int tt = luaL_getmetafield(L, idx, "__name"); /* try name */
      HostString kind = (tt == LUA_TSTRING) ? lua_tostring(L, -1) : luaL_typename(L, idx);
      if (lua_islightcfunction(L, idx))
      {
        /* light C function — no GC object, use string representation */
        auto fa = lua_funcaddress(L, idx);
        lua_pushstring(L, kind);
        lua_pushstring(L, fa.c_str());
        lua_pushfstring(L, "%s: %s");
      }
      else
      {
        lua_pushstring(L, kind);
        lua_pushvalue(L, idx);
        lua_pushfstring(L, "%s: %p");
      }
      if (tt != LUA_TNIL)
        lua_remove(L, -2); /* remove '__name' */
      break;
    }
    }
  }
  return lua_tolstring(L, -1);
}

LUALIB_API const InternalString* luaL_toistring(lua_State* L, int idx)
{
  luaL_tolstring(L, idx);      /* push the conversion */
  return lua_toistring(L, -1); /* borrowed from the pushed value */
}

/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
LUALIB_API void luaL_setfuncs(lua_State* L, const luaL_Reg* l, int nup)
{
  luaL_checkstack(L, nup, "too many upvalues");
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
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
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

LUALIB_API void luaL_addgsub(luaL_Buffer* b, const HostString& s,
                             const HostString& p, const HostString& r)
{
  size_t pos = 0;
  size_t wild;
  while ((wild = s.find(p, pos)) != HostString::npos)
  {
    luaL_addlstring(b, s, pos, wild - pos); /* push prefix */
    luaL_addstring(b, r);      /* push replacement in place of pattern */
    pos = wild + p.size();     /* continue after 'p' */
  }
  luaL_addlstring(b, s, pos, s.size() - pos); /* push last suffix */
}

LUALIB_API const InternalString* luaL_gsub(lua_State* L, const HostString& s,
                                           const HostString& p, const HostString& r)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addgsub(&b, s, p, r);
  luaL_pushresult(&b);
  return lua_toistring(L, -1); /* borrowed from the pushed result */
}

void* luaL_alloc(void*, void* ptr, size_t, size_t nsize)
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
  HostString msg = (lua_type(L, -1) == LUA_TSTRING)
                       ? lua_tostring(L, -1)
                       : HostString("error object is not a string");
  lua_writestringerror("PANIC: unprotected error in call to Lua API (%s)\n",
                       msg.c_str());
  return 0; /* return to Lua to abort */
}

/*
** Warning functions:
** warnfoff: warning system is off
** warnfon: ready to start a new message
** warnfcont: previous message is to be continued
*/
static void warnfoff(void* ud, const char* message, int tocont);
static void warnfon(void* ud, const char* message, int tocont);
static void warnfcont(void* ud, const char* message, int tocont);

/*
** Check whether message is a control message. If so, execute the
** control or ignore it if unknown.
*/
static int checkcontrol(lua_State* L, const char* message, int tocont)
{
  if (tocont || *(message++) != '@') /* not a control message? */
    return 0;
  else
  {
    if (strcmp(message, "off") == 0)
      lua_setwarnf(L, warnfoff, L); /* turn warnings off */
    else if (strcmp(message, "on") == 0)
      lua_setwarnf(L, warnfon, L); /* turn warnings on */
    return 1;                      /* it was a control message */
  }
}

static void warnfoff(void* ud, const char* message, int tocont)
{
  checkcontrol((lua_State*) ud, message, tocont);
}

/*
** Writes the message and handle 'tocont', finishing the message
** if needed and setting the next warn function.
*/
static void warnfcont(void* ud, const char* message, int tocont)
{
  lua_State* L = (lua_State*) ud;
  lua_writestringerror("%s", message); /* write message */
  if (tocont)                          /* not the last part? */
    lua_setwarnf(L, warnfcont, L);     /* to be continued */
  else
  {                                   /* last part */
    lua_writestringerror("%s", "\n"); /* finish message with end-of-line */
    lua_setwarnf(L, warnfon, L);      /* next call is a new message */
  }
}

static void warnfon(void* ud, const char* message, int tocont)
{
  if (checkcontrol((lua_State*) ud, message, tocont)) /* control message? */
    return;                                           /* nothing else to be done */
  lua_writestringerror("%s", "Lua warning: ");        /* start a new warning */
  warnfcont(ud, message, tocont);                     /* finish processing */
}

/*
** A function to compute an unsigned int with some level of
** randomness. Rely on Address Space Layout Randomization (if present)
** and the current time.
*/
#if !defined(luai_makeseed)

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

#endif

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
    luaL_error(L, "core and library have incompatible numeric types");
  else if (v != ver)
  {
    lua_pushnumber(L, ver);
    lua_pushnumber(L, v);
    luaL_error(L, "version mismatch: app. needs %f, Lua core provides %f");
  }
}

/*
** {======================================================================
** Public auxlib veneer: out-of-line wrappers (upstream macros, F8).
** All cold paths (ryanbean decision, 2026-07-25).
** =======================================================================
*/

LUALIB_API void luaL_checkversion(lua_State* L)
{
  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

LUALIB_API int luaL_loadfile(lua_State* L, const char* filename)
{
  return luaL_loadfilex(L, filename, nullptr); /* default mode */
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
  return HostString(luaL_checklstring(L, arg)); /* boundary copy */
}

LUALIB_API HostString luaL_optstring(lua_State* L, int arg, const HostString& d)
{
  if (lua_isnoneornil(L, arg))
    return d;
  return luaL_checkstring(L, arg);
}

LUALIB_API HostString luaL_typename(lua_State* L, int idx)
{
  return lua_typename(L, lua_type(L, idx));
}

LUALIB_API int luaL_dofile(lua_State* L, const char* filename)
{
  int status = luaL_loadfile(L, filename);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return status;
}

LUALIB_API int luaL_dostring(lua_State* L, const char* str)
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

LUALIB_API int luaL_loadbuffer(lua_State* L, const char* buff, size_t sz, const HostString& name)
{
  return luaL_loadbufferx(L, std::string_view{buff, sz}, name, HostString{});
}

LUALIB_API void luaL_pushfail(lua_State* L)
{
#if defined(LUA_FAILISFALSE)
  lua_pushboolean(L, 0);
#else
  lua_pushnil(L);
#endif
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
