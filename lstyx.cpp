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
#include "lgc.h"
#include "ltable.h"
#include "lctype.h"

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
constexpr char TAG_UPVAL_OPEN = 13; /* open upvalue: owner thread, stack level */
constexpr char TAG_SPECIAL = 14;    /* a value rebuilt by a closure (__persist) */
constexpr char TAG_PROTO_NEW = 15;  /* a prototype, written the first time it is met */
constexpr char TAG_PROTO_REF = 16;  /* a prototype already written */

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
/*
** How the walk reached the value being persisted, one node per step, kept
** on the C stack; it is turned into a path only when an error is raised.
*/
enum class Step : char
{
  Value,     /* table value; 'a': stack index of its key */
  Key,       /* table key; 'a': stack index of the key */
  Metatable,
  Upvalue,   /* 'a': stack index of the function, 'b': upvalue number */
  Stack,     /* 'b': thread stack slot */
  Special,   /* closure returned by __persist */
  PermKey,   /* key of a permanent */
};

struct PathNode
{
  const PathNode* up;
  Step step;
  int a;
  int b;
};

struct Persist
{
  lua_State* L;
  luaL_Buffer B;
  int perms;  /* stack index: permanents (object -> key) */
  int refs;   /* stack index: object -> reference id */
  lua_Integer refcount = 0;
  int depth = 0;
  const PathNode* path = nullptr;
};

/* records one step of the path while it is in scope */
struct PathStep
{
  Persist& P;
  PathNode node;
  PathStep(Persist& p, Step step, int a = 0, int b = 0) : P(p), node{p.path, step, a, b}
  {
    P.path = &node;
  }
  ~PathStep()
  {
    P.path = node.up;
  }
  PathStep(const PathStep&) = delete;
  PathStep& operator=(const PathStep&) = delete;
};

static bool is_name(byte_view s)
{
  if (s.size() == 0 || !lislalpha(s[0] & 0xFF))
    return false;
  for (size_t i = 1; i < s.size(); i++)
    if (!lislalnum(s[i] & 0xFF))
      return false;
  return true;
}

static void push_step(lua_State* L, const PathNode& n)
{
  switch (n.step)
  {
  case Step::Value:
    switch (lua_type(L, n.a))
    {
    case LUA_TSTRING:
      lua_pushvalue(L, n.a);
      lua_pushfstringi(L, is_name(*lua_tolstring(L, n.a)) ? lt::path_field_s : lt::path_string_key_s);
      return;
    case LUA_TNUMBER:
    case LUA_TBOOLEAN:
      luaL_tolstring(L, n.a);
      lua_pushfstringi(L, lt::path_index_s);
      return;
    default:
      lua_pushstring(L, luaL_typename(L, n.a));
      lua_pushfstringi(L, lt::path_object_key_s);
      return;
    }
  case Step::Key:
    lua_pushstring(L, luaL_typename(L, n.a));
    lua_pushfstringi(L, lt::path_key_s);
    return;
  case Step::Metatable:
    lua_pushistringref(L, lt::path_metatable);
    return;
  case Step::Upvalue:
  {
    const InternalString* name = lua_getupvaluei(L, n.a, n.b);
    if (name != nullptr)
      lua_pop(L, 1);
    if (name != nullptr && name->size() > 0)
    {
      lua_pushistringref(L, name->view());
      lua_pushfstringi(L, lt::path_upvalue_s);
    }
    else
    {
      lua_pushinteger(L, n.b);
      lua_pushfstringi(L, lt::path_upvalue_d);
    }
    return;
  }
  case Step::Stack:
    lua_pushinteger(L, n.b);
    lua_pushfstringi(L, lt::path_stack_d);
    return;
  case Step::Special:
    lua_pushistringref(L, lt::path_special);
    return;
  case Step::PermKey:
    lua_pushistringref(L, lt::path_permanent_key);
    return;
  }
}

static void push_path(lua_State* L, const PathNode* n)
{
  luaL_checkstacki(L, 4, std::nullopt);
  if (n == nullptr)
  {
    lua_pushistringref(L, lt::path_root);
    return;
  }
  push_path(L, n->up);
  push_step(L, *n);
  lua_concat(L, 2);
}

/* raises 'fmt' (its arguments already pushed) followed by the path */
[[noreturn]] static void persist_error(Persist& P, byte_view fmt)
{
  lua_State* L = P.L;
  lua_pushfstringi(L, fmt);
  push_path(L, P.path);
  luaL_errori(L, lt::s_at_s);
  std::abort();
}

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
    PathStep step(P, Step::PermKey);
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

/*
** The '__persist' field of a table's metatable: absent or true persists
** the table literally, false forbids it, and a function is called with the
** table and returns a closure that is persisted in its place; unpersist
** calls that closure to rebuild the table. Returns true if the table was
** written this way.
*/
static bool persist_special(Persist& P)
{
  lua_State* L = P.L;
  luaL_checkstacki(L, 3, std::nullopt);
  if (!lua_getmetatable(L, -1))
    return false;
  lua_pushistringref(L, lt::mm_persist);
  lua_rawget(L, -2);
  switch (lua_type(L, -1))
  {
  case LUA_TNIL:
    lua_pop(L, 2);
    return false;
  case LUA_TBOOLEAN:
  {
    bool allow = lua_toboolean(L, -1);
    lua_pop(L, 2);
    if (!allow)
      persist_error(P, lt::cannot_persist_a_table_its___persist_is_false);
    return false;
  }
  case LUA_TFUNCTION:
    lua_remove(L, -2);       /* the metatable */
    lua_pushvalue(L, -2);    /* the table */
    lua_call(L, 1, 1);
    if (!lua_isfunction(L, -1))
      persist_error(P, lt::mm_persist_must_return_a_function);
    luaL_addchar(&P.B, TAG_SPECIAL);
    luaL_addchar(&P.B, static_cast<char>(LUA_TTABLE));
    {
      PathStep step(P, Step::Special);
      persist(P);
    }
    lua_pop(L, 1);
    return true;
  default:
    persist_error(P, lt::mm_persist_must_be_a_boolean_or_a_function);
  }
}

static void persist_table(Persist& P)
{
  lua_State* L = P.L;
  int t = lua_gettop(L);
  luaL_checkstacki(L, 5, std::nullopt);
  luaL_addchar(&P.B, TAG_TABLE);
  TValue tv = getslot(L, L->ci->func + t);
  const Table* h = hvalue(&tv);
  put_i64(&P.B, h->asize());
  put_i64(&P.B, allocsizenode(h));
  put_i64(&P.B, h->asize() > 0 ? h->lenhint : 0);
  if (lua_getmetatable(L, t))
  {
    PathStep step(P, Step::Metatable);
    persist(P);
    lua_pop(L, 1);
  }
  else
    luaL_addchar(&P.B, TAG_NIL);
  lua_pushnil(L);
  while (lua_next(L, t))
  {
    int key = lua_gettop(L) - 1;
    lua_pushvalue(L, key);
    {
      PathStep step(P, Step::Key, key);
      persist(P);
    }
    lua_pop(L, 1);
    {
      PathStep step(P, Step::Value, key);
      persist(P);
    }
    lua_pop(L, 1); /* keep the key for the next iteration */
  }
  luaL_addchar(&P.B, TAG_NIL); /* end of entries */
}

/*
** A thread that is running (the caller, or one that resumed another) has
** frames that are not persisted yet; an open upvalue it owns is persisted
** as its current value instead.
*/
static bool is_running(lua_State* L, lua_State* T)
{
  return T == L || (T->status == LUA_OK && T->ci != &T->base_ci);
}

/* persists the value 'v' (pushed for the walk, then popped) */
static void persist_tvalue(Persist& P, const TValue* v)
{
  lua_State* L = P.L;
  setslot(L, L->top, v);
  L->top++;
  persist(P);
  lua_pop(L, 1);
}

static void persist_tstring(Persist& P, TString* ts)
{
  TValue v{};
  if (ts != nullptr)
    setsvalue(P.L, &v, ts);
  else
    setnilvalue(&v);
  persist_tvalue(P, &v);
}

/*
** A prototype is written once, field by field; later uses refer to it by
** id. Its constants and names are ordinary values (strings get reference
** ids like any other) and its nested prototypes are prototypes too.
*/
static void persist_proto(Persist& P, const Proto* p)
{
  lua_State* L = P.L;
  luaL_checkstacki(L, 2, std::nullopt);
  lua_pushlightuserdata(L, const_cast<Proto*>(p));
  lua_rawget(L, P.refs);
  if (!lua_isnil(L, -1))
  {
    luaL_addchar(&P.B, TAG_PROTO_REF);
    put_i64(&P.B, lua_tointeger(L, -1));
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  lua_pushlightuserdata(L, const_cast<Proto*>(p));
  lua_pushinteger(L, ++P.refcount);
  lua_rawset(L, P.refs);
  luaL_addchar(&P.B, TAG_PROTO_NEW);
  put_i64(&P.B, p->linedefined);
  put_i64(&P.B, p->lastlinedefined);
  put_i64(&P.B, p->numparams);
  luaL_addchar(&P.B, static_cast<char>(p->flag & ~PF_FIXED));
  put_i64(&P.B, p->maxstacksize);
  put_i64(&P.B, static_cast<lua_Integer>(p->code.size()));
  put_raw(&P.B, p->code.data(), p->code.size() * sizeof(Instruction));
  put_i64(&P.B, static_cast<lua_Integer>(p->k.size()));
  for (const TValue& k : p->k)
    persist_tvalue(P, &k);
  put_i64(&P.B, static_cast<lua_Integer>(p->upvalues.size()));
  for (const Upvaldesc& uv : p->upvalues)
  {
    luaL_addchar(&P.B, static_cast<char>(uv.instack));
    put_i64(&P.B, uv.idx);
    luaL_addchar(&P.B, static_cast<char>(uv.kind));
    persist_tstring(P, uv.name);
  }
  put_i64(&P.B, static_cast<lua_Integer>(p->p.size()));
  for (const Proto* nested : p->p)
    persist_proto(P, nested);
  persist_tstring(P, p->source);
  put_i64(&P.B, static_cast<lua_Integer>(p->lineinfo.size()));
  put_raw(&P.B, p->lineinfo.data(), p->lineinfo.size());
  put_i64(&P.B, static_cast<lua_Integer>(p->abslineinfo.size()));
  for (const AbsLineInfo& a : p->abslineinfo)
  {
    put_i64(&P.B, a.pc);
    put_i64(&P.B, a.line);
  }
  put_i64(&P.B, static_cast<lua_Integer>(p->locvars.size()));
  for (const LocVar& lv : p->locvars)
  {
    persist_tstring(P, lv.varname);
    put_i64(&P.B, lv.startpc);
    put_i64(&P.B, lv.endpc);
  }
}

static void persist_lclosure(Persist& P)
{
  lua_State* L = P.L;
  int f = lua_gettop(L);
  luaL_checkstacki(L, 4, std::nullopt);
  luaL_addchar(&P.B, TAG_LCLOSURE);

  LClosure* cl = clLvalue(&L->stack[L->ci->func + f]);
  persist_proto(P, cl->p);

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
    lua_pushlightuserdata(L, id);
    lua_pushinteger(L, ++P.refcount);
    lua_rawset(L, P.refs);
    PathStep step(P, Step::Upvalue, f, i);
    UpVal* uv = cl->upvals[i - 1];
    if (upisopen(uv) && !is_running(L, uv->owner))
    {
      luaL_addchar(&P.B, TAG_UPVAL_OPEN);
      setslotth(L, L->top, uv->owner);
      L->top++;
      persist(P);
      lua_pop(L, 1);
      put_i64(&P.B, uv->stkid);
      continue;
    }
    luaL_addchar(&P.B, TAG_UPVAL_NEW);
    lua_getupvalue(L, f, i);
    persist(P);
    lua_pop(L, 1);
  }
}

[[noreturn]] static void cannot_persist_thread_with(Persist& P, byte_view what)
{
  lua_pushlstring(P.L, what, 0, what.size());
  persist_error(P, lt::cannot_persist_a_thread_with_s);
}

static void persist_thread(Persist& P)
{
  lua_State* L = P.L;
  lua_State* T = lua_tothread(L, -1);
  luaL_checkstacki(L, 3, std::nullopt);
  if (is_running(L, T))
    persist_error(P, lt::cannot_persist_a_running_thread);
  if (T->status != LUA_OK && T->status != LUA_YIELD)
    cannot_persist_thread_with(P, lt::an_error_status);

  lua_Integer nframes = 0;
  StkId maxtop = T->top;
  if (T->ci != &T->base_ci)
  {
    for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
    {
      if (ci->k != nullptr)
        cannot_persist_thread_with(P, lt::a_continuation_function);
      if (ci->callstatus & (CIST_HOOKED | CIST_HOOKYIELD | CIST_FIN | CIST_TBC | CIST_CLSRET))
        cannot_persist_thread_with(P, lt::a_hook_or_finalizer_call);
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
  for (StkId i = 0; i < T->top; i++)
  {
    copyslot(L, L->top, T, i);
    L->top++;
    PathStep step(P, Step::Stack, 0, static_cast<int>(i));
    persist(P);
    lua_pop(L, 1);
  }
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
  put_i64(&P.B, T->ntbc);
  for (int k = 0; k < T->ntbc; k++)
    put_i64(&P.B, T->tbc[k]);
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
  case LUA_TUSERDATA:
  case LUA_TLIGHTUSERDATA:
    break;
  default:
    lua_pushstring(L, luaL_typename(L, -1));
    persist_error(P, lt::cannot_persist_a_s_value);
  }

  if (begin_object(P))
    return;
  if (lua_type(L, -1) == LUA_TUSERDATA || lua_type(L, -1) == LUA_TLIGHTUSERDATA)
    persist_error(P, lt::persisting_a_userdata_value_is_not_supported_yet);
  if (P.depth >= MAX_DEPTH)
    persist_error(P, lt::persisted_data_is_nested_too_deeply);
  P.depth++;
  switch (lua_type(L, -1))
  {
  case LUA_TSTRING:
    persist_string(P);
    break;
  case LUA_TTABLE:
    if (!persist_special(P))
      persist_table(P);
    break;
  case LUA_TFUNCTION:
    if (lua_iscfunction(L, -1))
      persist_error(P, lt::cannot_persist_a_C_function);
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

  [[noreturn]] void invalid() const
  {
    lua_pushinteger(L, static_cast<lua_Integer>(pos));
    luaL_errori(L, lt::invalid_persisted_data_at_byte_d);
    std::abort();
  }

  void need(size_t n)
  {
    if (data.size() - pos < n)
      invalid();
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
  lua_Integer asize = U.r.get_i64();
  lua_Integer hsize = U.r.get_i64();
  lua_Integer lenhint = U.r.get_i64();
  if (asize < 0 || asize > INT_MAX || hsize < 0 || hsize > INT_MAX ||
      (hsize & (hsize - 1)) != 0 || lenhint < 0 || lenhint > asize)
    U.r.invalid();
  /* same sizes as the original (the table limits are checked by
     lua_createtable): entries go where they were, with no rehash */
  lua_createtable(L, static_cast<int>(asize), static_cast<int>(hsize));
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
  TValue tv = getslot(L, L->ci->func + t);
  Table* h = hvalue(&tv);
  if (h->asize() != asize || allocsizenode(h) != hsize)
    U.r.invalid();
  h->lenhint = static_cast<int>(lenhint);
}

/*
** A value rebuilt by the closure its '__persist' function returned. Its
** reference id is taken before the closure is read, matching persist.
*/
static void unpersist_special(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  int type = U.r.get_byte();
  if (type != LUA_TTABLE)
    U.r.invalid();
  unpersist(U);
  if (!lua_isfunction(L, -1))
    U.r.invalid();
  lua_call(L, 0, 1);
  if (lua_type(L, -1) != type)
  {
    lua_pushstring(L, luaL_typename(L, -1));
    lua_pushstring(L, lua_typename(L, type));
    luaL_errori(L, lt::mm_persist_restore_returned_s_s_expected);
  }
  register_object(U, id);
}

/*
** A thread being restored is not running, so nothing may raise an error on
** it: its stack and frames are grown in the modes that report failure, work
** that can only raise runs protected on it (as `lua_resume` runs a
** coroutine), and a failure is raised on the calling thread, as
** `coroutine.resume` does when it grows a coroutine's stack.
*/
[[noreturn]] static void memory_error(lua_State* L)
{
  lua_pushistringref(L, lt::not_enough_memory);
  lua_error(L);
  std::abort();
}

/* finds or creates the open upvalue at 'level' of a thread (run protected) */
struct FindUpval : ProtectedCall
{
  StkId level = 0;
  UpVal* uv = nullptr;
  void run(lua_State* L1) override
  {
    uv = UpVal::luaF_findupval(L1, level);
  }
};

/* replaces upvalue 'i' of the closure at 'f' by an open upvalue of a thread */
static void unpersist_open_upvalue(Unpersist& U, int f, int i)
{
  lua_State* L = U.r.L;
  unpersist(U); /* the owner thread */
  lua_State* L1 = lua_tothread(L, -1);
  lua_Integer level = U.r.get_i64();
  if (L1 == nullptr || is_running(L, L1) || level < 0 || level >= L1->top)
    U.r.invalid();
  FindUpval fu;
  fu.level = level;
  if (luaD_rawrunprotected(L1, &fu) != LUA_OK)
    memory_error(L);
  LClosure* cl = clLvalue(&L->stack[L->ci->func + f]);
  cl->upvals[i - 1] = fu.uv;
  luaC_objbarrier(L, cl, fu.uv);
  lua_pop(L, 1);
}

/* reads a count, at most the number of bytes left (each item takes one) */
static int get_count(Unpersist& U, size_t item_bytes = 1)
{
  lua_Integer n = U.r.get_i64();
  if (n < 0 || n > INT_MAX ||
      static_cast<size_t>(n) > (U.r.data.size() - U.r.pos) / item_bytes)
    U.r.invalid();
  return static_cast<int>(n);
}

static int get_int(Unpersist& U)
{
  lua_Integer n = U.r.get_i64();
  if (n < INT_MIN || n > INT_MAX)
    U.r.invalid();
  return static_cast<int>(n);
}

/* reads a string or nil; returns the string (anchored by the caller's barrier) */
static TString* unpersist_name(Unpersist& U, GCObject* owner)
{
  lua_State* L = U.r.L;
  unpersist(U);
  TString* ts = nullptr;
  if (lua_type(L, -1) == LUA_TSTRING)
  {
    TValue v = getslot(L, L->top - 1);
    ts = tsvalue(&v);
    luaC_objbarrier(L, owner, ts);
  }
  else if (!lua_isnil(L, -1))
    U.r.invalid();
  lua_pop(L, 1);
  return ts;
}

/*
** Pushes a function holding the closure's prototype. A new prototype is
** built field by field, as the engine's loader builds one: it is anchored
** first (by a holder closure), and each part is made valid for the
** collector before anything that can run it (reading a value).
*/
static void unpersist_proto(Unpersist& U)
{
  lua_State* L = U.r.L;
  global_State* g = L->l_G;
  luaL_checkstacki(L, 3, std::nullopt);
  char tag = U.r.get_byte();
  if (tag == TAG_PROTO_REF)
  {
    lua_rawgeti(L, U.refs, U.r.get_i64());
    if (lua_type(L, -1) != LUA_TFUNCTION || lua_iscfunction(L, -1))
      U.r.invalid();
    return;
  }
  if (tag != TAG_PROTO_NEW)
    U.r.invalid();
  lua_Integer pid = ++U.refcount;
  lua_lock(L);
  Proto* p = Proto::luaF_newproto(L);
  LClosure* holder = LClosure::luaF_newLclosure(L, 0);
  holder->p = p;
  setslotclL(L, L->top, holder);
  L->top++;
  luaC_objbarrier(L, holder, p);
  lua_unlock(L);
  lua_pushvalue(L, -1);
  lua_rawseti(L, U.refs, pid);

  p->linedefined = get_int(U);
  p->lastlinedefined = get_int(U);
  p->numparams = get_int(U);
  p->flag = static_cast<lu_byte>(U.r.get_byte() & ~PF_FIXED);
  p->maxstacksize = get_int(U);
  if (p->numparams < 0 || p->maxstacksize < 0 || p->numparams > p->maxstacksize)
    U.r.invalid();

  int n = get_count(U, sizeof(Instruction));
  byte_view code = U.r.get_bytes(static_cast<size_t>(n) * sizeof(Instruction));
  lua_lock(L);
  p->code = luaM_alloc_Instruction_span(g, L, {}, static_cast<size_t>(n), true);
  lua_unlock(L);
  std::memcpy(p->code.data(), code.data(), code.size());

  n = get_count(U);
  lua_lock(L);
  p->k = luaM_alloc_TValue_span(g, L, {}, static_cast<size_t>(n), true);
  for (TValue& k : p->k)
    setnilvalue(&k);
  lua_unlock(L);
  for (TValue& k : p->k)
  {
    unpersist(U);
    int t = lua_type(L, -1);
    if (t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER && t != LUA_TSTRING)
      U.r.invalid();
    k = getslot(L, L->top - 1);
    luaC_barrier(L, p, &k);
    lua_pop(L, 1);
  }

  n = get_count(U);
  lua_lock(L);
  p->upvalues = luaM_alloc_Upvaldesc_span(g, L, {}, static_cast<size_t>(n), true);
  for (Upvaldesc& uv : p->upvalues)
    uv = Upvaldesc{nullptr, 0, 0, 0};
  lua_unlock(L);
  for (Upvaldesc& uv : p->upvalues)
  {
    uv.instack = static_cast<lu_byte>(U.r.get_byte());
    uv.idx = get_int(U);
    uv.kind = static_cast<lu_byte>(U.r.get_byte());
    uv.name = unpersist_name(U, p);
  }

  n = get_count(U);
  lua_lock(L);
  p->p = luaM_alloc_Proto_ptr_span(g, L, {}, static_cast<size_t>(n), true);
  for (Proto*& nested : p->p)
    nested = nullptr;
  lua_unlock(L);
  for (Proto*& nested : p->p)
  {
    unpersist_proto(U);
    TValue h = getslot(L, L->top - 1);
    nested = clLvalue(&h)->p;
    luaC_objbarrier(L, p, nested);
    lua_pop(L, 1);
  }

  p->source = unpersist_name(U, p);

  n = get_count(U);
  byte_view lines = U.r.get_bytes(static_cast<size_t>(n));
  lua_lock(L);
  p->lineinfo = luaM_alloc_ls_byte_span(g, L, {}, static_cast<size_t>(n), true);
  lua_unlock(L);
  std::memcpy(p->lineinfo.data(), lines.data(), lines.size());

  n = get_count(U, 16);
  lua_lock(L);
  p->abslineinfo = luaM_alloc_AbsLineInfo_span(g, L, {}, static_cast<size_t>(n), true);
  lua_unlock(L);
  for (AbsLineInfo& a : p->abslineinfo)
  {
    a.pc = get_int(U);
    a.line = get_int(U);
  }

  n = get_count(U);
  lua_lock(L);
  p->locvars = luaM_alloc_LocVar_span(g, L, {}, static_cast<size_t>(n), true);
  for (LocVar& lv : p->locvars)
    lv = LocVar{nullptr, 0, 0};
  lua_unlock(L);
  for (LocVar& lv : p->locvars)
  {
    lv.varname = unpersist_name(U, p);
    lv.startpc = get_int(U);
    lv.endpc = get_int(U);
  }
}

/*
** A closure is made as OP_CLOSURE makes one: a new closure on the shared
** prototype, anchored on the stack before its upvalues are created. This
** allocates, so it runs under the API lock, as an API function would.
*/
static void push_new_closure(lua_State* L, Proto* p)
{
  lua_lock(L);
  LClosure* cl = LClosure::luaF_newLclosure(L, static_cast<int>(p->upvalues.size()));
  cl->p = p;
  setslotclL(L, L->top, cl);
  L->top++;
  luaC_objbarrier(L, cl, p);
  cl->luaF_initupvals(L);
  lua_unlock(L);
}

static void unpersist_lclosure(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  unpersist_proto(U);
  TValue holder = getslot(L, L->top - 1);
  push_new_closure(L, clLvalue(&holder)->p);
  lua_remove(L, -2); /* the prototype's holder */
  int f = lua_gettop(L);
  register_object(U, id);
  lua_Integer nup = U.r.get_i64();
  LClosure* cl = clLvalue(&L->stack[L->ci->func + f]);
  if (nup != static_cast<lua_Integer>(cl->upvals.size()))
    U.r.invalid();
  for (int i = 1; i <= nup; i++)
  {
    char tag = U.r.get_byte();
    if (tag == TAG_UPVAL_NEW || tag == TAG_UPVAL_OPEN)
    {
      lua_Integer uid = ++U.refcount;
      lua_pushvalue(L, f);
      lua_rawseti(L, U.ups, uid);
      lua_pushinteger(L, i);
      lua_rawseti(L, U.ups, -uid);
      if (tag == TAG_UPVAL_OPEN)
        unpersist_open_upvalue(U, f, i);
      else
      {
        unpersist(U);
        lua_setupvalue(L, f, i);
      }
    }
    else if (tag == TAG_UPVAL_REF)
    {
      lua_Integer uid = U.r.get_i64();
      lua_rawgeti(L, U.ups, uid);
      lua_rawgeti(L, U.ups, -uid);
      if (!lua_isfunction(L, -2) || !lua_isinteger(L, -1))
        U.r.invalid();
      lua_upvaluejoin(L, f, i, lua_gettop(L) - 1, static_cast<int>(lua_tointeger(L, -1)));
      lua_pop(L, 2);
    }
    else
      U.r.invalid();
  }
}

/* the frame's function slot holds what its call status says it calls */
static bool valid_frame(lua_State* L1, const CallInfo* ci)
{
  TValue f = getslot(L1, ci->func);
  if (ci->callstatus & CIST_C)
    return ttisfunction(&f);
  if (!ttisLclosure(&f))
    return false;
  const Proto* p = clLvalue(&f)->p;
  return ci->savedpc >= 0 && ci->savedpc <= static_cast<int>(p->code.size());
}

static void unpersist_frames(Unpersist& U, lua_State* L1, lua_Integer nframes,
                             lua_Integer top, lua_Integer maxtop, int need);
static void unpersist_tbc(Unpersist& U, lua_State* L1, lua_Integer nframes, lua_Integer top);

/*
** The thread is valid at every point a collection can see it: its stack is
** filled while it has only its base frame, then the frames are read into
** spare CallInfos past 'ci' (only an emergency collection, which keeps
** them, can run meanwhile) and become part of the thread in one step.
*/
static void unpersist_thread(Unpersist& U, lua_Integer id)
{
  lua_State* L = U.r.L;
  luaL_checkstacki(L, 5, std::nullopt);
  lua_State* L1 = lua_newthread(L);
  register_object(U, id);

  char status = U.r.get_byte();
  lua_Integer top = U.r.get_i64();
  lua_Integer maxtop = U.r.get_i64();
  if ((status != LUA_OK && status != LUA_YIELD) || top < 1 || maxtop < top ||
      maxtop >= LUAI_MAXSTACK - LUA_MINSTACK)
    U.r.invalid();

  int need = static_cast<int>(maxtop) + LUA_MINSTACK;
  if (!lua_checkstack(L1, need - static_cast<int>(L1->top)))
    memory_error(L);
  L1->top = top;
  for (StkId i = 0; i < top; i++)
  {
    unpersist(U);
    copyslot(L1, i, L, L->top - 1);
    lua_pop(L, 1);
  }

  lua_Integer nframes = U.r.get_i64();
  if (nframes < 0 || (nframes > 0 && status != LUA_YIELD))
    U.r.invalid();
  if (nframes > 0)
    unpersist_frames(U, L1, nframes, top, maxtop, need);
  unpersist_tbc(U, L1, nframes, top);
  L1->status = static_cast<TStatus>(status);
}

/* frames are read into spare CallInfos and attached in one step */
static void unpersist_frames(Unpersist& U, lua_State* L1, lua_Integer nframes,
                             lua_Integer top, lua_Integer maxtop, int need)
{
  lua_State* L = U.r.L;
  /* a collection may have shrunk the stack while the values were read */
  if (!lua_checkstack(L1, need - static_cast<int>(L1->top)))
    memory_error(L);
  CallInfo* last = L1->ci;
  for (lua_Integer k = 0; k < nframes; k++)
  {
    CallInfo* ci = last->next;
    if (ci == nullptr)
    {
      L1->ci = last;
      ci = luaE_extendCI(L1, 0);
      L1->ci = &L1->base_ci;
      if (ci == nullptr)
        memory_error(L);
    }
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
    if (ci->func < 0 || ci->func >= top || ci->top < ci->func || ci->top > maxtop ||
        !valid_frame(L1, ci))
      U.r.invalid();
    last = ci;
  }
  L1->ci = last;
}

/* registers a to-be-closed variable of a thread (run protected) */
struct NewTbc : ProtectedCall
{
  StkId level = 0;
  void run(lua_State* L1) override
  {
    luaF_newtbcupval(L1, level);
  }
};

/*
** To-be-closed variables are registered once the thread's stack and frames
** are complete, with the engine's own luaF_newtbcupval, run protected on
** the thread; an error there is raised on the caller.
*/
static void unpersist_tbc(Unpersist& U, lua_State* L1, lua_Integer nframes, lua_Integer top)
{
  lua_State* L = U.r.L;
  lua_Integer ntbc = U.r.get_i64();
  if (ntbc < 0 || ntbc > top || (ntbc > 0 && nframes == 0))
    U.r.invalid();
  StkId prev = 0;
  for (lua_Integer k = 0; k < ntbc; k++)
  {
    lua_Integer level = U.r.get_i64();
    if (level <= prev || level >= top)
      U.r.invalid();
    prev = level;
    NewTbc nt;
    nt.level = level;
    TStatus st = luaD_rawrunprotected(L1, &nt);
    if (st == LUA_ERRMEM)
      memory_error(L);
    if (st != LUA_OK)
    {
      lua_xmove(L1, L, 1);
      lua_error(L);
    }
  }
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
      U.r.invalid();
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
  case TAG_SPECIAL:
    break;
  default:
    U.r.invalid();
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
      U.r.invalid();
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
  case TAG_SPECIAL:
    unpersist_special(U, id);
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
    U.r.invalid();
  U.r.pos = header.size();
  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 0);
  unpersist(U);
  if (U.r.pos != U.r.data.size())
    U.r.invalid();
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
