/*
** test_public_api.cpp
** Consumer-facing tests for the official Lua 5.5 C API.
** Only includes headers a real consumer would include:
** lua.h, lualib.h, lauxlib.h.
** Behavior expectations follow the official Lua 5.5 reference manual.
*/

#include <cstdio>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* ================= harness ================= */

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current = "";

static void check_impl(bool ok, const char* expr, int line)
{
  g_checks++;
  if (!ok)
  {
    g_failures++;
    std::printf("FAIL [%s] line %d: %s\n", g_current, line, expr);
  }
}
#define CHECK(cond) check_impl((cond), #cond, __LINE__)

/* ================= allocator ================= */

struct AllocStats
{
  unsigned long calls = 0;
};

static void* counting_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
  (void)osize;
  if (ud != nullptr)
    static_cast<AllocStats*>(ud)->calls++;
  if (nsize == 0)
  {
    std::free(ptr);
    return nullptr;
  }
  return std::realloc(ptr, nsize);
}

static AllocStats g_alloc_stats;

static lua_State* new_state()
{
  lua_State* L = lua_newstate(counting_alloc, &g_alloc_stats, 0);
  CHECK(L != nullptr);
  return L;
}

/* ================= load helpers ================= */

struct StringReader
{
  std::string_view src;
  bool done;
};

static std::string_view whole_reader(lua_State*, void* ud)
{
  StringReader* r = static_cast<StringReader*>(ud);
  if (r->done)
    return {};
  r->done = true;
  return r->src;
}

/* feeds the chunk one byte at a time (exercises the reader protocol) */
struct ByteReader
{
  std::string_view src;
  size_t pos;
};

static std::string_view byte_reader(lua_State*, void* ud)
{
  ByteReader* r = static_cast<ByteReader*>(ud);
  if (r->pos >= r->src.size())
    return {};
  return r->src.substr(r->pos++, 1);
}

static int load_string(lua_State* L, std::string_view code)
{
  StringReader r{code, false};
  return lua_load(L, whole_reader, &r, "=tpa"_hs, std::nullopt);
}

static int do_string(lua_State* L, std::string_view code)
{
  int st = load_string(L, code);
  if (st != LUA_OK)
    return st;
  return lua_pcall(L, 0, LUA_MULTRET, 0);
}

/* ================= C functions used by tests ================= */

static int cf_add(lua_State* L)
{
  lua_Integer a = lua_tointeger(L, 1);
  lua_Integer b = lua_tointeger(L, 2);
  lua_pushinteger(L, a + b);
  return 1;
}

static int cf_upvals(lua_State* L)
{
  lua_Integer a = lua_tointeger(L, lua_upvalueindex(1));
  lua_Integer b = lua_tointeger(L, lua_upvalueindex(2));
  lua_pushinteger(L, a + b);
  return 1;
}

static int cf_raise(lua_State* L)
{
  lua_pushliteral(L, "boom");
  return lua_error(L);
}

static int cf_raise_nil(lua_State* L)
{
  lua_pushnil(L);
  return lua_error(L);
}

static int cf_msgh(lua_State* L)
{
  std::string s("H:");
  std::optional<HostString> msg = lua_tostring(L, 1);
  if (msg.has_value())
  {
    std::string_view v = msg->to_utf8();
    s.append(v.data(), v.size());
  }
  lua_pushstring(L, HostString(std::string_view(s)));
  return 1;
}

static int cf_ret7(lua_State* L)
{
  lua_pushinteger(L, 7);
  return 1;
}

static int cf_panic(lua_State*)
{
  return 0;
}

static int cf_mm_add(lua_State* L)
{
  lua_pushinteger(L, 99);
  return 1;
}

static int cf_mm_true(lua_State* L)
{
  lua_pushboolean(L, 1);
  return 1;
}

static int cf_mm_index(lua_State* L)
{
  lua_pushinteger(L, 42);
  return 1;
}

static int g_newindex_hits = 0;
static int cf_mm_newindex(lua_State*)
{
  g_newindex_hits++;
  return 0;
}

static int cf_mm_concat(lua_State* L)
{
  lua_pushliteral(L, "CC");
  return 1;
}

static int cf_mm_len(lua_State* L)
{
  lua_pushinteger(L, 7);
  return 1;
}

static int g_gc_hits = 0;
static int cf_mm_gc(lua_State*)
{
  g_gc_hits++;
  return 0;
}

static int g_closed = 0;
static int cf_mm_close(lua_State*)
{
  g_closed++;
  return 0;
}

/* pushes a fresh table whose metatable maps `event` to `fn` */
static void push_table_with_mm(lua_State* L, const char* event, lua_CFunction fn)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, event);
  int r = lua_setmetatable(L, -2);
  CHECK(r == 1);
}

/* ================= dump writer ================= */

struct DumpBuf
{
  std::string bytes;
  int empty_calls = 0;
  bool empty_was_last = false;
};

static int dump_writer(lua_State*, std::string_view b, void* ud)
{
  DumpBuf* d = static_cast<DumpBuf*>(ud);
  if (b.empty())
  {
    d->empty_calls++;
    d->empty_was_last = true;
    return 0;
  }
  d->empty_was_last = false;
  d->bytes.append(b.data(), b.size());
  return 0;
}

static int failing_writer(lua_State*, std::string_view, void* ud)
{
  int* n = static_cast<int*>(ud);
  (*n)++;
  return 33; /* nonzero stops the dump */
}

/* ================= warnings ================= */

struct WarnLog
{
  std::string text;
  int calls = 0;
  int last_tocont = -1;
};

static void warn_collect(void* ud, const HostString& msg, int tocont)
{
  WarnLog* w = static_cast<WarnLog*>(ud);
  w->calls++;
  w->last_tocont = tocont;
  std::string_view v = msg.to_utf8();
  w->text.append(v.data(), v.size());
}

/* ================= continuations ================= */

static int g_cont_status = -999;
static lua_KContext g_cont_ctx = 0;

static int cont_call(lua_State* L, int status, lua_KContext ctx)
{
  g_cont_status = status;
  g_cont_ctx = ctx;
  return lua_gettop(L); /* return whole frame stack */
}

static int cf_callk(lua_State* L)
{
  /* arg 1: function to call (may yield across this call) */
  lua_pushvalue(L, 1);
  lua_callk(L, 0, LUA_MULTRET, 42, cont_call);
  return cont_call(L, LUA_OK, 42); /* no yield happened */
}

static int cont_yield(lua_State* L, int status, lua_KContext ctx)
{
  g_cont_status = status;
  g_cont_ctx = ctx;
  /* stack holds the values passed to the second resume; echo them */
  return lua_gettop(L);
}

static int cf_co(lua_State* L)
{
  CHECK(lua_isyieldable(L) == 1);
  lua_pushinteger(L, 100);
  return lua_yieldk(L, 1, 7, cont_yield);
}

static int cf_yield_tail(lua_State* L)
{
  /* yields all its arguments, plain lua_yield in tail position */
  return lua_yield(L, lua_gettop(L));
}

/* ================= to-be-closed ================= */

static int cf_tbc_closeslot(lua_State* L)
{
  int before = g_closed;
  push_table_with_mm(L, "__close", cf_mm_close);
  lua_toclose(L, -1);
  lua_closeslot(L, -1);
  CHECK(g_closed == before + 1);
  CHECK(lua_isnil(L, -1) == 1); /* slot value set to nil */
  return 0;
}

static int cf_tbc_settop(lua_State* L)
{
  int before = g_closed;
  int base = lua_gettop(L);
  push_table_with_mm(L, "__close", cf_mm_close);
  lua_toclose(L, -1);
  lua_settop(L, base); /* removing a tbc slot closes it */
  CHECK(g_closed == before + 1);
  return 0;
}

/* ================= debug helpers ================= */

static int cf_probe(lua_State* L)
{
  /* level 0: this C function */
  std::optional<lua_Debug> ar0 = lua_getstack(L, 0);
  CHECK(ar0.has_value());
  if (ar0.has_value())
  {
    std::optional<lua_Debug> in0 = lua_getinfo(L, "nSltu", *ar0);
    CHECK(in0.has_value());
    if (in0.has_value())
    {
      CHECK(in0->what == "C");
      CHECK(in0->currentline == -1);
      CHECK(in0->name == "probe");         /* called as global 'probe' */
      CHECK(in0->namewhat == "global");
    }
    /* 'f' pushes the running function */
    int top = lua_gettop(L);
    std::optional<lua_Debug> inf = lua_getinfo(L, "f", *ar0);
    CHECK(inf.has_value());
    CHECK(lua_gettop(L) == top + 1);
    CHECK(lua_tocfunction(L, -1) == cf_probe);
    lua_pop(L, 1);
  }

  /* level 1: the Lua caller */
  std::optional<lua_Debug> ar1 = lua_getstack(L, 1);
  CHECK(ar1.has_value());
  if (ar1.has_value())
  {
    std::optional<lua_Debug> in1 = lua_getinfo(L, "Sl", *ar1);
    CHECK(in1.has_value());
    if (in1.has_value())
    {
      CHECK(in1->what == "main");
      CHECK(in1->short_src == "tpa");
      CHECK(in1->currentline == 2); /* the line calling probe() */
    }

    /* locals of the caller: read then modify */
    int top = lua_gettop(L);
    std::optional<HostString> lname = lua_getlocal(L, &*ar1, 1);
    CHECK(lname.has_value() && *lname == "xyz");
    CHECK(lua_gettop(L) == top + 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    lua_pushinteger(L, 99);
    std::optional<HostString> sname = lua_setlocal(L, &*ar1, 1);
    CHECK(sname.has_value() && *sname == "xyz");
    CHECK(lua_gettop(L) == top); /* setlocal pops the value */

    std::optional<HostString> nolocal = lua_getlocal(L, &*ar1, 50);
    CHECK(!nolocal.has_value());
  }

  /* nonexistent level */
  std::optional<lua_Debug> arbad = lua_getstack(L, 99);
  CHECK(!arbad.has_value());
  return 0;
}

static int g_hook_calls = 0;
static int g_hook_rets = 0;
static int g_hook_lines = 0;
static int g_hook_counts = 0;

static void counting_hook(lua_State*, lua_Debug* ar)
{
  switch (ar->event)
  {
  case LUA_HOOKCALL:
  case LUA_HOOKTAILCALL:
    g_hook_calls++;
    break;
  case LUA_HOOKRET:
    g_hook_rets++;
    break;
  case LUA_HOOKLINE:
    g_hook_lines++;
    break;
  case LUA_HOOKCOUNT:
    g_hook_counts++;
    break;
  default:
    break;
  }
}

/* ================= tests ================= */

static void test_version_and_constants()
{
  CHECK(LUA_VERSION_NUM == 505);
  CHECK(LUA_VERSION_RELEASE_NUM == 50501);
  CHECK(LUA_MINSTACK == 20);
  CHECK(LUA_MULTRET == -1);
  CHECK(LUA_OK == 0 && LUA_YIELD == 1 && LUA_ERRRUN == 2 && LUA_ERRSYNTAX == 3 &&
        LUA_ERRMEM == 4 && LUA_ERRERR == 5);
  CHECK(LUA_TNONE == -1 && LUA_TNIL == 0 && LUA_TBOOLEAN == 1 &&
        LUA_TLIGHTUSERDATA == 2 && LUA_TNUMBER == 3 && LUA_TSTRING == 4 &&
        LUA_TTABLE == 5 && LUA_TFUNCTION == 6 && LUA_TUSERDATA == 7 &&
        LUA_TTHREAD == 8);
  CHECK(LUA_SIGNATURE == std::string_view("\x1bLua", 4));
  CHECK(!lua_ident.empty());
  /* lua_upvalueindex must produce pseudo-indices below the registry */
  CHECK(lua_upvalueindex(1) == LUA_REGISTRYINDEX - 1);
  CHECK(lua_upvalueindex(2) == LUA_REGISTRYINDEX - 2);

  lua_State* L = new_state();
  CHECK(lua_version(L) == static_cast<lua_Number>(LUA_VERSION_NUM));
  lua_close(L);
}

static void test_state()
{
  lua_State* L = new_state();

  /* atpanic returns the previous handler */
  lua_CFunction old = lua_atpanic(L, cf_panic);
  lua_CFunction prev = lua_atpanic(L, old);
  CHECK(prev == cf_panic);

  /* newthread pushes the thread */
  int top = lua_gettop(L);
  lua_State* L1 = lua_newthread(L);
  CHECK(L1 != nullptr);
  CHECK(lua_gettop(L) == top + 1);
  CHECK(lua_type(L, -1) == LUA_TTHREAD);
  CHECK(lua_tothread(L, -1) == L1);

  /* closethread with from == NULL */
  lua_pushinteger(L1, 1);
  int st = lua_closethread(L1, nullptr);
  CHECK(st == LUA_OK);
  CHECK(lua_gettop(L1) == 0);
  CHECK(lua_status(L1) == LUA_OK);

  /* registry well-known indices */
  int t = lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  CHECK(t == LUA_TTHREAD);
  CHECK(lua_tothread(L, -1) == L);
  t = lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  CHECK(t == LUA_TTABLE);
  lua_pushglobaltable(L);
  CHECK(lua_rawequal(L, -1, -2) == 1);
  lua_pop(L, 3);

  lua_close(L);
}

static void test_stack_manipulation()
{
  lua_State* L = new_state();

  CHECK(lua_gettop(L) == 0);
  for (int i = 1; i <= 5; i++)
    lua_pushinteger(L, i);
  CHECK(lua_gettop(L) == 5);

  /* absindex */
  CHECK(lua_absindex(L, -1) == 5);
  CHECK(lua_absindex(L, -5) == 1);
  CHECK(lua_absindex(L, 3) == 3);
  CHECK(lua_absindex(L, LUA_REGISTRYINDEX) == LUA_REGISTRYINDEX);

  /* pushvalue */
  lua_pushvalue(L, 2);
  CHECK(lua_gettop(L) == 6);
  CHECK(lua_tointeger(L, -1) == 2);
  lua_pop(L, 1);

  /* rotate toward top: [1,2,3,4,5] -> [1,5,2,3,4] */
  lua_rotate(L, 2, 1);
  CHECK(lua_tointeger(L, 2) == 5);
  CHECK(lua_tointeger(L, 3) == 2);
  CHECK(lua_tointeger(L, 5) == 4);
  /* rotate back */
  lua_rotate(L, 2, -1);
  for (int i = 1; i <= 5; i++)
    CHECK(lua_tointeger(L, i) == i);

  /* copy: overwrite slot 1 with slot 5 */
  lua_copy(L, 5, 1);
  CHECK(lua_tointeger(L, 1) == 5);
  CHECK(lua_gettop(L) == 5);
  lua_pushinteger(L, 1);
  lua_copy(L, -1, 1);
  lua_pop(L, 1);

  /* insert: move top into position 2 */
  lua_pushinteger(L, 9);
  lua_insert(L, 2);
  CHECK(lua_tointeger(L, 2) == 9);
  CHECK(lua_tointeger(L, 3) == 2);
  CHECK(lua_gettop(L) == 6);

  /* remove */
  lua_remove(L, 2);
  CHECK(lua_tointeger(L, 2) == 2);
  CHECK(lua_gettop(L) == 5);

  /* replace: move top into slot 1 */
  lua_pushinteger(L, 77);
  lua_replace(L, 1);
  CHECK(lua_tointeger(L, 1) == 77);
  CHECK(lua_gettop(L) == 5);

  /* settop growth fills with nil */
  lua_settop(L, 7);
  CHECK(lua_gettop(L) == 7);
  CHECK(lua_isnil(L, 6) == 1);
  CHECK(lua_isnil(L, 7) == 1);
  /* settop with negative index: -3 keeps all but two */
  lua_settop(L, -3);
  CHECK(lua_gettop(L) == 5);
  /* pop */
  lua_pop(L, 2);
  CHECK(lua_gettop(L) == 3);
  lua_settop(L, 0);
  CHECK(lua_gettop(L) == 0);

  /* checkstack */
  CHECK(lua_checkstack(L, 30) != 0);
  CHECK(lua_checkstack(L, 0) != 0);
  CHECK(lua_checkstack(L, INT_MAX) == 0);

  /* isnone/isnoneornil beyond top */
  lua_pushnil(L);
  CHECK(lua_isnone(L, 5) == 1);
  CHECK(lua_isnoneornil(L, 5) == 1);
  CHECK(lua_isnoneornil(L, 1) == 1);
  CHECK(lua_isnone(L, 1) == 0);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_access()
{
  lua_State* L = new_state();
  static int s_light;

  lua_pushnil(L);                              /* 1 */
  lua_pushboolean(L, 1);                       /* 2 */
  lua_pushinteger(L, 3);                       /* 3 */
  lua_pushnumber(L, 3.5);                      /* 4 */
  lua_pushliteral(L, "hello");                 /* 5 */
  lua_newtable(L);                             /* 6 */
  lua_pushcfunction(L, cf_add);                /* 7 */
  void* ud = lua_newuserdatauv(L, 16, 1);      /* 8 */
  lua_pushlightuserdata(L, &s_light);          /* 9 */
  lua_State* L1 = lua_newthread(L);            /* 10 */
  lua_pushliteral(L, "10");                    /* 11: numeric string */

  CHECK(ud != nullptr);

  /* lua_type */
  CHECK(lua_type(L, 1) == LUA_TNIL);
  CHECK(lua_type(L, 2) == LUA_TBOOLEAN);
  CHECK(lua_type(L, 3) == LUA_TNUMBER);
  CHECK(lua_type(L, 4) == LUA_TNUMBER);
  CHECK(lua_type(L, 5) == LUA_TSTRING);
  CHECK(lua_type(L, 6) == LUA_TTABLE);
  CHECK(lua_type(L, 7) == LUA_TFUNCTION);
  CHECK(lua_type(L, 8) == LUA_TUSERDATA);
  CHECK(lua_type(L, 9) == LUA_TLIGHTUSERDATA);
  CHECK(lua_type(L, 10) == LUA_TTHREAD);
  /* acceptable index above the top reads as LUA_TNONE */
  CHECK(lua_type(L, 15) == LUA_TNONE);

  /* typename */
  CHECK(lua_typename(L, LUA_TNIL) == "nil");
  CHECK(lua_typename(L, LUA_TBOOLEAN) == "boolean");
  CHECK(lua_typename(L, LUA_TLIGHTUSERDATA) == "userdata");
  CHECK(lua_typename(L, LUA_TNUMBER) == "number");
  CHECK(lua_typename(L, LUA_TSTRING) == "string");
  CHECK(lua_typename(L, LUA_TTABLE) == "table");
  CHECK(lua_typename(L, LUA_TFUNCTION) == "function");
  CHECK(lua_typename(L, LUA_TUSERDATA) == "userdata");
  CHECK(lua_typename(L, LUA_TTHREAD) == "thread");
  CHECK(lua_typename(L, LUA_TNONE) == "no value");

  /* is* predicates */
  CHECK(lua_isnil(L, 1) == 1);
  CHECK(lua_isboolean(L, 2) == 1);
  CHECK(lua_isnumber(L, 3) == 1);
  CHECK(lua_isnumber(L, 4) == 1);
  CHECK(lua_isnumber(L, 11) == 1);   /* numeric string counts */
  CHECK(lua_isnumber(L, 5) == 0);
  CHECK(lua_isinteger(L, 3) == 1);
  CHECK(lua_isinteger(L, 4) == 0);   /* 3.5 is a float */
  CHECK(lua_isinteger(L, 11) == 0);  /* strings are not integers */
  CHECK(lua_isstring(L, 5) == 1);
  CHECK(lua_isstring(L, 3) == 1);    /* numbers convertible to string */
  CHECK(lua_isstring(L, 6) == 0);
  CHECK(lua_istable(L, 6) == 1);
  CHECK(lua_isfunction(L, 7) == 1);
  CHECK(lua_iscfunction(L, 7) == 1);
  CHECK(lua_isuserdata(L, 8) == 1);
  CHECK(lua_isuserdata(L, 9) == 1);
  CHECK(lua_islightuserdata(L, 9) == 1);
  CHECK(lua_islightuserdata(L, 8) == 0);
  CHECK(lua_isthread(L, 10) == 1);

  /* conversions */
  CHECK(lua_tonumberx(L, 3) == std::optional<lua_Number>(3.0));
  CHECK(lua_tonumberx(L, 4) == std::optional<lua_Number>(3.5));
  CHECK(lua_tonumberx(L, 11) == std::optional<lua_Number>(10.0));
  CHECK(!lua_tonumberx(L, 1).has_value());
  CHECK(!lua_tonumberx(L, 6).has_value());
  CHECK(lua_tointegerx(L, 3) == std::optional<lua_Integer>(3));
  CHECK(!lua_tointegerx(L, 4).has_value()); /* 3.5 has no integer rep */
  CHECK(lua_tointegerx(L, 11) == std::optional<lua_Integer>(10));
  CHECK(lua_tonumber(L, 4) == 3.5);
  CHECK(lua_tonumber(L, 1) == 0.0);          /* default on failure */
  CHECK(lua_tointeger(L, 3) == 3);
  CHECK(lua_tointeger(L, 1) == 0);

  /* toboolean: false and nil are false, everything else true */
  CHECK(lua_toboolean(L, 1) == 0);
  CHECK(lua_toboolean(L, 2) == 1);
  CHECK(lua_toboolean(L, 3) == 1);
  lua_pushboolean(L, 0);
  CHECK(lua_toboolean(L, -1) == 0);
  lua_pushinteger(L, 0);
  CHECK(lua_toboolean(L, -1) == 1); /* zero is true */
  lua_pop(L, 2);

  /* tostring / tolstring */
  {
    std::optional<HostString> s = lua_tostring(L, 5);
    CHECK(s.has_value() && *s == "hello");
    std::string_view sv = lua_tolstring(L, 5);
    CHECK(sv == "hello");
    /* number is converted in place */
    lua_pushinteger(L, 12);
    std::string_view nv = lua_tolstring(L, -1);
    CHECK(nv == "12");
    CHECK(lua_type(L, -1) == LUA_TSTRING);
    lua_pop(L, 1);
    /* non-convertible yields a null view / nullopt */
    std::string_view tv = lua_tolstring(L, 6);
    CHECK(tv.data() == nullptr);
    CHECK(!lua_tostring(L, 6).has_value());
  }

  /* rawlen */
  CHECK(lua_rawlen(L, 5) == 5u);
  CHECK(lua_rawlen(L, 8) == 16u);
  lua_newtable(L);
  for (int i = 1; i <= 3; i++)
  {
    lua_pushinteger(L, i * 10);
    lua_rawseti(L, -2, i);
  }
  CHECK(lua_rawlen(L, -1) == 3u);
  lua_pop(L, 1);
  CHECK(lua_rawlen(L, 1) == 0u); /* other values: 0 */

  /* tocfunction */
  CHECK(lua_tocfunction(L, 7) == cf_add);
  CHECK(lua_tocfunction(L, 6) == nullptr);
  CHECK(load_string(L, "return 1") == LUA_OK);
  CHECK(lua_iscfunction(L, -1) == 0);
  CHECK(lua_tocfunction(L, -1) == nullptr); /* Lua function */
  lua_pop(L, 1);

  /* touserdata */
  CHECK(lua_touserdata(L, 8) == ud);
  CHECK(lua_touserdata(L, 9) == &s_light);
  CHECK(lua_touserdata(L, 6) == nullptr);

  /* tothread */
  CHECK(lua_tothread(L, 10) == L1);
  CHECK(lua_tothread(L, 6) == nullptr);

  /* topointer */
  CHECK(lua_topointer(L, 6) != nullptr);
  /* light C function: topointer returns the function address (official 5.5
     behavior, lapi.c LUA_VLCF case) */
  CHECK(lua_topointer(L, 7) == reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(cf_add)));
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_upvals, 1); /* C closure is collectable */
  CHECK(lua_topointer(L, -1) != nullptr);
  lua_pop(L, 1);
  CHECK(lua_topointer(L, 8) != nullptr);
  CHECK(lua_topointer(L, 10) != nullptr);
  CHECK(lua_topointer(L, 3) == nullptr); /* numbers have no pointer */
  CHECK(lua_topointer(L, 1) == nullptr);

  lua_close(L);
}

static void test_arith()
{
  lua_State* L = new_state();

  struct BinCase
  {
    int op;
    lua_Integer a, b, expected;
  };
  const BinCase intcases[] = {
      {LUA_OPADD, 5, 3, 8},   {LUA_OPSUB, 5, 3, 2},   {LUA_OPMUL, 5, 3, 15},
      {LUA_OPMOD, 7, 3, 1},   {LUA_OPIDIV, 7, 2, 3},  {LUA_OPBAND, 6, 3, 2},
      {LUA_OPBOR, 6, 3, 7},   {LUA_OPBXOR, 6, 3, 5},  {LUA_OPSHL, 1, 4, 16},
      {LUA_OPSHR, 16, 4, 1},
  };
  for (const BinCase& c : intcases)
  {
    lua_pushinteger(L, c.a);
    lua_pushinteger(L, c.b);
    lua_arith(L, c.op);
    CHECK(lua_gettop(L) == 1);
    CHECK(lua_isinteger(L, -1) == 1);
    CHECK(lua_tointeger(L, -1) == c.expected);
    lua_pop(L, 1);
  }

  /* division and power always produce floats */
  lua_pushinteger(L, 7);
  lua_pushinteger(L, 2);
  lua_arith(L, LUA_OPDIV);
  CHECK(lua_isinteger(L, -1) == 0);
  CHECK(lua_tonumber(L, -1) == 3.5);
  lua_pop(L, 1);

  lua_pushinteger(L, 2);
  lua_pushinteger(L, 10);
  lua_arith(L, LUA_OPPOW);
  CHECK(lua_isinteger(L, -1) == 0);
  CHECK(lua_tonumber(L, -1) == 1024.0);
  lua_pop(L, 1);

  /* unary ops take a single operand */
  lua_pushinteger(L, 5);
  lua_arith(L, LUA_OPUNM);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tointeger(L, -1) == -5);
  lua_pop(L, 1);

  lua_pushinteger(L, 0);
  lua_arith(L, LUA_OPBNOT);
  CHECK(lua_tointeger(L, -1) == -1);
  lua_pop(L, 1);

  /* 5.5: string coercion in arithmetic comes from the string library
     metamethods, not the core language */
  {
    lua_State* LS = new_state();
    luaL_openlibs(LS);
    lua_pushliteral(LS, "10");
    lua_pushliteral(LS, "4");
    lua_arith(LS, LUA_OPADD);
    CHECK(lua_tonumber(LS, -1) == 14.0);
    lua_pop(LS, 1);
    lua_close(LS);
  }

  /* metamethod dispatch */
  push_table_with_mm(L, "__add", cf_mm_add);
  lua_pushinteger(L, 1);
  lua_arith(L, LUA_OPADD);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static void test_compare()
{
  lua_State* L = new_state();

  lua_pushinteger(L, 1);
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 1);
  lua_pushliteral(L, "1");

  /* rawequal: no coercions, no metamethods */
  CHECK(lua_rawequal(L, 1, 3) == 1);
  CHECK(lua_rawequal(L, 1, 2) == 0);
  CHECK(lua_rawequal(L, 1, 4) == 0); /* number vs string */
  CHECK(lua_rawequal(L, 1, 15) == 0); /* invalid (but acceptable) index: 0 */
  CHECK(lua_rawequal(L, 15, 16) == 0);

  /* compare */
  CHECK(lua_compare(L, 1, 3, LUA_OPEQ) == 1);
  CHECK(lua_compare(L, 1, 2, LUA_OPEQ) == 0);
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  CHECK(lua_compare(L, 2, 1, LUA_OPLT) == 0);
  CHECK(lua_compare(L, 1, 3, LUA_OPLE) == 1);
  CHECK(lua_compare(L, 2, 1, LUA_OPLE) == 0);
  CHECK(lua_compare(L, 1, 4, LUA_OPEQ) == 0); /* different types */
  CHECK(lua_compare(L, 1, 15, LUA_OPEQ) == 0); /* invalid (acceptable) index */
  lua_settop(L, 0);

  /* int/float mixed comparison */
  lua_pushinteger(L, 1);
  lua_pushnumber(L, 1.5);
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  lua_settop(L, 0);

  /* string comparison is lexicographic */
  lua_pushliteral(L, "abc");
  lua_pushliteral(L, "abd");
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  lua_settop(L, 0);

  /* __eq metamethod for tables */
  push_table_with_mm(L, "__eq", cf_mm_true);
  push_table_with_mm(L, "__eq", cf_mm_true);
  CHECK(lua_rawequal(L, 1, 2) == 0);
  CHECK(lua_compare(L, 1, 2, LUA_OPEQ) == 1);
  /* __lt metamethod */
  push_table_with_mm(L, "__lt", cf_mm_true);
  push_table_with_mm(L, "__lt", cf_mm_true);
  CHECK(lua_compare(L, 3, 4, LUA_OPLT) == 1);
  lua_settop(L, 0);

  lua_close(L);
}

static void test_push()
{
  lua_State* L = new_state();

  /* pushlstring: substring selection, embedded zeros, returned view */
  {
    std::string_view src = "hello world";
    std::string_view r = lua_pushlstring(L, src, 6, 5);
    CHECK(r == "world");
    CHECK(lua_tolstring(L, -1) == "world");
    lua_pop(L, 1);

    std::string_view withnul("a\0b", 3);
    r = lua_pushlstring(L, withnul, 0, 3);
    CHECK(r.size() == 3);
    CHECK(lua_rawlen(L, -1) == 3u);
    std::string_view back = lua_tolstring(L, -1);
    CHECK(back.size() == 3 && std::memcmp(back.data(), "a\0b", 3) == 0);
    lua_pop(L, 1);

    r = lua_pushlstring(L, std::string_view(), 0, 0);
    CHECK(r.empty());
    CHECK(lua_rawlen(L, -1) == 0u);
    lua_pop(L, 1);
  }

  /* pushstring: value, and nullopt pushes nil (like NULL in C API) */
  lua_pushstring(L, "abc"_hs);
  CHECK(lua_tolstring(L, -1) == "abc");
  lua_pushstring(L, std::nullopt);
  CHECK(lua_isnil(L, -1) == 1);
  lua_pop(L, 2);

  /* pushliteral */
  lua_pushliteral(L, "lit");
  CHECK(lua_tolstring(L, -1) == "lit");
  lua_pop(L, 1);

  /* pushfstring: args are taken from the stack, popped, result pushed */
  {
    int t0 = lua_gettop(L);
    lua_pushliteral(L, "abc");
    lua_pushinteger(L, 7);
    HostString r = lua_pushfstring(L, "s=%s d=%d");
    CHECK(r == "s=abc d=7");
    CHECK(lua_gettop(L) == t0 + 1);
    CHECK(lua_tolstring(L, -1) == "s=abc d=7");
    lua_pop(L, 1);

    lua_pushinteger(L, 123);
    r = lua_pushfstring(L, "I=%I");
    CHECK(r == "I=123");
    lua_pop(L, 1);

    lua_pushinteger(L, 65);
    r = lua_pushfstring(L, "c=%c");
    CHECK(r == "c=A");
    lua_pop(L, 1);

    lua_pushnumber(L, 2.5);
    r = lua_pushfstring(L, "f=%f");
    CHECK(r == "f=2.5");
    lua_pop(L, 1);

    lua_pushinteger(L, 0x20AC);
    r = lua_pushfstring(L, "%U");
    CHECK(r == "\xE2\x82\xAC");
    lua_pop(L, 1);

    r = lua_pushfstring(L, "100%%");
    CHECK(r == "100%");
    lua_pop(L, 1);

    r = lua_pushfstring(L, "plain");
    CHECK(r == "plain");
    lua_pop(L, 1);
  }

  /* pushcclosure with upvalues, read back via upvalueindex */
  lua_pushinteger(L, 30);
  lua_pushinteger(L, 12);
  lua_pushcclosure(L, cf_upvals, 2);
  CHECK(lua_gettop(L) == 1); /* upvalues consumed */
  CHECK(lua_iscfunction(L, -1) == 1);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  /* pushboolean normalizes to true/false */
  lua_pushboolean(L, 5);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pushboolean(L, 0);
  CHECK(lua_toboolean(L, -1) == 0);
  lua_pop(L, 2);

  /* pushthread: 1 for main thread, 0 for coroutines */
  CHECK(lua_pushthread(L) == 1);
  CHECK(lua_tothread(L, -1) == L);
  lua_State* L1 = lua_newthread(L);
  CHECK(lua_pushthread(L1) == 0);
  CHECK(lua_tothread(L1, -1) == L1);
  lua_pop(L, 2);

  /* pushglobaltable */
  lua_pushglobaltable(L);
  CHECK(lua_istable(L, -1) == 1);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_get_set()
{
  lua_State* L = new_state();
  static int s_key1, s_key2;

  /* globals */
  lua_pushinteger(L, 11);
  lua_setglobal(L, "gv");
  int t = lua_getglobal(L, "gv");
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 11);
  t = lua_getglobal(L, "nosuchglobal");
  CHECK(t == LUA_TNIL);
  lua_pop(L, 2);

  /* register */
  lua_register(L, "add", cf_add);
  CHECK(lua_getglobal(L, "add") == LUA_TFUNCTION);
  lua_pop(L, 1);

  /* createtable / settable / gettable */
  lua_createtable(L, 4, 4);
  int tidx = lua_gettop(L);
  lua_pushliteral(L, "k");
  lua_pushinteger(L, 5);
  lua_settable(L, tidx);
  lua_pushliteral(L, "k");
  t = lua_gettable(L, tidx);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 5);
  lua_pop(L, 1);

  /* setfield / getfield */
  lua_pushinteger(L, 6);
  lua_setfield(L, tidx, "f");
  t = lua_getfield(L, tidx, "f");
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 6);
  CHECK(lua_getfield(L, tidx, "missing") == LUA_TNIL);
  lua_pop(L, 2);

  /* seti / geti */
  lua_pushinteger(L, 7);
  lua_seti(L, tidx, 3);
  t = lua_geti(L, tidx, 3);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  /* rawset / rawget */
  lua_pushliteral(L, "rk");
  lua_pushinteger(L, 8);
  lua_rawset(L, tidx);
  lua_pushliteral(L, "rk");
  t = lua_rawget(L, tidx);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 8);
  lua_pop(L, 1);

  /* rawseti / rawgeti */
  lua_pushinteger(L, 9);
  lua_rawseti(L, tidx, 4);
  t = lua_rawgeti(L, tidx, 4);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 9);
  lua_pop(L, 1);

  /* rawsetp / rawgetp */
  lua_pushinteger(L, 10);
  lua_rawsetp(L, tidx, &s_key1);
  t = lua_rawgetp(L, tidx, &s_key1);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 10);
  CHECK(lua_rawgetp(L, tidx, &s_key2) == LUA_TNIL);
  lua_pop(L, 2);
  lua_pop(L, 1); /* table */

  /* metatable get/set */
  lua_newtable(L);
  CHECK(lua_getmetatable(L, -1) == 0); /* none: pushes nothing */
  lua_newtable(L); /* metatable */
  lua_pushvalue(L, -1);
  lua_insert(L, -3); /* keep a copy below: [mtcopy, obj, mt] */
  CHECK(lua_setmetatable(L, -2) == 1);
  CHECK(lua_getmetatable(L, -1) == 1);
  CHECK(lua_rawequal(L, -1, -3) == 1); /* same metatable back */
  lua_pop(L, 3);

  /* __index / __newindex metamethods via gettable/settable */
  push_table_with_mm(L, "__index", cf_mm_index);
  lua_pushliteral(L, "anything");
  t = lua_gettable(L, -2);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  lua_pushliteral(L, "anything");
  CHECK(lua_rawget(L, -2) == LUA_TNIL); /* raw access skips __index */
  lua_pop(L, 2);

  g_newindex_hits = 0;
  push_table_with_mm(L, "__newindex", cf_mm_newindex);
  lua_pushliteral(L, "x");
  lua_pushinteger(L, 1);
  lua_settable(L, -3);
  CHECK(g_newindex_hits == 1);
  lua_pushliteral(L, "x");
  CHECK(lua_rawget(L, -2) == LUA_TNIL); /* __newindex intercepted the store */
  lua_pop(L, 2);

  /* userdata with user values */
  lua_newuserdatauv(L, 8, 2);
  int uidx = lua_gettop(L);
  CHECK(lua_getiuservalue(L, uidx, 1) == LUA_TNIL); /* starts as nil */
  lua_pop(L, 1);
  lua_pushinteger(L, 21);
  CHECK(lua_setiuservalue(L, uidx, 1) != 0);
  lua_pushinteger(L, 22);
  CHECK(lua_setiuservalue(L, uidx, 2) != 0);
  CHECK(lua_getiuservalue(L, uidx, 1) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 21);
  lua_pop(L, 1);
  CHECK(lua_getiuservalue(L, uidx, 2) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 22);
  lua_pop(L, 1);
  /* out-of-range user value */
  CHECK(lua_getiuservalue(L, uidx, 3) == LUA_TNONE);
  CHECK(lua_isnil(L, -1) == 1); /* still pushes nil */
  lua_pop(L, 1);
  lua_pushnil(L);
  CHECK(lua_setiuservalue(L, uidx, 3) == 0);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_call()
{
  lua_State* L = new_state();
  luaL_openselectedlibs(L, LUA_GLIBK, 0); /* base lib: error(), etc. */

  /* lua_call with C function */
  lua_pushcfunction(L, cf_add);
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 40);
  lua_call(L, 2, 1);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  /* Lua function, multiple results, LUA_MULTRET */
  CHECK(do_string(L, "return 1, 2, 3") == LUA_OK);
  CHECK(lua_gettop(L) == 3);
  CHECK(lua_tointeger(L, 1) == 1);
  CHECK(lua_tointeger(L, 3) == 3);
  lua_settop(L, 0);

  /* fixed nresults pads with nil / truncates */
  CHECK(load_string(L, "return 1, 2") == LUA_OK);
  lua_call(L, 0, 4);
  CHECK(lua_gettop(L) == 4);
  CHECK(lua_isnil(L, 4) == 1);
  lua_settop(L, 0);

  /* pcall catches errors; func+args removed, error object pushed */
  int base = lua_gettop(L);
  lua_pushcfunction(L, cf_raise);
  lua_pushinteger(L, 1); /* an arg, to check cleanup */
  int st = lua_pcall(L, 1, 0, 0);
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_gettop(L) == base + 1);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value() && *msg == "boom");
  }
  lua_pop(L, 1);

  /* runtime errors from Lua code carry position info */
  st = do_string(L, "local x = nil\nreturn x.y");
  CHECK(st == LUA_ERRRUN);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value());
    if (msg.has_value())
      CHECK(msg->to_utf8().find("tpa:2") != std::string_view::npos);
  }
  lua_pop(L, 1);

  /* message handler transforms the error */
  lua_pushcfunction(L, cf_msgh);
  int mh = lua_gettop(L);
  lua_pushcfunction(L, cf_raise);
  st = lua_pcall(L, 0, 0, mh);
  CHECK(st == LUA_ERRRUN);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value() && *msg == "H:boom");
  }
  lua_pop(L, 2);

  /* error handler that itself errors -> LUA_ERRERR */
  lua_pushcfunction(L, cf_raise);
  mh = lua_gettop(L);
  lua_pushcfunction(L, cf_raise);
  st = lua_pcall(L, 0, 0, mh);
  CHECK(st == LUA_ERRERR);
  lua_pop(L, 2);

  /* 5.5: a nil error object is replaced by a string message */
  lua_pushcfunction(L, cf_raise_nil);
  st = lua_pcall(L, 0, 0, 0);
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  /* non-string error objects are preserved */
  st = do_string(L, "error({code = 5})");
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "code") == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 5);
  lua_pop(L, 2);

  /* pcallk with immediate error behaves like pcall (k not invoked) */
  g_cont_status = -999;
  lua_pushcfunction(L, cf_raise);
  st = lua_pcallk(L, 0, 0, 0, 0, cont_call);
  CHECK(st == LUA_ERRRUN);
  CHECK(g_cont_status == -999);
  lua_pop(L, 1);

  /* callk without a yield: continuation called manually per protocol */
  g_cont_status = -999;
  g_cont_ctx = 0;
  lua_pushcfunction(L, cf_callk);
  lua_pushcfunction(L, cf_ret7);
  lua_call(L, 1, LUA_MULTRET);
  CHECK(g_cont_status == LUA_OK);
  CHECK(g_cont_ctx == 42);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_settop(L, 0);

  /* calling a callable table (__call metamethod) */
  push_table_with_mm(L, "__call", cf_ret7);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_load_dump()
{
  lua_State* L = new_state();

  /* good chunk */
  CHECK(load_string(L, "return 40 + 2") == LUA_OK);
  CHECK(lua_type(L, -1) == LUA_TFUNCTION);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  /* syntax error: LUA_ERRSYNTAX plus message */
  int base = lua_gettop(L);
  CHECK(load_string(L, "return +++") == LUA_ERRSYNTAX);
  CHECK(lua_gettop(L) == base + 1);
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  /* reader protocol: chunk delivered in 1-byte pieces */
  {
    ByteReader r{"return 6 * 7", 0};
    CHECK(lua_load(L, byte_reader, &r, "=bytes"_hs, std::nullopt) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
  }

  /* dump and reload */
  {
    CHECK(load_string(L, "return 10 + 11") == LUA_OK);
    DumpBuf d;
    int st = lua_dump(L, dump_writer, &d, 0);
    CHECK(st == 0);
    CHECK(!d.bytes.empty());
    CHECK(d.empty_calls >= 1);      /* 5.5: writer signaled end of dump */
    CHECK(d.empty_was_last);
    CHECK(lua_type(L, -1) == LUA_TFUNCTION); /* stack preserved */
    lua_pop(L, 1);

    /* binary chunk starts with the signature */
    CHECK(d.bytes.size() >= 4 &&
          std::string_view(d.bytes).substr(0, 4) == LUA_SIGNATURE);

    /* reload in binary mode */
    StringReader r{std::string_view(d.bytes), false};
    CHECK(lua_load(L, whole_reader, &r, "=dumped"_hs, "b"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 21);
    lua_pop(L, 1);

    /* mode "t" rejects binary chunks */
    StringReader r2{std::string_view(d.bytes), false};
    int lst = lua_load(L, whole_reader, &r2, "=dumped"_hs, "t"_hs);
    CHECK(lst != LUA_OK);
    lua_pop(L, 1);

    /* stripped dump also loads and runs */
    CHECK(load_string(L, "return 10 + 11") == LUA_OK);
    DumpBuf ds;
    CHECK(lua_dump(L, dump_writer, &ds, 1) == 0);
    lua_pop(L, 1);
    CHECK(!ds.bytes.empty() && ds.bytes.size() <= d.bytes.size());
    StringReader r3{std::string_view(ds.bytes), false};
    CHECK(lua_load(L, whole_reader, &r3, std::nullopt, "b"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 21);
    lua_pop(L, 1);
  }

  /* writer error code is returned */
  {
    CHECK(load_string(L, "return 1") == LUA_OK);
    int calls = 0;
    int st = lua_dump(L, failing_writer, &calls, 0);
    CHECK(st == 33);
    CHECK(calls == 1); /* nonzero return stops further calls */
    lua_pop(L, 1);
  }

  lua_close(L);
}

static void test_coroutines()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  /* C coroutine with yieldk continuation */
  {
    lua_State* L1 = lua_newthread(L);
    CHECK(lua_status(L1) == LUA_OK);
    CHECK(lua_isyieldable(L) == 0); /* main thread can't yield */

    lua_pushcfunction(L1, cf_co);
    lua_pushinteger(L1, 1);
    lua_ResumeResult rr = lua_resume(L1, L, 1);
    CHECK(rr.status == LUA_YIELD);
    CHECK(rr.nres == 1);
    CHECK(lua_tointeger(L1, -1) == 100);
    CHECK(lua_status(L1) == LUA_YIELD);

    lua_pop(L1, 1); /* remove yielded values */
    g_cont_status = -999;
    lua_pushinteger(L1, 8);
    lua_pushinteger(L1, 9);
    rr = lua_resume(L1, L, 2);
    CHECK(rr.status == LUA_OK);
    CHECK(g_cont_status == LUA_YIELD); /* continuation invoked on resume */
    CHECK(g_cont_ctx == 7);
    CHECK(rr.nres >= 2);
    CHECK(lua_tointeger(L1, -1) == 9);
    CHECK(lua_tointeger(L1, -2) == 8);
    CHECK(lua_status(L1) == LUA_OK);

    /* finished coroutine cannot be resumed.
       The stack must be cleared first: a finished coroutine has status
       LUA_OK, and "dead" is detected as "no function under the args"
       (top - (func+1) == nargs). With results still on the stack, resume
       would instead try to call the bottom slot as a function — verified
       identical in upstream 5.5 (coroutine.resume moves results off via
       lua_xmove before this point, so its stack is empty when dead). */
    lua_settop(L1, 0);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    CHECK(rr.nres == 1); /* the error object */
    {
      std::optional<HostString> msg = lua_tostring(L1, -1);
      CHECK(msg.has_value());
      if (msg.has_value())
        CHECK(msg->to_utf8().find("dead coroutine") != std::string_view::npos);
    }
    lua_pop(L, 1); /* thread */
  }

  /* plain lua_yield in tail position */
  {
    lua_State* L1 = lua_newthread(L);
    lua_pushcfunction(L1, cf_yield_tail);
    lua_pushinteger(L1, 5);
    lua_pushinteger(L1, 6);
    lua_ResumeResult rr = lua_resume(L1, L, 2);
    CHECK(rr.status == LUA_YIELD);
    CHECK(rr.nres == 2);
    CHECK(lua_tointeger(L1, -2) == 5);
    CHECK(lua_tointeger(L1, -1) == 6);
    lua_pop(L1, 2);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_OK);
    lua_pop(L, 1);
  }

  /* Lua coroutine: values flow both ways across resume/yield */
  {
    lua_State* L1 = lua_newthread(L);
    CHECK(load_string(L1,
        "local a, b = ...\n"
        "local c = coroutine.yield(a + b)\n"
        "return c * 2") == LUA_OK);
    lua_pushinteger(L1, 3);
    lua_pushinteger(L1, 4);
    lua_ResumeResult rr = lua_resume(L1, L, 2);
    CHECK(rr.status == LUA_YIELD);
    CHECK(rr.nres == 1);
    CHECK(lua_tointeger(L1, -1) == 7);
    lua_pop(L1, 1);
    lua_pushinteger(L1, 21);
    rr = lua_resume(L1, L, 1);
    CHECK(rr.status == LUA_OK);
    CHECK(rr.nres == 1);
    CHECK(lua_tointeger(L1, -1) == 42);
    lua_pop(L, 1);
  }

  /* yield across lua_callk with continuation */
  {
    lua_State* L1 = lua_newthread(L);
    lua_pushcfunction(L1, cf_callk);
    CHECK(load_string(L1, "coroutine.yield(5)\nreturn 9") == LUA_OK);
    g_cont_status = -999;
    lua_ResumeResult rr = lua_resume(L1, L, 1);
    CHECK(rr.status == LUA_YIELD);
    CHECK(rr.nres == 1);
    CHECK(lua_tointeger(L1, -1) == 5);
    lua_pop(L1, 1);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_OK);
    CHECK(g_cont_status == LUA_YIELD);
    CHECK(g_cont_ctx == 42);
    CHECK(lua_tointeger(L1, -1) == 9);
    lua_pop(L, 1);
  }

  /* error inside coroutine: status + error object; closethread resets */
  {
    lua_State* L1 = lua_newthread(L);
    CHECK(load_string(L1, "error('cofail')") == LUA_OK);
    lua_ResumeResult rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    {
      std::optional<HostString> msg = lua_tostring(L1, -1);
      CHECK(msg.has_value());
      if (msg.has_value())
        CHECK(msg->to_utf8().find("cofail") != std::string_view::npos);
    }
    CHECK(lua_status(L1) == LUA_ERRRUN);
    /* dead coroutine cannot be resumed. The error object must be removed
       first: lua_resume's error path sets ci->top == top (frame exactly
       full), so resume_error's push of the message trips the api_incr_top
       stack check if the error object is still there. Upstream 5.5 with
       LUA_USE_APICHECK asserts identically (ldo.c resume_error) — verified,
       not a divergence. */
    lua_settop(L1, 0);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    /* closethread on an errored thread: returns the error status with the
       error object on top; the thread itself is reset to LUA_OK */
    int cst = lua_closethread(L1, L);
    CHECK(cst == LUA_ERRRUN);
    CHECK(lua_status(L1) == LUA_OK);
    CHECK(lua_gettop(L1) == 1);
    CHECK(lua_type(L1, -1) == LUA_TSTRING);
    lua_settop(L1, 0);
    /* once reset, a fresh function can run on it */
    CHECK(load_string(L1, "return 'revived'") == LUA_OK);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_OK);
    lua_pop(L, 1);
  }

  /* xmove */
  {
    lua_State* L1 = lua_newthread(L);
    lua_pushinteger(L, 71);
    lua_pushinteger(L, 72);
    lua_xmove(L, L1, 2);
    CHECK(lua_gettop(L1) == 2);
    CHECK(lua_tointeger(L1, 1) == 71);
    CHECK(lua_tointeger(L1, 2) == 72);
    CHECK(lua_type(L, -1) == LUA_TTHREAD); /* only the thread remains */
    lua_pop(L, 1);
  }

  lua_close(L);
}

static void test_warnings()
{
  lua_State* L = new_state();
  WarnLog w;
  lua_setwarnf(L, warn_collect, &w);

  lua_warning(L, "part1;", 1);
  CHECK(w.calls == 1);
  CHECK(w.last_tocont == 1);
  lua_warning(L, "part2", 0);
  CHECK(w.calls == 2);
  CHECK(w.last_tocont == 0);
  CHECK(w.text == "part1;part2");

  /* disable warnings */
  lua_setwarnf(L, nullptr, nullptr);
  lua_warning(L, "ignored", 0);
  CHECK(w.calls == 2);

  lua_close(L);
}

static void test_gc()
{
  lua_State* L = new_state();

  lua_gc(L, LUA_GCSTOP, 0, 0, 0);
  CHECK(lua_gc(L, LUA_GCISRUNNING, 0, 0, 0) == 0);
  lua_gc(L, LUA_GCRESTART, 0, 0, 0);
  CHECK(lua_gc(L, LUA_GCISRUNNING, 0, 0, 0) != 0);

  int kb = lua_gc(L, LUA_GCCOUNT, 0, 0, 0);
  CHECK(kb >= 0);
  int b = lua_gc(L, LUA_GCCOUNTB, 0, 0, 0);
  CHECK(b >= 0 && b < 1024);

  CHECK(lua_gc(L, LUA_GCCOLLECT, 0, 0, 0) == 0);
  lua_gc(L, LUA_GCSTEP, 100, 0, 0);

  /* mode switches return the previous mode */
  int prev = lua_gc(L, LUA_GCINC, 0, 0, 0);
  CHECK(prev == LUA_GCGEN || prev == LUA_GCINC);
  prev = lua_gc(L, LUA_GCGEN, 0, 0, 0);
  CHECK(prev == LUA_GCINC);
  prev = lua_gc(L, LUA_GCINC, 0, 0, 0);
  CHECK(prev == LUA_GCGEN);

  /* parameters: -1 reads, otherwise sets and returns previous */
  int cur = lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, -1);
  CHECK(cur >= 0);
  int old = lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, 300);
  CHECK(old == cur);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, -1) == 300);
  lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, cur);
  /* all documented params are readable */
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMINORMUL, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMAJORMINOR, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMINORMAJOR, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPSTEPMUL, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPSTEPSIZE, -1) >= 0);

  /* __gc finalizer runs on collection */
  g_gc_hits = 0;
  lua_newuserdatauv(L, 4, 0);
  lua_newtable(L);
  lua_pushcfunction(L, cf_mm_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(g_gc_hits == 1);

  lua_close(L);
}

static void test_misc()
{
  lua_State* L = new_state();

  /* lua_next */
  lua_newtable(L);
  int tidx = lua_gettop(L);
  for (int i = 1; i <= 3; i++)
  {
    lua_pushinteger(L, i * 10);
    lua_rawseti(L, tidx, i);
  }
  lua_pushinteger(L, 10);
  lua_setfield(L, tidx, "x");
  int cnt = 0;
  lua_pushnil(L);
  while (lua_next(L, tidx) != 0)
  {
    cnt++;
    CHECK(lua_gettop(L) == tidx + 2); /* key + value */
    lua_pop(L, 1); /* keep key for next iteration */
  }
  CHECK(cnt == 4);
  CHECK(lua_gettop(L) == tidx);
  /* empty table iterates zero times */
  lua_newtable(L);
  lua_pushnil(L);
  CHECK(lua_next(L, -2) == 0);
  lua_pop(L, 2);

  /* lua_concat */
  lua_settop(L, 0);
  lua_pushliteral(L, "a");
  lua_pushinteger(L, 1);
  lua_pushliteral(L, "b");
  lua_concat(L, 3);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tolstring(L, -1) == "a1b");
  lua_pop(L, 1);
  lua_concat(L, 0); /* pushes empty string */
  CHECK(lua_tolstring(L, -1) == "");
  lua_pop(L, 1);
  lua_pushinteger(L, 5);
  lua_concat(L, 1); /* single value: unchanged */
  CHECK(lua_type(L, -1) == LUA_TNUMBER);
  lua_pop(L, 1);
  /* __concat metamethod */
  push_table_with_mm(L, "__concat", cf_mm_concat);
  lua_pushliteral(L, "z");
  lua_concat(L, 2);
  CHECK(lua_tolstring(L, -1) == "CC");
  lua_pop(L, 1);

  /* lua_len */
  lua_pushliteral(L, "abcd");
  lua_len(L, -1);
  CHECK(lua_tointeger(L, -1) == 4);
  lua_pop(L, 2);
  lua_newtable(L);
  for (int i = 1; i <= 3; i++)
  {
    lua_pushinteger(L, i);
    lua_rawseti(L, -2, i);
  }
  lua_len(L, -1);
  CHECK(lua_tointeger(L, -1) == 3);
  lua_pop(L, 2);
  push_table_with_mm(L, "__len", cf_mm_len);
  lua_len(L, -1);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 2);

  /* lua_stringtonumber: returns string size + 1 on success */
  {
    size_t r = lua_stringtonumber(L, "42");
    CHECK(r == 3u);
    CHECK(lua_isinteger(L, -1) == 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    r = lua_stringtonumber(L, "3.5");
    CHECK(r == 4u);
    CHECK(lua_isinteger(L, -1) == 0);
    CHECK(lua_tonumber(L, -1) == 3.5);
    lua_pop(L, 1);

    r = lua_stringtonumber(L, "0x10");
    CHECK(r == 5u);
    CHECK(lua_tointeger(L, -1) == 16);
    lua_pop(L, 1);

    r = lua_stringtonumber(L, " 42 "); /* surrounding spaces allowed */
    CHECK(r == 5u);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    int top = lua_gettop(L);
    CHECK(lua_stringtonumber(L, "abc") == 0u);
    CHECK(lua_stringtonumber(L, "") == 0u);
    CHECK(lua_gettop(L) == top); /* nothing pushed on failure */
  }

  /* lua_numbertocstring */
  {
    lua_pushinteger(L, 42);
    std::optional<HostString> s = lua_numbertocstring(L, -1);
    CHECK(s.has_value() && *s == "42");
    lua_pop(L, 1);
    lua_pushnumber(L, 0.5);
    s = lua_numbertocstring(L, -1);
    CHECK(s.has_value() && *s == "0.5");
    CHECK(lua_type(L, -1) == LUA_TNUMBER); /* value not modified */
    lua_pop(L, 1);
    lua_pushliteral(L, "42"); /* strings are not numbers here */
    s = lua_numbertocstring(L, -1);
    CHECK(!s.has_value());
    lua_pop(L, 1);
  }

  /* allocator access */
  {
    lua_Allocator a = lua_getallocf(L);
    CHECK(a.f == counting_alloc);
    CHECK(a.ud == &g_alloc_stats);
    unsigned long before = g_alloc_stats.calls;
    lua_pushlstring(L, "force an allocation with a long enough string", 0, 45);
    lua_pop(L, 1);
    CHECK(g_alloc_stats.calls > before);

    AllocStats other;
    lua_setallocf(L, counting_alloc, &other);
    lua_Allocator a2 = lua_getallocf(L);
    CHECK(a2.ud == &other);
    lua_setallocf(L, counting_alloc, &g_alloc_stats); /* restore */
  }

  /* extra space: application-usable raw memory associated with the state.
     NOTE: LUA_DEBUG builds use it internally for the lock counter (as does
     official Lua's ltests.h), so save and restore the original content. */
  {
    void* es = lua_getextraspace(L);
    CHECK(es != nullptr);
    void* saved;
    std::memcpy(&saved, es, sizeof(void*));
    void* marker = &saved;
    std::memcpy(es, &marker, sizeof(void*));
    void* back;
    std::memcpy(&back, lua_getextraspace(L), sizeof(void*));
    CHECK(back == marker);
    std::memcpy(es, &saved, sizeof(void*));
  }

  /* to-be-closed slots */
  g_closed = 0;
  lua_pushcfunction(L, cf_tbc_closeslot);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_OK);
  lua_pushcfunction(L, cf_tbc_settop);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_OK);
  CHECK(g_closed == 2);

  lua_close(L);
}

static void test_debug_api()
{
  lua_State* L = new_state();

  /* getinfo with '>' consumes the function on top */
  CHECK(load_string(L, "local function inner(a, b, ...) end\nreturn inner") == LUA_OK);
  lua_call(L, 0, 1);
  int top = lua_gettop(L);
  lua_pushvalue(L, -1);
  lua_Debug query{};
  std::optional<lua_Debug> info = lua_getinfo(L, ">Su", query);
  CHECK(lua_gettop(L) == top); /* '>' popped the function */
  CHECK(info.has_value());
  if (info.has_value())
  {
    CHECK(info->what == "Lua");
    CHECK(info->source == "=tpa");
    CHECK(info->short_src == "tpa");
    CHECK(info->linedefined == 1);
    CHECK(info->nparams == 2);
    CHECK(info->isvararg != 0);
    CHECK(info->nups == 0);
  }
  /* 'L' pushes the table of valid lines */
  lua_pushvalue(L, -1);
  info = lua_getinfo(L, ">L", query);
  CHECK(info.has_value());
  CHECK(lua_istable(L, -1) == 1);
  lua_pop(L, 2);

  /* invalid option rejected */
  lua_pushcfunction(L, cf_add);
  info = lua_getinfo(L, ">Z", query);
  CHECK(!info.has_value());

  /* main chunk info: what == "main" */
  CHECK(load_string(L, "return 1") == LUA_OK);
  info = lua_getinfo(L, ">S", query);
  CHECK(info.has_value());
  if (info.has_value())
    CHECK(info->what == "main");

  /* C function info via '>' */
  lua_pushcfunction(L, cf_add);
  info = lua_getinfo(L, ">S", query);
  CHECK(info.has_value());
  if (info.has_value())
  {
    CHECK(info->what == "C");
    CHECK(info->linedefined == -1);
    CHECK(info->lastlinedefined == -1);
  }

  /* runtime stack introspection + locals from inside a call */
  lua_register(L, "probe", cf_probe);
  CHECK(do_string(L, "local xyz = 42\nprobe()\nreturn xyz") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 99); /* setlocal changed it */
  lua_pop(L, 1);

  /* getstack fails outside any call */
  CHECK(!lua_getstack(L, 0).has_value());

  /* getlocal with NULL ar: parameter names of the function on top */
  CHECK(load_string(L, "local function f(alpha, beta) end\nreturn f") == LUA_OK);
  lua_call(L, 0, 1);
  top = lua_gettop(L);
  {
    std::optional<HostString> p1 = lua_getlocal(L, nullptr, 1);
    CHECK(p1.has_value() && *p1 == "alpha");
    std::optional<HostString> p2 = lua_getlocal(L, nullptr, 2);
    CHECK(p2.has_value() && *p2 == "beta");
    std::optional<HostString> p3 = lua_getlocal(L, nullptr, 3);
    CHECK(!p3.has_value());
    CHECK(lua_gettop(L) == top); /* pushes nothing in this mode */
  }
  lua_pop(L, 1);

  /* upvalues of a Lua closure */
  CHECK(do_string(L, "local u = 10\nreturn function() return u end") == LUA_OK);
  int fidx = lua_gettop(L);
  {
    std::optional<HostString> uname = lua_getupvalue(L, fidx, 1);
    CHECK(uname.has_value() && *uname == "u");
    CHECK(lua_tointeger(L, -1) == 10);
    lua_pop(L, 1);
    CHECK(!lua_getupvalue(L, fidx, 2).has_value());

    lua_pushinteger(L, 20);
    std::optional<HostString> sname = lua_setupvalue(L, fidx, 1);
    CHECK(sname.has_value() && *sname == "u");
    lua_pushvalue(L, fidx);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 20);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* upvalues of a C closure have empty names */
  lua_pushinteger(L, 1);
  lua_pushinteger(L, 2);
  lua_pushcclosure(L, cf_upvals, 2);
  {
    std::optional<HostString> uname = lua_getupvalue(L, -1, 1);
    CHECK(uname.has_value() && *uname == "");
    CHECK(lua_tointeger(L, -1) == 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* upvalueid: shared upvalues have the same id */
  CHECK(do_string(L,
      "local x = 1\n"
      "local function a() return x end\n"
      "local function b() return x end\n"
      "return a, b") == LUA_OK);
  int bidx = lua_gettop(L);
  int aidx = bidx - 1;
  CHECK(lua_upvalueid(L, aidx, 1) != nullptr);
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));
  lua_pop(L, 2);

  /* upvaluejoin: distinct upvalues become shared */
  CHECK(do_string(L,
      "local p = 1\n"
      "local q = 2\n"
      "local function a() return p end\n"
      "local function b() return q end\n"
      "return a, b") == LUA_OK);
  bidx = lua_gettop(L);
  aidx = bidx - 1;
  CHECK(lua_upvalueid(L, aidx, 1) != lua_upvalueid(L, bidx, 1));
  lua_upvaluejoin(L, bidx, 1, aidx, 1);
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));
  lua_pushvalue(L, bidx);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 1); /* b now sees a's upvalue */
  lua_pop(L, 3);

  /* hooks */
  g_hook_calls = g_hook_rets = g_hook_lines = g_hook_counts = 0;
  lua_sethook(L, counting_hook, LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
  CHECK(lua_gethook(L) == counting_hook);
  CHECK(lua_gethookmask(L) == (LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT));
  CHECK(lua_gethookcount(L) == 1);
  CHECK(do_string(L, "local function f() return 1 end\nlocal s = 0\nfor i = 1, 3 do s = s + f() end\nreturn s") == LUA_OK);
  lua_pop(L, 1);
  CHECK(g_hook_calls > 0);
  CHECK(g_hook_rets > 0);
  CHECK(g_hook_lines > 0);
  CHECK(g_hook_counts > 0);
  lua_sethook(L, nullptr, 0, 0);
  CHECK(lua_gethook(L) == nullptr);
  CHECK(lua_gethookmask(L) == 0);
  CHECK(lua_gethookcount(L) == 0);

  lua_close(L);
}

static void test_libraries()
{
  /* luaL_openlibs: all standard globals present */
  {
    lua_State* L = new_state();
    luaL_openlibs(L);
    const char* const globals[] = {"string", "table", "math", "os", "io",
                                   "coroutine", "debug", "utf8", "package"};
    for (const char* g : globals)
    {
      CHECK(lua_getglobal(L, g) == LUA_TTABLE);
      lua_pop(L, 1);
    }
    /* base library */
    CHECK(lua_getglobal(L, "print") == LUA_TFUNCTION);
    CHECK(lua_getglobal(L, "_G") == LUA_TTABLE);
    CHECK(lua_getglobal(L, "_VERSION") == LUA_TSTRING);
    {
      std::optional<HostString> v = lua_tostring(L, -1);
      CHECK(v.has_value() && *v == "Lua 5.5");
    }
    lua_pop(L, 3);

    /* stdlib smoke test through the C API */
    CHECK(do_string(L, "return string.rep('ab', 3)") == LUA_OK);
    CHECK(lua_tolstring(L, -1) == "ababab");
    lua_pop(L, 1);
    lua_close(L);
  }

  /* luaL_openselectedlibs: only requested libs are loaded */
  {
    lua_State* L = new_state();
    luaL_openselectedlibs(L, LUA_GLIBK | LUA_MATHLIBK, 0);
    CHECK(lua_getglobal(L, "math") == LUA_TTABLE);
    CHECK(lua_getglobal(L, "print") == LUA_TFUNCTION);
    CHECK(lua_getglobal(L, "string") == LUA_TNIL);
    CHECK(lua_getglobal(L, "io") == LUA_TNIL);
    lua_pop(L, 4);
    lua_close(L);
  }

  /* individual luaopen_* functions are plain C functions */
  {
    lua_State* L = new_state();
    lua_pushcfunction(L, luaopen_string);
    lua_pushliteral(L, "string");
    lua_call(L, 1, 1);
    CHECK(lua_istable(L, -1) == 1);
    CHECK(lua_getfield(L, -1, "rep") == LUA_TFUNCTION);
    lua_pop(L, 2);
    lua_close(L);
  }

  /* library name constants */
  CHECK(LUA_COLIBNAME->view() == "coroutine");
  CHECK(LUA_TABLIBNAME->view() == "table");
  CHECK(LUA_IOLIBNAME->view() == "io");
  CHECK(LUA_OSLIBNAME->view() == "os");
  CHECK(LUA_STRLIBNAME->view() == "string");
  CHECK(LUA_UTF8LIBNAME->view() == "utf8");
  CHECK(LUA_MATHLIBNAME->view() == "math");
  CHECK(LUA_DBLIBNAME->view() == "debug");
  CHECK(LUA_LOADLIBNAME->view() == "package");
}

/* ================= negative paths ================= */

/* checks the error object on top is a string containing 'what'; pops it */
static bool top_msg_contains(lua_State* L, std::string_view what)
{
  std::optional<HostString> m = lua_tostring(L, -1);
  bool ok = m.has_value() && m->to_utf8().find(what) != std::string_view::npos;
  lua_pop(L, 1);
  return ok;
}

/* --- panic injection --- */
struct PanicJump
{
};
static std::string g_panic_msg;
static int cf_panic_record(lua_State* L)
{
  std::optional<HostString> m = lua_tostring(L, -1);
  if (m.has_value())
    g_panic_msg = std::string(m->to_utf8());
  /* per the manual, the panic function's last chance is to never return
     (longjmp / throw out); returning would abort() the process */
  throw PanicJump{};
}

/* --- failure-injecting allocators --- */
static void* null_alloc(void*, void* ptr, size_t, size_t nsize)
{
  if (nsize == 0)
  {
    std::free(ptr);
    return nullptr;
  }
  return nullptr; /* refuse every allocation */
}

struct DenyAlloc
{
  bool deny = false;
};
static void* deny_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
  DenyAlloc* d = static_cast<DenyAlloc*>(ud);
  if (nsize == 0)
  {
    std::free(ptr);
    return nullptr;
  }
  /* when denying, refuse new allocations and growth; allow shrinks so
     the emergency GC can still make progress */
  if (d->deny && (ptr == nullptr || nsize > osize))
    return nullptr;
  return std::realloc(ptr, nsize);
}

/* --- C functions exercising raising API calls under pcall --- */
static int cf_neg_arith(lua_State* L)
{
  lua_arith(L, LUA_OPADD); /* args on stack: table + number -> raises */
  return 1;
}
static int cf_neg_concat(lua_State* L)
{
  lua_concat(L, 2);
  return 1;
}
static int cf_neg_len(lua_State* L)
{
  lua_len(L, 1);
  return 1;
}
static int cf_neg_compare(lua_State* L)
{
  lua_pushboolean(L, lua_compare(L, 1, 2, LUA_OPLT));
  return 1;
}
static int cf_neg_gettable(lua_State* L)
{
  lua_gettable(L, 1);
  return 1;
}
static int cf_neg_settable(lua_State* L)
{
  lua_settable(L, 1);
  return 0;
}

static void test_negative()
{
  /* panic function fires on an unprotected error with the error object on
     top of the stack; a non-returning handler can jump out and the state
     may then only be closed */
  {
    lua_State* L = new_state();
    lua_atpanic(L, cf_panic_record);
    g_panic_msg.clear();
    bool jumped = false;
    try
    {
      lua_pushstring(L, "unprotected!"_hs);
      lua_error(L);
    }
    catch (PanicJump&)
    {
      jumped = true;
    }
    CHECK(jumped);
    CHECK(g_panic_msg.find("unprotected!") != std::string::npos);
    lua_close(L);
  }

  /* lua_newstate reports allocator failure by returning NULL */
  {
    lua_State* L = lua_newstate(null_alloc, nullptr, 0);
    CHECK(L == nullptr);
  }

  /* in-flight allocation failure surfaces as LUA_ERRMEM from pcall */
  {
    DenyAlloc da;
    lua_State* L = lua_newstate(deny_alloc, &da, 0);
    CHECK(L != nullptr);
    CHECK(load_string(L,
        "local t = {}\n"
        "for i = 1, 1000000 do t[i] = i end\n"
        "return #t") == LUA_OK);
    da.deny = true;
    int st = lua_pcall(L, 0, 0, 0);
    da.deny = false;
    CHECK(st == LUA_ERRMEM);
    CHECK(top_msg_contains(L, "memory"));
    /* state remains usable after a recovered memory error */
    CHECK(do_string(L, "return 1 + 1") == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);
    lua_close(L);
  }

  /* raising API calls surface as LUA_ERRRUN under pcall */
  {
    lua_State* L = new_state();

    lua_pushcfunction(L, cf_neg_arith);
    lua_createtable(L, 0, 0);
    lua_pushinteger(L, 1);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "arithmetic"));

    lua_pushcfunction(L, cf_neg_concat);
    lua_createtable(L, 0, 0);
    lua_pushstring(L, "x"_hs);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "concatenate"));

    lua_pushcfunction(L, cf_neg_len);
    lua_pushboolean(L, 1);
    CHECK(lua_pcall(L, 1, 1, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "length"));

    lua_pushcfunction(L, cf_neg_compare);
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 0);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "compare"));

    lua_pushcfunction(L, cf_neg_gettable);
    lua_pushnil(L);
    CHECK(lua_pcall(L, 1, 1, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "index"));

    lua_pushcfunction(L, cf_neg_settable);
    lua_pushinteger(L, 5); /* not a table */
    lua_pushstring(L, "k"_hs);
    lua_pushstring(L, "v"_hs);
    CHECK(lua_pcall(L, 3, 0, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "index"));

    lua_close(L);
  }

  /* conversions on an above-top (none) index: defaults, not crashes.
     Note: the index must still be "acceptable" (within the frame's stack
     space, LUA_MINSTACK slots here) — truly out-of-range indices violate
     the API contract and trip the api check, same as upstream. */
  {
    lua_State* L = new_state();
    CHECK(lua_type(L, 15) == LUA_TNONE);
    CHECK(!lua_tonumberx(L, 15).has_value());
    CHECK(!lua_tointegerx(L, 15).has_value());
    CHECK(!lua_tostring(L, 15).has_value());
    CHECK(lua_toboolean(L, 15) == 0);
    CHECK(lua_tocfunction(L, 15) == nullptr);
    CHECK(lua_touserdata(L, 15) == nullptr);
    CHECK(lua_tothread(L, 15) == nullptr);
    CHECK(lua_rawlen(L, 15) == 0u);
    lua_close(L);
  }

  /* empty chunk is a valid load producing a function with no results */
  {
    lua_State* L = new_state();
    CHECK(load_string(L, "") == LUA_OK);
    CHECK(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK);
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
  }
}

/* ================= auxiliary library (lauxlib.h) ================= */

/* trampoline: run an arbitrary body (non-capturing lambda) under pcall */
static lua_CFunction g_body = nullptr;
static int cf_body(lua_State* L)
{
  return g_body(L);
}

static void test_auxlib_checks()
{
  lua_State* L = new_state();

  /* successful check/opt calls return converted values */
  g_body = [](lua_State* Lb) -> int {
    CHECK(luaL_checkinteger(Lb, 1) == 42);
    CHECK(luaL_checknumber(Lb, 2) == 2.5);
    CHECK(luaL_checklstring(Lb, 3) == "abc");
    /* a number argument is accepted and converted in place */
    CHECK(luaL_checkstring(Lb, 4).to_utf8() == "7");
    CHECK(lua_type(Lb, 4) == LUA_TSTRING);
    /* opt* fall back to defaults for absent/nil args */
    CHECK(luaL_optinteger(Lb, 5, 9) == 9);
    CHECK(luaL_optnumber(Lb, 5, 1.5) == 1.5);
    CHECK(luaL_optlstring(Lb, 5, "def") == "def");
    CHECK(!luaL_optstring(Lb, 5, std::nullopt).has_value());
    CHECK(luaL_optinteger(Lb, 1, 9) == 42); /* present arg wins */
    luaL_checktype(Lb, 1, LUA_TNUMBER);
    luaL_checkany(Lb, 1);
    luaL_checkstack(Lb, 20, "grow"_hs);
    luaL_argcheck(Lb, true, 1, "unused"_hs);
    luaL_argexpected(Lb, true, 1, "number"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushinteger(L, 42);
  lua_pushnumber(L, 2.5);
  lua_pushstring(L, "abc"_hs);
  lua_pushinteger(L, 7);
  CHECK(lua_pcall(L, 4, 0, 0) == LUA_OK);

  /* checkinteger rejects non-numbers */
  g_body = [](lua_State* Lb) -> int {
    luaL_checkinteger(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "x"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "bad argument"));

  /* checkinteger rejects floats with no integer representation */
  lua_pushcfunction(L, cf_body);
  lua_pushnumber(L, 3.5);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "no integer representation"));

  /* checklstring rejects tables */
  g_body = [](lua_State* Lb) -> int {
    luaL_checklstring(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_createtable(L, 0, 0);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "string expected"));

  /* checktype mismatch */
  g_body = [](lua_State* Lb) -> int {
    luaL_checktype(Lb, 1, LUA_TTABLE);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushinteger(L, 1);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "table expected"));

  /* checkany with no argument at all */
  g_body = [](lua_State* Lb) -> int {
    luaL_checkany(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "value expected"));

  /* argerror / typeerror / argcheck(false) */
  g_body = [](lua_State* Lb) -> int {
    return luaL_argerror(Lb, 2, "custom complaint"_hs);
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "custom complaint"));

  g_body = [](lua_State* Lb) -> int {
    return luaL_typeerror(Lb, 1, "widget"_hs);
  };
  lua_pushcfunction(L, cf_body);
  lua_pushinteger(L, 1);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "widget expected"));

  g_body = [](lua_State* Lb) -> int {
    luaL_argcheck(Lb, false, 1, "must be positive"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushinteger(L, -1);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "must be positive"));

  /* checkstack failure raises (rather than returning 0) */
  g_body = [](lua_State* Lb) -> int {
    luaL_checkstack(Lb, INT_MAX, "boom"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "stack overflow"));

  /* checkoption: match, default for absent arg, and rejection */
  static constexpr std::string_view k_opts[] = {"one", "two", "three"};
  g_body = [](lua_State* Lb) -> int {
    CHECK(luaL_checkoption(Lb, 1, std::string_view{}, k_opts) == 2);
    CHECK(luaL_checkoption(Lb, 2, "two", k_opts) == 1); /* absent -> def */
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "three"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_OK);

  g_body = [](lua_State* Lb) -> int {
    return luaL_checkoption(Lb, 1, std::string_view{}, k_opts);
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "four"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "invalid option"));

  /* luaL_error: format args are taken from the stack; location prefixed
     when available (a C function has none, so the message is bare) */
  g_body = [](lua_State* Lb) -> int {
    lua_pushinteger(Lb, 7);
    return luaL_error(Lb, "oops %d"_hs);
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "oops 7"));

  lua_close(L);
}

static int cf_custom_tostring(lua_State* L)
{
  lua_pushstring(L, "custom!"_hs);
  return 1;
}

static void test_auxlib_meta()
{
  lua_State* L = new_state();

  /* newmetatable: creates once, finds thereafter; sets __name */
  CHECK(luaL_newmetatable(L, "TPA.Widget"_hs) == 1);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "__name"_hs) == LUA_TSTRING);
  {
    std::optional<HostString> n = lua_tostring(L, -1);
    CHECK(n.has_value() && n->to_utf8() == "TPA.Widget");
  }
  lua_pop(L, 1);
  CHECK(luaL_newmetatable(L, "TPA.Widget"_hs) == 0);
  CHECK(luaL_getmetatable(L, "TPA.Widget"_hs) == LUA_TTABLE);
  CHECK(lua_rawequal(L, -1, -2) == 1);
  CHECK(lua_rawequal(L, -1, -3) == 1);
  lua_pop(L, 3);

  /* setmetatable/testudata/checkudata */
  void* ud = lua_newuserdatauv(L, 8, 0);
  luaL_setmetatable(L, "TPA.Widget"_hs);
  CHECK(luaL_testudata(L, -1, "TPA.Widget"_hs) == ud);
  CHECK(luaL_testudata(L, -1, "TPA.Other"_hs) == nullptr);
  CHECK(luaL_checkudata(L, -1, "TPA.Widget"_hs) == ud);
  lua_createtable(L, 0, 0);
  CHECK(luaL_testudata(L, -1, "TPA.Widget"_hs) == nullptr);
  lua_pop(L, 2);

  /* checkudata mismatch raises a type error */
  g_body = [](lua_State* Lb) -> int {
    luaL_checkudata(Lb, 1, "TPA.Missing"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_newuserdatauv(L, 4, 0);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "TPA.Missing expected"));

  /* getmetafield / callmeta on a table with a custom metatable */
  lua_createtable(L, 0, 0); /* the object */
  lua_createtable(L, 0, 2); /* its metatable */
  lua_pushcfunction(L, cf_custom_tostring);
  lua_setfield(L, -2, "__tostring"_hs);
  lua_pushstring(L, "special"_hs);
  lua_setfield(L, -2, "kind"_hs);
  lua_setmetatable(L, -2);
  int obj = lua_gettop(L);

  int before = lua_gettop(L);
  CHECK(luaL_getmetafield(L, obj, "kind"_hs) == LUA_TSTRING);
  {
    std::optional<HostString> k = lua_tostring(L, -1);
    CHECK(k.has_value() && k->to_utf8() == "special");
  }
  lua_pop(L, 1);
  /* absent field: returns LUA_TNIL and pushes nothing */
  CHECK(luaL_getmetafield(L, obj, "nope"_hs) == LUA_TNIL);
  CHECK(lua_gettop(L) == before);

  CHECK(luaL_callmeta(L, obj, "__tostring"_hs) == 1);
  {
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "custom!");
  }
  lua_pop(L, 1);
  CHECK(luaL_callmeta(L, obj, "__nope"_hs) == 0);
  CHECK(lua_gettop(L) == obj);

  /* luaL_tolstring conversions (result is also pushed) */
  lua_pushinteger(L, 12);
  CHECK(luaL_tolstring(L, -1).to_utf8() == "12");
  lua_pop(L, 2);
  lua_pushboolean(L, 1);
  CHECK(luaL_tolstring(L, -1).to_utf8() == "true");
  lua_pop(L, 2);
  lua_pushnil(L);
  CHECK(luaL_tolstring(L, -1).to_utf8() == "nil");
  lua_pop(L, 2);
  CHECK(luaL_tolstring(L, obj).to_utf8() == "custom!"); /* via __tostring */
  lua_pop(L, 1);
  lua_createtable(L, 0, 0); /* plain table: "table: 0x..." */
  CHECK(luaL_tolstring(L, -1).to_utf8().substr(0, 7) == "table: ");
  lua_pop(L, 2);
  lua_createtable(L, 0, 0); /* __name (no __tostring): "Widget: 0x..." */
  lua_createtable(L, 0, 1);
  lua_pushstring(L, "Widget"_hs);
  lua_setfield(L, -2, "__name"_hs);
  lua_setmetatable(L, -2);
  CHECK(luaL_tolstring(L, -1).to_utf8().substr(0, 8) == "Widget: ");
  lua_pop(L, 3);

  lua_close(L);
}

static void test_auxlib_load()
{
  lua_State* L = new_state();

  /* loadstring + dostring */
  CHECK(luaL_loadstring(L, "return 6 * 7"_hs) == LUA_OK);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  CHECK(luaL_loadstring(L, "return +++"_hs) == LUA_ERRSYNTAX);
  lua_pop(L, 1);
  CHECK(luaL_dostring(L, "return 2 + 3"_hs) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 5);
  lua_pop(L, 1);
  CHECK(luaL_dostring(L, "error('dsfail')"_hs) != LUA_OK);
  CHECK(top_msg_contains(L, "dsfail"));

  /* loadbufferx: text mode, and mode enforcement against binary chunks */
  CHECK(luaL_loadbufferx(L, "return 1", "=lb"_hs, "t"_hs) ==
        LUA_OK);
  lua_pop(L, 1);
  DumpBuf d;
  CHECK(luaL_loadstring(L, "return 99"_hs) == LUA_OK);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(d.bytes.size() > 0);
  /* binary chunk loads in "b" mode... */
  CHECK(luaL_loadbufferx(L, d.bytes, "=bin"_hs, "b"_hs) ==
        LUA_OK);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);
  /* ...but is rejected in "t" mode */
  CHECK(luaL_loadbufferx(L, d.bytes, "=bin"_hs, "t"_hs) ==
        LUA_ERRSYNTAX);
  CHECK(top_msg_contains(L, "binary chunk"));
  /* loadbuffer (no mode) accepts either */
  CHECK(luaL_loadbuffer(L, "return 4", "=lb2"_hs) == LUA_OK);
  lua_pop(L, 1);

  /* loadfile/loadfilex/dofile against a real file */
  const char* path = "/tmp/tpa_luaL_chunk.lua";
  {
    std::FILE* f = std::fopen(path, "w");
    CHECK(f != nullptr);
    if (f != nullptr)
    {
      std::fputs("return 41 + 1", f);
      std::fclose(f);
    }
  }
  CHECK(luaL_loadfile(L, HostString(path)) == LUA_OK);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  CHECK(luaL_loadfilex(L, HostString(path), "t"_hs) == LUA_OK);
  lua_pop(L, 1);
  CHECK(luaL_dofile(L, HostString(path)) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  std::remove(path);

  /* missing file: LUA_ERRFILE with a "cannot open" message */
  CHECK(luaL_loadfile(L, "/tmp/tpa_no_such_file.lua"_hs) ==
        LUA_ERRFILE);
  CHECK(top_msg_contains(L, "cannot open"));

  lua_close(L);
}

static void test_auxlib_refs_buffer()
{
  lua_State* L = new_state();

  /* ref/unref against the registry */
  lua_pushstring(L, "payload"_hs);
  int r1 = luaL_ref(L, LUA_REGISTRYINDEX); /* pops the value */
  CHECK(r1 != LUA_REFNIL && r1 != LUA_NOREF);
  CHECK(lua_rawgeti(L, LUA_REGISTRYINDEX, r1) == LUA_TSTRING);
  {
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "payload");
  }
  lua_pop(L, 1);
  /* nil gets the sentinel ref */
  lua_pushnil(L);
  CHECK(luaL_ref(L, LUA_REGISTRYINDEX) == LUA_REFNIL);
  /* released slots are reused */
  luaL_unref(L, LUA_REGISTRYINDEX, r1);
  lua_pushinteger(L, 5);
  int r2 = luaL_ref(L, LUA_REGISTRYINDEX);
  CHECK(r2 == r1);
  luaL_unref(L, LUA_REGISTRYINDEX, r2);
  luaL_unref(L, LUA_REGISTRYINDEX, LUA_NOREF); /* no-op */
  luaL_unref(L, LUA_REGISTRYINDEX, LUA_REFNIL); /* no-op */

  /* buffer: chars, strings, substrings, values, raw area, and buffsub */
  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    luaL_addchar(&B, 'a');
    luaL_addstring(&B, "bc"_hs);
    luaL_addlstring(&B, "wxyz", 1, 2); /* "xy" */
    CHECK(luaL_bufflen(&B) == 5u);
    lua_pushinteger(L, 99);
    luaL_addvalue(&B); /* converts and appends "99", pops */
    size_t off = luaL_prepbuffsize(&B, 3);
    std::span<char> area = luaL_buffarea(&B);
    std::memcpy(area.data() + off, "!!!", 3);
    luaL_addsize(&B, 3);
    luaL_buffsub(&B, 1); /* drop one '!' */
    luaL_pushresult(&B);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "abcxy99!!");
    lua_pop(L, 1);
  }

  /* buffinitsize + pushresultsize */
  {
    luaL_Buffer B;
    size_t off = luaL_buffinitsize(L, &B, 4);
    std::memcpy(luaL_buffarea(&B).data() + off, "ABCD", 4);
    luaL_pushresultsize(&B, 4);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "ABCD");
    lua_pop(L, 1);
  }

  /* prepbuffer reserves the default chunk */
  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    (void)luaL_prepbuffer(&B);
    luaL_pushresult(&B); /* nothing committed -> empty string */
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8().empty());
    lua_pop(L, 1);
  }

  /* gsub: plain textual replacement, result returned and pushed */
  {
    HostString g = luaL_gsub(L, "hello world"_hs, "o"_hs,
                             "0"_hs);
    CHECK(g.to_utf8() == "hell0 w0rld");
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "hell0 w0rld");
    lua_pop(L, 1);
  }

  /* addgsub inside a buffer */
  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    luaL_addstring(&B, "["_hs);
    luaL_addgsub(&B, "a.b.c"_hs, "."_hs, "/"_hs);
    luaL_addstring(&B, "]"_hs);
    luaL_pushresult(&B);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "[a/b/c]");
    lua_pop(L, 1);
  }

  lua_close(L);
}

static int cf_getup(lua_State* L)
{
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int cf_push_where(lua_State* L)
{
  luaL_where(L, 1);
  return 1;
}

static void test_auxlib_misc()
{
  lua_State* L = new_state();

  luaL_checkversion(L);
  (void)luaL_makeseed(L);

  /* header constants */
  CHECK(LUA_ERRFILE == LUA_ERRERR + 1);
  CHECK(LUA_NOREF == -2 && LUA_REFNIL == -1);
  CHECK(LUA_GNAME->view() == "_G");
  CHECK(LUA_LOADED_TABLE->view() == "_LOADED");
  CHECK(LUA_PRELOAD_TABLE->view() == "_PRELOAD");
  CHECK(LUA_FILEHANDLE->view() == "FILE*");

  /* pushfail pushes the failure value (nil) */
  luaL_pushfail(L);
  CHECK(lua_type(L, -1) == LUA_TNIL);
  lua_pop(L, 1);

  /* typename */
  lua_pushinteger(L, 1);
  CHECK(luaL_typename(L, -1).to_utf8() == "number");
  CHECK(luaL_typename(L, 10).to_utf8() == "no value");
  lua_pop(L, 1);

  /* luaL_len on a sequence */
  lua_createtable(L, 3, 0);
  for (int i = 1; i <= 3; i++)
  {
    lua_pushinteger(L, i * 10);
    lua_rawseti(L, -2, i);
  }
  CHECK(luaL_len(L, -1) == 3);
  lua_pop(L, 1);

  /* luaL_len error when __len does not produce an integer */
  g_body = [](lua_State* Lb) -> int {
    lua_createtable(Lb, 0, 0);
    lua_createtable(Lb, 0, 1);
    lua_pushcfunction(Lb, [](lua_State* Lm) -> int {
      lua_pushstring(Lm, "nan"_hs);
      return 1;
    });
    lua_setfield(Lb, -2, "__len"_hs);
    lua_setmetatable(Lb, -2);
    luaL_len(Lb, -1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "length"));

  /* luaL_where: no call info at the base level; chunk:line inside Lua */
  luaL_where(L, 0);
  {
    std::optional<HostString> w = lua_tostring(L, -1);
    CHECK(w.has_value() && w->to_utf8().empty());
  }
  lua_pop(L, 1);
  lua_pushcfunction(L, cf_push_where);
  lua_setglobal(L, "wherefn"_hs);
  CHECK(do_string(L, "return wherefn()") == LUA_OK);
  {
    std::optional<HostString> w = lua_tostring(L, -1);
    CHECK(w.has_value() && w->to_utf8().substr(0, 6) == "tpa:1:");
  }
  lua_pop(L, 1);

  /* traceback */
  luaL_traceback(L, L, "MSG"_hs, 0);
  {
    std::optional<HostString> t = lua_tostring(L, -1);
    CHECK(t.has_value());
    if (t.has_value())
    {
      CHECK(t->to_utf8().find("MSG") != std::string_view::npos);
      CHECK(t->to_utf8().find("stack traceback") != std::string_view::npos);
    }
  }
  lua_pop(L, 1);

  /* getsubtable: creates then finds */
  CHECK(luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs) == 0);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs) == 1);
  CHECK(lua_rawequal(L, -1, -2) == 1);
  lua_pop(L, 2);

  /* setfuncs with a shared upvalue */
  lua_createtable(L, 0, 1);
  lua_pushinteger(L, 123); /* the upvalue */
  {
    const luaL_Reg regs[] = {
        {"getup", cf_getup},
        {"holder", nullptr}, /* placeholder -> false */
        {std::string_view{}, nullptr},
    };
    luaL_setfuncs(L, regs, 1);
  }
  CHECK(lua_getfield(L, -1, "getup"_hs) == LUA_TFUNCTION);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 123);
  lua_pop(L, 1);
  CHECK(lua_getfield(L, -1, "holder"_hs) == LUA_TBOOLEAN);
  CHECK(lua_toboolean(L, -1) == 0);
  lua_pop(L, 2);

  /* fileresult: success and errno-decorated failure */
  CHECK(luaL_fileresult(L, 1, std::nullopt) == 1);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 1);
  errno = ENOENT;
  CHECK(luaL_fileresult(L, 0, "nofile.txt"_hs) == 3);
  CHECK(lua_type(L, -3) == LUA_TNIL); /* fail */
  {
    std::optional<HostString> m = lua_tostring(L, -2);
    CHECK(m.has_value() &&
          m->to_utf8().find("nofile.txt") != std::string_view::npos);
  }
  CHECK(lua_tointeger(L, -1) == ENOENT);
  lua_pop(L, 3);

  /* execresult: successful termination */
  errno = 0;
  CHECK(luaL_execresult(L, 0) == 3);
  CHECK(lua_toboolean(L, -3) == 1);
  {
    std::optional<HostString> w = lua_tostring(L, -2);
    CHECK(w.has_value() && w->to_utf8() == "exit");
  }
  CHECK(lua_tointeger(L, -1) == 0);
  lua_pop(L, 3);

  lua_close(L);

  /* luaL_newstate + requiref (glb both ways) + LOADED bookkeeping */
  {
    lua_State* L2 = luaL_newstate();
    CHECK(L2 != nullptr);
    luaL_requiref(L2, "string"_hs, luaopen_string, 1);
    CHECK(lua_type(L2, -1) == LUA_TTABLE); /* module left on the stack */
    CHECK(lua_getglobal(L2, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -2) == 1);
    lua_pop(L2, 1);
    CHECK(luaL_getsubtable(L2, LUA_REGISTRYINDEX, "_LOADED"_hs) == 1);
    CHECK(lua_getfield(L2, -1, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -3) == 1);
    lua_pop(L2, 3);
    /* glb=0: registered but no global */
    luaL_requiref(L2, "mathmod"_hs, luaopen_math, 0);
    CHECK(lua_type(L2, -1) == LUA_TTABLE);
    CHECK(lua_getglobal(L2, "mathmod"_hs) == LUA_TNIL);
    lua_pop(L2, 2);
    /* the default luaL_newstate allocator and seed produce a working VM */
    luaL_openlibs(L2);
    CHECK(luaL_dostring(L2, "return tostring(6*7)"_hs) == LUA_OK);
    {
      std::optional<HostString> s = lua_tostring(L2, -1);
      CHECK(s.has_value() && s->to_utf8() == "42");
    }
    lua_pop(L2, 1);
    lua_close(L2);
  }
}

/* ================= dump limits & upvalue survival (review #1) ================= */

static void test_dump_limits()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  /* NOTE (correction to the review premise): official 5.5 changed lua_dump.
     In 5.4 dumping a non-Lua function returned a nonzero status; in 5.5 it
     is an api_check violation ("Lua function expected") — an assert under
     API checks, UB otherwise (verified against stock 5.5.1: it crashes in
     luaU_dump). This fork asserts identically, so there is nothing legal
     to test in-process for C functions. */

  /* two closures sharing an upvalue */
  CHECK(do_string(L,
      "local x = 77\n"
      "local function a() x = x + 1; return x end\n"
      "local function b() return x end\n"
      "return a, b") == LUA_OK);
  int bidx = lua_gettop(L);
  int aidx = bidx - 1;
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));

  /* dump A and load it back */
  DumpBuf d;
  lua_pushvalue(L, aidx);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d.bytes, "=re"_hs, "b"_hs) ==
        LUA_OK);
  int lidx = lua_gettop(L);

  /* Official 5.5 (verified on stock 5.5.1): upvalue VALUES do not survive a
     dump/load round-trip. The loaded closure's FIRST upvalue is set to the
     global environment (regardless of its name); any others are nil. */
  {
    std::optional<HostString> uname = lua_getupvalue(L, lidx, 1);
    CHECK(uname.has_value());
    CHECK(lua_type(L, -1) == LUA_TTABLE); /* the globals table, not 77 */
    lua_pushglobaltable(L);
    CHECK(lua_rawequal(L, -1, -2) == 1);
    lua_pop(L, 2);
  }
  /* sharing does not survive either: fresh upvalue, not A's */
  CHECK(lua_upvalueid(L, lidx, 1) != lua_upvalueid(L, aidx, 1));
  /* the original pair still shares */
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));
  lua_pop(L, 1); /* loaded closure */

  /* a closure with a second (non-first) upvalue: it loads back as nil */
  CHECK(do_string(L,
      "local u1, u2 = 10, 20\n"
      "return function() return u1, u2 end") == LUA_OK);
  DumpBuf d2;
  CHECK(lua_dump(L, dump_writer, &d2, 0) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d2.bytes, "=re2"_hs, "b"_hs) ==
        LUA_OK);
  lidx = lua_gettop(L);
  {
    std::optional<HostString> u1 = lua_getupvalue(L, lidx, 1);
    CHECK(u1.has_value());
    CHECK(lua_type(L, -1) == LUA_TTABLE); /* first := globals */
    lua_pop(L, 1);
    std::optional<HostString> u2 = lua_getupvalue(L, lidx, 2);
    CHECK(u2.has_value());
    CHECK(lua_type(L, -1) == LUA_TNIL); /* others := nil */
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* strip=1: debug info is dropped — source becomes "=?" (verified on
     stock 5.5.1), linedefined survives (it is not debug info) */
  CHECK(do_string(L, "return function() return 1 end") == LUA_OK);
  DumpBuf d3;
  CHECK(lua_dump(L, dump_writer, &d3, 1) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d3.bytes, "=s"_hs, "b"_hs) ==
        LUA_OK);
  {
    lua_Debug q{};
    std::optional<lua_Debug> info = lua_getinfo(L, ">S", q);
    CHECK(info.has_value());
    if (info.has_value())
    {
      CHECK(info->source == "=?");
      CHECK(info->linedefined == 1);
    }
  }

  lua_pop(L, lua_gettop(L));
  lua_close(L);
}

/* ====== C-function identity durability & representation (review #2/#3/#6) ====== */

static void test_cfunction_identity()
{
  lua_State* L = new_state();

  /* pushing the same C function twice yields equal identities */
  lua_pushcfunction(L, cf_add);
  lua_pushcfunction(L, cf_add);
  const void* p0 = lua_topointer(L, -1);
  CHECK(p0 != nullptr);
  CHECK(lua_topointer(L, -1) == lua_topointer(L, -2));
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_tocfunction(L, -2) == cf_add);
  lua_pop(L, 2);

  /* light C function vs C closure representation (review #3).
     Official 5.5: lua_pushcclosure with 0 upvalues pushes a LIGHT C
     function (a value, not a collectable object); with upvalues it
     creates a C closure (a fresh collectable object each time). */
  lua_pushcclosure(L, cf_add, 0); /* light */
  CHECK(lua_iscfunction(L, -1) == 1);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0); /* address-derived, stable */
  /* a light C function has no upvalues */
  CHECK(!lua_getupvalue(L, -1, 1).has_value());

  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1); /* closure #1 */
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1); /* closure #2 */
  CHECK(lua_iscfunction(L, -1) == 1 && lua_iscfunction(L, -2) == 1);
  /* same C function inside... */
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_tocfunction(L, -1) == lua_tocfunction(L, -2));
  /* ...but each closure is a distinct object, and distinct from light */
  CHECK(lua_topointer(L, -1) != lua_topointer(L, -2));
  CHECK(lua_topointer(L, -1) != p0);
  CHECK(lua_topointer(L, -2) != p0);
  /* closure upvalue exists and its name is "" for C closures */
  {
    std::optional<HostString> un = lua_getupvalue(L, -1, 1);
    CHECK(un.has_value());
    if (un.has_value())
      CHECK(un->to_utf8().empty());
    lua_pop(L, 1);
    CHECK(!lua_getupvalue(L, -1, 2).has_value()); /* only one upvalue */
  }
  lua_pop(L, 3);

  /* durability (review #2): store the C function in the registry, as a
     table key, in a metatable slot, and in a Lua-closure upvalue; then
     force two full GC cycles and check identity from every location */
  lua_pushcfunction(L, cf_add);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_createtable(L, 0, 1); /* t: cf as key */
  lua_pushcfunction(L, cf_add);
  lua_pushinteger(L, 1234);
  lua_settable(L, -3);
  int tidx = lua_gettop(L);

  lua_createtable(L, 0, 0); /* obj with mt.__index = cf */
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, cf_add);
  lua_setfield(L, -2, "__index"_hs);
  lua_setmetatable(L, -2);
  int oidx = lua_gettop(L);

  CHECK(load_string(L, "local f = ... \nreturn function() return f end") ==
        LUA_OK);
  lua_pushcfunction(L, cf_add);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK); /* Lua closure capturing cf */
  int cidx = lua_gettop(L);

  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  /* registry */
  CHECK(lua_rawgeti(L, LUA_REGISTRYINDEX, ref) == LUA_TFUNCTION);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);

  /* table key: same function must find the entry */
  lua_pushcfunction(L, cf_add);
  CHECK(lua_gettable(L, tidx) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 1234);
  lua_pop(L, 1);

  /* metatable slot via __index dispatch */
  CHECK(lua_getmetatable(L, oidx) == 1);
  CHECK(lua_getfield(L, -1, "__index"_hs) == LUA_TFUNCTION);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  lua_pop(L, 2);

  /* Lua-closure upvalue */
  {
    std::optional<HostString> un = lua_getupvalue(L, cidx, 1);
    CHECK(un.has_value());
    CHECK(lua_tocfunction(L, -1) == cf_add);
    CHECK(lua_topointer(L, -1) == p0);
    lua_pop(L, 1);
  }

  lua_pop(L, 3); /* t, obj, closure */
  lua_close(L);

  /* multiple independent states (review #6) */
  {
    lua_State* La = new_state();
    lua_State* Lb = new_state();
    lua_pushcfunction(La, cf_add);
    lua_setglobal(La, "probe"_hs);
    lua_pushcfunction(Lb, cf_add);
    lua_setglobal(Lb, "probe"_hs);
    CHECK(lua_getglobal(La, "probe"_hs) == LUA_TFUNCTION);
    CHECK(lua_getglobal(Lb, "probe"_hs) == LUA_TFUNCTION);
    CHECK(lua_tocfunction(La, -1) == lua_tocfunction(Lb, -1));
    /* light C function identity agrees across states */
    CHECK(lua_topointer(La, -1) == lua_topointer(Lb, -1));
    const void* pb = lua_topointer(Lb, -1);
    lua_pop(La, 1);
    /* closing one state must not disturb the other */
    lua_close(La);
    CHECK(lua_tocfunction(Lb, -1) == cf_add);
    CHECK(lua_topointer(Lb, -1) == pb);
    lua_pushinteger(Lb, 41);
    lua_pushinteger(Lb, 1);
    lua_pushcfunction(Lb, cf_add); /* still callable */
    lua_rotate(Lb, -3, 1);
    CHECK(lua_pcall(Lb, 2, 1, 0) == LUA_OK);
    lua_pop(Lb, 2);
    lua_close(Lb);
  }
}

/* ============== continuation chain callk->pcallk->yieldk (review #4) ============== */

static std::string g_klog;
static void klog(const char* tag, lua_KContext ctx, int status)
{
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s(%lld,%d);", tag,
                static_cast<long long>(ctx), status);
  g_klog += buf;
}

static int k_chain_yield(lua_State* L, int status, lua_KContext ctx)
{
  klog("kY", ctx, status);
  lua_pushinteger(L, 100);
  return 1;
}
static int cf_chain_yielder(lua_State* L)
{
  return lua_yieldk(L, 0, 333, k_chain_yield);
}

static int k_chain_pcall(lua_State* L, int status, lua_KContext ctx)
{
  klog("kP", ctx, status);
  (void)L;
  return 1; /* pass through the inner result */
}
static int cf_chain_B(lua_State* L)
{
  lua_getglobal(L, "innerchunk"_hs);
  return k_chain_pcall(L, lua_pcallk(L, 0, 1, 0, 222, k_chain_pcall), 222);
}

static int k_chain_call(lua_State* L, int status, lua_KContext ctx)
{
  klog("kA", ctx, status);
  (void)L;
  return 1;
}
static int cf_chain_A(lua_State* L)
{
  lua_getglobal(L, "outerchunk"_hs);
  lua_callk(L, 0, 1, 111, k_chain_call);
  return k_chain_call(L, LUA_OK, 111);
}

static void test_continuation_chain()
{
  lua_State* L = new_state();
  lua_pushcfunction(L, cf_chain_yielder);
  lua_setglobal(L, "cyield"_hs);
  lua_pushcfunction(L, cf_chain_B);
  lua_setglobal(L, "B"_hs);
  CHECK(do_string(L, "function innerchunk() return cyield() end\n"
                     "function outerchunk() return B() end") == LUA_OK);

  lua_State* co = lua_newthread(L);
  lua_pushcfunction(co, cf_chain_A);
  g_klog.clear();
  lua_ResumeResult rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_YIELD);
  CHECK(g_klog.empty()); /* no continuation ran yet */

  /* full GC cycles must not disturb pending continuations */
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_OK);
  CHECK(rr.nres == 1);
  /* verified against stock 5.5.1: innermost continuation first, each with
     its own ctx, all with status LUA_YIELD (value 1) */
  CHECK(g_klog == "kY(333,1);kP(222,1);kA(111,1);");
  CHECK(lua_tointeger(co, -1) == 100);
  lua_pop(L, 1); /* thread */
  lua_close(L);
}

/* ================= per-thread hooks (review #5) ================= */

static int g_hookA_hits = 0;
static int g_hookB_hits = 0;
static const lua_State* g_hookA_thread = nullptr;
static const lua_State* g_hookB_thread = nullptr;
static void hook_A(lua_State* L, lua_Debug*)
{
  g_hookA_hits++;
  g_hookA_thread = L;
}
static void hook_B(lua_State* L, lua_Debug*)
{
  g_hookB_hits++;
  g_hookB_thread = L;
}

static void test_thread_hooks()
{
  lua_State* L = new_state();

  /* inheritance pinning: official 5.5 copies hook, mask, and count to a
     new thread at creation (verified on stock 5.5.1: fn and mask both
     inherited) */
  lua_sethook(L, hook_A, LUA_MASKCALL | LUA_MASKLINE, 0);
  lua_State* co = lua_newthread(L);
  CHECK(lua_gethook(co) == hook_A);
  CHECK(lua_gethookmask(co) == (LUA_MASKCALL | LUA_MASKLINE));

  /* overriding the coroutine's hook must not touch the creator's */
  lua_sethook(co, hook_B, LUA_MASKLINE, 0);
  CHECK(lua_gethook(L) == hook_A);
  CHECK(lua_gethook(co) == hook_B);
  CHECK(lua_gethookmask(L) == (LUA_MASKCALL | LUA_MASKLINE));
  CHECK(lua_gethookmask(co) == LUA_MASKLINE);

  /* isolation: each hook fires only on its own thread */
  g_hookA_hits = g_hookB_hits = 0;
  g_hookA_thread = g_hookB_thread = nullptr;
  CHECK(do_string(L, "local s = 0\nfor i = 1, 3 do s = s + i end\nreturn s") ==
        LUA_OK);
  lua_pop(L, 1);
  CHECK(g_hookA_hits > 0);
  CHECK(g_hookB_hits == 0);
  CHECK(g_hookA_thread == L);

  int a_after_main = g_hookA_hits;
  CHECK(load_string(co, "local y = 0\n"
                        "y = y + 1\n"
                        "coroutine.yield(y)\n"
                        "y = y + 1\n"
                        "return y") == LUA_OK);
  /* coroutine.yield needs the coroutine lib on this state */
  luaL_requiref(co, "coroutine"_hs, luaopen_coroutine, 1);
  lua_pop(co, 1);
  lua_ResumeResult rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_YIELD);
  CHECK(g_hookB_hits > 0);
  CHECK(g_hookB_thread == co);
  CHECK(g_hookA_hits == a_after_main); /* main hook untouched by co */

  /* counts survive yield/resume: hook keeps firing after resume */
  int b_at_yield = g_hookB_hits;
  lua_pop(co, rr.nres);
  rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_OK);
  CHECK(g_hookB_hits > b_at_yield);
  CHECK(g_hookA_hits == a_after_main);

  /* clearing one thread's hook leaves the other's in place */
  lua_sethook(co, nullptr, 0, 0);
  CHECK(lua_gethook(co) == nullptr);
  CHECK(lua_gethook(L) == hook_A);
  lua_sethook(L, nullptr, 0, 0);

  lua_close(L);
}

/* ================= io library handle semantics (review #7) ================= */

static void test_io_handles()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  /* default handle identity: io.output()/io.input() return the same
     object every time, and they are the std handles */
  CHECK(do_string(L, "return io.output() == io.output(), "
                     "io.input() == io.input(), "
                     "io.stdout == io.output(), io.stdin == io.input()") ==
        LUA_OK);
  CHECK(lua_toboolean(L, -4) == 1);
  CHECK(lua_toboolean(L, -3) == 1);
  CHECK(lua_toboolean(L, -2) == 1);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 4);

  /* tostring(file): "file (0x...)" open, "file (closed)" after close;
     operating on a closed file raises "attempt to use a closed file" */
  CHECK(do_string(L,
      "local f = assert(io.open('/tmp/tpa_io_probe.txt', 'w'))\n"
      "local open_str = tostring(f)\n"
      "f:close()\n"
      "local closed_str = tostring(f)\n"
      "local ok, err = pcall(function() return f:write('x') end)\n"
      "return open_str, closed_str, ok, tostring(err)") == LUA_OK);
  {
    std::optional<HostString> os_ = lua_tostring(L, -4);
    CHECK(os_.has_value() && os_->to_utf8().substr(0, 6) == "file (");
    CHECK(os_.has_value() &&
          os_->to_utf8().find("closed") == std::string_view::npos);
    std::optional<HostString> cs = lua_tostring(L, -3);
    CHECK(cs.has_value() && cs->to_utf8() == "file (closed)");
    CHECK(lua_toboolean(L, -2) == 0);
    std::optional<HostString> err = lua_tostring(L, -1);
    CHECK(err.has_value() && err->to_utf8().find(
              "attempt to use a closed file") != std::string_view::npos);
  }
  lua_pop(L, 4);
  std::remove("/tmp/tpa_io_probe.txt");

  /* closing std handles is guarded: returns fail + "cannot close standard
     file" and the handle stays usable */
  CHECK(do_string(L,
      "local ok, err = io.stdout:close()\n"
      "local usable = pcall(function() return io.stdout:write('') end)\n"
      "return ok, tostring(err), usable") == LUA_OK);
  CHECK(lua_type(L, -3) == LUA_TNIL);
  {
    std::optional<HostString> err = lua_tostring(L, -2);
    CHECK(err.has_value() && err->to_utf8().find(
              "cannot close standard file") != std::string_view::npos);
  }
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 3);

  /* popen close returns the exec-result triple (verified on stock 5.5.1:
     failure -> fail,"exit",code; success -> true,"exit",0) */
  CHECK(do_string(L,
      "local p = assert(io.popen('exit 3'))\n"
      "local a, b, c = p:close()\n"
      "local p2 = assert(io.popen('true'))\n"
      "local a2, b2, c2 = p2:close()\n"
      "return a, b, c, a2, b2, c2") == LUA_OK);
  CHECK(lua_type(L, -6) == LUA_TNIL); /* fail */
  {
    std::optional<HostString> what = lua_tostring(L, -5);
    CHECK(what.has_value() && what->to_utf8() == "exit");
  }
  CHECK(lua_tointeger(L, -4) == 3);
  CHECK(lua_toboolean(L, -3) == 1); /* true */
  {
    std::optional<HostString> what2 = lua_tostring(L, -2);
    CHECK(what2.has_value() && what2->to_utf8() == "exit");
  }
  CHECK(lua_tointeger(L, -1) == 0);
  lua_pop(L, 6);

  lua_close(L);
}

/* ============== restored luaL surface: newlib/opt/prepbuffspan (review #8) ============== */

static bool g_opt_check_called = false;
static lua_Integer opt_check_probe(lua_State* L, int n)
{
  g_opt_check_called = true;
  return luaL_checkinteger(L, n);
}

static void test_auxlib_restored()
{
  lua_State* L = new_state();

  /* luaL_newlib: registers a module table with the given functions */
  {
    static const luaL_Reg mod[] = {
        {"add", cf_add},
        {"probe", cf_raise},
        {std::string_view{}, nullptr},
    };
    luaL_newlib(L, mod);
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    /* exactly the registered functions, no extras */
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
      CHECK(lua_type(L, -1) == LUA_TFUNCTION);
      count++;
      lua_pop(L, 1);
    }
    CHECK(count == 2);
    /* call through the table */
    CHECK(lua_getfield(L, -1, "add"_hs) == LUA_TFUNCTION);
    lua_pushinteger(L, 20);
    lua_pushinteger(L, 22);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 2);
  }

  /* luaL_newlibtable: presized empty table, nothing registered yet */
  {
    static const luaL_Reg mod[] = {
        {"add", cf_add},
        {std::string_view{}, nullptr},
    };
    luaL_newlibtable(L, mod);
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    lua_pushnil(L);
    CHECK(lua_next(L, -2) == 0); /* empty — presizing has no semantics */
    luaL_setfuncs(L, mod, 0);    /* and it works as the setfuncs target */
    CHECK(lua_getfield(L, -1, "add"_hs) == LUA_TFUNCTION);
    lua_pop(L, 2);
  }

  /* luaL_opt: the check function must NOT run when the arg is none/nil
     (laziness is the contract). Args: 1=nil, 2=nil, 3=55; a trailing
     4th position is none. */
  g_body = [](lua_State* Lb) -> int {
    g_opt_check_called = false;
    lua_Integer v = luaL_opt(Lb, opt_check_probe, 1, 7);
    CHECK(v == 7);
    CHECK(!g_opt_check_called); /* nil arg: check not invoked */
    v = luaL_opt(Lb, opt_check_probe, 4, 7);
    CHECK(v == 7);
    CHECK(!g_opt_check_called); /* none (missing) arg: check not invoked */
    v = luaL_opt(Lb, opt_check_probe, 3, 7);
    CHECK(v == 55);
    CHECK(g_opt_check_called); /* present arg: check invoked */
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushnil(L);
  lua_pushnil(L);
  lua_pushinteger(L, 55);
  CHECK(lua_pcall(L, 3, 0, 0) == LUA_OK);

  /* luaL_prepbuffspan: write through area/off, and re-prep after growth
     must return a fresh valid pair */
  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    luaL_BuffSpan s1 = luaL_prepbuffspan(&B, 3);
    std::memcpy(s1.area.data() + s1.off, "abc", 3);
    luaL_addsize(&B, 3);
    /* force growth: a large second prep may reallocate; the returned pair
       must be used instead of the stale one */
    luaL_BuffSpan s2 = luaL_prepbuffspan(&B, 4096);
    CHECK(s2.off == 3u); /* content written so far is preserved */
    CHECK(s2.area.size() >= s2.off + 4096u);
    std::memset(s2.area.data() + s2.off, 'x', 4096);
    luaL_addsize(&B, 4096);
    luaL_pushresult(&B);
    std::optional<HostString> r = lua_tostring(L, -1);
    CHECK(r.has_value());
    if (r.has_value())
    {
      std::string_view v = r->to_utf8();
      CHECK(v.size() == 3u + 4096u);
      CHECK(v.substr(0, 3) == "abc");
      CHECK(v[3] == 'x' && v[v.size() - 1] == 'x');
    }
    lua_pop(L, 1);
  }

  lua_close(L);
}

/* ============ number-to-string conversions (std::to_chars sites) ============ */

static void test_num2str()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  /* --- luaO_tostringbuff, integer branch (lobject.cpp) --- */
  /* C-level coercion: lua_tolstring converts the number in place */
  lua_pushinteger(L, 0);
  CHECK(lua_tolstring(L, -1) == "0");
  lua_pop(L, 1);
  lua_pushinteger(L, 42);
  CHECK(lua_tolstring(L, -1) == "42");
  lua_pop(L, 1);
  lua_pushinteger(L, -42);
  CHECK(lua_tolstring(L, -1) == "-42");
  lua_pop(L, 1);
  lua_pushinteger(L, LUA_MAXINTEGER);
  CHECK(lua_tolstring(L, -1) == "9223372036854775807");
  lua_pop(L, 1);
  lua_pushinteger(L, LUA_MININTEGER);
  CHECK(lua_tolstring(L, -1) == "-9223372036854775808");
  lua_pop(L, 1);
  /* Lua-level tostring, incl. that the boundary constants agree */
  CHECK(do_string(L, "return tostring(0), tostring(-7), "
                     "tostring(math.maxinteger), tostring(math.mininteger)") ==
        LUA_OK);
  {
    std::optional<HostString> a = lua_tostring(L, -4);
    std::optional<HostString> b = lua_tostring(L, -3);
    std::optional<HostString> c = lua_tostring(L, -2);
    std::optional<HostString> d = lua_tostring(L, -1);
    CHECK(a.has_value() && a->to_utf8() == "0");
    CHECK(b.has_value() && b->to_utf8() == "-7");
    CHECK(c.has_value() && c->to_utf8() == "9223372036854775807");
    CHECK(d.has_value() && d->to_utf8() == "-9223372036854775808");
  }
  lua_pop(L, 4);

  /* --- num2straux exponent digits: string.format("%a"/"%A") (lstrlib.cpp) --- */
  /* exact %a mantissa formatting can vary; assert structure + round-trip */
  CHECK(do_string(L,
      "local vals = {1.0, 0.5, 2^-10, 1.5, 2^100, 2^-100, 1e300, 0.0}\n"
      "for _, v in ipairs(vals) do\n"
      "  local s = string.format('%a', v)\n"
      "  if s:sub(1, 2) ~= '0x' then return false, 'prefix: ' .. s end\n"
      "  if not s:find('p[+-]%d') then return false, 'exp: ' .. s end\n"
      "  if tonumber(s) ~= v then return false, 'roundtrip: ' .. s end\n"
      "end\n"
      "return true, 'ok'") == LUA_OK);
  CHECK(lua_toboolean(L, -2) == 1);
  if (lua_toboolean(L, -2) != 1)
  {
    std::optional<HostString> why = lua_tostring(L, -1);
    if (why.has_value())
      std::printf("  %%a failure: %s\n", std::string(why->to_utf8()).c_str());
  }
  lua_pop(L, 2);
  /* exponent sign and multi-digit exponents */
  CHECK(do_string(L,
      "return string.format('%a', 1.0), string.format('%a', 0.5),\n"
      "       string.format('%a', 2^100), string.format('%a', 2^-100),\n"
      "       string.format('%A', 0.5)") == LUA_OK);
  {
    std::optional<HostString> one = lua_tostring(L, -5);
    std::optional<HostString> half = lua_tostring(L, -4);
    std::optional<HostString> big = lua_tostring(L, -3);
    std::optional<HostString> tiny = lua_tostring(L, -2);
    std::optional<HostString> upper = lua_tostring(L, -1);
    CHECK(one.has_value() &&
          one->to_utf8().find("p+0") != std::string_view::npos);
    CHECK(half.has_value() &&
          half->to_utf8().find("p-1") != std::string_view::npos);
    CHECK(big.has_value() &&
          big->to_utf8().find("p+100") != std::string_view::npos);
    CHECK(tiny.has_value() &&
          tiny->to_utf8().find("p-100") != std::string_view::npos);
    CHECK(upper.has_value() &&
          upper->to_utf8().find("0X") != std::string_view::npos);
    CHECK(upper.has_value() &&
          upper->to_utf8().find("P-1") != std::string_view::npos);
  }
  lua_pop(L, 5);

  /* --- %q escaping of control characters (lstrlib.cpp) --- */
  /* control char NOT followed by a digit: short escape */
  CHECK(do_string(L, "return string.format('%q', '\\1x')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\1x\"");
  }
  lua_pop(L, 1);
  /* control char FOLLOWED by a digit: zero-padded 3-digit escape */
  CHECK(do_string(L, "return string.format('%q', '\\1' .. '1')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\0011\"");
  }
  lua_pop(L, 1);
  /* NUL followed by a digit gets the same padding */
  CHECK(do_string(L, "return string.format('%q', '\\0' .. '7')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\0007\"");
  }
  lua_pop(L, 1);
  /* %q output must round-trip through load */
  CHECK(do_string(L,
      "local s = '\\1' .. '1' .. '\\0' .. 'x\\n\"\\\\tail\\31' .. '9'\n"
      "local chunk = 'return ' .. string.format('%q', s)\n"
      "local f = assert(load(chunk))\n"
      "return f() == s") == LUA_OK);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 1);

  /* --- integer branches of %q for numbers, plus %d (lstrlib.cpp) --- */
  /* NOTE: the reviewed to_chars decimal branch lives in addliteral, the
     %q number path (reached by %q, not %d); mininteger takes the 0x
     branch above it. %d goes through formatint — covered as well. */
  CHECK(do_string(L,
      "return string.format('%q', 0), string.format('%q', -12345),\n"
      "       string.format('%q', math.maxinteger),\n"
      "       string.format('%q', math.mininteger)") == LUA_OK);
  {
    std::optional<HostString> z = lua_tostring(L, -4);
    std::optional<HostString> neg = lua_tostring(L, -3);
    std::optional<HostString> mx = lua_tostring(L, -2);
    std::optional<HostString> mn = lua_tostring(L, -1);
    CHECK(z.has_value() && z->to_utf8() == "0");
    CHECK(neg.has_value() && neg->to_utf8() == "-12345");
    CHECK(mx.has_value() && mx->to_utf8() == "9223372036854775807");
    /* official 5.5 quotes mininteger in hex so it reloads exactly */
    CHECK(mn.has_value() && mn->to_utf8() == "0x8000000000000000");
  }
  lua_pop(L, 4);
  /* %q integers round-trip through load with exact identity */
  CHECK(do_string(L,
      "for _, n in ipairs({0, -12345, math.maxinteger, math.mininteger}) do\n"
      "  local f = assert(load('return ' .. string.format('%q', n)))\n"
      "  local r = f()\n"
      "  if r ~= n or math.type(r) ~= 'integer' then return false end\n"
      "end\n"
      "return true") == LUA_OK);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 1);
  /* %d exact output */
  CHECK(do_string(L,
      "return string.format('%d', 0), string.format('%d', -5),\n"
      "       string.format('%d', math.maxinteger),\n"
      "       string.format('%d', math.mininteger)") == LUA_OK);
  {
    std::optional<HostString> z = lua_tostring(L, -4);
    std::optional<HostString> neg = lua_tostring(L, -3);
    std::optional<HostString> mx = lua_tostring(L, -2);
    std::optional<HostString> mn = lua_tostring(L, -1);
    CHECK(z.has_value() && z->to_utf8() == "0");
    CHECK(neg.has_value() && neg->to_utf8() == "-5");
    CHECK(mx.has_value() && mx->to_utf8() == "9223372036854775807");
    CHECK(mn.has_value() && mn->to_utf8() == "-9223372036854775808");
  }
  lua_pop(L, 4);

  lua_close(L);
}

/* ================= driver ================= */

struct TestEntry
{
  const char* name;
  void (*fn)();
};

/* ================= string coercion and pushfstring ================= */

static void test_string_coercion()
{
  lua_State* L = new_state();

  /* lua_tolstring on a number converts the value in place */
  lua_pushinteger(L, 42);
  CHECK(lua_type(L, -1) == LUA_TNUMBER);
  CHECK(lua_tolstring(L, -1) == "42");
  CHECK(lua_type(L, -1) == LUA_TSTRING); /* converted in place */
  lua_pop(L, 1);

  lua_pushnumber(L, 2.5);
  CHECK(lua_tolstring(L, -1) == "2.5");
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  /* strings pass through untouched */
  lua_pushliteral(L, "already");
  CHECK(lua_tolstring(L, -1) == "already");
  lua_pop(L, 1);

  /* non-coercible values yield an empty view, stack untouched */
  lua_pushboolean(L, 1);
  CHECK(lua_tolstring(L, -1).empty());
  CHECK(lua_type(L, -1) == LUA_TBOOLEAN);
  lua_pop(L, 1);

  /* lua_pushfstring: plain text, %% escape; result pushed and returned */
  {
    HostString r = lua_pushfstring(L, "hello there"_hs);
    CHECK(lua_type(L, -1) == LUA_TSTRING);
    CHECK(lua_tolstring(L, -1) == "hello there");
    CHECK(r == "hello there");
    lua_pop(L, 1);

    lua_pushfstring(L, "50%% done"_hs);
    CHECK(lua_tolstring(L, -1) == "50% done");
    lua_pop(L, 1);

    lua_pushfstring(L, ""_hs);
    CHECK(lua_tolstring(L, -1).empty());
    lua_pop(L, 1);
  }

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= lua_funcaddress ================= */

static void test_funcaddress()
{
  lua_State* L = new_state();

  /* reports the address of a light C function; stable per function */
  lua_pushcfunction(L, cf_add);
  HostString a1 = lua_funcaddress(L, -1);
  HostString a2 = lua_funcaddress(L, -1);
  CHECK(!a1.to_utf8().empty());
  CHECK(a1 == a2);

  /* distinct functions report distinct addresses */
  lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
  HostString a3 = lua_funcaddress(L, -1);
  CHECK(!a3.to_utf8().empty());
  CHECK(!(a1 == a3));
  lua_pop(L, 2);

  lua_close(L);
}

/* ================= arithmetic on floats via lua_arith ================= */

static void test_arith_floats()
{
  lua_State* L = new_state();

  /* floor division on floats stays float */
  lua_pushnumber(L, 7.0);
  lua_pushnumber(L, 2.0);
  lua_arith(L, LUA_OPIDIV);
  CHECK(lua_isinteger(L, -1) == 0);
  CHECK(lua_tonumber(L, -1) == 3.0);
  lua_pop(L, 1);

  lua_pushnumber(L, -7.5);
  lua_pushnumber(L, 2.0);
  lua_arith(L, LUA_OPIDIV);
  CHECK(lua_tonumber(L, -1) == -4.0);
  lua_pop(L, 1);

  /* float modulo */
  lua_pushnumber(L, 7.5);
  lua_pushnumber(L, 2.0);
  lua_arith(L, LUA_OPMOD);
  CHECK(lua_tonumber(L, -1) == 1.5);
  lua_pop(L, 1);

  /* mixed int/float promotes to float */
  lua_pushinteger(L, 3);
  lua_pushnumber(L, 0.5);
  lua_arith(L, LUA_OPADD);
  CHECK(lua_isinteger(L, -1) == 0);
  CHECK(lua_tonumber(L, -1) == 3.5);
  lua_pop(L, 1);

  /* unary minus on a float */
  lua_pushnumber(L, 2.5);
  lua_arith(L, LUA_OPUNM);
  CHECK(lua_tonumber(L, -1) == -2.5);
  lua_pop(L, 1);

  /* float operands that fit integers still float-divide */
  lua_pushnumber(L, 1.0);
  lua_pushnumber(L, 4.0);
  lua_arith(L, LUA_OPDIV);
  CHECK(lua_tonumber(L, -1) == 0.25);
  lua_pop(L, 1);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= fixed (in-place) binary chunks ================= */

static void test_fixed_chunks()
{
  lua_State* L = new_state();
  DumpBuf d; /* must outlive the loaded closure (destroyed after lua_close) */

  CHECK(load_string(L, "local acc = 0\n"
                       "for i = 1, 5 do acc = acc + i end\n"
                       "return acc * 2") == LUA_OK);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(!d.bytes.empty());

  /* mode "B": the chunk's code stays in the caller's buffer */
  StringReader r{std::string_view(d.bytes), false};
  CHECK(lua_load(L, whole_reader, &r, "=fixed"_hs, "B"_hs) == LUA_OK);
  CHECK(lua_type(L, -1) == LUA_TFUNCTION);

  /* callable, repeatedly, and across a full GC cycle */
  lua_pushvalue(L, -1);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 30);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 30);
  lua_pop(L, 1);

  /* a stripped dump loads fixed as well */
  {
    CHECK(load_string(L, "return 'fixed-stripped'") == LUA_OK);
    DumpBuf ds;
    CHECK(lua_dump(L, dump_writer, &ds, 1) == 0);
    lua_pop(L, 1);
    StringReader rs{std::string_view(ds.bytes), false};
    CHECK(lua_load(L, whole_reader, &rs, std::nullopt, "B"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tolstring(L, -1) == "fixed-stripped");
    lua_pop(L, 1);
    lua_close(L); /* close before ds dies: fixed chunks reference it */
    L = nullptr;
  }
  /* note: L closed above; d outlives it as required */
}

/* ================= userdata with uservalues across GC ================= */

static void test_userdata_uservalues_gc()
{
  lua_State* L = new_state();

  void* p = lua_newuserdatauv(L, 64, 3);
  CHECK(p != nullptr);
  CHECK(lua_type(L, -1) == LUA_TUSERDATA);
  std::memset(p, 0xAB, 64);

  /* uservalue 1: a table with content */
  lua_createtable(L, 0, 1);
  lua_pushinteger(L, 7);
  lua_setfield(L, -2, "x"_hs);
  CHECK(lua_setiuservalue(L, -2, 1) != 0);
  /* uservalue 2: a string */
  lua_pushliteral(L, "uv-two");
  CHECK(lua_setiuservalue(L, -2, 2) != 0);
  /* uservalue 3: a number */
  lua_pushinteger(L, 12345);
  CHECK(lua_setiuservalue(L, -2, 3) != 0);

  /* anchor it and run full GC cycles: traversal must keep the uservalues */
  lua_setfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(lua_getfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs) == LUA_TUSERDATA);
  CHECK(lua_touserdata(L, -1) == p);
  CHECK(static_cast<unsigned char*>(p)[0] == 0xAB); /* payload intact */
  CHECK(static_cast<unsigned char*>(p)[63] == 0xAB);

  CHECK(lua_getiuservalue(L, -1, 1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "x"_hs) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 2);
  CHECK(lua_getiuservalue(L, -1, 2) == LUA_TSTRING);
  CHECK(lua_tolstring(L, -1) == "uv-two");
  lua_pop(L, 1);
  CHECK(lua_getiuservalue(L, -1, 3) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 12345);
  lua_pop(L, 1);

  /* beyond the last uservalue: none */
  CHECK(lua_getiuservalue(L, -1, 4) == LUA_TNONE);
  lua_pop(L, 2);

  /* unanchor and collect: no crash, memory reclaimed */
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= upvalue pseudo-indices in C functions ================= */

static int cf_read_upval_type(lua_State* L)
{
  /* a light C function has no upvalues: the slot reads as "none" */
  lua_pushinteger(L, lua_type(L, lua_upvalueindex(1)));
  return 1;
}

static int cf_counter(lua_State* L)
{
  /* a C closure that updates its own upvalue on every call */
  lua_Integer n = lua_tointeger(L, lua_upvalueindex(1)) + 1;
  lua_pushinteger(L, n);
  lua_replace(L, lua_upvalueindex(1)); /* write back */
  lua_pushinteger(L, n);
  return 1;
}

static void test_c_upvalue_indices()
{
  lua_State* L = new_state();

  /* light C function reading a nonexistent upvalue */
  lua_pushcfunction(L, cf_read_upval_type);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == LUA_TNONE);
  lua_pop(L, 1);

  /* C closure keeping state in an upvalue it rewrites itself */
  lua_pushinteger(L, 0);
  lua_pushcclosure(L, cf_counter, 1);
  for (lua_Integer expect = 1; expect <= 3; expect++)
  {
    lua_pushvalue(L, -1);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == expect);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* reading past a C closure's last upvalue is "none" as well */
  lua_pushinteger(L, 9);
  lua_pushcclosure(L,
                   [](lua_State* LL) -> int
                   {
                     lua_pushinteger(LL, lua_type(LL, lua_upvalueindex(2)));
                     return 1;
                   },
                   1);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == LUA_TNONE);
  lua_pop(L, 1);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= yielding from a C hook ================= */

static void yielding_count_hook(lua_State* L, lua_Debug* ar)
{
  if (ar->event == LUA_HOOKCOUNT)
    lua_yield(L, 0); /* C hooks may suspend the running coroutine */
}

static void test_hook_yield()
{
  lua_State* L = new_state();
  lua_State* co = lua_newthread(L);

  lua_sethook(co, yielding_count_hook, LUA_MASKCOUNT, 25);
  CHECK(lua_gethook(co) == yielding_count_hook);

  CHECK(load_string(co, "local x = 0\n"
                        "for i = 1, 200 do x = x + i end\n"
                        "return x") == LUA_OK);

  int yields = 0;
  lua_ResumeResult rr = lua_resume(co, L, 0);
  while (rr.status == LUA_YIELD)
  {
    CHECK(rr.nres == 0); /* hook yields carry no values */
    yields++;
    CHECK(yields < 10000);
    rr = lua_resume(co, L, 0); /* continues the interrupted Lua code */
  }
  CHECK(rr.status == LUA_OK);
  CHECK(yields > 0); /* the hook did actually suspend execution */
  CHECK(rr.nres == 1);
  CHECK(lua_tointeger(co, -1) == 20100); /* result unharmed by suspensions */

  lua_pop(L, 1); /* the thread */
  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= selective stdlib loading ================= */

static void test_selected_libs()
{
  lua_State* L = new_state();

  /* load base+package+math; preload (but do not load) table */
  luaL_openselectedlibs(L, LUA_GLIBK | LUA_LOADLIBK | LUA_MATHLIBK,
                        LUA_TABLIBK);

  /* loaded libs are globals and work */
  CHECK(do_string(L, "return math.floor(3.7)") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 3);
  lua_pop(L, 1);

  /* unselected libs are absent */
  CHECK(do_string(L, "return type(string), type(os), type(io)") == LUA_OK);
  CHECK(lua_tolstring(L, -2) == "nil");
  CHECK(lua_tolstring(L, -1) == "nil");
  lua_pop(L, 3);

  /* preloaded lib is not a global but require() finds it */
  CHECK(do_string(L, "return type(table)") == LUA_OK);
  CHECK(lua_tolstring(L, -1) == "nil");
  lua_pop(L, 1);
  CHECK(do_string(L, "local t = require('table')\n"
                     "local a = {3, 1, 2}\n"
                     "t.sort(a)\n"
                     "return a[1], a[3]") == LUA_OK);
  CHECK(lua_tointeger(L, -2) == 1);
  CHECK(lua_tointeger(L, -1) == 3);
  lua_pop(L, 2);

  lua_close(L);
}

/* ================= host-internal API ================= */
/* The host-facing extensions in lua_internal.h (memory limits,
   lua_newuserdata, lua_resetthread) are tested in test_internal_api.cpp;
   this file includes only the official consumer headers. */

int main()
{
  const TestEntry tests[] = {
      {"version_and_constants", test_version_and_constants},
      {"state", test_state},
      {"stack_manipulation", test_stack_manipulation},
      {"access", test_access},
      {"arith", test_arith},
      {"compare", test_compare},
      {"push", test_push},
      {"get_set", test_get_set},
      {"call", test_call},
      {"load_dump", test_load_dump},
      {"coroutines", test_coroutines},
      {"warnings", test_warnings},
      {"gc", test_gc},
      {"misc", test_misc},
      {"debug_api", test_debug_api},
      {"libraries", test_libraries},
      {"negative", test_negative},
      {"auxlib_checks", test_auxlib_checks},
      {"auxlib_meta", test_auxlib_meta},
      {"auxlib_load", test_auxlib_load},
      {"auxlib_refs_buffer", test_auxlib_refs_buffer},
      {"auxlib_misc", test_auxlib_misc},
      {"dump_limits", test_dump_limits},
      {"cfunction_identity", test_cfunction_identity},
      {"continuation_chain", test_continuation_chain},
      {"thread_hooks", test_thread_hooks},
      {"io_handles", test_io_handles},
      {"auxlib_restored", test_auxlib_restored},
      {"num2str", test_num2str},
      {"string_coercion", test_string_coercion},
      {"funcaddress", test_funcaddress},
      {"arith_floats", test_arith_floats},
      {"fixed_chunks", test_fixed_chunks},
      {"userdata_uservalues_gc", test_userdata_uservalues_gc},
      {"c_upvalue_indices", test_c_upvalue_indices},
      {"hook_yield", test_hook_yield},
      {"selected_libs", test_selected_libs},
  };

  for (const TestEntry& t : tests)
  {
    g_current = t.name;
    t.fn();
  }

  std::printf("test_public_api: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
