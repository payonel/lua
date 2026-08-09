
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

static void push_table_with_mm(lua_State* L, const char* event, lua_CFunction fn)
{
  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, event);
  int r = lua_setmetatable(L, -2);
  CHECK(r == 1);
}

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
  return 33;
}

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

static int g_cont_status = -999;
static lua_KContext g_cont_ctx = 0;

static int cont_call(lua_State* L, int status, lua_KContext ctx)
{
  g_cont_status = status;
  g_cont_ctx = ctx;
  return lua_gettop(L);
}

static int cf_callk(lua_State* L)
{
  lua_pushvalue(L, 1);
  lua_callk(L, 0, LUA_MULTRET, 42, cont_call);
  return cont_call(L, LUA_OK, 42);
}

static int cont_yield(lua_State* L, int status, lua_KContext ctx)
{
  g_cont_status = status;
  g_cont_ctx = ctx;
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
  return lua_yield(L, lua_gettop(L));
}

static int cf_tbc_closeslot(lua_State* L)
{
  int before = g_closed;
  push_table_with_mm(L, "__close", cf_mm_close);
  lua_toclose(L, -1);
  lua_closeslot(L, -1);
  CHECK(g_closed == before + 1);
  CHECK(lua_isnil(L, -1) == 1);
  return 0;
}

static int cf_tbc_settop(lua_State* L)
{
  int before = g_closed;
  int base = lua_gettop(L);
  push_table_with_mm(L, "__close", cf_mm_close);
  lua_toclose(L, -1);
  lua_settop(L, base);
  CHECK(g_closed == before + 1);
  return 0;
}

static int cf_probe(lua_State* L)
{
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
      CHECK(in0->name == "probe");
      CHECK(in0->namewhat == "global");
    }
    int top = lua_gettop(L);
    std::optional<lua_Debug> inf = lua_getinfo(L, "f", *ar0);
    CHECK(inf.has_value());
    CHECK(lua_gettop(L) == top + 1);
    CHECK(lua_tocfunction(L, -1) == cf_probe);
    lua_pop(L, 1);
  }

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
      CHECK(in1->currentline == 2);
    }

    int top = lua_gettop(L);
    std::optional<HostString> lname = lua_getlocal(L, &*ar1, 1);
    CHECK(lname.has_value() && *lname == "xyz");
    CHECK(lua_gettop(L) == top + 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    lua_pushinteger(L, 99);
    std::optional<HostString> sname = lua_setlocal(L, &*ar1, 1);
    CHECK(sname.has_value() && *sname == "xyz");
    CHECK(lua_gettop(L) == top);

    std::optional<HostString> nolocal = lua_getlocal(L, &*ar1, 50);
    CHECK(!nolocal.has_value());
  }

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
  CHECK(lua_upvalueindex(1) == LUA_REGISTRYINDEX - 1);
  CHECK(lua_upvalueindex(2) == LUA_REGISTRYINDEX - 2);

  lua_State* L = new_state();
  CHECK(lua_version(L) == static_cast<lua_Number>(LUA_VERSION_NUM));
  lua_close(L);
}

static void test_state()
{
  lua_State* L = new_state();

  lua_CFunction old = lua_atpanic(L, cf_panic);
  lua_CFunction prev = lua_atpanic(L, old);
  CHECK(prev == cf_panic);

  int top = lua_gettop(L);
  lua_State* L1 = lua_newthread(L);
  CHECK(L1 != nullptr);
  CHECK(lua_gettop(L) == top + 1);
  CHECK(lua_type(L, -1) == LUA_TTHREAD);
  CHECK(lua_tothread(L, -1) == L1);

  lua_pushinteger(L1, 1);
  int st = lua_closethread(L1, nullptr);
  CHECK(st == LUA_OK);
  CHECK(lua_gettop(L1) == 0);
  CHECK(lua_status(L1) == LUA_OK);

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

  CHECK(lua_absindex(L, -1) == 5);
  CHECK(lua_absindex(L, -5) == 1);
  CHECK(lua_absindex(L, 3) == 3);
  CHECK(lua_absindex(L, LUA_REGISTRYINDEX) == LUA_REGISTRYINDEX);

  lua_pushvalue(L, 2);
  CHECK(lua_gettop(L) == 6);
  CHECK(lua_tointeger(L, -1) == 2);
  lua_pop(L, 1);

  lua_rotate(L, 2, 1);
  CHECK(lua_tointeger(L, 2) == 5);
  CHECK(lua_tointeger(L, 3) == 2);
  CHECK(lua_tointeger(L, 5) == 4);
  lua_rotate(L, 2, -1);
  for (int i = 1; i <= 5; i++)
    CHECK(lua_tointeger(L, i) == i);

  lua_copy(L, 5, 1);
  CHECK(lua_tointeger(L, 1) == 5);
  CHECK(lua_gettop(L) == 5);
  lua_pushinteger(L, 1);
  lua_copy(L, -1, 1);
  lua_pop(L, 1);

  lua_pushinteger(L, 9);
  lua_insert(L, 2);
  CHECK(lua_tointeger(L, 2) == 9);
  CHECK(lua_tointeger(L, 3) == 2);
  CHECK(lua_gettop(L) == 6);

  lua_remove(L, 2);
  CHECK(lua_tointeger(L, 2) == 2);
  CHECK(lua_gettop(L) == 5);

  lua_pushinteger(L, 77);
  lua_replace(L, 1);
  CHECK(lua_tointeger(L, 1) == 77);
  CHECK(lua_gettop(L) == 5);

  lua_settop(L, 7);
  CHECK(lua_gettop(L) == 7);
  CHECK(lua_isnil(L, 6) == 1);
  CHECK(lua_isnil(L, 7) == 1);
  lua_settop(L, -3);
  CHECK(lua_gettop(L) == 5);
  lua_pop(L, 2);
  CHECK(lua_gettop(L) == 3);
  lua_settop(L, 0);
  CHECK(lua_gettop(L) == 0);

  CHECK(lua_checkstack(L, 30) != 0);
  CHECK(lua_checkstack(L, 0) != 0);
  CHECK(lua_checkstack(L, INT_MAX) == 0);

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

  lua_pushnil(L);
  lua_pushboolean(L, 1);
  lua_pushinteger(L, 3);
  lua_pushnumber(L, 3.5);
  lua_pushliteral(L, "hello");
  lua_newtable(L);
  lua_pushcfunction(L, cf_add);
  void* ud = lua_newuserdatauv(L, 16, 1);
  lua_pushlightuserdata(L, &s_light);
  lua_State* L1 = lua_newthread(L);
  lua_pushliteral(L, "10");

  CHECK(ud != nullptr);

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
  CHECK(lua_type(L, 15) == LUA_TNONE);

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

  CHECK(lua_isnil(L, 1) == 1);
  CHECK(lua_isboolean(L, 2) == 1);
  CHECK(lua_isnumber(L, 3) == 1);
  CHECK(lua_isnumber(L, 4) == 1);
  CHECK(lua_isnumber(L, 11) == 1);
  CHECK(lua_isnumber(L, 5) == 0);
  CHECK(lua_isinteger(L, 3) == 1);
  CHECK(lua_isinteger(L, 4) == 0);
  CHECK(lua_isinteger(L, 11) == 0);
  CHECK(lua_isstring(L, 5) == 1);
  CHECK(lua_isstring(L, 3) == 1);
  CHECK(lua_isstring(L, 6) == 0);
  CHECK(lua_istable(L, 6) == 1);
  CHECK(lua_isfunction(L, 7) == 1);
  CHECK(lua_iscfunction(L, 7) == 1);
  CHECK(lua_isuserdata(L, 8) == 1);
  CHECK(lua_isuserdata(L, 9) == 1);
  CHECK(lua_islightuserdata(L, 9) == 1);
  CHECK(lua_islightuserdata(L, 8) == 0);
  CHECK(lua_isthread(L, 10) == 1);

  CHECK(lua_tonumberx(L, 3) == std::optional<lua_Number>(3.0));
  CHECK(lua_tonumberx(L, 4) == std::optional<lua_Number>(3.5));
  CHECK(lua_tonumberx(L, 11) == std::optional<lua_Number>(10.0));
  CHECK(!lua_tonumberx(L, 1).has_value());
  CHECK(!lua_tonumberx(L, 6).has_value());
  CHECK(lua_tointegerx(L, 3) == std::optional<lua_Integer>(3));
  CHECK(!lua_tointegerx(L, 4).has_value());
  CHECK(lua_tointegerx(L, 11) == std::optional<lua_Integer>(10));
  CHECK(lua_tonumber(L, 4) == 3.5);
  CHECK(lua_tonumber(L, 1) == 0.0);
  CHECK(lua_tointeger(L, 3) == 3);
  CHECK(lua_tointeger(L, 1) == 0);

  CHECK(lua_toboolean(L, 1) == 0);
  CHECK(lua_toboolean(L, 2) == 1);
  CHECK(lua_toboolean(L, 3) == 1);
  lua_pushboolean(L, 0);
  CHECK(lua_toboolean(L, -1) == 0);
  lua_pushinteger(L, 0);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 2);

  {
    std::optional<HostString> s = lua_tostring(L, 5);
    CHECK(s.has_value() && *s == "hello");
    auto sv = lua_tolstring(L, 5);
    CHECK(sv.has_value() && *sv == "hello");
    lua_pushinteger(L, 12);
    auto nv = lua_tolstring(L, -1);
    CHECK(nv.has_value() && *nv == "12");
    CHECK(lua_type(L, -1) == LUA_TSTRING);
    lua_pop(L, 1);
    auto tv = lua_tolstring(L, 6);
    CHECK(!tv.has_value());
    CHECK(!lua_tostring(L, 6).has_value());
  }

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
  CHECK(lua_rawlen(L, 1) == 0u);

  CHECK(lua_tocfunction(L, 7) == cf_add);
  CHECK(lua_tocfunction(L, 6) == nullptr);
  CHECK(load_string(L, "return 1") == LUA_OK);
  CHECK(lua_iscfunction(L, -1) == 0);
  CHECK(lua_tocfunction(L, -1) == nullptr); /* Lua function */
  lua_pop(L, 1);

  CHECK(lua_touserdata(L, 8) == ud);
  CHECK(lua_touserdata(L, 9) == &s_light);
  CHECK(lua_touserdata(L, 6) == nullptr);

  CHECK(lua_tothread(L, 10) == L1);
  CHECK(lua_tothread(L, 6) == nullptr);

  CHECK(lua_topointer(L, 6) != nullptr);
  CHECK(lua_topointer(L, 7) == reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(cf_add)));
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_upvals, 1);
  CHECK(lua_topointer(L, -1) != nullptr);
  lua_pop(L, 1);
  CHECK(lua_topointer(L, 8) != nullptr);
  CHECK(lua_topointer(L, 10) != nullptr);
  CHECK(lua_topointer(L, 3) == nullptr);
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

  lua_pushinteger(L, 5);
  lua_arith(L, LUA_OPUNM);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tointeger(L, -1) == -5);
  lua_pop(L, 1);

  lua_pushinteger(L, 0);
  lua_arith(L, LUA_OPBNOT);
  CHECK(lua_tointeger(L, -1) == -1);
  lua_pop(L, 1);

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

  CHECK(lua_rawequal(L, 1, 3) == 1);
  CHECK(lua_rawequal(L, 1, 2) == 0);
  CHECK(lua_rawequal(L, 1, 4) == 0);
  CHECK(lua_rawequal(L, 1, 15) == 0);
  CHECK(lua_rawequal(L, 15, 16) == 0);

  CHECK(lua_compare(L, 1, 3, LUA_OPEQ) == 1);
  CHECK(lua_compare(L, 1, 2, LUA_OPEQ) == 0);
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  CHECK(lua_compare(L, 2, 1, LUA_OPLT) == 0);
  CHECK(lua_compare(L, 1, 3, LUA_OPLE) == 1);
  CHECK(lua_compare(L, 2, 1, LUA_OPLE) == 0);
  CHECK(lua_compare(L, 1, 4, LUA_OPEQ) == 0);
  CHECK(lua_compare(L, 1, 15, LUA_OPEQ) == 0);
  lua_settop(L, 0);

  lua_pushinteger(L, 1);
  lua_pushnumber(L, 1.5);
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  lua_settop(L, 0);

  lua_pushliteral(L, "abc");
  lua_pushliteral(L, "abd");
  CHECK(lua_compare(L, 1, 2, LUA_OPLT) == 1);
  lua_settop(L, 0);

  push_table_with_mm(L, "__eq", cf_mm_true);
  push_table_with_mm(L, "__eq", cf_mm_true);
  CHECK(lua_rawequal(L, 1, 2) == 0);
  CHECK(lua_compare(L, 1, 2, LUA_OPEQ) == 1);
  push_table_with_mm(L, "__lt", cf_mm_true);
  push_table_with_mm(L, "__lt", cf_mm_true);
  CHECK(lua_compare(L, 3, 4, LUA_OPLT) == 1);
  lua_settop(L, 0);

  lua_close(L);
}

static void test_push()
{
  lua_State* L = new_state();

  {
    std::string_view src = "hello world";
    std::string_view r = lua_pushlstring(L, src, 6, 5);
    CHECK(r == "world");
    CHECK(*lua_tolstring(L, -1) == "world");
    lua_pop(L, 1);

    std::string_view withnul("a\0b", 3);
    r = lua_pushlstring(L, withnul, 0, 3);
    CHECK(r.size() == 3);
    CHECK(lua_rawlen(L, -1) == 3u);
    std::string_view back = *lua_tolstring(L, -1);
    CHECK(back.size() == 3 && std::memcmp(back.data(), "a\0b", 3) == 0);
    lua_pop(L, 1);

    r = lua_pushlstring(L, std::string_view(), 0, 0);
    CHECK(r.empty());
    CHECK(lua_rawlen(L, -1) == 0u);
    lua_pop(L, 1);
  }

  lua_pushstring(L, "abc"_hs);
  CHECK(lua_tolstring(L, -1) == "abc");
  lua_pushstring(L, std::nullopt);
  CHECK(lua_isnil(L, -1) == 1);
  lua_pop(L, 2);

  lua_pushliteral(L, "lit");
  CHECK(lua_tolstring(L, -1) == "lit");
  lua_pop(L, 1);

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

  lua_pushinteger(L, 30);
  lua_pushinteger(L, 12);
  lua_pushcclosure(L, cf_upvals, 2);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_iscfunction(L, -1) == 1);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  lua_pushboolean(L, 5);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pushboolean(L, 0);
  CHECK(lua_toboolean(L, -1) == 0);
  lua_pop(L, 2);

  CHECK(lua_pushthread(L) == 1);
  CHECK(lua_tothread(L, -1) == L);
  lua_State* L1 = lua_newthread(L);
  CHECK(lua_pushthread(L1) == 0);
  CHECK(lua_tothread(L1, -1) == L1);
  lua_pop(L, 2);

  lua_pushglobaltable(L);
  CHECK(lua_istable(L, -1) == 1);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_get_set()
{
  lua_State* L = new_state();
  static int s_key1, s_key2;

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

  lua_pushinteger(L, 6);
  lua_setfield(L, tidx, "f");
  t = lua_getfield(L, tidx, "f");
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 6);
  CHECK(lua_getfield(L, tidx, "missing") == LUA_TNIL);
  lua_pop(L, 2);

  lua_pushinteger(L, 7);
  lua_seti(L, tidx, 3);
  t = lua_geti(L, tidx, 3);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  lua_pushliteral(L, "rk");
  lua_pushinteger(L, 8);
  lua_rawset(L, tidx);
  lua_pushliteral(L, "rk");
  t = lua_rawget(L, tidx);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 8);
  lua_pop(L, 1);

  lua_pushinteger(L, 9);
  lua_rawseti(L, tidx, 4);
  t = lua_rawgeti(L, tidx, 4);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 9);
  lua_pop(L, 1);

  lua_pushinteger(L, 10);
  lua_rawsetp(L, tidx, &s_key1);
  t = lua_rawgetp(L, tidx, &s_key1);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 10);
  CHECK(lua_rawgetp(L, tidx, &s_key2) == LUA_TNIL);
  lua_pop(L, 2);
  lua_pop(L, 1);

  lua_newtable(L);
  CHECK(lua_getmetatable(L, -1) == 0);
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_insert(L, -3);
  CHECK(lua_setmetatable(L, -2) == 1);
  CHECK(lua_getmetatable(L, -1) == 1);
  CHECK(lua_rawequal(L, -1, -3) == 1);
  lua_pop(L, 3);

  push_table_with_mm(L, "__index", cf_mm_index);
  lua_pushliteral(L, "anything");
  t = lua_gettable(L, -2);
  CHECK(t == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  lua_pushliteral(L, "anything");
  CHECK(lua_rawget(L, -2) == LUA_TNIL);
  lua_pop(L, 2);

  g_newindex_hits = 0;
  push_table_with_mm(L, "__newindex", cf_mm_newindex);
  lua_pushliteral(L, "x");
  lua_pushinteger(L, 1);
  lua_settable(L, -3);
  CHECK(g_newindex_hits == 1);
  lua_pushliteral(L, "x");
  CHECK(lua_rawget(L, -2) == LUA_TNIL);
  lua_pop(L, 2);

  lua_newuserdatauv(L, 8, 2);
  int uidx = lua_gettop(L);
  CHECK(lua_getiuservalue(L, uidx, 1) == LUA_TNIL);
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
  CHECK(lua_getiuservalue(L, uidx, 3) == LUA_TNONE);
  CHECK(lua_isnil(L, -1) == 1);
  lua_pop(L, 1);
  lua_pushnil(L);
  CHECK(lua_setiuservalue(L, uidx, 3) == 0);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_call()
{
  lua_State* L = new_state();
  luaL_openselectedlibs(L, LUA_GLIBK, 0);

  lua_pushcfunction(L, cf_add);
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 40);
  lua_call(L, 2, 1);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  CHECK(do_string(L, "return 1, 2, 3") == LUA_OK);
  CHECK(lua_gettop(L) == 3);
  CHECK(lua_tointeger(L, 1) == 1);
  CHECK(lua_tointeger(L, 3) == 3);
  lua_settop(L, 0);

  CHECK(load_string(L, "return 1, 2") == LUA_OK);
  lua_call(L, 0, 4);
  CHECK(lua_gettop(L) == 4);
  CHECK(lua_isnil(L, 4) == 1);
  lua_settop(L, 0);

  int base = lua_gettop(L);
  lua_pushcfunction(L, cf_raise);
  lua_pushinteger(L, 1);
  int st = lua_pcall(L, 1, 0, 0);
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_gettop(L) == base + 1);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value() && *msg == "boom");
  }
  lua_pop(L, 1);

  st = do_string(L, "local x = nil\nreturn x.y");
  CHECK(st == LUA_ERRRUN);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value());
    if (msg.has_value())
      CHECK(msg->to_utf8().find("tpa:2") != std::string_view::npos);
  }
  lua_pop(L, 1);

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

  lua_pushcfunction(L, cf_raise);
  mh = lua_gettop(L);
  lua_pushcfunction(L, cf_raise);
  st = lua_pcall(L, 0, 0, mh);
  CHECK(st == LUA_ERRERR);
  lua_pop(L, 2);

  lua_pushcfunction(L, cf_raise_nil);
  st = lua_pcall(L, 0, 0, 0);
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  st = do_string(L, "error({code = 5})");
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "code") == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 5);
  lua_pop(L, 2);

  g_cont_status = -999;
  lua_pushcfunction(L, cf_raise);
  st = lua_pcallk(L, 0, 0, 0, 0, cont_call);
  CHECK(st == LUA_ERRRUN);
  CHECK(g_cont_status == -999);
  lua_pop(L, 1);

  g_cont_status = -999;
  g_cont_ctx = 0;
  lua_pushcfunction(L, cf_callk);
  lua_pushcfunction(L, cf_ret7);
  lua_call(L, 1, LUA_MULTRET);
  CHECK(g_cont_status == LUA_OK);
  CHECK(g_cont_ctx == 42);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_settop(L, 0);

  push_table_with_mm(L, "__call", cf_ret7);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_load_dump()
{
  lua_State* L = new_state();

  CHECK(load_string(L, "return 40 + 2") == LUA_OK);
  CHECK(lua_type(L, -1) == LUA_TFUNCTION);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  int base = lua_gettop(L);
  CHECK(load_string(L, "return +++") == LUA_ERRSYNTAX);
  CHECK(lua_gettop(L) == base + 1);
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  {
    ByteReader r{"return 6 * 7", 0};
    CHECK(lua_load(L, byte_reader, &r, "=bytes"_hs, std::nullopt) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
  }

  {
    CHECK(load_string(L, "return 10 + 11") == LUA_OK);
    DumpBuf d;
    int st = lua_dump(L, dump_writer, &d, 0);
    CHECK(st == 0);
    CHECK(!d.bytes.empty());
    CHECK(d.empty_calls >= 1);
    CHECK(d.empty_was_last);
    CHECK(lua_type(L, -1) == LUA_TFUNCTION);
    lua_pop(L, 1);

    CHECK(d.bytes.size() >= 4 &&
          std::string_view(d.bytes).substr(0, 4) == LUA_SIGNATURE);

    StringReader r{std::string_view(d.bytes), false};
    CHECK(lua_load(L, whole_reader, &r, "=dumped"_hs, "b"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 21);
    lua_pop(L, 1);

    StringReader r2{std::string_view(d.bytes), false};
    int lst = lua_load(L, whole_reader, &r2, "=dumped"_hs, "t"_hs);
    CHECK(lst != LUA_OK);
    lua_pop(L, 1);

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

  {
    CHECK(load_string(L, "return 1") == LUA_OK);
    int calls = 0;
    int st = lua_dump(L, failing_writer, &calls, 0);
    CHECK(st == 33);
    CHECK(calls == 1);
    lua_pop(L, 1);
  }

  lua_close(L);
}

static void test_coroutines()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  {
    lua_State* L1 = lua_newthread(L);
    CHECK(lua_status(L1) == LUA_OK);
    CHECK(lua_isyieldable(L) == 0);

    lua_pushcfunction(L1, cf_co);
    lua_pushinteger(L1, 1);
    lua_ResumeResult rr = lua_resume(L1, L, 1);
    CHECK(rr.status == LUA_YIELD);
    CHECK(rr.nres == 1);
    CHECK(lua_tointeger(L1, -1) == 100);
    CHECK(lua_status(L1) == LUA_YIELD);

    lua_pop(L1, 1);
    g_cont_status = -999;
    lua_pushinteger(L1, 8);
    lua_pushinteger(L1, 9);
    rr = lua_resume(L1, L, 2);
    CHECK(rr.status == LUA_OK);
    CHECK(g_cont_status == LUA_YIELD);
    CHECK(g_cont_ctx == 7);
    CHECK(rr.nres >= 2);
    CHECK(lua_tointeger(L1, -1) == 9);
    CHECK(lua_tointeger(L1, -2) == 8);
    CHECK(lua_status(L1) == LUA_OK);

    lua_settop(L1, 0);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    CHECK(rr.nres == 1);
    {
      std::optional<HostString> msg = lua_tostring(L1, -1);
      CHECK(msg.has_value());
      if (msg.has_value())
        CHECK(msg->to_utf8().find("dead coroutine") != std::string_view::npos);
    }
    lua_pop(L, 1); /* thread */
  }

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
    lua_settop(L1, 0);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    int cst = lua_closethread(L1, L);
    CHECK(cst == LUA_ERRRUN);
    CHECK(lua_status(L1) == LUA_OK);
    CHECK(lua_gettop(L1) == 1);
    CHECK(lua_type(L1, -1) == LUA_TSTRING);
    lua_settop(L1, 0);
    CHECK(load_string(L1, "return 'revived'") == LUA_OK);
    rr = lua_resume(L1, L, 0);
    CHECK(rr.status == LUA_OK);
    lua_pop(L, 1);
  }

  {
    lua_State* L1 = lua_newthread(L);
    lua_pushinteger(L, 71);
    lua_pushinteger(L, 72);
    lua_xmove(L, L1, 2);
    CHECK(lua_gettop(L1) == 2);
    CHECK(lua_tointeger(L1, 1) == 71);
    CHECK(lua_tointeger(L1, 2) == 72);
    CHECK(lua_type(L, -1) == LUA_TTHREAD);
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

  int prev = lua_gc(L, LUA_GCINC, 0, 0, 0);
  CHECK(prev == LUA_GCGEN || prev == LUA_GCINC);
  prev = lua_gc(L, LUA_GCGEN, 0, 0, 0);
  CHECK(prev == LUA_GCINC);
  prev = lua_gc(L, LUA_GCINC, 0, 0, 0);
  CHECK(prev == LUA_GCGEN);

  int cur = lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, -1);
  CHECK(cur >= 0);
  int old = lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, 300);
  CHECK(old == cur);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, -1) == 300);
  lua_gc(L, LUA_GCPARAM, 0, LUA_GCPPAUSE, cur);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMINORMUL, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMAJORMINOR, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPMINORMAJOR, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPSTEPMUL, -1) >= 0);
  CHECK(lua_gc(L, LUA_GCPARAM, 0, LUA_GCPSTEPSIZE, -1) >= 0);

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
    CHECK(lua_gettop(L) == tidx + 2);
    lua_pop(L, 1);
  }
  CHECK(cnt == 4);
  CHECK(lua_gettop(L) == tidx);
  lua_newtable(L);
  lua_pushnil(L);
  CHECK(lua_next(L, -2) == 0);
  lua_pop(L, 2);

  lua_settop(L, 0);
  lua_pushliteral(L, "a");
  lua_pushinteger(L, 1);
  lua_pushliteral(L, "b");
  lua_concat(L, 3);
  CHECK(lua_gettop(L) == 1);
  CHECK(lua_tolstring(L, -1) == "a1b");
  lua_pop(L, 1);
  lua_concat(L, 0);
  CHECK(lua_tolstring(L, -1) == "");
  lua_pop(L, 1);
  lua_pushinteger(L, 5);
  lua_concat(L, 1);
  CHECK(lua_type(L, -1) == LUA_TNUMBER);
  lua_pop(L, 1);
  push_table_with_mm(L, "__concat", cf_mm_concat);
  lua_pushliteral(L, "z");
  lua_concat(L, 2);
  CHECK(lua_tolstring(L, -1) == "CC");
  lua_pop(L, 1);

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

    r = lua_stringtonumber(L, " 42 ");
    CHECK(r == 5u);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    int top = lua_gettop(L);
    CHECK(lua_stringtonumber(L, "abc") == 0u);
    CHECK(lua_stringtonumber(L, "") == 0u);
    CHECK(lua_gettop(L) == top);
  }

  {
    lua_pushinteger(L, 42);
    std::optional<HostString> s = lua_numbertocstring(L, -1);
    CHECK(s.has_value() && *s == "42");
    lua_pop(L, 1);
    lua_pushnumber(L, 0.5);
    s = lua_numbertocstring(L, -1);
    CHECK(s.has_value() && *s == "0.5");
    CHECK(lua_type(L, -1) == LUA_TNUMBER);
    lua_pop(L, 1);
    lua_pushliteral(L, "42");
    s = lua_numbertocstring(L, -1);
    CHECK(!s.has_value());
    lua_pop(L, 1);
  }

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
    lua_setallocf(L, counting_alloc, &g_alloc_stats);
  }

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

  CHECK(load_string(L, "local function inner(a, b, ...) end\nreturn inner") == LUA_OK);
  lua_call(L, 0, 1);
  int top = lua_gettop(L);
  lua_pushvalue(L, -1);
  lua_Debug query{};
  std::optional<lua_Debug> info = lua_getinfo(L, ">Su", query);
  CHECK(lua_gettop(L) == top);
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
  lua_pushvalue(L, -1);
  info = lua_getinfo(L, ">L", query);
  CHECK(info.has_value());
  CHECK(lua_istable(L, -1) == 1);
  lua_pop(L, 2);

  lua_pushcfunction(L, cf_add);
  info = lua_getinfo(L, ">Z", query);
  CHECK(!info.has_value());

  CHECK(load_string(L, "return 1") == LUA_OK);
  info = lua_getinfo(L, ">S", query);
  CHECK(info.has_value());
  if (info.has_value())
    CHECK(info->what == "main");

  lua_pushcfunction(L, cf_add);
  info = lua_getinfo(L, ">S", query);
  CHECK(info.has_value());
  if (info.has_value())
  {
    CHECK(info->what == "C");
    CHECK(info->linedefined == -1);
    CHECK(info->lastlinedefined == -1);
  }

  lua_register(L, "probe", cf_probe);
  CHECK(do_string(L, "local xyz = 42\nprobe()\nreturn xyz") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);

  CHECK(!lua_getstack(L, 0).has_value());

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
    CHECK(lua_gettop(L) == top);
  }
  lua_pop(L, 1);

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
  CHECK(lua_tointeger(L, -1) == 1);
  lua_pop(L, 3);

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
    CHECK(lua_getglobal(L, "print") == LUA_TFUNCTION);
    CHECK(lua_getglobal(L, "_G") == LUA_TTABLE);
    CHECK(lua_getglobal(L, "_VERSION") == LUA_TSTRING);
    {
      std::optional<HostString> v = lua_tostring(L, -1);
      CHECK(v.has_value() && *v == "Lua 5.5");
    }
    lua_pop(L, 3);

    CHECK(do_string(L, "return string.rep('ab', 3)") == LUA_OK);
    CHECK(lua_tolstring(L, -1) == "ababab");
    lua_pop(L, 1);
    lua_close(L);
  }

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

  CHECK(LUA_COLIBNAME == "coroutine");
  CHECK(LUA_TABLIBNAME == "table");
  CHECK(LUA_IOLIBNAME == "io");
  CHECK(LUA_OSLIBNAME == "os");
  CHECK(LUA_STRLIBNAME == "string");
  CHECK(LUA_UTF8LIBNAME == "utf8");
  CHECK(LUA_MATHLIBNAME == "math");
  CHECK(LUA_DBLIBNAME == "debug");
  CHECK(LUA_LOADLIBNAME == "package");
}

static bool top_msg_contains(lua_State* L, std::string_view what)
{
  std::optional<HostString> m = lua_tostring(L, -1);
  bool ok = m.has_value() && m->to_utf8().find(what) != std::string_view::npos;
  lua_pop(L, 1);
  return ok;
}

struct PanicJump
{
};
static std::string g_panic_msg;
static int cf_panic_record(lua_State* L)
{
  std::optional<HostString> m = lua_tostring(L, -1);
  if (m.has_value())
    g_panic_msg = std::string(m->to_utf8());
  throw PanicJump{};
}

static void* null_alloc(void*, void* ptr, size_t, size_t nsize)
{
  if (nsize == 0)
  {
    std::free(ptr);
    return nullptr;
  }
  return nullptr;
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
  if (d->deny && (ptr == nullptr || nsize > osize))
    return nullptr;
  return std::realloc(ptr, nsize);
}

static int cf_neg_arith(lua_State* L)
{
  lua_arith(L, LUA_OPADD);
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

  {
    lua_State* L = lua_newstate(null_alloc, nullptr, 0);
    CHECK(L == nullptr);
  }

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
    CHECK(do_string(L, "return 1 + 1") == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 2);
    lua_pop(L, 1);
    lua_close(L);
  }

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
    lua_pushinteger(L, 5);
    lua_pushstring(L, "k"_hs);
    lua_pushstring(L, "v"_hs);
    CHECK(lua_pcall(L, 3, 0, 0) == LUA_ERRRUN);
    CHECK(top_msg_contains(L, "index"));

    lua_close(L);
  }

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

  {
    lua_State* L = new_state();
    CHECK(load_string(L, "") == LUA_OK);
    CHECK(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK);
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
  }
}

static lua_CFunction g_body = nullptr;
static int cf_body(lua_State* L)
{
  return g_body(L);
}

static void test_auxlib_checks()
{
  lua_State* L = new_state();

  g_body = [](lua_State* Lb) -> int {
    CHECK(luaL_checkinteger(Lb, 1) == 42);
    CHECK(luaL_checknumber(Lb, 2) == 2.5);
    CHECK(luaL_checklstring(Lb, 3) == "abc");
    CHECK(luaL_checkstring(Lb, 4).to_utf8() == "7");
    CHECK(lua_type(Lb, 4) == LUA_TSTRING);
    CHECK(luaL_optinteger(Lb, 5, 9) == 9);
    CHECK(luaL_optnumber(Lb, 5, 1.5) == 1.5);
    CHECK(luaL_optlstring(Lb, 5, "def") == "def");
    CHECK(!luaL_optstring(Lb, 5, std::nullopt).has_value());
    CHECK(luaL_optinteger(Lb, 1, 9) == 42);
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

  g_body = [](lua_State* Lb) -> int {
    luaL_checkinteger(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "x"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "bad argument"));

  lua_pushcfunction(L, cf_body);
  lua_pushnumber(L, 3.5);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "no integer representation"));

  g_body = [](lua_State* Lb) -> int {
    luaL_checklstring(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_createtable(L, 0, 0);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "string expected"));

  g_body = [](lua_State* Lb) -> int {
    luaL_checktype(Lb, 1, LUA_TTABLE);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushinteger(L, 1);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "table expected"));

  g_body = [](lua_State* Lb) -> int {
    luaL_checkany(Lb, 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "value expected"));

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

  g_body = [](lua_State* Lb) -> int {
    luaL_checkstack(Lb, INT_MAX, "boom"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "stack overflow"));

  static const HostString k_opts[] = {"one", "two", "three"};
  g_body = [](lua_State* Lb) -> int {
    CHECK(luaL_checkoption(Lb, 1, std::nullopt, k_opts) == 2);
    CHECK(luaL_checkoption(Lb, 2, HostString("two"), k_opts) == 1);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "three"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_OK);

  g_body = [](lua_State* Lb) -> int {
    return luaL_checkoption(Lb, 1, std::nullopt, k_opts);
  };
  lua_pushcfunction(L, cf_body);
  lua_pushstring(L, "four"_hs);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "invalid option"));

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

  void* ud = lua_newuserdatauv(L, 8, 0);
  luaL_setmetatable(L, "TPA.Widget"_hs);
  CHECK(luaL_testudata(L, -1, "TPA.Widget"_hs) == ud);
  CHECK(luaL_testudata(L, -1, "TPA.Other"_hs) == nullptr);
  CHECK(luaL_checkudata(L, -1, "TPA.Widget"_hs) == ud);
  lua_createtable(L, 0, 0);
  CHECK(luaL_testudata(L, -1, "TPA.Widget"_hs) == nullptr);
  lua_pop(L, 2);

  g_body = [](lua_State* Lb) -> int {
    luaL_checkudata(Lb, 1, "TPA.Missing"_hs);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_newuserdatauv(L, 4, 0);
  CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
  CHECK(top_msg_contains(L, "TPA.Missing expected"));

  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 2);
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

  lua_pushinteger(L, 12);
  CHECK(luaL_tolstring(L, -1) == "12");
  lua_pop(L, 2);
  lua_pushboolean(L, 1);
  CHECK(luaL_tolstring(L, -1) == "true");
  lua_pop(L, 2);
  lua_pushnil(L);
  CHECK(luaL_tolstring(L, -1) == "nil");
  lua_pop(L, 2);
  CHECK(luaL_tolstring(L, obj) == "custom!");
  lua_pop(L, 1);
  lua_createtable(L, 0, 0);
  CHECK(luaL_tolstring(L, -1).substr(0, 7) == "table: ");
  lua_pop(L, 2);
  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 1);
  lua_pushstring(L, "Widget"_hs);
  lua_setfield(L, -2, "__name"_hs);
  lua_setmetatable(L, -2);
  CHECK(luaL_tolstring(L, -1).substr(0, 8) == "Widget: ");
  lua_pop(L, 3);

  lua_close(L);
}

static void test_auxlib_load()
{
  lua_State* L = new_state();

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

  CHECK(luaL_loadbufferx(L, "return 1", "=lb"_hs, "t"_hs) ==
        LUA_OK);
  lua_pop(L, 1);
  DumpBuf d;
  CHECK(luaL_loadstring(L, "return 99"_hs) == LUA_OK);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(d.bytes.size() > 0);
  CHECK(luaL_loadbufferx(L, d.bytes, "=bin"_hs, "b"_hs) ==
        LUA_OK);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d.bytes, "=bin"_hs, "t"_hs) ==
        LUA_ERRSYNTAX);
  CHECK(top_msg_contains(L, "binary chunk"));
  CHECK(luaL_loadbuffer(L, "return 4", "=lb2"_hs) == LUA_OK);
  lua_pop(L, 1);

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

  CHECK(luaL_loadfile(L, "/tmp/tpa_no_such_file.lua"_hs) ==
        LUA_ERRFILE);
  CHECK(top_msg_contains(L, "cannot open"));

  lua_close(L);
}

static void test_auxlib_refs_buffer()
{
  lua_State* L = new_state();

  lua_pushstring(L, "payload"_hs);
  int r1 = luaL_ref(L, LUA_REGISTRYINDEX);
  CHECK(r1 != LUA_REFNIL && r1 != LUA_NOREF);
  CHECK(lua_rawgeti(L, LUA_REGISTRYINDEX, r1) == LUA_TSTRING);
  {
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "payload");
  }
  lua_pop(L, 1);
  lua_pushnil(L);
  CHECK(luaL_ref(L, LUA_REGISTRYINDEX) == LUA_REFNIL);
  luaL_unref(L, LUA_REGISTRYINDEX, r1);
  lua_pushinteger(L, 5);
  int r2 = luaL_ref(L, LUA_REGISTRYINDEX);
  CHECK(r2 == r1);
  luaL_unref(L, LUA_REGISTRYINDEX, r2);
  luaL_unref(L, LUA_REGISTRYINDEX, LUA_NOREF);
  luaL_unref(L, LUA_REGISTRYINDEX, LUA_REFNIL);

  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    luaL_addchar(&B, 'a');
    luaL_addstring(&B, "bc"_hs);
    luaL_addlstring(&B, "wxyz", 1, 2);
    CHECK(luaL_bufflen(&B) == 5u);
    lua_pushinteger(L, 99);
    luaL_addvalue(&B);
    size_t off = luaL_prepbuffsize(&B, 3);
    std::span<char> area = luaL_buffarea(&B);
    std::memcpy(area.data() + off, "!!!", 3);
    luaL_addsize(&B, 3);
    luaL_buffsub(&B, 1);
    luaL_pushresult(&B);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "abcxy99!!");
    lua_pop(L, 1);
  }

  {
    luaL_Buffer B;
    size_t off = luaL_buffinitsize(L, &B, 4);
    std::memcpy(luaL_buffarea(&B).data() + off, "ABCD", 4);
    luaL_pushresultsize(&B, 4);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "ABCD");
    lua_pop(L, 1);
  }

  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    (void)luaL_prepbuffer(&B);
    luaL_pushresult(&B);
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8().empty());
    lua_pop(L, 1);
  }

  {
    HostString g = luaL_gsub(L, "hello world"_hs, "o"_hs,
                             "0"_hs);
    CHECK(g.to_utf8() == "hell0 w0rld");
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "hell0 w0rld");
    lua_pop(L, 1);
  }

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

  CHECK(LUA_ERRFILE == LUA_ERRERR + 1);
  CHECK(LUA_NOREF == -2 && LUA_REFNIL == -1);
  CHECK(LUA_GNAME == "_G");
  CHECK(LUA_LOADED_TABLE == "_LOADED");
  CHECK(LUA_PRELOAD_TABLE == "_PRELOAD");
  CHECK(LUA_FILEHANDLE == "FILE*");

  luaL_pushfail(L);
  CHECK(lua_type(L, -1) == LUA_TNIL);
  lua_pop(L, 1);

  lua_pushinteger(L, 1);
  CHECK(luaL_typename(L, -1).to_utf8() == "number");
  CHECK(luaL_typename(L, 10).to_utf8() == "no value");
  lua_pop(L, 1);

  lua_createtable(L, 3, 0);
  for (int i = 1; i <= 3; i++)
  {
    lua_pushinteger(L, i * 10);
    lua_rawseti(L, -2, i);
  }
  CHECK(luaL_len(L, -1) == 3);
  lua_pop(L, 1);

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

  CHECK(luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs) == 0);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs) == 1);
  CHECK(lua_rawequal(L, -1, -2) == 1);
  lua_pop(L, 2);

  lua_createtable(L, 0, 1);
  lua_pushinteger(L, 123);
  {
    const luaL_Reg regs[] = {
        {"getup", cf_getup},
        {"holder", nullptr},
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

  {
    lua_State* L2 = luaL_newstate();
    CHECK(L2 != nullptr);
    luaL_requiref(L2, "string"_hs, luaopen_string, 1);
    CHECK(lua_type(L2, -1) == LUA_TTABLE);
    CHECK(lua_getglobal(L2, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -2) == 1);
    lua_pop(L2, 1);
    CHECK(luaL_getsubtable(L2, LUA_REGISTRYINDEX, "_LOADED"_hs) == 1);
    CHECK(lua_getfield(L2, -1, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -3) == 1);
    lua_pop(L2, 3);
    luaL_requiref(L2, "mathmod"_hs, luaopen_math, 0);
    CHECK(lua_type(L2, -1) == LUA_TTABLE);
    CHECK(lua_getglobal(L2, "mathmod"_hs) == LUA_TNIL);
    lua_pop(L2, 2);
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

static void test_dump_limits()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  CHECK(do_string(L,
      "local x = 77\n"
      "local function a() x = x + 1; return x end\n"
      "local function b() return x end\n"
      "return a, b") == LUA_OK);
  int bidx = lua_gettop(L);
  int aidx = bidx - 1;
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));

  DumpBuf d;
  lua_pushvalue(L, aidx);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d.bytes, "=re"_hs, "b"_hs) ==
        LUA_OK);
  int lidx = lua_gettop(L);

  {
    std::optional<HostString> uname = lua_getupvalue(L, lidx, 1);
    CHECK(uname.has_value());
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    lua_pushglobaltable(L);
    CHECK(lua_rawequal(L, -1, -2) == 1);
    lua_pop(L, 2);
  }
  CHECK(lua_upvalueid(L, lidx, 1) != lua_upvalueid(L, aidx, 1));
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));
  lua_pop(L, 1);

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
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    lua_pop(L, 1);
    std::optional<HostString> u2 = lua_getupvalue(L, lidx, 2);
    CHECK(u2.has_value());
    CHECK(lua_type(L, -1) == LUA_TNIL);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

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

static void test_cfunction_identity()
{
  lua_State* L = new_state();

  lua_pushcfunction(L, cf_add);
  lua_pushcfunction(L, cf_add);
  const void* p0 = lua_topointer(L, -1);
  CHECK(p0 != nullptr);
  CHECK(lua_topointer(L, -1) == lua_topointer(L, -2));
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_tocfunction(L, -2) == cf_add);
  lua_pop(L, 2);

  lua_pushcclosure(L, cf_add, 0);
  CHECK(lua_iscfunction(L, -1) == 1);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  CHECK(!lua_getupvalue(L, -1, 1).has_value());

  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1);
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1);
  CHECK(lua_iscfunction(L, -1) == 1 && lua_iscfunction(L, -2) == 1);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_tocfunction(L, -1) == lua_tocfunction(L, -2));
  CHECK(lua_topointer(L, -1) != lua_topointer(L, -2));
  CHECK(lua_topointer(L, -1) != p0);
  CHECK(lua_topointer(L, -2) != p0);
  {
    std::optional<HostString> un = lua_getupvalue(L, -1, 1);
    CHECK(un.has_value());
    if (un.has_value())
      CHECK(un->to_utf8().empty());
    lua_pop(L, 1);
    CHECK(!lua_getupvalue(L, -1, 2).has_value());
  }
  lua_pop(L, 3);

  lua_pushcfunction(L, cf_add);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, cf_add);
  lua_pushinteger(L, 1234);
  lua_settable(L, -3);
  int tidx = lua_gettop(L);

  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, cf_add);
  lua_setfield(L, -2, "__index"_hs);
  lua_setmetatable(L, -2);
  int oidx = lua_gettop(L);

  CHECK(load_string(L, "local f = ... \nreturn function() return f end") ==
        LUA_OK);
  lua_pushcfunction(L, cf_add);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  int cidx = lua_gettop(L);

  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(lua_rawgeti(L, LUA_REGISTRYINDEX, ref) == LUA_TFUNCTION);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);

  lua_pushcfunction(L, cf_add);
  CHECK(lua_gettable(L, tidx) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 1234);
  lua_pop(L, 1);

  CHECK(lua_getmetatable(L, oidx) == 1);
  CHECK(lua_getfield(L, -1, "__index"_hs) == LUA_TFUNCTION);
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  lua_pop(L, 2);

  {
    std::optional<HostString> un = lua_getupvalue(L, cidx, 1);
    CHECK(un.has_value());
    CHECK(lua_tocfunction(L, -1) == cf_add);
    CHECK(lua_topointer(L, -1) == p0);
    lua_pop(L, 1);
  }

  lua_pop(L, 3);
  lua_close(L);

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
    CHECK(lua_topointer(La, -1) == lua_topointer(Lb, -1));
    const void* pb = lua_topointer(Lb, -1);
    lua_pop(La, 1);
    lua_close(La);
    CHECK(lua_tocfunction(Lb, -1) == cf_add);
    CHECK(lua_topointer(Lb, -1) == pb);
    lua_pushinteger(Lb, 41);
    lua_pushinteger(Lb, 1);
    lua_pushcfunction(Lb, cf_add);
    lua_rotate(Lb, -3, 1);
    CHECK(lua_pcall(Lb, 2, 1, 0) == LUA_OK);
    lua_pop(Lb, 2);
    lua_close(Lb);
  }
}

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
  return 1;
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
  CHECK(g_klog.empty());

  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_OK);
  CHECK(rr.nres == 1);
  CHECK(g_klog == "kY(333,1);kP(222,1);kA(111,1);");
  CHECK(lua_tointeger(co, -1) == 100);
  lua_pop(L, 1); /* thread */
  lua_close(L);
}

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

  lua_sethook(L, hook_A, LUA_MASKCALL | LUA_MASKLINE, 0);
  lua_State* co = lua_newthread(L);
  CHECK(lua_gethook(co) == hook_A);
  CHECK(lua_gethookmask(co) == (LUA_MASKCALL | LUA_MASKLINE));

  lua_sethook(co, hook_B, LUA_MASKLINE, 0);
  CHECK(lua_gethook(L) == hook_A);
  CHECK(lua_gethook(co) == hook_B);
  CHECK(lua_gethookmask(L) == (LUA_MASKCALL | LUA_MASKLINE));
  CHECK(lua_gethookmask(co) == LUA_MASKLINE);

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
  luaL_requiref(co, "coroutine"_hs, luaopen_coroutine, 1);
  lua_pop(co, 1);
  lua_ResumeResult rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_YIELD);
  CHECK(g_hookB_hits > 0);
  CHECK(g_hookB_thread == co);
  CHECK(g_hookA_hits == a_after_main);

  int b_at_yield = g_hookB_hits;
  lua_pop(co, rr.nres);
  rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_OK);
  CHECK(g_hookB_hits > b_at_yield);
  CHECK(g_hookA_hits == a_after_main);

  lua_sethook(co, nullptr, 0, 0);
  CHECK(lua_gethook(co) == nullptr);
  CHECK(lua_gethook(L) == hook_A);
  lua_sethook(L, nullptr, 0, 0);

  lua_close(L);
}

static void test_io_handles()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  CHECK(do_string(L, "return io.output() == io.output(), "
                     "io.input() == io.input(), "
                     "io.stdout == io.output(), io.stdin == io.input()") ==
        LUA_OK);
  CHECK(lua_toboolean(L, -4) == 1);
  CHECK(lua_toboolean(L, -3) == 1);
  CHECK(lua_toboolean(L, -2) == 1);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 4);

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
  CHECK(lua_toboolean(L, -3) == 1);
  {
    std::optional<HostString> what2 = lua_tostring(L, -2);
    CHECK(what2.has_value() && what2->to_utf8() == "exit");
  }
  CHECK(lua_tointeger(L, -1) == 0);
  lua_pop(L, 6);

  lua_close(L);
}

static bool g_opt_check_called = false;
static lua_Integer opt_check_probe(lua_State* L, int n)
{
  g_opt_check_called = true;
  return luaL_checkinteger(L, n);
}

static void test_auxlib_restored()
{
  lua_State* L = new_state();

  {
    static const luaL_Reg mod[] = {
        {"add", cf_add},
        {"probe", cf_raise},
    };
    luaL_newlib(L, mod);
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
      CHECK(lua_type(L, -1) == LUA_TFUNCTION);
      count++;
      lua_pop(L, 1);
    }
    CHECK(count == 2);
    CHECK(lua_getfield(L, -1, "add"_hs) == LUA_TFUNCTION);
    lua_pushinteger(L, 20);
    lua_pushinteger(L, 22);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 2);
  }

  g_body = [](lua_State* Lb) -> int {
    g_opt_check_called = false;
    lua_Integer v = luaL_opt(Lb, opt_check_probe, 1, 7);
    CHECK(v == 7);
    CHECK(!g_opt_check_called);
    v = luaL_opt(Lb, opt_check_probe, 4, 7);
    CHECK(v == 7);
    CHECK(!g_opt_check_called);
    v = luaL_opt(Lb, opt_check_probe, 3, 7);
    CHECK(v == 55);
    CHECK(g_opt_check_called);
    return 0;
  };
  lua_pushcfunction(L, cf_body);
  lua_pushnil(L);
  lua_pushnil(L);
  lua_pushinteger(L, 55);
  CHECK(lua_pcall(L, 3, 0, 0) == LUA_OK);

  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    luaL_BuffSpan s1 = luaL_prepbuffspan(&B, 3);
    std::memcpy(s1.area.data() + s1.off, "abc", 3);
    luaL_addsize(&B, 3);
    luaL_BuffSpan s2 = luaL_prepbuffspan(&B, 4096);
    CHECK(s2.off == 3u);
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

static void test_num2str()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

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

  CHECK(do_string(L, "return string.format('%q', '\\1x')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\1x\"");
  }
  lua_pop(L, 1);
  CHECK(do_string(L, "return string.format('%q', '\\1' .. '1')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\0011\"");
  }
  lua_pop(L, 1);
  CHECK(do_string(L, "return string.format('%q', '\\0' .. '7')") == LUA_OK);
  {
    std::optional<HostString> q = lua_tostring(L, -1);
    CHECK(q.has_value() && q->to_utf8() == "\"\\0007\"");
  }
  lua_pop(L, 1);
  CHECK(do_string(L,
      "local s = '\\1' .. '1' .. '\\0' .. 'x\\n\"\\\\tail\\31' .. '9'\n"
      "local chunk = 'return ' .. string.format('%q', s)\n"
      "local f = assert(load(chunk))\n"
      "return f() == s") == LUA_OK);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 1);

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
    CHECK(mn.has_value() && mn->to_utf8() == "0x8000000000000000");
  }
  lua_pop(L, 4);
  CHECK(do_string(L,
      "for _, n in ipairs({0, -12345, math.maxinteger, math.mininteger}) do\n"
      "  local f = assert(load('return ' .. string.format('%q', n)))\n"
      "  local r = f()\n"
      "  if r ~= n or math.type(r) ~= 'integer' then return false end\n"
      "end\n"
      "return true") == LUA_OK);
  CHECK(lua_toboolean(L, -1) == 1);
  lua_pop(L, 1);
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

struct TestEntry
{
  const char* name;
  void (*fn)();
};

static void test_string_coercion()
{
  lua_State* L = new_state();

  lua_pushinteger(L, 42);
  CHECK(lua_type(L, -1) == LUA_TNUMBER);
  CHECK(lua_tolstring(L, -1) == "42");
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  lua_pushnumber(L, 2.5);
  CHECK(lua_tolstring(L, -1) == "2.5");
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);

  lua_pushliteral(L, "already");
  CHECK(lua_tolstring(L, -1) == "already");
  lua_pop(L, 1);

  lua_pushboolean(L, 1);
  CHECK(!lua_tolstring(L, -1).has_value());
  CHECK(lua_type(L, -1) == LUA_TBOOLEAN);
  lua_pop(L, 1);

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
    CHECK(lua_tolstring(L, -1)->empty());
    lua_pop(L, 1);
  }

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static void test_funcaddress()
{
  lua_State* L = new_state();

  lua_pushcfunction(L, cf_add);
  HostString a1 = lua_funcaddress(L, -1);
  HostString a2 = lua_funcaddress(L, -1);
  CHECK(!a1.to_utf8().empty());
  CHECK(a1 == a2);

  lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
  HostString a3 = lua_funcaddress(L, -1);
  CHECK(!a3.to_utf8().empty());
  CHECK(!(a1 == a3));
  lua_pop(L, 2);

  lua_close(L);
}

static void test_arith_floats()
{
  lua_State* L = new_state();

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

  lua_pushnumber(L, 7.5);
  lua_pushnumber(L, 2.0);
  lua_arith(L, LUA_OPMOD);
  CHECK(lua_tonumber(L, -1) == 1.5);
  lua_pop(L, 1);

  lua_pushinteger(L, 3);
  lua_pushnumber(L, 0.5);
  lua_arith(L, LUA_OPADD);
  CHECK(lua_isinteger(L, -1) == 0);
  CHECK(lua_tonumber(L, -1) == 3.5);
  lua_pop(L, 1);

  lua_pushnumber(L, 2.5);
  lua_arith(L, LUA_OPUNM);
  CHECK(lua_tonumber(L, -1) == -2.5);
  lua_pop(L, 1);

  lua_pushnumber(L, 1.0);
  lua_pushnumber(L, 4.0);
  lua_arith(L, LUA_OPDIV);
  CHECK(lua_tonumber(L, -1) == 0.25);
  lua_pop(L, 1);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static void test_fixed_chunks()
{
  lua_State* L = new_state();
  DumpBuf d;

  CHECK(load_string(L, "local acc = 0\n"
                       "for i = 1, 5 do acc = acc + i end\n"
                       "return acc * 2") == LUA_OK);
  CHECK(lua_dump(L, dump_writer, &d, 0) == 0);
  lua_pop(L, 1);
  CHECK(!d.bytes.empty());

  StringReader r{std::string_view(d.bytes), false};
  CHECK(lua_load(L, whole_reader, &r, "=fixed"_hs, "B"_hs) == LUA_OK);
  CHECK(lua_type(L, -1) == LUA_TFUNCTION);

  lua_pushvalue(L, -1);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 30);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 30);
  lua_pop(L, 1);

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
    lua_close(L);
    L = nullptr;
  }
}

static void test_userdata_uservalues_gc()
{
  lua_State* L = new_state();

  void* p = lua_newuserdatauv(L, 64, 3);
  CHECK(p != nullptr);
  CHECK(lua_type(L, -1) == LUA_TUSERDATA);
  std::memset(p, 0xAB, 64);

  lua_createtable(L, 0, 1);
  lua_pushinteger(L, 7);
  lua_setfield(L, -2, "x"_hs);
  CHECK(lua_setiuservalue(L, -2, 1) != 0);
  lua_pushliteral(L, "uv-two");
  CHECK(lua_setiuservalue(L, -2, 2) != 0);
  lua_pushinteger(L, 12345);
  CHECK(lua_setiuservalue(L, -2, 3) != 0);

  lua_setfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(lua_getfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs) == LUA_TUSERDATA);
  CHECK(lua_touserdata(L, -1) == p);
  CHECK(static_cast<unsigned char*>(p)[0] == 0xAB);
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

  CHECK(lua_getiuservalue(L, -1, 4) == LUA_TNONE);
  lua_pop(L, 2);

  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "udgc_anchor"_hs);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static int cf_read_upval_type(lua_State* L)
{
  lua_pushinteger(L, lua_type(L, lua_upvalueindex(1)));
  return 1;
}

static int cf_counter(lua_State* L)
{
  lua_Integer n = lua_tointeger(L, lua_upvalueindex(1)) + 1;
  lua_pushinteger(L, n);
  lua_replace(L, lua_upvalueindex(1));
  lua_pushinteger(L, n);
  return 1;
}

static void test_c_upvalue_indices()
{
  lua_State* L = new_state();

  lua_pushcfunction(L, cf_read_upval_type);
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == LUA_TNONE);
  lua_pop(L, 1);

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

static void yielding_count_hook(lua_State* L, lua_Debug* ar)
{
  if (ar->event == LUA_HOOKCOUNT)
    lua_yield(L, 0);
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
    CHECK(rr.nres == 0);
    yields++;
    CHECK(yields < 10000);
    rr = lua_resume(co, L, 0);
  }
  CHECK(rr.status == LUA_OK);
  CHECK(yields > 0);
  CHECK(rr.nres == 1);
  CHECK(lua_tointeger(co, -1) == 20100);

  lua_pop(L, 1);
  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static void test_selected_libs()
{
  lua_State* L = new_state();

  luaL_openselectedlibs(L, LUA_GLIBK | LUA_LOADLIBK | LUA_MATHLIBK,
                        LUA_TABLIBK);

  CHECK(do_string(L, "return math.floor(3.7)") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 3);
  lua_pop(L, 1);

  CHECK(do_string(L, "return type(string), type(os), type(io)") == LUA_OK);
  CHECK(lua_tolstring(L, -2) == "nil");
  CHECK(lua_tolstring(L, -1) == "nil");
  lua_pop(L, 3);

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
