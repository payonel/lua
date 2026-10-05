/*
** Styx: persistence of Lua values (see docs/styx.md)
*/

#include <cstring>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"
#include "lstate.h"
#include "ldo.h"
#include "lfunc.h"
#include "lapi.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

/* value tags in the persisted stream */
constexpr char TAG_NIL = 0;
constexpr char TAG_FALSE = 1;
constexpr char TAG_TRUE = 2;
constexpr char TAG_INTEGER = 3;
constexpr char TAG_FLOAT = 4;
constexpr char TAG_STRING = 5;
constexpr char TAG_TABLE = 6;
constexpr char TAG_LCLOSURE = 7;
constexpr char TAG_THREAD = 8;
constexpr char TAG_REF = 9;      /* back reference to an object already written */
constexpr char TAG_PERM = 10;    /* object replaced by its permanent key */
constexpr char TAG_UPVAL_NEW = 11;
constexpr char TAG_UPVAL_REF = 12;

constexpr int MAX_DEPTH = 10000;

static void put_raw(luaL_Buffer* B, const void* p, size_t n)
{
  luaL_addlstring(B, byte_view(static_cast<const char*>(p), n), 0, n);
}

static void put_i64(luaL_Buffer* B, lua_Integer v)
{
  put_raw(B, &v, sizeof(v));
}

/*
** Persisting. The value to persist is on top of the stack; every function
** leaves the stack as it found it.
*/
struct Persist
{
  lua_State* L;
  luaL_Buffer B;
  int perms;  /* stack index: permanents (object -> key) */
  int refs;   /* stack index: object -> reference id */
  lua_Integer refcount = 0;
  int depth = 0;
};

static void persist(Persist& P);

/* true if the object on top was fully handled (reference or permanent) */
static bool begin_object(Persist& P)
{
  lua_State* L = P.L;
  lua_pushvalue(L, -1);
  lua_rawget(L, P.refs);
  if (!lua_isnil(L, -1))
  {
    luaL_addchar(&P.B, TAG_REF);
    put_i64(&P.B, lua_tointeger(L, -1));
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  lua_pushvalue(L, -1);
  lua_pushinteger(L, ++P.refcount);
  lua_rawset(L, P.refs);
  lua_pushvalue(L, -1);
  lua_rawget(L, P.perms);
  if (!lua_isnil(L, -1))
  {
    luaL_addchar(&P.B, TAG_PERM);
    luaL_addchar(&P.B, static_cast<char>(lua_type(L, -2)));
    persist(P); /* the permanent key */
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  return false;
}

static void persist_string(Persist& P)
{
  byte_view s = *lua_tolstring(P.L, -1);
  luaL_addchar(&P.B, TAG_STRING);
  put_i64(&P.B, static_cast<lua_Integer>(s.size()));
  luaL_addlstring(&P.B, s, 0, s.size());
}

static void persist_table(Persist& P)
{
  lua_State* L = P.L;
  int t = lua_gettop(L);
  luaL_checkstacki(L, 5, std::nullopt);
  luaL_addchar(&P.B, TAG_TABLE);
  if (lua_getmetatable(L, t))
  {
    persist(P);
    lua_pop(L, 1);
  }
  else
    luaL_addchar(&P.B, TAG_NIL);
  lua_pushnil(L);
  while (lua_next(L, t))
  {
    lua_pushvalue(L, -2);
    persist(P);
    lua_pop(L, 1);
    persist(P);
    lua_pop(L, 1); /* keep the key for the next iteration */
  }
  luaL_addchar(&P.B, TAG_NIL); /* end of entries */
}

struct ProtoWriter : lua_Writer
{
  luaL_Buffer* B;
  int write(lua_State*, std::optional<byte_view> b) override
  {
    if (b.has_value())
      luaL_addlstring(B, *b, 0, b->size());
    return 0;
  }
};

static void persist_lclosure(Persist& P)
{
  lua_State* L = P.L;
  int f = lua_gettop(L);
  luaL_checkstacki(L, 4, std::nullopt);
  luaL_addchar(&P.B, TAG_LCLOSURE);

  luaL_Buffer pb;
  luaL_buffinit(L, &pb);
  ProtoWriter w;
  w.B = &pb;
  lua_dump(L, &w, 0);
  size_t n = luaL_bufflen(&pb);
  put_i64(&P.B, static_cast<lua_Integer>(n));
  luaL_addlstring(&P.B, byte_view(luaL_buffaddr(&pb).data(), n), 0, n);

  LClosure* cl = clLvalue(&L->stack[L->ci->func + f]);
  int nup = static_cast<int>(cl->upvals.size());
  put_i64(&P.B, nup);
  for (int i = 1; i <= nup; i++)
  {
    void* id = lua_upvalueid(L, f, i);
    lua_pushlightuserdata(L, id);
    lua_rawget(L, P.refs);
    if (!lua_isnil(L, -1))
    {
      luaL_addchar(&P.B, TAG_UPVAL_REF);
      put_i64(&P.B, lua_tointeger(L, -1));
      lua_pop(L, 1);
      continue;
    }
    lua_pop(L, 1);
    if (upisopen(cl->upvals[i - 1]))
      luaL_errori(L, lt::cannot_persist_an_open_upvalue);
    lua_pushlightuserdata(L, id);
    lua_pushinteger(L, ++P.refcount);
    lua_rawset(L, P.refs);
    luaL_addchar(&P.B, TAG_UPVAL_NEW);
    lua_getupvalue(L, f, i);
    persist(P);
    lua_pop(L, 1);
  }
}

[[noreturn]] static void cannot_persist_thread_with(lua_State* L, byte_view what)
{
  lua_pushlstring(L, what, 0, what.size());
  luaL_errori(L, lt::cannot_persist_a_thread_with_s);
  std::abort();
}

static void persist_thread(Persist& P)
{
  lua_State* L = P.L;
  lua_State* T = lua_tothread(L, -1);
  luaL_checkstacki(L, 3, std::nullopt);
  if (T == L || (T->status == LUA_OK && T->ci != &T->base_ci))
    luaL_errori(L, lt::cannot_persist_a_running_thread);
  if (T->status != LUA_OK && T->status != LUA_YIELD)
    cannot_persist_thread_with(L, lt::an_error_status);
  if (T->openupval != nullptr)
    cannot_persist_thread_with(L, lt::open_upvalues);
  if (T->ntbc != 0)
    cannot_persist_thread_with(L, lt::to_be_closed_variables);

  lua_Integer nframes = 0;
  StkId maxtop = T->top;
  if (T->ci != &T->base_ci)
  {
    for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
    {
      if (ci->k != nullptr)
        cannot_persist_thread_with(L, lt::a_continuation_function);
      if (ci->callstatus & (CIST_HOOKED | CIST_HOOKYIELD | CIST_FIN | CIST_TBC | CIST_CLSRET))
        cannot_persist_thread_with(L, lt::a_hook_or_finalizer_call);
      nframes++;
      if (ci->top > maxtop)
        maxtop = ci->top;
      if (ci == T->ci)
        break;
    }
  }

  luaL_addchar(&P.B, TAG_THREAD);
  luaL_addchar(&P.B, static_cast<char>(T->status));
  put_i64(&P.B, T->top);
  put_i64(&P.B, maxtop);
  put_i64(&P.B, nframes);
  if (nframes > 0)
  {
    for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
    {
      put_i64(&P.B, ci->func);
      put_i64(&P.B, ci->top);
      put_i64(&P.B, ci->savedpc);
      put_i64(&P.B, ci->nextraargs);
      put_i64(&P.B, ci->callstatus);
      put_i64(&P.B, ci->old_errfunc);
      put_i64(&P.B, ci->ctx);
      put_i64(&P.B, ci->funcidx);
      put_i64(&P.B, ci->nyield);
      put_i64(&P.B, ci->nres);
      if (ci == T->ci)
        break;
    }
  }
  for (StkId i = 0; i < T->top; i++)
  {
    copyslot(L, L->top, T, i);
    L->top++;
    persist(P);
    lua_pop(L, 1);
  }
}

static void persist(Persist& P)
{
  lua_State* L = P.L;
  switch (lua_type(L, -1))
  {
  case LUA_TNIL:
    luaL_addchar(&P.B, TAG_NIL);
    return;
  case LUA_TBOOLEAN:
    luaL_addchar(&P.B, lua_toboolean(L, -1) ? TAG_TRUE : TAG_FALSE);
    return;
  case LUA_TNUMBER:
    if (lua_isinteger(L, -1))
    {
      lua_Integer i = lua_tointeger(L, -1);
      luaL_addchar(&P.B, TAG_INTEGER);
      put_i64(&P.B, i);
    }
    else
    {
      lua_Number n = lua_tonumber(L, -1);
      luaL_addchar(&P.B, TAG_FLOAT);
      put_raw(&P.B, &n, sizeof(n));
    }
    return;
  case LUA_TSTRING:
  case LUA_TTABLE:
  case LUA_TFUNCTION:
  case LUA_TTHREAD:
    break;
  default:
    lua_pushstring(L, luaL_typename(L, -1));
    luaL_errori(L, lt::cannot_persist_a_s_value);
  }

  if (begin_object(P))
    return;
  if (P.depth >= MAX_DEPTH)
    luaL_errori(L, lt::persisted_data_is_nested_too_deeply);
  P.depth++;
  switch (lua_type(L, -1))
  {
  case LUA_TSTRING:
    persist_string(P);
    break;
  case LUA_TTABLE:
    persist_table(P);
    break;
  case LUA_TFUNCTION:
    if (lua_iscfunction(L, -1))
      luaL_errori(L, lt::cannot_persist_a_C_function);
    persist_lclosure(P);
    break;
  default:
    persist_thread(P);
    break;
  }
  P.depth--;
}

/*
** Unpersisting. Every function pushes exactly one value.
*/
struct Reader
{
  lua_State* L;
  byte_view data;
  size_t pos = 0;

  void need(size_t n)
  {
    if (data.size() - pos < n)
      luaL_errori(L, lt::invalid_persisted_data);
  }

  char get_byte()
  {
    need(1);
    return data[pos++];
  }

  void get_raw(void* p, size_t n)
  {
    need(n);
    std::memcpy(p, data.substr(pos, n).data(), n);
    pos += n;
  }

  lua_Integer get_i64()
  {
    lua_Integer v;
    get_raw(&v, sizeof(v));
    return v;
  }

  byte_view get_bytes(size_t n)
  {
    need(n);
    byte_view v = data.substr(pos, n);
    pos += n;
    return v;
  }
};

struct Unpersist
{
  Reader r;
  int perms; /* stack index: permanents (key -> object) */
  int refs;  /* stack index: reference id -> object */
  int ups;   /* stack index: upvalue id -> closure, -id -> upvalue index */
  lua_Integer refcount = 0;
  int depth = 0;
};

static void unpersist(Unpersist& U);

static void register_object(Unpersist& U, lua_Integer id)
{
  lua_pushvalue(U.r.L, -1);
  lua_rawseti(U.r.L, U.refs, id);
}

static void unpersist_table(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  lua_createtable(L, 0, 0);
  int t = lua_gettop(L);
  register_object(U, id);
  unpersist(U); /* metatable */
  if (lua_isnil(L, -1))
    lua_pop(L, 1);
  else
    lua_setmetatable(L, t);
  for (;;)
  {
    unpersist(U); /* key */
    if (lua_isnil(L, -1))
    {
      lua_pop(L, 1);
      break;
    }
    unpersist(U); /* value */
    lua_rawset(L, t);
  }
}

struct BytesReader : lua_Reader
{
  byte_view bytes;
  bool done = false;
  std::optional<byte_view> read(lua_State*) override
  {
    if (done)
      return std::nullopt;
    done = true;
    return bytes;
  }
};

static void unpersist_lclosure(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  lua_Integer n = U.r.get_i64();
  if (n < 0)
    luaL_errori(L, lt::invalid_persisted_data);
  BytesReader br;
  br.bytes = U.r.get_bytes(static_cast<size_t>(n));
  if (lua_load(L, &br, "=styx", "b") != LUA_OK)
    lua_error(L);
  int f = lua_gettop(L);
  register_object(U, id);
  lua_Integer nup = U.r.get_i64();
  LClosure* cl = clLvalue(&L->stack[L->ci->func + f]);
  if (nup != static_cast<lua_Integer>(cl->upvals.size()))
    luaL_errori(L, lt::invalid_persisted_data);
  for (int i = 1; i <= nup; i++)
  {
    char tag = U.r.get_byte();
    if (tag == TAG_UPVAL_NEW)
    {
      lua_Integer uid = ++U.refcount;
      lua_pushvalue(L, f);
      lua_rawseti(L, U.ups, uid);
      lua_pushinteger(L, i);
      lua_rawseti(L, U.ups, -uid);
      unpersist(U);
      lua_setupvalue(L, f, i);
    }
    else if (tag == TAG_UPVAL_REF)
    {
      lua_Integer uid = U.r.get_i64();
      lua_rawgeti(L, U.ups, uid);
      lua_rawgeti(L, U.ups, -uid);
      if (!lua_isfunction(L, -2) || !lua_isinteger(L, -1))
        luaL_errori(L, lt::invalid_persisted_data);
      lua_upvaluejoin(L, f, i, lua_gettop(L) - 1, static_cast<int>(lua_tointeger(L, -1)));
      lua_pop(L, 2);
    }
    else
      luaL_errori(L, lt::invalid_persisted_data);
  }
}

static void unpersist_thread(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  lua_State* L1 = lua_newthread(L);
  register_object(U, id);

  char status = U.r.get_byte();
  lua_Integer top = U.r.get_i64();
  lua_Integer maxtop = U.r.get_i64();
  lua_Integer nframes = U.r.get_i64();
  if ((status != LUA_OK && status != LUA_YIELD) || top < 1 || maxtop < top ||
      maxtop >= LUAI_MAXSTACK - LUA_MINSTACK || nframes < 0 || (nframes > 0 && status != LUA_YIELD))
    luaL_errori(L, lt::invalid_persisted_data);

  int need = static_cast<int>(maxtop) + LUA_MINSTACK;
  if (need > L1->stack_last)
    luaD_reallocstack(L1, need, 1);
  L1->top = top;
  for (lua_Integer k = 0; k < nframes; k++)
  {
    CallInfo* ci = L1->ci->next != nullptr ? L1->ci->next : luaE_extendCI(L1, 1);
    L1->ci = ci;
    ci->func = U.r.get_i64();
    ci->top = U.r.get_i64();
    ci->savedpc = static_cast<int>(U.r.get_i64());
    ci->nextraargs = static_cast<int>(U.r.get_i64());
    ci->callstatus = static_cast<l_uint32>(U.r.get_i64());
    ci->old_errfunc = U.r.get_i64();
    ci->ctx = U.r.get_i64();
    ci->funcidx = static_cast<int>(U.r.get_i64());
    ci->nyield = static_cast<int>(U.r.get_i64());
    ci->nres = static_cast<int>(U.r.get_i64());
    ci->k = nullptr;
    ci->trap = 0;
    if (ci->func < 0 || ci->func >= top || ci->top < ci->func || ci->top > maxtop)
      luaL_errori(L, lt::invalid_persisted_data);
  }
  for (StkId i = 0; i < top; i++)
  {
    unpersist(U);
    copyslot(L1, i, L, L->top - 1);
    lua_pop(L, 1);
  }
  L1->status = static_cast<TStatus>(status);
}

static void unpersist(Unpersist& U)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  char tag = U.r.get_byte();
  switch (tag)
  {
  case TAG_NIL:
    lua_pushnil(L);
    return;
  case TAG_FALSE:
    lua_pushboolean(L, 0);
    return;
  case TAG_TRUE:
    lua_pushboolean(L, 1);
    return;
  case TAG_INTEGER:
    lua_pushinteger(L, U.r.get_i64());
    return;
  case TAG_FLOAT:
  {
    lua_Number n;
    U.r.get_raw(&n, sizeof(n));
    lua_pushnumber(L, n);
    return;
  }
  case TAG_REF:
  {
    lua_Integer id = U.r.get_i64();
    lua_rawgeti(L, U.refs, id);
    if (lua_isnil(L, -1))
      luaL_errori(L, lt::invalid_persisted_data);
    return;
  }
  case TAG_PERM:
  {
    char type = U.r.get_byte();
    lua_Integer id = ++U.refcount;
    unpersist(U); /* the permanent key */
    lua_pushvalue(L, -1);
    lua_rawget(L, U.perms);
    if (lua_isnil(L, -1))
    {
      luaL_tolstring(L, -2);
      luaL_errori(L, lt::bad_permanent_value_for_key_s);
    }
    if (lua_type(L, -1) != type)
    {
      lua_pushstring(L, lua_typename(L, type));
      lua_pushstring(L, luaL_typename(L, -2));
      luaL_errori(L, lt::bad_permanent_value_s_expected_got_s);
    }
    lua_remove(L, -2);
    register_object(U, id);
    return;
  }
  case TAG_STRING:
  case TAG_TABLE:
  case TAG_LCLOSURE:
  case TAG_THREAD:
    break;
  default:
    luaL_errori(L, lt::invalid_persisted_data);
  }

  if (U.depth >= MAX_DEPTH)
    luaL_errori(L, lt::persisted_data_is_nested_too_deeply);
  U.depth++;
  lua_Integer id = ++U.refcount;
  switch (tag)
  {
  case TAG_STRING:
  {
    lua_Integer n = U.r.get_i64();
    if (n < 0)
      luaL_errori(L, lt::invalid_persisted_data);
    byte_view s = U.r.get_bytes(static_cast<size_t>(n));
    lua_pushlstring(L, s, 0, s.size());
    register_object(U, id);
    break;
  }
  case TAG_TABLE:
    unpersist_table(U, id);
    break;
  case TAG_LCLOSURE:
    unpersist_lclosure(U, id);
    break;
  default:
    unpersist_thread(U, id);
    break;
  }
  U.depth--;
}

/* persist([perms,] value) */
static int styx_persist(lua_State* L)
{
  luaL_checkany(L, 1);
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
  }
  else
  {
    lua_createtable(L, 0, 0);
    lua_insert(L, 1);
  }
  lua_createtable(L, 0, 0);
  Persist P;
  P.L = L;
  P.perms = 1;
  P.refs = 3;
  luaL_buffinit(L, &P.B);
  luaL_addlstring(&P.B, lt::styx_header, 0, lt::styx_header.size());
  lua_pushvalue(L, 2);
  persist(P);
  lua_pop(L, 1);
  luaL_pushresult(&P.B);
  return 1;
}

/* unpersist([perms,] string) */
static int styx_unpersist(lua_State* L)
{
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
  }
  else
  {
    lua_createtable(L, 0, 0);
    lua_insert(L, 1);
  }
  Unpersist U{Reader{L, luaL_checklstring(L, 2)}, 1, 3, 4};
  const byte_view header = lt::styx_header;
  if (U.r.data.size() < header.size() || U.r.data.substr(0, header.size()) != header)
    return luaL_errori(L, lt::invalid_persisted_data);
  U.r.pos = header.size();
  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 0);
  unpersist(U);
  if (U.r.pos != U.r.data.size())
    return luaL_errori(L, lt::invalid_persisted_data);
  return 1;
}

static int styx_settings(lua_State* L)
{
  return luaL_errori(L, lt::not_implemented);
}

static CFuncInfo cfi_persist(styx_persist);
static CFuncInfo cfi_unpersist(styx_unpersist);
static CFuncInfo cfi_settings(styx_settings);

static const luaL_RegCFI funcs[] = {
  {lt::persist, &cfi_persist},
  {lt::unpersist, &cfi_unpersist},
  {lt::settings, &cfi_settings},
};

}

LUAMOD_API int luaopen_styx(lua_State* L)
{
  luaL_newlibcfi(L, lua::lib::funcs);
  return 1;
}
