/*
** $Id: lauxlib.c $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

/*
** This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "util_cast.h"
#include "lstate.h"
#include "lmem.h"
#include "literals.h"

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
static bool findfield(lua_State* L, int objidx, int level)
{
  if (level == 0 || !lua_istable(L, -1))
    return false; /* not found */
  lua_pushnil(L); /* start 'next' loop */
  while (lua_next(L, -2))
  { /* for each pair in table */
    if (lua_type(L, -2) == LUA_TSTRING)
    { /* ignore non-string keys */
      if (lua_rawequal(L, objidx, -1))
      {                /* found object? */
        lua_pop(L, 1); /* remove value (but keep name) */
        return true;
      }
      else if (findfield(L, objidx, level - 1))
      { /* try recursively */
        /* stack: lib_name, lib_table, field_name (top) */
        lua_pushistringref(L, lt::dot); /* place '.' between the two names */
        lua_replace(L, -3);      /* (in the slot occupied by table) */
        lua_concat(L, 3);        /* lib_name.field_name */
        return true;
      }
    }
    lua_pop(L, 1); /* remove value */
  }
  return false; /* not found */
}

/*
** Search for a name for a function in all loaded modules
*/
static bool pushglobalfuncname(lua_State* L, lua_State* L1, InternalDebug* ar)
{
  int top = lua_gettop(L);
  lua_pushframefunction(L, L1, *ar); /* (the frame is of 'L1': its stack has the function) */
  lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  luaL_checkstacki(L, 6, lt::not_enough_stack); /* slots for 'findfield' */
  if (findfield(L, top + 1, 2))
  {
    byte_view name = *lua_tolstring(L, -1);
    if (name.starts_with(lt::_G_dot))
    {                                                    /* name start with '_G.'? */
      lua_pushlstring(L, name, 3, name.size() - 3);      /* push name without prefix */
      lua_remove(L, -2);                                 /* remove original name */
    }
    lua_copy(L, -1, top + 1); /* copy name to proper place */
    lua_settop(L, top + 1);   /* remove table "loaded" and name copy */
    return true;
  }
  else
  {
    lua_settop(L, top); /* remove function and global table */
    return false;
  }
}

static void pushfuncname(lua_State* L, lua_State* L1, InternalDebug* ar)
{
  if (ar->namewhat.has_value())
  {
    lua_pushlstring(L, ar->namewhat.value(), 0, ar->namewhat->size());
    lua_pushlstring(L, ar->name.value(), 0, ar->name->size());
    lua_pushfstringi(L, lt::fmt_s_quoted_s); /* use it */
  }
  else if (ar->what[0] == 'm')                             /* main? */
    lua_pushistringref(L, lt::main_chunk);
  else if (pushglobalfuncname(L, L1, ar))
  {                       /* try a global name */
    lua_pushvalue(L, -1);
    lua_pushfstringi(L, lt::function_s);
    lua_remove(L, -2); /* remove name */
  }
  else if (ar->what[0] != 'C')
  { /* for Lua functions, use <file:line> */
    byte_view ss = ar->short_src_out.str_view();
    lua_pushlstring(L, ss, 0, ss.size());
    lua_pushinteger(L, ar->linedefined);
    lua_pushfstringi(L, lt::function_s_d);
  }
  else /* nothing left... */
    lua_pushistringref(L, lt::qmark);
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

void luaL_traceback(lua_State* L, lua_State* L1,
                               const std::optional<HostString>& msg, int level)
{
  luaL_tracebacki(L, L1, msg ? std::optional<byte_view>(msg->to_utf8()) : std::nullopt, level);
}

void luaL_tracebacki(lua_State* L, lua_State* L1, std::optional<byte_view> msg, int level)
{
  luaL_Buffer b;
  std::optional<InternalDebug> oar;
  int last = lastlevel(L1);
  int limit2show = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  luaL_buffinit(L, &b);
  if (msg.has_value())
  {
    luaL_addlstring(&b, *msg, 0, msg->size());
    luaL_addchar(&b, '\n');
  }
  luaL_addlstring(&b, lt::stack_traceback, 0, lt::stack_traceback.size());
  while ((oar = lua_getstacki(L1, level++)))
  {
    if (limit2show-- == 0)
    {                                     /* too many levels? */
      int n = last - level - LEVELS2 + 1; /* number of levels to skip */
      lua_pushinteger(L, n);
      lua_pushfstringi(L, lt::n_t_t_skipping_d_levels);
      luaL_addvalue(&b); /* add warning about skip */
      level += n;        /* and skip to last levels */
    }
    else
    {
      InternalDebug ar = *lua_getinfoi(L1, lt::Slnt_chars, oar.value());
      byte_view ss = ar.short_src_out.str_view();
      if (ar.currentline <= 0)
      {
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushfstringi(L, lt::n_t_s_in);
      }
      else
      {
        lua_pushlstring(L, ss, 0, ss.size());
        lua_pushinteger(L, ar.currentline);
        lua_pushfstringi(L, lt::n_t_s_d_in);
      }
      luaL_addvalue(&b);
      pushfuncname(L, L1, &ar);
      luaL_addvalue(&b);
      if (ar.istailcall)
        luaL_addlstring(&b, lt::n_t_tail_calls, 0, lt::n_t_tail_calls.size());
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

int luaL_argerror(lua_State* L, int arg, const HostString& extramsg)
{
  return luaL_argerrori(L, arg, extramsg.to_utf8());
}

int luaL_argerrori(lua_State* L, int arg, byte_view extramsg)
{
  byte_view argword = lt::argument;
  std::optional<InternalDebug> oar = lua_getstacki(L, 0);
  if (!oar)
  { /* no stack frame? */
    lua_pushinteger(L, arg);
    lua_pushlstring(L, extramsg, 0, extramsg.size());
    return luaL_errori(L, lt::bad_argument_d_s);
  }
  InternalDebug ar = *lua_getinfoi(L, lt::nt, oar.value());
  if (arg <= ar.extraargs) /* error in an extra argument? */
    argword = lt::extra_argument;
  else
  {
    arg -= ar.extraargs; /* do not count extra arguments */
    if (ar.namewhat == lt::method)
    {               /* colon syntax? */
      arg--;        /* do not count (extra) self argument */
      if (arg == 0)
      { /* error in self argument? */
        lua_pushlstring(L, ar.name.value(), 0, ar.name->size());
        lua_pushlstring(L, extramsg, 0, extramsg.size());
        return luaL_errori(L, lt::calling_s_on_bad_self_s);
      }
      /* else go through; error in a regular argument */
    }
  }
  if (!ar.name.has_value())
    ar.name = pushglobalfuncname(L, L, &ar) ? *lua_tolstring(L, -1) : lt::qmark;
  lua_pushistringref(L, argword);
  lua_pushinteger(L, arg);
  lua_pushlstring(L, ar.name.value(), 0, ar.name->size());
  lua_pushlstring(L, extramsg, 0, extramsg.size());
  return luaL_errori(L, lt::bad_s_d_to_s_s);
}

int luaL_typeerror(lua_State* L, int arg, const HostString& tname)
{
  return luaL_typeerrori(L, arg, tname.to_utf8());
}

int luaL_typeerrori(lua_State* L, int arg, byte_view tname)
{
  byte_view typearg; /* name for the type of the actual argument */
  if (luaL_getmetafieldi(L, arg, lt::mm_name) == LUA_TSTRING)
    typearg = lua_toistring(L, -1)->view(); /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = lt::light_userdata; /* special name for messages */
  else
    typearg = lua_typenamei(L, lua_type(L, arg)); /* standard name */
  lua_pushlstring(L, tname, 0, tname.size());
  lua_pushlstring(L, typearg, 0, typearg.size());
  const InternalString* msg = lua_pushfstringi(L, lt::s_expected_got_s);
  return luaL_argerrori(L, arg, msg->view());
}

static void tag_error(lua_State* L, int arg, int tag)
{
  luaL_typeerrori(L, arg, lua_typenamei(L, tag));
}

/*
** The use of 'lua_pushfstring' ensures this function does not
** need reserved stack space when called.
*/

void luaL_where(lua_State* L, int level)
{
  std::optional<InternalDebug> oar = lua_getstacki(L, level);
  if (oar)
  {                                                /* check function at level */
    InternalDebug ar = *lua_getinfoi(L, lt::Sl_chars, oar.value()); /* get info about it */
    if (ar.currentline > 0)
    { /* is there info? */
      byte_view ss = ar.short_src_out.str_view();
      lua_pushlstring(L, ss, 0, ss.size());
      lua_pushinteger(L, ar.currentline);
      lua_pushfstringi(L, lt::fmt_s_d_colon_sp);
      return;
    }
  }
  lua_pushfstringi(L, lt::empty_str); /* else, no information available... */
}

int luaL_error(lua_State* L, const HostString& fmt)
{
  return luaL_errori(L, fmt.to_utf8());
}

int luaL_errori(lua_State* L, byte_view fmt)
{
  lua_pushfstringi(L, fmt);
  luaL_where(L, 1);
  lua_rotate(L, -2, 1);
  lua_concat(L, 2);
  return lua_error(L);
}

int luaL_fileresulti(lua_State* L, bool stat, std::optional<byte_view> fname, int en)
{
  if (stat)
  {
    lua_pushboolean(L, true);
    return 1;
  }
  else
  {
    luaL_pushfail(L);
    byte_view errstr = (en != 0) ? os_strerror(en) : lt::no_extra_info;
    if (fname.has_value())
    {
      lua_pushlstring(L, fname.value(), 0, fname->size());
      lua_pushlstring(L, errstr, 0, errstr.size());
      lua_pushfstringi(L, lt::fmt_s_colon_s);
    }
    else
      lua_pushlstring(L, errstr, 0, errstr.size());
    lua_pushinteger(L, en);
    return 3;
  }
}

int luaL_fileresult(lua_State* L, bool stat, const std::optional<HostString>& fname)
{
  return luaL_fileresulti(L, stat, fname ? std::optional<byte_view>(fname->to_utf8()) : std::nullopt, errno);
}

int luaL_execresult(lua_State* L, int stat)
{
  return luaL_execresulti(L, stat, errno);
}

int luaL_execresulti(lua_State* L, int stat, int en)
{
  if (stat != 0 && en != 0) /* error with an 'errno'? */
    return luaL_fileresulti(L, false, {}, en);
  else
  {
    OsExecStatus es = os_decode_exec(stat);
    byte_view what = es.exited ? lt::exit_kw : lt::signal_kw;
    if (es.exited && es.code == 0) /* successful termination? */
      lua_pushboolean(L, true);
    else
      luaL_pushfail(L);
    lua_pushistringref(L, what);
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

bool luaL_newmetatable(lua_State* L, const HostString& tname)
{
  return luaL_newmetatablei(L, tname.to_utf8());
}

bool luaL_newmetatablei(lua_State* L, byte_view tname)
{
  if (luaL_getmetatablei(L, tname) != LUA_TNIL) /* name already in use? */
    return false;                              /* leave previous value on top, but return 0 */
  lua_pop(L, 1);
  lua_createtable(L, 0, 2);
  lua_pushlstring(L, tname, 0, tname.size());
  lua_setfieldi(L, -2, lt::mm_name); /* metatable.__name = tname */
  lua_pushvalue(L, -1);
  lua_setfieldi(L, LUA_REGISTRYINDEX, tname); /* registry.name = metatable */
  return true;
}

void luaL_setmetatable(lua_State* L, const HostString& tname)
{
  luaL_setmetatablei(L, tname.to_utf8());
}

void luaL_setmetatablei(lua_State* L, byte_view tname)
{
  luaL_getmetatablei(L, tname);
  lua_setmetatable(L, -2);
}

void* luaL_testudata(lua_State* L, int ud, const HostString& tname)
{
  return luaL_testudatai(L, ud, tname.to_utf8());
}

void* luaL_testudatai(lua_State* L, int ud, byte_view tname)
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

void* luaL_checkudata(lua_State* L, int ud, const HostString& tname)
{
  return luaL_checkudatai(L, ud, tname.to_utf8());
}

void* luaL_checkudatai(lua_State* L, int ud, byte_view tname)
{
  void* p = luaL_testudatai(L, ud, tname);
  luaL_argexpectedi(L, p != nullptr, ud, tname);
  return p;
}

lua_UserObject* luaL_testuserobject(lua_State* L, int ud, const HostString& tname)
{
  return luaL_testuserobjecti(L, ud, tname.to_utf8());
}

lua_UserObject* luaL_testuserobjecti(lua_State* L, int ud, byte_view tname)
{
  if (luaL_testudatai(L, ud, tname) == nullptr)
    return nullptr;
  return lua_touserobject(L, ud);
}

lua_UserObject* luaL_checkuserobject(lua_State* L, int ud, const HostString& tname)
{
  return luaL_checkuserobjecti(L, ud, tname.to_utf8());
}

lua_UserObject* luaL_checkuserobjecti(lua_State* L, int ud, byte_view tname)
{
  lua_UserObject* p = luaL_testuserobjecti(L, ud, tname);
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
                      std::optional<byte_view> def,
                      std::span<const byte_view> lst)
{
  byte_view name = def.has_value()
                              ? luaL_optlstring(L, arg, *def)
                              : luaL_checklstring(L, arg);
  for (size_t i = 0; i < lst.size(); i++)
    if (name == lst[i])
      return (int)i;
  lua_pushlstring(L, name, 0, name.size());
  const InternalString* msg = lua_pushfstringi(L, lt::invalid_option_s);
  return luaL_argerrori(L, arg, msg->view());
}

int luaL_checkoption(lua_State* L, int arg,
                                const std::optional<HostString>& def,
                                const std::span<const HostString>& lst)
{
  byte_view name = def
                              ? luaL_optlstring(L, arg, def->to_utf8())
                              : luaL_checklstring(L, arg);
  for (size_t i = 0; i < lst.size(); i++)
    if (name == lst[i].to_utf8())
      return (int)i;
  lua_pushlstring(L, name, 0, name.size());
  const InternalString* msg = lua_pushfstringi(L, lt::invalid_option_s);
  return luaL_argerrori(L, arg, msg->view());
}

/*
** Ensures the stack has at least 'space' extra slots, raising an error
** if it cannot fulfill the request. (The error handling needs a few
** extra slots to format the error message. In case of an error without
** this extra space, Lua will generate the same 'stack overflow' error,
** but without 'msg'.)
*/

void luaL_checkstack(lua_State* L, int space, const std::optional<HostString>& msg)
{
  luaL_checkstacki(L, space, msg ? std::optional<byte_view>(msg->to_utf8()) : std::nullopt);
}

void luaL_checkstacki(lua_State* L, int space, std::optional<byte_view> msg)
{
  if (l_unlikely(!lua_checkstack(L, space)))
  {
    if (msg.has_value() && lua_checkstack(L, 1))
    {
      lua_pushlstring(L, *msg, 0, msg->size());
      luaL_errori(L, lt::stack_overflow_s);
    }
    else
      luaL_errori(L, lt::stack_overflow);
  }
}

void luaL_checktype(lua_State* L, int arg, int t)
{
  if (l_unlikely(lua_type(L, arg) != t))
    tag_error(L, arg, t);
}

void luaL_checkany(lua_State* L, int arg)
{
  if (l_unlikely(lua_type(L, arg) == LUA_TNONE))
    luaL_argerrori(L, arg, lt::value_expected);
}

byte_view luaL_checklstring(lua_State* L, int arg)
{
  auto s = lua_tolstring(L, arg);
  if (l_unlikely(!s))
    tag_error(L, arg, LUA_TSTRING);
  return *s;
}

byte_view luaL_optlstring(
    lua_State* L, int arg, byte_view def)
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

const InternalString* luaL_optistring(lua_State* L, int arg, const InternalString* def)
{
  if (lua_isnoneornil(L, arg))
    return def;
  else
    return luaL_checkistring(L, arg);
}

lua_Number luaL_checknumber(lua_State* L, int arg)
{
  std::optional<lua_Number> d = lua_tonumberx(L, arg);
  if (l_unlikely(!d))
    tag_error(L, arg, LUA_TNUMBER);
  return *d;
}

lua_Number luaL_optnumber(lua_State* L, int arg, lua_Number def)
{
  return (lua_isnoneornil(L, arg) ? def : luaL_checknumber(L, arg));
}

static void interror(lua_State* L, int arg)
{
  if (lua_isnumber(L, arg))
    luaL_argerrori(L, arg, lt::number_has_no_integer_representation);
  else
    tag_error(L, arg, LUA_TNUMBER);
}

lua_Integer luaL_checkinteger(lua_State* L, int arg)
{
  std::optional<lua_Integer> d = lua_tointegerx(L, arg);
  if (l_unlikely(!d))
  {
    interror(L, arg);
  }
  return *d;
}

lua_Integer luaL_optinteger(lua_State* L, int arg,
                                       lua_Integer def)
{
  return (lua_isnoneornil(L, arg) ? def : luaL_checkinteger(L, arg));
}

/* }====================================================== */

/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

static void bb_growTo(luaL_Buffer* B, size_t cap)
{
  if (cap == B->bytes.size())
    return;
  assert(B->L != nullptr);
  global_State* g = B->L->l_G;
  byte_owner nb = luaM_alloc_char_span(g, nullptr, B->bytes, cap, false);
  if (l_unlikely(nb.data() == nullptr && cap > 0))
  {
    lua_pushistringref(B->L, lt::not_enough_memory);
    lua_error(B->L); /* raise a memory error */
  }
  B->bytes = std::move(nb);
}

static void bb_append(luaL_Buffer* B, byte_view b)
{
  if (!b.empty())
  {
    if (b.size() > B->bytes.size() - B->used)
      bb_growTo(B, B->used + b.size());
    B->bytes.copy_from(b, B->used);
    B->used += b.size();
  }
}

static void bb_dealloc(luaL_Buffer* B) noexcept
{
  if (!B->bytes.empty())
  {
    global_State* g = B->L->l_G;
    luaM_alloc_char_span(g, nullptr, B->bytes, 0, false);
    B->bytes = {};
  }
}

luaL_Buffer::~luaL_Buffer()
{
  bb_dealloc(this);
}

static size_t newbuffsize(luaL_Buffer* B, size_t sz)
{
  size_t newsize = B->bytes.size();
  size_t len = B->used;
  if (l_unlikely(sz >= MAX_SIZE - len))
    return luaL_errori(B->L, lt::resulting_string_too_large);
  if (newsize <= MAX_SIZE / 3 * 2) /* no overflow? */
    newsize += newsize >> 1;     /* new size *= 1.5 */
  if (newsize < len + sz)          /* not big enough? */
    newsize = len + sz;
  if (newsize < LUAL_BUFFERSIZE)
    newsize = LUAL_BUFFERSIZE;
  return newsize;
}

static size_t prepbuff(luaL_Buffer* B, size_t sz)
{
  if (sz > B->bytes.size() - B->used)
  {
    size_t newsize = newbuffsize(B, sz);
    bb_growTo(B, newsize);
  }

  assert(sz <= B->bytes.size() - B->used);
  return B->used;
}

size_t luaL_prepbuffsize(luaL_Buffer* B, size_t sz)
{
  return prepbuff(B, sz);
}

void luaL_addlstring(luaL_Buffer* B, byte_view s, size_t pos, size_t l)
{
  if (l > 0)
  {
    prepbuff(B, l);
    bb_append(B, s.substr(pos, l));
  }
}

void luaL_addstring(luaL_Buffer* B, const HostString& s)
{
  byte_view sv = s.to_utf8();
  luaL_addlstring(B, sv, 0, sv.size());
}

/* the buffer's bytes become the string's: they stop counting as the buffer's */
void luaL_pushresult(luaL_Buffer* B)
{
  lua_State* L = B->L;
  size_t len = B->used;
  byte_owner block = std::move(B->bytes);
  B->bytes = {};
  B->used = 0;
  if (!block.empty())
    luaM_alloc_char_span(L->l_G, nullptr, block, 0, false);
  lua_pushiblock(L, std::move(block), len);
}

void luaL_pushresultsize(luaL_Buffer* B, size_t sz)
{
  luaL_addsize(B, sz);
  luaL_pushresult(B);
}

void luaL_addvalue(luaL_Buffer* B)
{
  lua_State* L = B->L;
  byte_view s = *lua_tolstring(L, -1);
  prepbuff(B, s.size());
  bb_append(B, s);
  lua_pop(L, 1); /* pop string */
}

void luaL_Box::freed(lua_State* L)
{
  if (!bytes.empty())
    luaM_alloc_char_span(L->l_G, nullptr, bytes, 0, false); /* an abandoned buffer */
  bytes = {};
}

/* its state: the four integers, the bytes used, then the bytes */
int luaL_Box::persistints(lua_Integer* out) const
{
  for (int i = 0; i < 4; i++)
    out[i] = ints[i];
  out[4] = static_cast<lua_Integer>(used);
  return 5;
}

byte_view luaL_Box::persistbytes() const
{
  return bytes.view().substr(0, used);
}

static std::unique_ptr<lua_UserObject> makebox(lua_State* L, const lua_Integer* ints, int nints, byte_view bytes)
{
  if (nints != 5 || ints[4] < 0 || static_cast<size_t>(ints[4]) != bytes.size())
    return nullptr;
  auto box = std::make_unique<luaL_Box>();
  for (int i = 0; i < 4; i++)
    box->ints[i] = ints[i];
  if (!bytes.empty())
  {
    byte_owner nb = luaM_alloc_char_span(L->l_G, nullptr, {}, bytes.size(), false);
    if (nb.data() == nullptr)
    {
      lua_pushistringref(L, lt::not_enough_memory);
      lua_error(L); /* raise a memory error */
    }
    nb.copy_from(bytes, 0);
    box->bytes = std::move(nb);
    box->used = bytes.size();
  }
  return box;
}

struct BoxKind
{
  BoxKind() { lua_registerobjectkind("box", makebox); }
};
static BoxKind boxkind;

/* what a box is counted, besides the bytes it keeps */
constexpr size_t LUAL_BOX_SIZE = 72;

/* pushes a new box */
luaL_Box* luaL_newbox(lua_State* L)
{
  auto box = std::make_unique<luaL_Box>();
  luaL_Box* p = box.get();
  lua_newuserobject(L, std::move(box), LUAL_BOX_SIZE, 0);
  return p;
}

luaL_Box* luaL_tobox(lua_State* L, int idx)
{
  luaL_Box* box = dynamic_cast<luaL_Box*>(lua_touserobject(L, idx));
  assert(box != nullptr);
  return box;
}

/* moves the contents of 'B' into 'box' ('B' is left empty) */
void luaL_buffsave(luaL_Buffer* B, luaL_Box* box)
{
  assert(box->bytes.empty());
  box->bytes = std::move(B->bytes);
  box->used = B->used;
  B->bytes = {};
  B->used = 0;
}

/* moves the contents saved in 'box' back into 'B' */
void luaL_buffload(luaL_Buffer* B, luaL_Box* box)
{
  assert(B->bytes.empty());
  B->bytes = std::move(box->bytes);
  B->used = box->used;
  box->bytes = {};
  box->used = 0;
}

bool luaL_slicespent(lua_State* L, size_t bytes)
{
  return lua_spend(L, (int)(bytes / LUAL_SLICESTEP));
}

void luaL_slicepoint(lua_State* L, luaL_Buffer* B, int box, lua_KContext ctx, lua_KFunction k)
{
  if (lua_gettop(L) < box)
  {
    assert(lua_gettop(L) == box - 1);
    luaL_newbox(L);
  }
  luaL_Box* bx = luaL_tobox(L, box);
  luaL_buffsave(B, bx);
  lua_checkpointk(L, ctx, k);
  luaL_buffload(B, bx);
}

void luaL_buffinit(lua_State* L, luaL_Buffer* B)
{
  assert(B->bytes.empty() || L == B->L);
  B->L = L;
  B->used = 0;
}

size_t luaL_buffinitsize(lua_State* L, luaL_Buffer* B, size_t sz)
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

int luaL_ref(lua_State* L, int t)
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

void luaL_unref(lua_State* L, int t, int ref)
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

struct LoadF : lua_Reader
{
  int n;        /* number of pre-read characters */
  OsFile* f;         /* file being read */
  byte_owner buff = byte_owner(BUFSIZ); /* area for reading file */
  std::optional<byte_view> read(lua_State* L) override;
};

static std::optional<byte_view> getF(lua_State*, LoadF* lf)
{
  if (lf->n > 0)
  {                          /* are there pre-read characters to be read? */
    int len = lf->n;
    lf->n = 0;               /* no more pre-read characters */
    return lf->buff.view().substr(0, len); /* return them (chars already in buffer) */
  }

  /* read a block from file */
  /* 'fread' can return > 0 *and* set the EOF flag. If next call to
      'getF' called 'fread', it might still wait for user input.
      The next check avoids this problem. */
  if (os_feof(lf->f))
    return std::nullopt;
  size_t n = os_fread(lf->f, lf->buff, 0, lf->buff.size()); /* read block */
  if (n == 0)
    return std::nullopt;

  return lf->buff.view().substr(0, n);
}

std::optional<byte_view> LoadF::read(lua_State* L)
{
  return getF(L, this);
}

static int errfile(lua_State* L, byte_view what, int fnameindex, int err)
{
  byte_view filename = lua_tolstring(L, fnameindex)->substr(1);
  if (err != 0)
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    byte_view errmsg = os_strerror(err);
    lua_pushlstring(L, errmsg, 0, errmsg.size());
    lua_pushfstringi(L, lt::cannot_s_s_s);
  }
  else
  {
    lua_pushlstring(L, what, 0, what.size());
    lua_pushlstring(L, filename, 0, filename.size());
    lua_pushfstringi(L, lt::cannot_s_s);
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
struct FirstChar
{
  bool skipped;
  int c;
};

static FirstChar skipcomment(OsFile* f)
{
  int c = skipBOM(f);
  if (c == '#')
  { /* first line is a comment (Unix exec. file)? */
    do
    { /* skip first line */
      c = os_getbyte(f);
    } while (c != OS_EOF && c != '\n');
    return {true, os_getbyte(f)}; /* there was a comment */
  }
  else
    return {false, c}; /* no comment */
}

/*
** A file load in a C function with continuation 'k' and 'ctx' ('loadfile',
** 'dofile' and 'require'): with a budget, a file named is read whole first
** (see 'lua_loadkread'), and its parse has check points, where a freeze
** stops it; it goes on in 'k', which ends it with 'luaL_loadfileikresume'
** and 'base', the slot after the stack the load began with (its chunk name
** is there while it loads, its box above). It ends as 'luaL_loadfilei':
** the function, or the error message, at 'base'. (No continuation, or
** stdin: the file is read as the parse goes, in one step.)
*/
static lua_LoadSpec filespec(lua_State* L, std::optional<byte_view> mode, int base, lua_KContext ctx,
                             lua_KFunction k)
{
  lua_LoadSpec ls;
  ls.readeridx = LUA_LOADBOXED;
  ls.pieceidx = 0;
  ls.boxidx = base + 1;
  ls.name = lua_toistring(L, base)->view();
  ls.mode = mode;
  ls.ctx = ctx;
  ls.k = k;
  return ls;
}

static int loadfileend(lua_State* L, int status, int base)
{
  lua_replace(L, base); /* the function or the message */
  lua_settop(L, base);
  return status;
}

int luaL_loadfileikresume(lua_State* L, int status, std::optional<byte_view> mode, int base, lua_KContext ctx,
                          lua_KFunction k)
{
  return loadfileend(L, lua_loadkresume(L, status, filespec(L, mode, base, ctx, k)), base);
}

int luaL_loadfilei(lua_State* L, std::optional<byte_view> filename, std::optional<byte_view> mode)
{
  return luaL_loadfileik(L, filename, mode, 0, nullptr);
}

int luaL_loadfileik(lua_State* L, std::optional<byte_view> filename, std::optional<byte_view> mode, lua_KContext ctx,
                    lua_KFunction k)
{
  LoadF lf;
  int status;
  bool readstatus;
  int c;
  int fnameindex = lua_gettop(L) + 1; /* index of filename on the stack */
  const bool bNullname = !filename.has_value();
  if (bNullname)
  {
    lua_pushistringref(L, lt::stdin_1);
    lf.f = os_stdin();
  }
  else
  {
    lua_pushistringref(L, *filename);
    lua_pushfstringi(L, lt::fmt_at_s);
    OsOpenResult o = os_fopen(*filename, lt::r_mode);
    lf.f = o.f;
    if (lf.f == nullptr)
      return errfile(L, lt::open_kw, fnameindex, o.err);
  }
  lf.n = 0;
  FirstChar first = skipcomment(lf.f); /* read initial portion */
  c = first.c;
  if (first.skipped)
    lf.buff[lf.n++] = '\n';  /* add newline to correct line numbers */
  if (c == LUA_SIGNATURE[0])
  {           /* binary file? */
    lf.n = 0; /* remove possible newline */
    if (!bNullname)
    { /* "real" file? */
      int rerr = os_freopen(lf.f, *filename, lt::rb_mode); /* reopen in binary mode */
      if (rerr != 0)
        return errfile(L, lt::reopen, fnameindex, rerr);
      c = skipcomment(lf.f).c; /* re-read initial portion */
    }
  }
  if (c != OS_EOF)
    lf.buff[lf.n++] = (char)c; /* 'c' is the first character */
  if (k != nullptr && !bNullname)
  {
    lua_pushnil(L); /* its box */
    lua_LoadSpec ls = filespec(L, mode, fnameindex, ctx, k);
    bool boxed;
    try
    {
      boxed = lua_loadkread(L, ls, &lf);
    }
    catch (...)
    {
      os_fclose(lf.f);
      throw;
    }
    if (boxed)
    {
      readstatus = os_ferror(lf.f);
      os_fclose(lf.f);
      if (readstatus)
      {
        lua_settop(L, fnameindex);
        return errfile(L, lt::read_kw, fnameindex, 0);
      }
      return loadfileend(L, lua_loadk(L, ls), fnameindex);
    }
    lua_pop(L, 1); /* (no budget) */
  }
  status = lua_loadi(L, &lf, lua_toistring(L, -1)->view(), mode);
  readstatus = os_ferror(lf.f);
  if (!bNullname)
    os_fclose(lf.f); /* close file (even in case of errors) */
  if (readstatus)
  {
    lua_settop(L, fnameindex); /* ignore results from 'lua_load' */
    return errfile(L, lt::read_kw, fnameindex, 0);
  }
  lua_remove(L, fnameindex);
  return status;
}

int luaL_loadfilex(lua_State* L, const std::optional<HostString>& filename, const std::optional<HostString>& mode)
{
  std::optional<byte_view> fn = filename.has_value() ? std::optional<byte_view>(filename->to_utf8()) : std::nullopt;
  std::optional<byte_view> m = mode.has_value() ? std::optional<byte_view>(mode->to_utf8()) : std::nullopt;
  return luaL_loadfilei(L, fn, m);
}

struct LoadS : lua_Reader
{
  byte_view chunk {lt::empty_str};
  std::optional<byte_view> read(lua_State* L) override;
};

static std::optional<byte_view> getS(lua_State*, LoadS* ls)
{
  if (ls->chunk.empty())
    return std::nullopt;
  byte_view chunk = ls->chunk;
  ls->chunk = byte_view(lt::empty_str);
  return chunk;
}

std::optional<byte_view> LoadS::read(lua_State* L)
{
  return getS(L, this);
}

int luaL_loadbufferi(lua_State* L, byte_view buff, std::optional<byte_view> name, std::optional<byte_view> mode)
{
  LoadS ls;
  ls.chunk = buff;
  return lua_loadi(L, &ls, name, mode);
}

int luaL_loadbufferx(lua_State* L, byte_view buff, const std::optional<HostString>& name, const std::optional<HostString>& mode)
{
  return luaL_loadbufferi(L, buff,
                          name.has_value() ? std::optional<byte_view>(name->to_utf8()) : std::nullopt,
                          mode.has_value() ? std::optional<byte_view>(mode->to_utf8()) : std::nullopt);
}

int luaL_loadstring(lua_State* L, const HostString& s)
{
  return luaL_loadbufferi(L, s.to_utf8(), s.to_utf8(), lt::t_char);
}

/* }====================================================== */

int luaL_getmetafield(lua_State* L, int obj, const HostString& event)
{
  return luaL_getmetafieldi(L, obj, event.to_utf8());
}

int luaL_getmetafieldi(lua_State* L, int obj, byte_view event)
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

bool luaL_callmeta(lua_State* L, int obj, const HostString& event)
{
  return luaL_callmetai(L, obj, event.to_utf8());
}

bool luaL_callmetai(lua_State* L, int obj, byte_view event)
{
  obj = lua_absindex(L, obj);
  if (luaL_getmetafieldi(L, obj, event) == LUA_TNIL) /* no metafield? */
    return false;
  lua_pushvalue(L, obj);
  lua_call(L, 1, 1);
  return true;
}

lua_Integer luaL_len(lua_State* L, int idx)
{
  lua_len(L, idx);
  return luaL_lenresult(L);
}

/* 'luaL_len' with '__len' called with continuation 'k' */
lua_Integer luaL_lenk(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k)
{
  lua_lenk(L, idx, ctx, k);
  return luaL_lenresult(L);
}

/* the length 'lua_len' pushed, which must be an integer (popped) */
lua_Integer luaL_lenresult(lua_State* L)
{
  std::optional<lua_Integer> l = lua_tointegerx(L, -1);
  if (l_unlikely(!l))
    luaL_errori(L, lt::object_length_is_not_an_integer);
  lua_pop(L, 1); /* remove object */
  return *l;
}

/*
** Pushes the string form of the value at 'idx': the result of its
** '__tostring', called with continuation 'k' (unchecked: see
** 'luaL_tostringresult'), or the default form. Returns whether it called
** '__tostring'.
*/
bool luaL_toistringk(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k)
{
  idx = lua_absindex(L, idx);
  if (luaL_getmetafieldi(L, idx, lt::mm_tostring) != LUA_TNIL)
  { /* metafield? */
    lua_pushvalue(L, idx);
    lua_callk(L, 1, 1, ctx, k);
    return true;
  }
  luaL_tostringdefault(L, idx);
  return false;
}

/* pushes the string form of the value at 'idx' that does not use '__tostring' */
void luaL_tostringdefault(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  switch (lua_type(L, idx))
  {
  case LUA_TNUMBER:
  {
    byte_inline<LUA_N2SBUFFSZ> buff;
    byte_view n = *lua_numbertocstringi(L, idx, buff);
    lua_pushlstring(L, n, 0, n.size());
    break;
  }
  case LUA_TSTRING:
    lua_pushvalue(L, idx);
    break;
  case LUA_TBOOLEAN:
    lua_pushistringref(L, lua_toboolean(L, idx) ? lt::true_kw : lt::false_kw);
    break;
  case LUA_TNIL:
    lua_pushistringref(L, lt::nil);
    break;
  default:
  {
    int tt = luaL_getmetafieldi(L, idx, lt::mm_name); /* try name */
    byte_view kind = (tt == LUA_TSTRING)
                                ? *lua_tolstring(L, -1)
                                : lua_typenamei(L, lua_type(L, idx));
    if (lua_islightcfunction(L, idx))
    {
      InternalString fa = lua_funcaddressi(L, idx);
      lua_pushlstring(L, kind, 0, kind.size());
      lua_pushlstring(L, fa.view(), 0, fa.size());
      lua_pushfstringi(L, lt::fmt_s_colon_s);
    }
    else
    {
      lua_pushlstring(L, kind, 0, kind.size());
      lua_pushvalue(L, idx);
      lua_pushfstringi(L, lt::fmt_s_colon_p);
    }
    if (tt != LUA_TNIL)
      lua_remove(L, -2); /* remove '__name' */
    break;
  }
  }
}

/* the string 'luaL_toistringk' pushed, which must be a string */
const InternalString* luaL_tostringresult(lua_State* L)
{
  if (!lua_isstring(L, -1))
    luaL_errori(L, lt::tostring_must_return_a_string);
  return lua_toistring(L, -1);
}

const InternalString* luaL_toistring(lua_State* L, int idx)
{
  luaL_toistringk(L, idx, 0, nullptr);
  return luaL_tostringresult(L);
}

byte_view luaL_tolstring(lua_State* L, int idx)
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
  luaL_checkstacki(L, nup, lt::too_many_upvalues);
  for (const auto& reg : funcs)
  {
    if (reg.func == nullptr) /* placeholder? */
      lua_pushboolean(L, false);
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

void luaL_setfuncscfi(lua_State* L, std::span<const luaL_RegCFI> funcs)
{
  for (const auto& reg : funcs)
  {
    if (reg.cfi == nullptr)
      lua_pushboolean(L, false);
    else
      lua_pushcfuncinfo(L, reg.cfi);
    lua_setfieldi(L, -2, reg.name);
  }
}

void luaL_newlibcfi(lua_State* L, std::span<const luaL_RegCFI> funcs)
{
  luaL_checkversion(L);
  lua_createtable(L, 0, (int)funcs.size());
  luaL_setfuncscfi(L, funcs);
}

void luaL_setfuncs(lua_State* L, const std::span<const luaL_Reg>& funcs, int nup)
{
  luaL_checkstacki(L, nup, lt::too_many_upvalues);
  for (const auto& reg : funcs)
  {
    if (reg.func == nullptr) /* placeholder? */
      lua_pushboolean(L, false);
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

void luaL_newlibtable(lua_State* L, const std::span<const luaL_Reg>& funcs)
{
  lua_createtable(L, 0, (int)funcs.size());
}

void luaL_newlib(lua_State* L, const std::span<const luaL_Reg>& funcs)
{
  luaL_checkversion(L);
  luaL_newlibtable(L, funcs);
  luaL_setfuncs(L, funcs, 0);
}

/*
** ensure that stack[idx][fname] has a table and push that table
** into the stack
*/

bool luaL_getsubtable(lua_State* L, int idx, const HostString& fname)
{
  return luaL_getsubtablei(L, idx, fname.to_utf8());
}

bool luaL_getsubtablei(lua_State* L, int idx, byte_view fname)
{
  if (lua_getfieldi(L, idx, fname) == LUA_TTABLE)
    return true; /* table already there */
  else
  {
    lua_pop(L, 1); /* remove previous result */
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1);        /* copy to be left at top */
    lua_setfieldi(L, idx, fname); /* assign new table to field */
    return false;                /* false, because did not find table there */
  }
}

/*
** Stripped-down 'require': After checking "loaded" table, calls 'openf'
** to open a module, registers the result in 'package.loaded' table and,
** if 'glb' is true, also registers the result in the global table.
** Leaves resulting module on the top.
*/

void luaL_requiref(lua_State* L, const HostString& modname,
                              lua_CFunction openf, bool glb)
{
  luaL_requirefi(L, modname.to_utf8(), openf, glb);
}

void luaL_requirefi(lua_State* L, byte_view modname,
                    lua_CFunction openf, bool glb)
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

/*
** The freeze-ready 'luaL_requiref' and 'luaL_getsubtable' are internal C
** functions that the helpers call with 'lua_callk': each step that can
** run user code (an '__index', an '__newindex', the opening function) has
** its own continuation, and the user's 'k' goes on with the results.
** Stack of 'requireff': modname, openf, glb, then LOADED and the module.
*/
static int requireff(lua_State* L, int status, lua_KContext ctx);

static int requireff_start(lua_State* L)
{
  return requireff(L, LUA_OK, 0);
}

static int requireff(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  byte_view modname = *lua_tolstring(L, 1);
  int at = (int)ctx;
  for (;;)
  {
    switch (at)
    {
    case 0:
      luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE); /* 4 */
      lua_getfieldik(L, 4, modname, 1, requireff); /* LOADED[modname] */
      [[fallthrough]];
    case 1:
      if (lua_toboolean(L, -1)) /* package already loaded? */
      {
        at = 3;
        continue;
      }
      lua_pop(L, 1); /* remove field */
      lua_pushvalue(L, 2);
      lua_pushvalue(L, 1); /* argument to open function */
      lua_callk(L, 1, 1, 2, requireff); /* call 'openf' to open module */
      [[fallthrough]];
    case 2:
      lua_pushvalue(L, -1); /* make copy of module (call result) */
      lua_setfieldik(L, 4, modname, 3, requireff); /* LOADED[modname] = module */
      [[fallthrough]];
    case 3:
      lua_remove(L, 4); /* remove LOADED table */
      if (lua_toboolean(L, 3))
      {
        lua_pushvalue(L, -1); /* copy of module */
        lua_setglobalik(L, modname, 4, requireff); /* _G[modname] = module */
      }
      [[fallthrough]];
    default:
      return 1;
    }
  }
}

static KNamer knamer_requireff("auxlib.requireff", requireff);

static CFuncInfo cfi_requireff(requireff_start, "auxlib.requireff");

void luaL_requirefk(lua_State* L, const HostString& modname, lua_CFunction openf, bool glb,
                    lua_KContext ctx, lua_KFunction k)
{
  lua_pushcfuncinfo(L, &cfi_requireff);
  lua_pushlstring(L, modname.to_utf8(), 0, modname.to_utf8().size());
  lua_pushcfunction(L, openf);
  lua_pushboolean(L, glb);
  lua_callk(L, 3, 1, ctx, k);
}

/* stack of 'getsubtableff': the table, the field name */
static int getsubtableff(lua_State* L, int status, lua_KContext ctx);

static int getsubtableff_start(lua_State* L)
{
  return getsubtableff(L, LUA_OK, 0);
}

static int getsubtableff(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  byte_view fname = *lua_tolstring(L, 2);
  switch (ctx)
  {
  case 0:
    lua_getfieldik(L, 1, fname, 1, getsubtableff);
    [[fallthrough]];
  case 1:
    if (lua_type(L, -1) == LUA_TTABLE)
    { /* table already there */
      lua_pushboolean(L, true);
      return 2;
    }
    lua_pop(L, 1); /* remove previous result */
    lua_newtable(L);
    lua_pushvalue(L, -1); /* copy to be left at top */
    lua_setfieldik(L, 1, fname, 2, getsubtableff); /* assign new table to field */
    [[fallthrough]];
  default:
    lua_pushboolean(L, false); /* did not find table there */
    return 2;
  }
}

static KNamer knamer_getsubtableff("auxlib.getsubtableff", getsubtableff);

static CFuncInfo cfi_getsubtableff(getsubtableff_start, "auxlib.getsubtableff");

bool luaL_getsubtablek(lua_State* L, int idx, const HostString& fname, lua_KContext ctx, lua_KFunction k)
{
  lua_pushcfuncinfo(L, &cfi_getsubtableff);
  lua_pushvalue(L, idx);
  lua_pushlstring(L, fname.to_utf8(), 0, fname.to_utf8().size());
  lua_callk(L, 2, 2, ctx, k);
  bool found = lua_toboolean(L, -1);
  return found;
}

bool luaL_callmetak(lua_State* L, int obj, const HostString& e, lua_KContext ctx, lua_KFunction k)
{
  obj = lua_absindex(L, obj);
  if (luaL_getmetafieldi(L, obj, e.to_utf8()) == LUA_TNIL) /* no metafield? */
    return false;
  lua_pushvalue(L, obj);
  lua_callk(L, 1, 1, ctx, k);
  return true;
}

byte_view luaL_tolstringk(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k)
{
  luaL_toistringk(L, idx, ctx, k);
  return luaL_tolstringresult(L);
}

byte_view luaL_tolstringresult(lua_State* L)
{
  return luaL_tostringresult(L)->view();
}

int luaL_dofilek(lua_State* L, const std::optional<HostString>& filename, lua_KContext ctx, lua_KFunction k)
{
  int status = luaL_loadfile(L, filename);
  if (status == LUA_OK)
    status = lua_pcallk(L, 0, LUA_MULTRET, 0, ctx, k);
  return status;
}

int luaL_dostringk(lua_State* L, const HostString& str, lua_KContext ctx, lua_KFunction k)
{
  int status = luaL_loadstring(L, str);
  if (status == LUA_OK)
    status = lua_pcallk(L, 0, LUA_MULTRET, 0, ctx, k);
  return status;
}

void luaL_addgsubi(luaL_Buffer* b, byte_view s,
                   byte_view p, byte_view r)
{
  size_t pos = 0;
  size_t wild;
  while ((wild = s.find(p, pos)) != byte_view::npos)
  {
    luaL_addlstring(b, s, pos, wild - pos); /* push prefix */
    luaL_addlstring(b, r, 0, r.size()); /* push replacement in place of pattern */
    pos = wild + p.size();              /* continue after 'p' */
  }
  luaL_addlstring(b, s, pos, s.size() - pos); /* push last suffix */
}

void luaL_addgsub(luaL_Buffer* b, const HostString& s, const HostString& p, const HostString& r)
{
  luaL_addgsubi(b, s.to_utf8(), p.to_utf8(), r.to_utf8());
}

byte_view luaL_gsubi(lua_State* L, byte_view s, byte_view p, byte_view r)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addgsubi(&b, s, p, r);
  luaL_pushresult(&b);
  return *lua_tolstring(L, -1);
}

HostString luaL_gsub(lua_State* L, const HostString& s, const HostString& p, const HostString& r)
{
  return HostString(luaL_gsubi(L, s.to_utf8(), p.to_utf8(), r.to_utf8()));
}

static struct AuxAlloc : lua_Alloc
{
  bool permit(size_t, size_t, int) override
  {
    return true;
  }
} luaL_alloc;

/*
** Standard panic function just prints an error message. The test
** with 'lua_type' avoids possible memory errors in 'lua_tostring'.
*/
static int panic(lua_State* L)
{
  byte_view msg = (lua_type(L, -1) == LUA_TSTRING)
                                  ? lua_toistring(L, -1)->view()
                                  : lt::error_object_is_not_a_string;
  os_print_stderr(lt::PANIC_unprotected_error_in_call_to_Lua_API);
  os_print_stderr(msg);
  os_print_stderr(lt::n_2);
  os_flush_stderr();
  return 0; /* return to Lua to abort */
}

/*
** Warning functions:
** warnfoff: warning system is off
** warnfon: ready to start a new message
** warnfcont: previous message is to be continued
*/
static struct WarnOff : lua_WarnFunction
{
  void warn(lua_State* L, const HostString& message, int tocont) override;
} warnfoff;
static struct WarnOn : lua_WarnFunction
{
  void warn(lua_State* L, const HostString& message, int tocont) override;
} warnfon;
static struct WarnCont : lua_WarnFunction
{
  void warn(lua_State* L, const HostString& message, int tocont) override;
} warnfcont;

/*
** Check whether message is a control message. If so, execute the
** control or ignore it if unknown.
*/
static bool checkcontrol(lua_State* L, const HostString& message, int tocont)
{
  byte_view m = message.to_utf8();
  if (tocont || m.empty() || m[0] != '@') /* not a control message? */
    return false;
  else
  {
    byte_view tail = byte_view(m).substr(1);
    if (tail == lt::off_2)
      lua_setwarnf(L, &warnfoff); /* turn warnings off */
    else if (tail == lt::on_2)
      lua_setwarnf(L, &warnfon); /* turn warnings on */
    return true;                   /* it was a control message */
  }
}

void WarnOff::warn(lua_State* L, const HostString& message, int tocont)
{
  checkcontrol(L, message, tocont);
}

/*
** Writes the message and handle 'tocont', finishing the message
** if needed and setting the next warn function.
*/
void WarnCont::warn(lua_State* L, const HostString& message, int tocont)
{
  os_print_stderr(message.to_utf8());
  os_flush_stderr();
  if (tocont)                          /* not the last part? */
    lua_setwarnf(L, &warnfcont);     /* to be continued */
  else
  {                                /* last part */
    os_print_stderr(lt::n_3); /* finish message with end-of-line */
    os_flush_stderr();
    lua_setwarnf(L, &warnfon);   /* next call is a new message */
  }
}

void WarnOn::warn(lua_State* L, const HostString& message, int tocont)
{
  if (checkcontrol(L, message, tocont)) /* control message? */
    return;                                           /* nothing else to be done */
  os_print_stderr(lt::Lua_warning); /* start a new warning */
  os_flush_stderr();
  warnfcont.warn(L, message, tocont);         /* finish processing */
}

/*
** A function to compute an unsigned int with some level of
** randomness. Relies on the current time.
*/

static unsigned int luai_makeseed(void)
{
  unsigned long long nanos =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count();
  unsigned long long millis =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count();
  unsigned int buff[4];
  buff[0] = nanos & 0xFFFFFFFFu;
  buff[1] = (unsigned int) ((nanos >> 32) & 0xFFFFFFFFu);
  buff[2] = millis & 0xFFFFFFFFu;
  buff[3] = (unsigned int) ((millis >> 32) & 0xFFFFFFFFu);
  unsigned int res = buff[0];
  for (unsigned int i = 1; i < 4; i++)
    res ^= (res >> 3) + (res << 7) + buff[i];
  return res;
}

unsigned int luaL_makeseed(lua_State*)
{
  return luai_makeseed();
}

/*
** Use the name with parentheses so that headers can redefine it
** as a macro.
*/

lua_State*(luaL_newstate) (void)
{
  lua_State* L = _lua_newstate(&luaL_alloc, luaL_makeseed(nullptr));
  if (l_likely(L))
  {
    lua_atpanic(L, &panic);
    lua_setwarnf(L, &warnfon);
  }
  return L;
}

void luaL_checkversion_(lua_State* L, lua_Number ver, size_t sz)
{
  lua_Number v = lua_version(L);
  if (sz != LUAL_NUMSIZES) /* check numeric types */
    luaL_errori(L, lt::core_and_library_have_incompatible_numeric_types);
  else if (v != ver)
  {
    lua_pushnumber(L, ver);
    lua_pushnumber(L, v);
    luaL_errori(L, lt::version_mismatch_app_needs_f_Lua_core_provides_f);
  }
}

void luaL_checkversion(lua_State* L)
{
  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

int luaL_loadfile(lua_State* L, const std::optional<HostString>& filename)
{

  if (!filename)
    return luaL_loadfilei(L, std::nullopt, std::nullopt);
  byte_view sv = filename->to_utf8();
  return luaL_loadfilei(L, sv, std::nullopt);
}

void luaL_argcheck(lua_State* L, bool cond, int arg, const HostString& extramsg)
{
  if (luai_unlikely(!cond))
    luaL_argerrori(L, arg, extramsg.to_utf8());
}

void luaL_argchecki(lua_State* L, bool cond, int arg, byte_view extramsg)
{
  if (luai_unlikely(!cond))
    luaL_argerrori(L, arg, extramsg);
}

void luaL_argexpected(lua_State* L, bool cond, int arg, const HostString& tname)
{
  if (luai_unlikely(!cond))
    luaL_typeerrori(L, arg, tname.to_utf8());
}

void luaL_argexpectedi(lua_State* L, bool cond, int arg, byte_view tname)
{
  if (luai_unlikely(!cond))
    luaL_typeerrori(L, arg, tname);
}

HostString luaL_checkstring(lua_State* L, int arg)
{
  return HostString(luaL_checklstring(L, arg));
}

std::optional<HostString> luaL_optstring(lua_State* L, int arg,
                                                    const std::optional<HostString>& d)
{
  if (lua_isnoneornil(L, arg))
    return d;
  return luaL_checkstring(L, arg);
}

HostString luaL_typename(lua_State* L, int idx)
{
  return lua_typename(L, lua_type(L, idx));
}

int luaL_dofile(lua_State* L, const std::optional<HostString>& filename)
{
  int status = luaL_loadfile(L, filename);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return status;
}

int luaL_dostring(lua_State* L, const HostString& str)
{
  int status = luaL_loadstring(L, str);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return status;
}

int luaL_getmetatable(lua_State* L, const HostString& tname)
{
  return luaL_getmetatablei(L, tname.to_utf8());
}

int luaL_getmetatablei(lua_State* L, byte_view tname)
{
  return lua_getfieldi(L, LUA_REGISTRYINDEX, tname);
}

int luaL_loadbuffer(lua_State* L, byte_view buff, const std::optional<HostString>& name)
{
  return luaL_loadbufferi(L, buff,
                          name.has_value() ? std::optional<byte_view>(name->to_utf8()) : std::nullopt,
                          std::nullopt);
}

void luaL_pushfail(lua_State* L)
{
  lua_pushnil(L);
}

size_t luaL_bufflen(const luaL_Buffer* bf)
{
  return bf->used;
}

byte_span luaL_buffaddr(luaL_Buffer* bf)
{
  return bf->bytes;
}

void luaL_addsize(luaL_Buffer* B, size_t s)
{
  B->used += s;
}

void luaL_buffsub(luaL_Buffer* B, size_t s)
{
  B->used -= s;
}

size_t luaL_prepbuffer(luaL_Buffer* B)
{
  return luaL_prepbuffsize(B, LUAL_BUFFERSIZE);
}

/* }====================================================================== */

