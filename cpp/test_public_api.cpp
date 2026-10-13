
#include <cstdio>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lobject.h"
#ifdef LUA_DEBUG
#include "ltests.h"
#endif

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

struct AllocStats : lua_Alloc
{
  unsigned long calls = 0;

  bool permit(size_t, size_t, int) override
  {
    calls++;
    return true;
  }
};

static AllocStats g_alloc_stats;

static lua_State* new_state()
{
  lua_State* L = lua_newstate(&g_alloc_stats, 0);
  CHECK(L != nullptr);
  return L;
}

struct StringReader : lua_Reader
{
  byte_view src;
  bool done;

  StringReader(byte_view s, bool d) : src(s), done(d) {}

  std::optional<byte_view> read(lua_State*) override
  {
    if (done)
      return std::nullopt;
    done = true;
    return src;
  }
};

struct ByteReader : lua_Reader
{
  byte_view src;
  size_t pos;

  ByteReader(byte_view s, size_t p) : src(s), pos(p) {}

  std::optional<byte_view> read(lua_State*) override
  {
    if (pos >= src.size())
      return std::nullopt;
    return src.substr(pos++, 1);
  }
};

static int load_string(lua_State* L, byte_view code)
{
  StringReader r{code, false};
  return lua_load(L, &r, "=tpa"_hs, std::nullopt);
}

static int do_string(lua_State* L, byte_view code)
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
    byte_view v = msg->to_utf8();
    s.append(v.data(), v.size());
  }
  lua_pushstring(L, HostString(byte_view(s.c_str())));
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
  lua_pushboolean(L, true);
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

struct DumpBuf : lua_Writer
{
  std::string bytes;
  int empty_calls = 0;
  bool empty_was_last = false;

  byte_view view() const
  {
    return byte_view(bytes.data(), bytes.size());
  }

  int write(lua_State*, std::optional<byte_view> b) override
  {
    if (!b.has_value() || b->empty())
    {
      empty_calls++;
      empty_was_last = true;
      return 0;
    }
    empty_was_last = false;
    bytes.append(b->data(), b->size());
    return 0;
  }
};

struct FailingWriter : lua_Writer
{
  int calls = 0;

  int write(lua_State*, std::optional<byte_view>) override
  {
    calls++;
    return 33;
  }
};

struct WarnLog : lua_WarnFunction
{
  std::string text;
  int calls = 0;
  int last_tocont = -1;

  void warn(lua_State*, const HostString& msg, int tocont) override
  {
    calls++;
    last_tocont = tocont;
    byte_view v = msg.to_utf8();
    text.append(v.data(), v.size());
  }
};

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
  CHECK(lua_isyieldable(L));
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
  CHECK(lua_isnil(L, -1));
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
    std::optional<HostString> sname = lua_setlocal(L, *ar1, 1);
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
  CHECK(LUA_SIGNATURE == byte_view("\x1bLua", 4));
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
  CHECK(lua_rawequal(L, -1, -2));
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
  CHECK(lua_isnil(L, 6));
  CHECK(lua_isnil(L, 7));
  lua_settop(L, -3);
  CHECK(lua_gettop(L) == 5);
  lua_pop(L, 2);
  CHECK(lua_gettop(L) == 3);
  lua_settop(L, 0);
  CHECK(lua_gettop(L) == 0);

  CHECK(lua_checkstack(L, 30));
  CHECK(lua_checkstack(L, 0));
  CHECK(!lua_checkstack(L, INT_MAX));

  lua_pushnil(L);
  CHECK(lua_isnone(L, 5));
  CHECK(lua_isnoneornil(L, 5));
  CHECK(lua_isnoneornil(L, 1));
  CHECK(!lua_isnone(L, 1));
  lua_pop(L, 1);

  lua_close(L);
}

static void test_access()
{
  lua_State* L = new_state();
  static int s_light;

  lua_pushnil(L);
  lua_pushboolean(L, true);
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

  CHECK(lua_isnil(L, 1));
  CHECK(lua_isboolean(L, 2));
  CHECK(lua_isnumber(L, 3));
  CHECK(lua_isnumber(L, 4));
  CHECK(lua_isnumber(L, 11));
  CHECK(!lua_isnumber(L, 5));
  CHECK(lua_isinteger(L, 3));
  CHECK(!lua_isinteger(L, 4));
  CHECK(!lua_isinteger(L, 11));
  CHECK(lua_isstring(L, 5));
  CHECK(lua_isstring(L, 3));
  CHECK(!lua_isstring(L, 6));
  CHECK(lua_istable(L, 6));
  CHECK(lua_isfunction(L, 7));
  CHECK(lua_iscfunction(L, 7));
  CHECK(lua_isuserdata(L, 8));
  CHECK(lua_isuserdata(L, 9));
  CHECK(lua_islightuserdata(L, 9));
  CHECK(!lua_islightuserdata(L, 8));
  CHECK(lua_isthread(L, 10));

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

  CHECK(!lua_toboolean(L, 1));
  CHECK(lua_toboolean(L, 2));
  CHECK(lua_toboolean(L, 3));
  lua_pushboolean(L, false);
  CHECK(!lua_toboolean(L, -1));
  lua_pushinteger(L, 0);
  CHECK(lua_toboolean(L, -1));
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
  CHECK(!lua_iscfunction(L, -1));
  CHECK(lua_tocfunction(L, -1) == nullptr); /* Lua function */
  lua_pop(L, 1);

  CHECK(lua_touserdata(L, 8) == ud);
  CHECK(lua_touserdata(L, 9) == &s_light);
  CHECK(lua_touserdata(L, 6) == nullptr);

  CHECK(lua_tothread(L, 10) == L1);
  CHECK(lua_tothread(L, 6) == nullptr);

  CHECK(lua_topointer(L, 6) != nullptr);
  CHECK(lua_topointer(L, 7) != nullptr);
  CHECK(lua_topointer(L, 7) == lua_topointer(L, 7));
  lua_pushcfunction(L, cf_raise);
  CHECK(lua_topointer(L, -1) != lua_topointer(L, 7));
  lua_pop(L, 1);
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
    CHECK(lua_isinteger(L, -1));
    CHECK(lua_tointeger(L, -1) == c.expected);
    lua_pop(L, 1);
  }

  lua_pushinteger(L, 7);
  lua_pushinteger(L, 2);
  lua_arith(L, LUA_OPDIV);
  CHECK(!lua_isinteger(L, -1));
  CHECK(lua_tonumber(L, -1) == 3.5);
  lua_pop(L, 1);

  lua_pushinteger(L, 2);
  lua_pushinteger(L, 10);
  lua_arith(L, LUA_OPPOW);
  CHECK(!lua_isinteger(L, -1));
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

  CHECK(lua_rawequal(L, 1, 3));
  CHECK(!lua_rawequal(L, 1, 2));
  CHECK(!lua_rawequal(L, 1, 4));
  CHECK(!lua_rawequal(L, 1, 15));
  CHECK(!lua_rawequal(L, 15, 16));

  CHECK(lua_compare(L, 1, 3, LUA_OPEQ));
  CHECK(!lua_compare(L, 1, 2, LUA_OPEQ));
  CHECK(lua_compare(L, 1, 2, LUA_OPLT));
  CHECK(!lua_compare(L, 2, 1, LUA_OPLT));
  CHECK(lua_compare(L, 1, 3, LUA_OPLE));
  CHECK(!lua_compare(L, 2, 1, LUA_OPLE));
  CHECK(!lua_compare(L, 1, 4, LUA_OPEQ));
  CHECK(!lua_compare(L, 1, 15, LUA_OPEQ));
  lua_settop(L, 0);

  lua_pushinteger(L, 1);
  lua_pushnumber(L, 1.5);
  CHECK(lua_compare(L, 1, 2, LUA_OPLT));
  lua_settop(L, 0);

  lua_pushliteral(L, "abc");
  lua_pushliteral(L, "abd");
  CHECK(lua_compare(L, 1, 2, LUA_OPLT));
  lua_settop(L, 0);

  push_table_with_mm(L, "__eq", cf_mm_true);
  push_table_with_mm(L, "__eq", cf_mm_true);
  CHECK(!lua_rawequal(L, 1, 2));
  CHECK(lua_compare(L, 1, 2, LUA_OPEQ));
  push_table_with_mm(L, "__lt", cf_mm_true);
  push_table_with_mm(L, "__lt", cf_mm_true);
  CHECK(lua_compare(L, 3, 4, LUA_OPLT));
  lua_settop(L, 0);

  lua_close(L);
}

static void test_push()
{
  lua_State* L = new_state();

  {
    byte_view src = "hello world";
    byte_view r = lua_pushlstring(L, src, 6, 5);
    CHECK(r == "world");
    CHECK(*lua_tolstring(L, -1) == "world");
    lua_pop(L, 1);

    byte_view withnul("a\0b", 3);
    r = lua_pushlstring(L, withnul, 0, 3);
    CHECK(r.size() == 3);
    CHECK(lua_rawlen(L, -1) == 3u);
    byte_view back = *lua_tolstring(L, -1);
    CHECK(back.size() == 3 && std::memcmp(back.data(), "a\0b", 3) == 0);
    lua_pop(L, 1);

    r = lua_pushlstring(L, byte_view(), 0, 0);
    CHECK(r.empty());
    CHECK(lua_rawlen(L, -1) == 0u);
    lua_pop(L, 1);
  }

  lua_pushstring(L, "abc"_hs);
  CHECK(lua_tolstring(L, -1) == "abc");
  lua_pushstring(L, std::nullopt);
  CHECK(lua_isnil(L, -1));
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
  CHECK(lua_iscfunction(L, -1));
  lua_call(L, 0, 1);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  lua_pushboolean(L, true);
  CHECK(lua_toboolean(L, -1));
  lua_pushboolean(L, false);
  CHECK(!lua_toboolean(L, -1));
  lua_pop(L, 2);

  CHECK(lua_pushthread(L));
  CHECK(lua_tothread(L, -1) == L);
  lua_State* L1 = lua_newthread(L);
  CHECK(!lua_pushthread(L1));
  CHECK(lua_tothread(L1, -1) == L1);
  lua_pop(L, 2);

  lua_pushglobaltable(L);
  CHECK(lua_istable(L, -1));
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
  CHECK(!lua_getmetatable(L, -1));
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_insert(L, -3);
  CHECK(lua_setmetatable(L, -2) == 1);
  CHECK(lua_getmetatable(L, -1));
  CHECK(lua_rawequal(L, -1, -3));
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
  CHECK(lua_setiuservalue(L, uidx, 1));
  lua_pushinteger(L, 22);
  CHECK(lua_setiuservalue(L, uidx, 2));
  CHECK(lua_getiuservalue(L, uidx, 1) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 21);
  lua_pop(L, 1);
  CHECK(lua_getiuservalue(L, uidx, 2) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 22);
  lua_pop(L, 1);
  CHECK(lua_getiuservalue(L, uidx, 3) == LUA_TNONE);
  CHECK(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_pushnil(L);
  CHECK(!lua_setiuservalue(L, uidx, 3));
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
  CHECK(lua_isnil(L, 4));
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
      CHECK(msg->to_utf8().find("tpa:2") != byte_view::npos);
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

static int cf_raise_table(lua_State* L)
{
  lua_newtable(L);
  lua_pushinteger(L, 7);
  lua_setfield(L, -2, "code");
  return lua_error(L);
}

static int cf_luaL_error_plain(lua_State* L)
{
  return luaL_error(L, "plain fail"_hs);
}

static int cf_deep_pcall(lua_State* L)
{
  lua_Integer n = lua_tointeger(L, 1);
  if (n <= 0)
  {
    lua_pushliteral(L, "bottom");
    return lua_error(L);
  }
  lua_pushcfunction(L, cf_deep_pcall);
  lua_pushinteger(L, n - 1);
  int st = lua_pcall(L, 1, 0, 0);
  CHECK(st == LUA_ERRRUN);
  return lua_error(L);
}

static void test_error_handling()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  lua_pushcfunction(L, cf_raise_table);
  int st = lua_pcall(L, 0, 0, 0);
  CHECK(st == LUA_ERRRUN);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "code") == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 2);

  lua_pushcfunction(L, cf_luaL_error_plain);
  st = lua_pcall(L, 0, 0, 0);
  CHECK(st == LUA_ERRRUN);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value());
    if (msg.has_value())
      CHECK(msg->to_utf8().find("plain fail") != byte_view::npos);
  }
  lua_pop(L, 1);

  int base = lua_gettop(L);
  for (int i = 0; i < 1000; i++)
  {
    lua_pushcfunction(L, cf_raise);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
    lua_pop(L, 1);
  }
  CHECK(lua_gettop(L) == base);
  lua_pushcfunction(L, cf_add);
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 3);
  CHECK(lua_pcall(L, 2, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 5);
  lua_pop(L, 1);

  lua_pushcfunction(L, cf_deep_pcall);
  lua_pushinteger(L, 40);
  st = lua_pcall(L, 1, 0, 0);
  CHECK(st == LUA_ERRRUN);
  {
    std::optional<HostString> msg = lua_tostring(L, -1);
    CHECK(msg.has_value() && *msg == "bottom");
  }
  lua_pop(L, 1);

  {
    lua_State* co = lua_newthread(L);
    CHECK(load_string(co,
                      "local x <close> = setmetatable({}, {__close=function() "
                      "_G.closed = (_G.closed or 0) + 1 end})\n"
                      "error('boom')\n") == LUA_OK);
    lua_ResumeResult rr = lua_resume(co, L, 0);
    CHECK(rr.status == LUA_ERRRUN);
    lua_getglobal(L, "closed");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
    int cst = lua_closethread(co, L);
    CHECK(cst == LUA_ERRRUN);
    lua_getglobal(L, "closed");
    CHECK(lua_tointeger(L, -1) == 1);
    lua_pop(L, 1);
    lua_pop(L, 1);
  }

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
    byte_view ro("return 6 * 7");
    ByteReader r{ro, 0};
    CHECK(lua_load(L, &r, "=bytes"_hs, std::nullopt) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
  }

  {
    CHECK(load_string(L, "return 10 + 11") == LUA_OK);
    DumpBuf d;
    int st = lua_dump(L, &d, false);
    CHECK(st == 0);
    CHECK(!d.bytes.empty());
    CHECK(d.empty_calls >= 1);
    CHECK(d.empty_was_last);
    CHECK(lua_type(L, -1) == LUA_TFUNCTION);
    lua_pop(L, 1);

    CHECK(d.bytes.size() >= 4 && d.view().substr(0, 4) == LUA_SIGNATURE);

    StringReader r{d.view(), false};
    CHECK(lua_load(L, &r, "=dumped"_hs, "b"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 21);
    lua_pop(L, 1);

    StringReader r2{d.view(), false};
    int lst = lua_load(L, &r2, "=dumped"_hs, "t"_hs);
    CHECK(lst != LUA_OK);
    lua_pop(L, 1);

    CHECK(load_string(L, "return 10 + 11") == LUA_OK);
    DumpBuf ds;
    CHECK(lua_dump(L, &ds, true) == 0);
    lua_pop(L, 1);
    CHECK(!ds.bytes.empty() && ds.bytes.size() <= d.bytes.size());
    StringReader r3{byte_view(ds.view()), false};
    CHECK(lua_load(L, &r3, std::nullopt, "b"_hs) == LUA_OK);
    lua_call(L, 0, 1);
    CHECK(lua_tointeger(L, -1) == 21);
    lua_pop(L, 1);
  }

  {
    CHECK(load_string(L, "return 1") == LUA_OK);
    FailingWriter fw;
    int st = lua_dump(L, &fw, false);
    CHECK(st == 33);
    CHECK(fw.calls == 1);
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
    CHECK(!lua_isyieldable(L));

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
        CHECK(msg->to_utf8().find("dead coroutine") != byte_view::npos);
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
        CHECK(msg->to_utf8().find("cofail") != byte_view::npos);
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
  lua_setwarnf(L, &w);

  lua_warning(L, "part1;", 1);
  CHECK(w.calls == 1);
  CHECK(w.last_tocont == 1);
  lua_warning(L, "part2", 0);
  CHECK(w.calls == 2);
  CHECK(w.last_tocont == 0);
  CHECK(w.text == "part1;part2");

  lua_setwarnf(L, nullptr);
  lua_warning(L, "ignored", 0);
  CHECK(w.calls == 2);

  lua_close(L);
}

/*
** Exercises every branch of checkcontrol (lauxlib.cpp) through the
** default warn functions installed by luaL_newstate. Warn output goes
** to stderr, so capture it around each emission.
*/
static int g_stderr_saved = -1;
static const char* const k_warncap_path = "tpa_warncap.txt";

static void stderr_capture_begin()
{
  fflush(stderr);
  g_stderr_saved = dup(2);
  int fd = open(k_warncap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0);
  dup2(fd, 2);
  close(fd);
}

static std::string stderr_capture_end()
{
  fflush(stderr);
  dup2(g_stderr_saved, 2);
  close(g_stderr_saved);
  g_stderr_saved = -1;
  std::string out;
  FILE* f = fopen(k_warncap_path, "rb");
  CHECK(f != nullptr);
  char buff[512];
  size_t n;
  while ((n = fread(buff, 1, sizeof(buff), f)) > 0)
    out.append(buff, n);
  fclose(f);
  remove(k_warncap_path);
  return out;
}

static std::string warn_and_capture(lua_State* L, const char* msg, int tocont)
{
  stderr_capture_begin();
  lua_warning(L, msg, tocont);
  return stderr_capture_end();
}

static void test_warn_control()
{
  lua_State* L = luaL_newstate(); /* installs warnfon by default */
  CHECK(L != nullptr);

  /* branch: m[0] != '@' — regular message, warnings on */
  CHECK(warn_and_capture(L, "plain", 0) == "Lua warning: plain\n");

  /* branch: m.empty() — not a control message */
  CHECK(warn_and_capture(L, "", 0) == "Lua warning: \n");

  /* branch: tocont — "@off" as first part of a continued message is
     content, not control */
  CHECK(warn_and_capture(L, "@off", 1) == "Lua warning: @off");
  CHECK(warn_and_capture(L, "!", 0) == "!\n");
  CHECK(warn_and_capture(L, "still-on", 0) == "Lua warning: still-on\n");

  /* branch: unknown control — consumed, state unchanged */
  CHECK(warn_and_capture(L, "@bogus", 0) == "");
  CHECK(warn_and_capture(L, "after-bogus", 0) == "Lua warning: after-bogus\n");

  /* branch: tail == "off" — turns warnings off */
  CHECK(warn_and_capture(L, "@off", 0) == "");
  CHECK(warn_and_capture(L, "hidden", 0) == "");

  /* while off: empty and unknown-control messages stay silent */
  CHECK(warn_and_capture(L, "", 0) == "");
  CHECK(warn_and_capture(L, "@bogus", 0) == "");

  /* branch: tail == "on" — reachable from warnfoff too */
  CHECK(warn_and_capture(L, "@on", 0) == "");
  CHECK(warn_and_capture(L, "visible", 0) == "Lua warning: visible\n");

  lua_close(L);
}

/*
** Reaches unmakemask (ldblib.cpp) through db_gethook: every mask bit
** individually, combined, count-only, and the external-hook path.
*/
static void dummy_hook(lua_State*, lua_Debug*) {}

/* luaL_debughook is the hook debug.sethook installs, on any thread */
static void test_debughook_identity()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  CHECK(luaL_debughook() != nullptr);
  CHECK(lua_gethook(L) == nullptr);
  CHECK(do_string(L, "debug.sethook(function() end, 'l')") == LUA_OK);
  CHECK(lua_gethook(L) == luaL_debughook());
  CHECK(do_string(L, "co = coroutine.create(function() end) debug.sethook(co, function() end, '', 5)") == LUA_OK);
  lua_getglobal(L, "co");
  CHECK(lua_gethook(lua_tothread(L, -1)) == luaL_debughook());
  lua_settop(L, 0);
  CHECK(do_string(L, "debug.sethook()") == LUA_OK);
  CHECK(lua_gethook(L) == nullptr);
  lua_sethook(L, dummy_hook, LUA_MASKLINE, 0);
  CHECK(lua_gethook(L) != luaL_debughook());
  CHECK(do_string(L, "return debug.gethook()") == LUA_OK);
  CHECK(lua_tolstring(L, 1) == "external hook");
  lua_sethook(L, nullptr, 0, 0);
  lua_close(L);
}

static void test_db_gethook_mask()
{
  lua_State* L = luaL_newstate();
  CHECK(L != nullptr);
  luaL_openlibs(L);

  /* no hook: gethook fails before reaching unmakemask */
  CHECK(do_string(L, "return debug.gethook()") == LUA_OK);
  CHECK(lua_isnil(L, -1));
  lua_settop(L, 0);

  /* each mask character round-trips: sethook 'c'/'r'/'l' */
  const char* masks[] = {"c", "r", "l", "crl"};
  for (const char* m : masks)
  {
    lua_pushstring(L, m);
    lua_setglobal(L, "m");
    CHECK(do_string(L, "debug.sethook(function() end, m, 0)\n"
                       "local f, mask, count = debug.gethook()\n"
                       "return type(f), mask, count") == LUA_OK);
    CHECK(lua_tolstring(L, -3) == "function");
    CHECK(*lua_tolstring(L, -2) == byte_view(m));
    CHECK(lua_tointeger(L, -1) == 0);
    lua_settop(L, 0);
  }

  /* count-only hook: mask has only LUA_MASKCOUNT, unmakemask emits "" */
  CHECK(do_string(L, "debug.sethook(function() end, '', 7)\n"
                     "local f, mask, count = debug.gethook()\n"
                     "return type(f), mask, count") == LUA_OK);
  CHECK(lua_tolstring(L, -3) == "function");
  CHECK(*lua_tolstring(L, -2) == "");
  CHECK(lua_tointeger(L, -1) == 7);
  lua_settop(L, 0);

  /* external C hook: db_gethook reports "external hook" and still
     converts the mask through unmakemask */
  lua_sethook(L, counting_hook, LUA_MASKCALL | LUA_MASKLINE, 0);
  CHECK(do_string(L, "return debug.gethook()") == LUA_OK);
  CHECK(lua_tolstring(L, -3) == "external hook");
  CHECK(*lua_tolstring(L, -2) == "cl");
  CHECK(lua_tointeger(L, -1) == 0);
  lua_settop(L, 0);

  /* mask with count: 'crl' plus count keeps count out of the string */
  CHECK(do_string(L, "debug.sethook(function() end, 'crl', 3)\n"
                     "local _, mask, count = debug.gethook()\n"
                     "return mask, count") == LUA_OK);
  CHECK(*lua_tolstring(L, -2) == "crl");
  CHECK(lua_tointeger(L, -1) == 3);
  lua_settop(L, 0);

  lua_close(L);
}

/*
** Covers db_debug (debug.debug REPL): executed line, load error,
** runtime error, 'cont' exit, and EOF exit. Feeds stdin from a file
** and captures the stderr prompt/error output.
*/
static int g_stdin_saved = -1;
static const char* const k_dbgin_path = "tpa_dbgin.txt";

static void stdin_feed_begin(const char* content)
{
  FILE* f = fopen(k_dbgin_path, "wb");
  CHECK(f != nullptr);
  fwrite(content, 1, strlen(content), f);
  fclose(f);
  g_stdin_saved = dup(0);
  int fd = open(k_dbgin_path, O_RDONLY);
  CHECK(fd >= 0);
  dup2(fd, 0);
  close(fd);
}

static void stdin_feed_end()
{
  dup2(g_stdin_saved, 0);
  close(g_stdin_saved);
  g_stdin_saved = -1;
  clearerr(stdin);
  remove(k_dbgin_path);
}

static void test_db_debug()
{
  lua_State* L = luaL_newstate();
  CHECK(L != nullptr);
  luaL_openlibs(L);

  /* one good line, one load error, one runtime error, then 'cont' */
  stdin_feed_begin("x = 42\n"
                   "this is not lua ((\n"
                   "error('boom')\n"
                   "cont\n");
  stderr_capture_begin();
  int st = do_string(L, "debug.debug(); return x");
  std::string err = stderr_capture_end();
  stdin_feed_end();
  CHECK(st == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 42); /* good line executed */
  lua_settop(L, 0);
  CHECK(err.find("lua_debug> ") != std::string::npos);
  CHECK(err.find("(debug command)") != std::string::npos); /* load error printed */
  CHECK(err.find("boom") != std::string::npos);            /* runtime error printed */

  /* EOF on stdin also exits the loop */
  stdin_feed_begin("y = 7\n");
  stderr_capture_begin();
  st = do_string(L, "debug.debug(); return y");
  std::string err2 = stderr_capture_end();
  stdin_feed_end();
  CHECK(st == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_settop(L, 0);
  CHECK(err2.find("lua_debug> ") != std::string::npos);
  CHECK(err2.find("boom") == std::string::npos); /* fresh session, no stale errors */

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

static int g_userobject_dtors = 0;
static int g_userobject_gc_value = 0;

struct CountedObject : lua_UserObject
{
  int value = 0;
  ~CountedObject() override { g_userobject_dtors++; }
};

static int cf_userobject_gc(lua_State* L)
{
  CountedObject* o = static_cast<CountedObject*>(lua_touserobject(L, 1));
  g_userobject_gc_value = (o != nullptr) ? o->value : -1;
  return 0;
}

static int gc_bytes(lua_State* L)
{
  return lua_gc(L, LUA_GCCOUNT, 0, 0, 0) * 1024 + lua_gc(L, LUA_GCCOUNTB, 0, 0, 0);
}

static void test_userobject()
{
  lua_State* L = new_state();
  g_userobject_dtors = 0;
  g_userobject_gc_value = 0;
  lua_gc(L, LUA_GCSTOP, 0, 0, 0);

  int before = gc_bytes(L);
  std::unique_ptr<CountedObject> o = std::make_unique<CountedObject>();
  CountedObject* raw = o.get();
  raw->value = 42;
  lua_newuserobject(L, std::move(o), 4096, 1);
  CHECK(gc_bytes(L) - before >= 4096);
  CHECK(lua_type(L, -1) == LUA_TUSERDATA);
  CHECK(lua_touserobject(L, -1) == raw);
  CHECK(lua_touserdata(L, -1) != nullptr);

  lua_pushinteger(L, 7);
  CHECK(lua_setiuservalue(L, -2, 1));
  CHECK(lua_getiuservalue(L, -1, 1) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  lua_pushinteger(L, 1);
  CHECK(lua_touserobject(L, -1) == nullptr);
  lua_pop(L, 1);
  lua_newuserdatauv(L, 8, 0);
  CHECK(lua_touserobject(L, -1) == nullptr);
  lua_pop(L, 1);

  CHECK(luaL_newmetatable(L, "test.userobject"_hs));
  lua_pushcfunction(L, cf_userobject_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  CHECK(luaL_testuserobject(L, -1, "test.userobject"_hs) == raw);
  CHECK(luaL_testuserobject(L, -1, "test.other"_hs) == nullptr);
  CHECK(luaL_checkuserobject(L, -1, "test.userobject"_hs) == raw);
  lua_pop(L, 1);

  lua_gc(L, LUA_GCRESTART, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(g_userobject_gc_value == 42);
  CHECK(g_userobject_dtors == 1);
  CHECK(gc_bytes(L) - before < 4096);

  lua_newuserobject(L, std::make_unique<CountedObject>(), 16, 0);
  lua_close(L);
  CHECK(g_userobject_dtors == 2);
}

static int g_zud_gc = 0;
static int cf_zud_gc(lua_State*)
{
  g_zud_gc++;
  return 0;
}

static void test_zero_size_userdata()
{
  lua_State* L = new_state();

  void* a = lua_newuserdatauv(L, 0, 0);
  int ai = lua_gettop(L);
  void* b = lua_newuserdatauv(L, 0, 0);
  int bi = lua_gettop(L);
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  CHECK(a != b);
  CHECK(lua_touserdata(L, ai) == a);
  CHECK(lua_touserdata(L, bi) == b);
  CHECK(lua_rawlen(L, ai) == 0);
  CHECK(lua_rawlen(L, bi) == 0);
  CHECK(lua_topointer(L, ai) != nullptr);
  CHECK(lua_topointer(L, ai) != lua_topointer(L, bi));
  CHECK(lua_rawequal(L, ai, ai));
  CHECK(!lua_rawequal(L, ai, bi));

  lua_newtable(L);
  int t = lua_gettop(L);
  lua_pushvalue(L, ai);
  lua_pushinteger(L, 111);
  lua_settable(L, t);
  lua_pushvalue(L, bi);
  lua_pushinteger(L, 222);
  lua_settable(L, t);
  lua_pushvalue(L, ai);
  CHECK(lua_gettable(L, t) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 111);
  lua_pop(L, 1);
  lua_pushvalue(L, bi);
  CHECK(lua_gettable(L, t) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 222);
  lua_pop(L, 1);

  lua_settop(L, 0);

  luaL_newmetatable(L, "ZUD"_hs);
  lua_pop(L, 1);
  void* c = lua_newuserdatauv(L, 0, 0);
  CHECK(c != nullptr);
  luaL_setmetatable(L, "ZUD"_hs);
  CHECK(luaL_testudata(L, -1, "ZUD"_hs) == c);
  CHECK(luaL_testudata(L, -1, "ZUD.other"_hs) == nullptr);
  lua_settop(L, 0);

  void* d = lua_newuserdatauv(L, 0, 2);
  int di = lua_gettop(L);
  CHECK(d != nullptr);
  CHECK(lua_rawlen(L, di) == 0);
  lua_newtable(L);
  lua_pushinteger(L, 7);
  lua_setfield(L, -2, "marker");
  CHECK(lua_setiuservalue(L, di, 1));
  lua_pushinteger(L, 99);
  CHECK(lua_setiuservalue(L, di, 2));
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(lua_getiuservalue(L, di, 1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "marker") == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 7);
  lua_pop(L, 2);
  CHECK(lua_getiuservalue(L, di, 2) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);
  lua_settop(L, 0);

  g_zud_gc = 0;
  lua_newuserdatauv(L, 0, 0);
  lua_newtable(L);
  lua_pushcfunction(L, cf_zud_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(g_zud_gc == 1);

  lua_close(L);
}

struct SizeDeny : lua_Alloc
{
  size_t deny_size;

  explicit SizeDeny(size_t n) : deny_size(n) {}

  bool permit(size_t, size_t nsize, int) override
  {
    if (nsize == 0)
      return true;
    if (deny_size != 0 && nsize == deny_size)
      return false;
    return true;
  }
};

/* tracks bytes in use and denies growth past 'cap' (frees always pass) */
struct CapDeny : lua_Alloc
{
  size_t used = 0;
  size_t cap = 0;
  bool permit(size_t osize, size_t nsize, int) override
  {
    if (nsize > osize && cap != 0 && used + (nsize - osize) > cap)
      return false;
    used = used + nsize - osize;
    return true;
  }
};

/*
** One gate for memory: string bytes are asked of the allocator like any
** other allocation, and given back to it when the string is collected.
*/
static void test_strings_through_allocator()
{
  CapDeny cd;
  lua_State* L = lua_newstate(&cd, 0);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  size_t before = cd.used;
  CHECK(luaL_dostring(L, "big = ('x'):rep(1000000)") == LUA_OK);
  CHECK(cd.used - before >= 1000000);
  CHECK(luaL_dostring(L, "big = nil") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(cd.used - before < 1000);
  /* a cap in the allocator stops string growth too */
  cd.cap = cd.used + 100000;
  CHECK(luaL_dostring(L, "big = ('x'):rep(1000000)") == LUA_ERRMEM);
  cd.cap = 0;
  lua_close(L);
  CHECK(cd.used == 0);
}

/*
** A long string made by '..' or loaded from a binary chunk is made in its
** final place (B23): under a cap it needs room for itself once, as in stock
** (before, a work buffer for its bytes was counted with it, so a result
** larger than about half the free room failed)
*/
static void test_long_strings_made_in_place()
{
  CapDeny cd;
  lua_State* L = lua_newstate(&cd, 0);
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "a = ('a'):rep(2 << 20) "
                         "chunk = string.dump(load('return ' .. string.format('%q', ('b'):rep(2 << 20)), '=c'))") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  cd.cap = cd.used + (7 << 20); /* room for 'a .. a .. a' once */
  CHECK(luaL_dostring(L, "return #(a .. a .. a)") == LUA_OK && lua_tointeger(L, -1) == (6 << 20));
  lua_settop(L, 0);
  cd.cap = 0;
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  cd.cap = cd.used + (3 << 20); /* room for the loaded string once */
  CHECK(luaL_dostring(L, "return #load(chunk, 'chunk', 'b')()") == LUA_OK && lua_tointeger(L, -1) == (2 << 20));
  lua_settop(L, 0);
  cd.cap = 0;
  lua_close(L);
  CHECK(cd.used == 0);
}

/* denies the first request for at least 'size' bytes with tag 'tag', once */
struct DenyOnce : lua_Alloc
{
  size_t size = 0;
  int tag = 0;
  int denied = 0;
  bool permit(size_t osize, size_t nsize, int t) override
  {
    if (denied == 0 && size != 0 && t == tag && nsize > osize && nsize - osize >= size)
    {
      denied++;
      return false;
    }
    return true;
  }
};

/*
** When the bytes of a new string are refused once, the emergency collection
** that follows must not collect the string being created.
*/
static void test_string_emergency_collection()
{
  DenyOnce d;
  lua_State* L = lua_newstate(&d, 0);
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "a = ('y'):rep(61728) garbage = {} "
                         "for i = 1, 1000 do garbage[i] = {i} end garbage = nil") == LUA_OK);
  d.size = 123457;   /* a .. a .. 'y': the string with its bytes */
  d.tag = LUA_TSTRING;
  int st = luaL_dostring(L, "s = a .. a .. 'y' t = {} for i = 1, 100 do t[i] = ('z'):rep(i) .. i end "
                            "assert(#s == 123457 and s:sub(-1) == 'y')");
  if (st != LUA_OK)
    std::printf("  %s\n", std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()).c_str());
  CHECK(st == LUA_OK);
  CHECK(d.denied == 1);
  CHECK(luaL_dostring(L, "collectgarbage() assert(#s == 123457 and s:byte(1) == 121)") == LUA_OK);
  lua_close(L);
}

/*
** A long concatenation whose result cannot be created leaves nothing
** counted: after repeated failures a collection brings the count back to
** where it was, and the state closes cleanly (closing checks that
** everything was released).
*/
static void test_oom_long_concat()
{
  CapDeny cd;
  lua_State* L = lua_newstate(&cd, 0);
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "big = ('x'):rep(100000) parts = {}") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  int before = lua_gc(L, LUA_GCCOUNT, 0, 0, 0);
  /* no room for the result */
  cd.cap = cd.used + 50000;
  for (int i = 0; i < 20; i++)
  {
    CHECK(luaL_loadstring(L, "parts[#parts + 1] = big .. 'tail'") == LUA_OK);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRMEM);
    lua_settop(L, 0);
  }
  cd.cap = 0;
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  int after = lua_gc(L, LUA_GCCOUNT, 0, 0, 0);
  CHECK(after - before < 50);   /* in Kbytes; each leak would be ~100K */
  lua_close(L);
}

static int cf_make_arraytable(lua_State* L)
{
  lua_createtable(L, 300, 0);
  return 1;
}

static void test_oom_between_array_allocs()
{
  SizeDeny sd{0};
  lua_State* L = lua_newstate(&sd, 0);
  CHECK(L != nullptr);
  luaL_openselectedlibs(L, LUA_GLIBK, 0);

  lua_pushcfunction(L, cf_make_arraytable);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_rawlen(L, -1) == 0);
  lua_settop(L, 0);

  sd.deny_size = 300;
  lua_pushcfunction(L, cf_make_arraytable);
  int st = lua_pcall(L, 0, 1, 0);
  sd.deny_size = 0;
  CHECK(st == LUA_ERRMEM);
  lua_settop(L, 0);

  lua_newtable(L);
  lua_pushinteger(L, 42);
  lua_rawseti(L, -2, 1);
  CHECK(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 42);
  lua_pop(L, 2);

  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

  CHECK(do_string(L, "local t = {} for i = 1, 300 do t[i] = i end return #t") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 300);
  lua_settop(L, 0);

  lua_pushcfunction(L, cf_make_arraytable);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_settop(L, 0);

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
  while (lua_next(L, tidx))
  {
    cnt++;
    CHECK(lua_gettop(L) == tidx + 2);
    lua_pop(L, 1);
  }
  CHECK(cnt == 4);
  CHECK(lua_gettop(L) == tidx);
  lua_newtable(L);
  lua_pushnil(L);
  CHECK(!lua_next(L, -2));
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
    CHECK(lua_isinteger(L, -1));
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    r = lua_stringtonumber(L, "3.5");
    CHECK(r == 4u);
    CHECK(!lua_isinteger(L, -1));
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
    lua_Alloc* a = lua_getallocf(L);
    CHECK(a == &g_alloc_stats);
    unsigned long before = g_alloc_stats.calls;
    lua_pushlstring(L, "force an allocation with a long enough string", 0, 45);
    lua_pop(L, 1);
    CHECK(g_alloc_stats.calls > before);

    AllocStats other;
    lua_setallocf(L, &other);
    lua_Alloc* a2 = lua_getallocf(L);
    CHECK(a2 == &other);
    lua_setallocf(L, &g_alloc_stats);
  }

  {
    void* saved = lua_getextra(L);
    int marker = 0;
    lua_setextra(L, &marker);
    CHECK(lua_getextra(L) == &marker);
    lua_State* L1 = lua_newthread(L);
    CHECK(lua_getextra(L1) == &marker);
    lua_setextra(L1, nullptr);
    CHECK(lua_getextra(L) == &marker);
    lua_pop(L, 1);
    lua_setextra(L, saved);
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
  CHECK(lua_istable(L, -1));
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
    CHECK(lua_istable(L, -1));
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

static bool top_msg_contains(lua_State* L, byte_view what)
{
  std::optional<HostString> m = lua_tostring(L, -1);
  bool ok = m.has_value() && m->to_utf8().find(what) != byte_view::npos;
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
    g_panic_msg = std::string(m->to_utf8().data());
  throw PanicJump{};
}

struct NullAlloc : lua_Alloc
{
  bool permit(size_t, size_t, int) override
  {
    return false;
  }
};

struct DenyAlloc : lua_Alloc
{
  bool deny = false;

  bool permit(size_t osize, size_t nsize, int) override
  {
    if (nsize == 0)
      return true;
    if (deny && nsize > osize)
      return false;
    return true;
  }
};

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
    NullAlloc na;
    lua_State* L = lua_newstate(&na, 0);
    CHECK(L == nullptr);
  }

  {
    DenyAlloc da;
    lua_State* L = lua_newstate(&da, 0);
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
    lua_pushboolean(L, true);
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
    CHECK(!lua_toboolean(L, 15));
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

  CHECK(luaL_newmetatable(L, "TPA.Widget"_hs));
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(lua_getfield(L, -1, "__name"_hs) == LUA_TSTRING);
  {
    std::optional<HostString> n = lua_tostring(L, -1);
    CHECK(n.has_value() && n->to_utf8() == "TPA.Widget");
  }
  lua_pop(L, 1);
  CHECK(!luaL_newmetatable(L, "TPA.Widget"_hs));
  CHECK(luaL_getmetatable(L, "TPA.Widget"_hs) == LUA_TTABLE);
  CHECK(lua_rawequal(L, -1, -2));
  CHECK(lua_rawequal(L, -1, -3));
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

  CHECK(luaL_callmeta(L, obj, "__tostring"_hs));
  {
    std::optional<HostString> s = lua_tostring(L, -1);
    CHECK(s.has_value() && s->to_utf8() == "custom!");
  }
  lua_pop(L, 1);
  CHECK(!luaL_callmeta(L, obj, "__nope"_hs));
  CHECK(lua_gettop(L) == obj);

  lua_pushinteger(L, 12);
  CHECK(luaL_tolstring(L, -1) == "12");
  lua_pop(L, 2);
  lua_pushboolean(L, true);
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
  CHECK(lua_dump(L, &d, false) == 0);
  lua_pop(L, 1);
  CHECK(d.bytes.size() > 0);
  CHECK(luaL_loadbufferx(L, d.view(), "=bin"_hs, "b"_hs) ==
        LUA_OK);
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 99);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d.view(), "=bin"_hs, "t"_hs) ==
        LUA_ERRSYNTAX);
  CHECK(top_msg_contains(L, "binary chunk"));
  CHECK(luaL_loadbuffer(L, "return 4", "=lb2"_hs) == LUA_OK);
  lua_pop(L, 1);

  const char* path = "tpa_luaL_chunk.lua";
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
    byte_span area = luaL_buffaddr(&B);
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
    std::memcpy(luaL_buffaddr(&B).data() + off, "ABCD", 4);
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
      CHECK(t->to_utf8().find("MSG") != byte_view::npos);
      CHECK(t->to_utf8().find("stack traceback") != byte_view::npos);
    }
  }
  lua_pop(L, 1);

  CHECK(!luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs));
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  CHECK(luaL_getsubtable(L, LUA_REGISTRYINDEX, "TPA.SUB"_hs));
  CHECK(lua_rawequal(L, -1, -2));
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
  CHECK(!lua_toboolean(L, -1));
  lua_pop(L, 2);

  CHECK(luaL_fileresult(L, true, std::nullopt) == 1);
  CHECK(lua_toboolean(L, -1));
  lua_pop(L, 1);
  errno = ENOENT;
  CHECK(luaL_fileresult(L, false, "nofile.txt"_hs) == 3);
  CHECK(lua_type(L, -3) == LUA_TNIL); /* fail */
  {
    std::optional<HostString> m = lua_tostring(L, -2);
    CHECK(m.has_value() &&
          m->to_utf8().find("nofile.txt") != byte_view::npos);
  }
  CHECK(lua_tointeger(L, -1) == ENOENT);
  lua_pop(L, 3);

  errno = 0;
  CHECK(luaL_execresult(L, 0) == 3);
  CHECK(lua_toboolean(L, -3));
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
    luaL_requiref(L2, "string"_hs, luaopen_string, true);
    CHECK(lua_type(L2, -1) == LUA_TTABLE);
    CHECK(lua_getglobal(L2, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -2));
    lua_pop(L2, 1);
    CHECK(luaL_getsubtable(L2, LUA_REGISTRYINDEX, "_LOADED"_hs));
    CHECK(lua_getfield(L2, -1, "string"_hs) == LUA_TTABLE);
    CHECK(lua_rawequal(L2, -1, -3));
    lua_pop(L2, 3);
    luaL_requiref(L2, "mathmod"_hs, luaopen_math, false);
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
  CHECK(lua_dump(L, &d, false) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d.view(), "=re"_hs, "b"_hs) == LUA_OK);
  int lidx = lua_gettop(L);

  {
    std::optional<HostString> uname = lua_getupvalue(L, lidx, 1);
    CHECK(uname.has_value());
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    lua_pushglobaltable(L);
    CHECK(lua_rawequal(L, -1, -2));
    lua_pop(L, 2);
  }
  CHECK(lua_upvalueid(L, lidx, 1) != lua_upvalueid(L, aidx, 1));
  CHECK(lua_upvalueid(L, aidx, 1) == lua_upvalueid(L, bidx, 1));
  lua_pop(L, 1);

  CHECK(do_string(L,
      "local u1, u2 = 10, 20\n"
      "return function() return u1, u2 end") == LUA_OK);
  DumpBuf d2;
  CHECK(lua_dump(L, &d2, false) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d2.view(), "=re2"_hs, "b"_hs) == LUA_OK);
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
  CHECK(lua_dump(L, &d3, true) == 0);
  lua_pop(L, 1);
  CHECK(luaL_loadbufferx(L, d3.view(), "=s"_hs, "b"_hs) ==
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

[[maybe_unused]] static void test_cfunction_identity()
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
  CHECK(lua_iscfunction(L, -1));
  CHECK(lua_tocfunction(L, -1) == cf_add);
  CHECK(lua_topointer(L, -1) == p0);
  CHECK(!lua_getupvalue(L, -1, 1).has_value());

  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1);
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, cf_add, 1);
  CHECK(lua_iscfunction(L, -1) && lua_iscfunction(L, -2));
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

  CHECK(lua_getmetatable(L, oidx));
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
  luaL_requiref(co, "coroutine"_hs, luaopen_coroutine, true);
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
  CHECK(lua_toboolean(L, -4));
  CHECK(lua_toboolean(L, -3));
  CHECK(lua_toboolean(L, -2));
  CHECK(lua_toboolean(L, -1));
  lua_pop(L, 4);

  CHECK(do_string(L,
      "local f = assert(io.open('tpa_io_probe.txt', 'w'))\n"
      "local open_str = tostring(f)\n"
      "f:close()\n"
      "local closed_str = tostring(f)\n"
      "local ok, err = pcall(function() return f:write('x') end)\n"
      "return open_str, closed_str, ok, tostring(err)") == LUA_OK);
  {
    std::optional<HostString> os_ = lua_tostring(L, -4);
    CHECK(os_.has_value() && os_->to_utf8().substr(0, 6) == "file (");
    CHECK(os_.has_value() &&
          os_->to_utf8().find("closed") == byte_view::npos);
    std::optional<HostString> cs = lua_tostring(L, -3);
    CHECK(cs.has_value() && cs->to_utf8() == "file (closed)");
    CHECK(!lua_toboolean(L, -2));
    std::optional<HostString> err = lua_tostring(L, -1);
    CHECK(err.has_value() && err->to_utf8().find(
              "attempt to use a closed file") != byte_view::npos);
  }
  lua_pop(L, 4);
  std::remove("tpa_io_probe.txt");

  CHECK(do_string(L,
      "local ok, err = io.stdout:close()\n"
      "local usable = pcall(function() return io.stdout:write('') end)\n"
      "return ok, tostring(err), usable") == LUA_OK);
  CHECK(lua_type(L, -3) == LUA_TNIL);
  {
    std::optional<HostString> err = lua_tostring(L, -2);
    CHECK(err.has_value() && err->to_utf8().find(
              "cannot close standard file") != byte_view::npos);
  }
  CHECK(lua_toboolean(L, -1));
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
  CHECK(lua_toboolean(L, -3));
  {
    std::optional<HostString> what2 = lua_tostring(L, -2);
    CHECK(what2.has_value() && what2->to_utf8() == "exit");
  }
  CHECK(lua_tointeger(L, -1) == 0);
  lua_pop(L, 6);

  lua_close(L);
}

struct luaL_BuffSpan
{
  byte_span area;
  size_t off;
};

inline luaL_BuffSpan luaL_prepbuffspan(luaL_Buffer* B, size_t sz)
{
  size_t off = luaL_prepbuffsize(B, sz);
  return {luaL_buffaddr(B), off};
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
    luaL_newlibtable(L, mod);
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    CHECK(lua_rawlen(L, -1) == 0); /* pre-sized but empty */
    lua_pushnil(L);
    CHECK(!lua_next(L, -2));     /* no entries registered */
    lua_pop(L, 1);
    luaL_newlib(L, mod);
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2))
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
      byte_view v = r->to_utf8();
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
  CHECK(lua_toboolean(L, -2));
  if (!lua_toboolean(L, -2))
  {
    std::optional<HostString> why = lua_tostring(L, -1);
    if (why.has_value())
      std::printf("  %%a failure: %s\n", std::string(why->to_utf8().data()).c_str());
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
          one->to_utf8().find("p+0") != byte_view::npos);
    CHECK(half.has_value() &&
          half->to_utf8().find("p-1") != byte_view::npos);
    CHECK(big.has_value() &&
          big->to_utf8().find("p+100") != byte_view::npos);
    CHECK(tiny.has_value() &&
          tiny->to_utf8().find("p-100") != byte_view::npos);
    CHECK(upper.has_value() &&
          upper->to_utf8().find("0X") != byte_view::npos);
    CHECK(upper.has_value() &&
          upper->to_utf8().find("P-1") != byte_view::npos);
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
  CHECK(lua_toboolean(L, -1));
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
  CHECK(lua_toboolean(L, -1));
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

  lua_pushboolean(L, true);
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

static void test_arith_floats()
{
  lua_State* L = new_state();

  lua_pushnumber(L, 7.0);
  lua_pushnumber(L, 2.0);
  lua_arith(L, LUA_OPIDIV);
  CHECK(!lua_isinteger(L, -1));
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
  CHECK(!lua_isinteger(L, -1));
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
  CHECK(lua_dump(L, &d, false) == 0);
  lua_pop(L, 1);
  CHECK(!d.bytes.empty());

  StringReader r{d.view(), false};
  CHECK(lua_load(L, &r, "=fixed"_hs, "B"_hs) == LUA_OK);
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
    CHECK(lua_dump(L, &ds, true) == 0);
    lua_pop(L, 1);
    StringReader rs{ds.view(), false};
    CHECK(lua_load(L, &rs, std::nullopt, "B"_hs) == LUA_OK);
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
  CHECK(lua_setiuservalue(L, -2, 1));
  lua_pushliteral(L, "uv-two");
  CHECK(lua_setiuservalue(L, -2, 2));
  lua_pushinteger(L, 12345);
  CHECK(lua_setiuservalue(L, -2, 3));

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

constexpr int TPA_MAXSHORTLEN = 40;

static const void* push_and_ptr(lua_State* L, const std::string& s)
{
  lua_pushlstring(L, byte_view(s.data(), s.size()), 0, s.size());
  return lua_topointer(L, -1);
}

static void test_intern_identity()
{
  lua_State* L = new_state();
  luaL_openlibs(L);

  const void* p1 = push_and_ptr(L, "intern-me");
  const void* p2 = push_and_ptr(L, "intern-me");
  CHECK(p1 != nullptr && p1 == p2);
  lua_pop(L, 2);

  lua_pushliteral(L, "intern");
  lua_pushliteral(L, "-me");
  lua_concat(L, 2);
  CHECK(lua_topointer(L, -1) == p1);
  lua_pop(L, 1);

  CHECK(do_string(L, "return string.format('%s-%s', 'intern', 'me')") == LUA_OK);
  CHECK(lua_topointer(L, -1) == p1);
  lua_pop(L, 1);

  {
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    for (char c : std::string("intern-me"))
      luaL_addchar(&B, c);
    luaL_pushresult(&B);
    CHECK(lua_topointer(L, -1) == p1);
    lua_pop(L, 1);
  }

  CHECK(do_string(L, "return 'intern-me'") == LUA_OK);
  CHECK(lua_topointer(L, -1) == p1);
  lua_pop(L, 1);

  const void* pn1 = push_and_ptr(L, std::string("a\0b", 3));
  const void* pn2 = push_and_ptr(L, std::string("a\0b", 3));
  const void* pn3 = push_and_ptr(L, std::string("a\0c", 3));
  CHECK(pn1 == pn2);
  CHECK(pn1 != pn3);
  lua_pop(L, 3);

  std::string s40(40, 'x'), s41(41, 'x');
  const void* q1 = push_and_ptr(L, s40);
  const void* q2 = push_and_ptr(L, s40);
  CHECK(q1 == q2);
  const void* r1 = push_and_ptr(L, s41);
  const void* r2 = push_and_ptr(L, s41);
  CHECK(r1 != r2);
  CHECK(lua_rawequal(L, -1, -2));
  lua_pop(L, 4);

  lua_close(L);
}

static void test_intern_resize_stress()
{
  lua_State* L = new_state();

  constexpr int N = 20000;
  lua_createtable(L, 0, N);
  int tab = lua_gettop(L);

  char buf[32];
  for (int i = 0; i < N; i++)
  {
    int len = std::snprintf(buf, sizeof buf, "k%d", i);
    lua_pushlstring(L, byte_view(buf, ((size_t)(len))), 0, ((size_t)(len)));
    lua_rawseti(L, tab, i + 1);
  }

  for (int i = 0; i < N; i++)
  {
    int len = std::snprintf(buf, sizeof buf, "k%d", i);
    lua_pushlstring(L, byte_view(buf, ((size_t)(len))), 0, ((size_t)(len)));
    lua_rawgeti(L, tab, i + 1);
    CHECK(lua_topointer(L, -1) == lua_topointer(L, -2));
    CHECK(lua_rawequal(L, -1, -2));
    lua_pop(L, 2);
  }

  lua_pop(L, 1);
  lua_close(L);
}

static void test_intern_collisions()
{

  struct Pair { std::string a, b; };
  std::vector<Pair> pairs;
  {
    std::unordered_map<size_t, int> seen;
    char buf[32];
    for (int i = 0; pairs.size() < 4 && i < 2000000; i++)
    {
      int len = std::snprintf(buf, sizeof buf, "c%d", i);
      size_t h = InternalString::hashBytes(buf, ((size_t)(len)));
      auto it = seen.find(h);
      if (it == seen.end())
        seen.emplace(h, i);
      else
      {
        char buf2[32];
        int len2 = std::snprintf(buf2, sizeof buf2, "c%d", it->second);
        pairs.push_back({std::string(buf2, ((size_t)(len2))),
                         std::string(buf, ((size_t)(len)))});
      }
    }
  }
  CHECK(pairs.size() == 4);

  for (const Pair& pr : pairs)
  {
    CHECK(InternalString::hashBytes(pr.a.data(), pr.a.size()) ==
          InternalString::hashBytes(pr.b.data(), pr.b.size()));

    lua_State* L = new_state();

    const void* pa = push_and_ptr(L, pr.a);
    const void* pb = push_and_ptr(L, pr.b);
    CHECK(pa != pb);
    CHECK(!lua_rawequal(L, -1, -2));
    CHECK(push_and_ptr(L, pr.a) == pa);
    CHECK(push_and_ptr(L, pr.b) == pb);
    lua_pop(L, 4);

    lua_pushlstring(L, byte_view(pr.b.data(), pr.b.size()), 0, pr.b.size());
    lua_setglobal(L, "anchorB");
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

    CHECK(push_and_ptr(L, pr.b) == pb);
    const void* pa2 = push_and_ptr(L, pr.a);
    CHECK(pa2 != nullptr);
    CHECK(push_and_ptr(L, pr.a) == pa2);
    lua_pop(L, 3);

    lua_pushlstring(L, byte_view(pr.a.data(), pr.a.size()), 0, pr.a.size());
    lua_setglobal(L, "anchorA");
    lua_pushnil(L);
    lua_setglobal(L, "anchorB");
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);

    const void* pa3 = push_and_ptr(L, pr.a);
    CHECK(pa3 != nullptr);
    CHECK(push_and_ptr(L, pr.b) != nullptr);
    lua_pop(L, 2);

    lua_close(L);
  }
}

static void test_intern_gc_churn()
{
  lua_State* L = new_state();

  constexpr int WAVES = 30;
  constexpr int PER_WAVE = 400;
  char buf[48];

  lua_createtable(L, 0, 0);
  int keep = lua_gettop(L);

  for (int w = 0; w < WAVES; w++)
  {
    for (int i = 0; i < PER_WAVE; i++)
    {
      int len = std::snprintf(buf, sizeof buf, "w%d_%d", w, i);
      lua_pushlstring(L, byte_view(buf, ((size_t)(len))), 0, ((size_t)(len)));
      if (i % 2 == 0)
      {
        lua_pushboolean(L, true);
        lua_settable(L, keep);
      }
      else
        lua_pop(L, 1);
    }
    if (w % 3 == 0)
      lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
    else
      lua_gc(L, LUA_GCSTEP, 50, 0, 0);

    for (int i = 0; i < PER_WAVE; i += 2)
    {
      int len = std::snprintf(buf, sizeof buf, "w%d_%d", w, i);
      lua_pushlstring(L, byte_view(buf, ((size_t)(len))), 0, ((size_t)(len)));
      lua_gettable(L, keep);
      CHECK(lua_toboolean(L, -1));
      lua_pop(L, 1);
    }
  }

  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(push_and_ptr(L, "post-churn") != nullptr);
  lua_pop(L, 1);

  lua_close(L);
}

static void test_intern_long_keys()
{
  lua_State* L = new_state();

  lua_createtable(L, 0, 8);
  int tab = lua_gettop(L);

  for (int len : {TPA_MAXSHORTLEN, TPA_MAXSHORTLEN + 1, 100, 5000})
  {
    std::string s(((size_t)(len)), 'q');
    s.back() = ((char)('0' + (len % 10)));

    lua_pushlstring(L, byte_view(s.data(), s.size()), 0, s.size());
    lua_pushinteger(L, len);
    lua_settable(L, tab);

    lua_pushlstring(L, byte_view(s.data(), s.size()), 0, s.size());
    CHECK(lua_gettable(L, tab) == LUA_TNUMBER);
    CHECK(lua_tointeger(L, -1) == len);
    lua_pop(L, 1);
  }

  std::string nl(100, 'z');
  nl[50] = '\0';
  lua_pushlstring(L, byte_view(nl.data(), nl.size()), 0, nl.size());
  lua_pushliteral(L, "nul-key");
  lua_settable(L, tab);
  lua_pushlstring(L, byte_view(nl.data(), nl.size()), 0, nl.size());
  CHECK(lua_gettable(L, tab) == LUA_TSTRING);
  lua_pop(L, 1);

  std::string big(3000, 'B');
  lua_pushlstring(L, byte_view(big.data(), big.size()), 0, big.size());
  lua_pushinteger(L, 777);
  lua_settable(L, tab);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  lua_pushlstring(L, byte_view(big.data(), big.size()), 0, big.size());
  CHECK(lua_gettable(L, tab) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 777);
  lua_pop(L, 1);

  lua_pop(L, 1);
  lua_close(L);
}

/*
** If OOM strikes while populating a new metatable, the half-initialized
** table persists in the registry; an 'if (luaL_newmetatable(...))' guard
** then skips repopulation forever. Metamethod setup must be unconditional.
*/

struct OomAllocState : lua_Alloc
{
  int allocs_remaining; /* -1 = unlimited; 0 = next alloc fails */
  bool active;

  OomAllocState(int remaining, bool on) : allocs_remaining(remaining), active(on) {}

  bool permit(size_t, size_t nsize, int) override
  {
    if (nsize == 0)
      return true;
    if (active && allocs_remaining >= 0)
    {
      if (allocs_remaining == 0)
        return false; /* simulate OOM */
      allocs_remaining--;
    }
    return true;
  }
};

static int cf_setup_mt_always(lua_State* L)
{
  luaL_newmetatable(L, "OOM.Always"_hs);
  lua_pushcfunction(L, [](lua_State* Ls) -> int {
    (void)Ls;
    return 0;
  });
  lua_setfield(L, -2, "__gc"_hs);
  lua_pushcfunction(L, [](lua_State* Ls) -> int {
    (void)Ls;
    return 0;
  });
  lua_setfield(L, -2, "__close"_hs);
  lua_pushcfunction(L, [](lua_State* Ls) -> int {
    (void)Ls;
    return 0;
  });
  lua_setfield(L, -2, "__index"_hs);
  lua_pushcfunction(L, [](lua_State* Ls) -> int {
    (void)Ls;
    return 0;
  });
  lua_setfield(L, -2, "__newindex"_hs);
  lua_pushcfunction(L, [](lua_State* Ls) -> int {
    (void)Ls;
    return 0;
  });
  lua_setfield(L, -2, "__tostring"_hs);
  return 1;
}

static int cf_setup_mt_guarded(lua_State* L)
{
  if (luaL_newmetatable(L, "OOM.Guarded"_hs))
  {
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
      (void)Ls;
      return 0;
    });
    lua_setfield(L, -2, "__gc"_hs);
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
      (void)Ls;
      return 0;
    });
    lua_setfield(L, -2, "__close"_hs);
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
      (void)Ls;
      return 0;
    });
    lua_setfield(L, -2, "__index"_hs);
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
      (void)Ls;
      return 0;
    });
    lua_setfield(L, -2, "__newindex"_hs);
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
      (void)Ls;
      return 0;
    });
    lua_setfield(L, -2, "__tostring"_hs);
  }
  return 1;
}

static void test_newmetatable_oom()
{
  /* unconditional setup recovers from OOM */
  {
    OomAllocState oom_state{-1, false};
    lua_State* L = lua_newstate(&oom_state, 0);
    CHECK(L != nullptr);
    luaL_openlibs(L);

    bool had_oom = false;
    for (int budget = 0; budget < 200; budget++)
    {
      lua_pushcfunction(L, cf_setup_mt_always);

      oom_state.allocs_remaining = budget;
      oom_state.active = true;

      int status = lua_pcall(L, 0, 1, 0);

      oom_state.active = false;

      if (status == LUA_ERRMEM)
      {
        had_oom = true;
        lua_pop(L, 1); /* pop error */
      }
      else if (status == LUA_OK)
      {
        CHECK(lua_getfield(L, -1, "__gc"_hs) != LUA_TNIL);
        lua_pop(L, 1);
        CHECK(lua_getfield(L, -1, "__close"_hs) != LUA_TNIL);
        lua_pop(L, 1);
        CHECK(lua_getfield(L, -1, "__index"_hs) != LUA_TNIL);
        lua_pop(L, 1);
        CHECK(lua_getfield(L, -1, "__newindex"_hs) != LUA_TNIL);
        lua_pop(L, 1);
        CHECK(lua_getfield(L, -1, "__tostring"_hs) != LUA_TNIL);
        lua_pop(L, 1);
        lua_pop(L, 1); /* pop metatable */
        break;
      }
      else
      {
        lua_pop(L, 1);
      }
    }
    CHECK(had_oom);
    lua_close(L);
  }

  /* if-guarded setup loses metamethods after OOM */
  {
    OomAllocState oom_state{-1, false};
    lua_State* L = lua_newstate(&oom_state, 0);
    CHECK(L != nullptr);
    luaL_openlibs(L);

    bool bug_triggered = false;
    for (int budget = 0; budget < 200 && !bug_triggered; budget++)
    {
      /* Clear metatable from prior iteration */
      lua_pushnil(L);
      lua_setfield(L, LUA_REGISTRYINDEX, "OOM.Guarded"_hs);

      lua_pushcfunction(L, cf_setup_mt_guarded);

      oom_state.allocs_remaining = budget;
      oom_state.active = true;

      int status = lua_pcall(L, 0, 1, 0);

      oom_state.active = false;

      if (status == LUA_ERRMEM)
      {
        lua_pop(L, 1); /* pop error */

        /* Check if metatable got stuck in registry */
        int tt = lua_getfield(L, LUA_REGISTRYINDEX, "OOM.Guarded"_hs);
        if (tt == LUA_TTABLE)
        {
          lua_pop(L, 1);
          /* Retry with unlimited memory */
          oom_state.allocs_remaining = -1;
          lua_pushcfunction(L, cf_setup_mt_guarded);
          int st2 = lua_pcall(L, 0, 1, 0);
          CHECK(st2 == LUA_OK);
          int has_last = lua_getfield(L, -1, "__tostring"_hs);
          if (has_last == LUA_TNIL)
            bug_triggered = true;
          lua_pop(L, 2); /* pop field + metatable */
        }
        else
        {
          lua_pop(L, 1);
        }
      }
      else if (status == LUA_OK)
      {
        lua_pop(L, 1);
      }
      else
      {
        lua_pop(L, 1);
      }
    }
    CHECK(bug_triggered);
    lua_close(L);
  }
}

/* ===== CFUNC STRESS TESTS ===== */

#include <ctime>

/* Generate many distinct C functions via template */
template <int N>
static int cf_unique(lua_State* L)
{
  lua_pushinteger(L, N);
  return 1;
}

static int cf_noop(lua_State*) { return 0; }
static int cf_add1(lua_State* L) { lua_pushinteger(L, lua_tointeger(L, 1) + 1); return 1; }
static int cf_add2(lua_State* L) { lua_pushinteger(L, lua_tointeger(L, 2) + 2); return 1; }
static int cf_id(lua_State* L) { lua_pushvalue(L, 1); return 1; }

/*
** Group 1: Realistic cfunc-heavy Lua patterns.
** Exercises: pairs/ipairs iteration, pcall with C handlers, table.sort
** with C comparator, string.gmatch iterator, tostring metamethods.
** These all push cfuncs internally on every call/iteration.
*/
static void test_cfunc_stress_group1()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  clock_t start = clock();

  /* Heavy pairs usage — pairs() pushes 'next' cfunc each call */
  int status = luaL_dostring(L, R"(
    local t = {}
    for i = 1, 2000000 do
      for k, v in pairs(t) do end
    end
  )"_hs);
  CHECK(status == LUA_OK);

  /* Heavy ipairs usage — ipairs() pushes iterator cfunc each call */
  status = luaL_dostring(L, R"(
    local t = {}
    for i = 1, 2000000 do
      for _, v in ipairs(t) do end
    end
  )"_hs);
  CHECK(status == LUA_OK);

  /* pcall with C function — pushes error handler cfunc */
  status = luaL_dostring(L, R"(
    local function work() return 1 + 2 end
    for i = 1, 1000000 do
      pcall(work)
    end
  )"_hs);
  CHECK(status == LUA_OK);

  /* table.sort with default comparator (uses C lt internally) */
  status = luaL_dostring(L, R"(
    for i = 1, 50000 do
      local t = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0}
      table.sort(t)
    end
  )"_hs);
  CHECK(status == LUA_OK);

  /* string.gmatch — returns C iterator function */
  status = luaL_dostring(L, R"(
    local s = "the quick brown fox jumps over the lazy dog"
    for i = 1, 200000 do
      for w in s:gmatch("%a+") do end
    end
  )"_hs);
  CHECK(status == LUA_OK);

  /* tostring — calls C tostring metamethod on numbers */
  status = luaL_dostring(L, R"(
    for i = 1, 1000000 do
      tostring(i)
    end
  )"_hs);
  CHECK(status == LUA_OK);

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group1 (realistic cfunc patterns): %.0f ms\n", ms);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/*
** Group 2: Many cfuncs pushed and called in a tight loop.
** Uses pre-created static CFuncInfos (realistic: library functions reused).
*/
static CFuncInfo cfi_unique0(cf_unique<0>);
static CFuncInfo cfi_unique1(cf_unique<1>);
static CFuncInfo cfi_unique2(cf_unique<2>);
static CFuncInfo cfi_unique3(cf_unique<3>);
static CFuncInfo cfi_unique4(cf_unique<4>);
static CFuncInfo cfi_unique5(cf_unique<5>);
static CFuncInfo cfi_unique6(cf_unique<6>);
static CFuncInfo cfi_unique7(cf_unique<7>);

static CFuncInfo* g2_funcs[] = {
  &cfi_unique0, &cfi_unique1, &cfi_unique2, &cfi_unique3,
  &cfi_unique4, &cfi_unique5, &cfi_unique6, &cfi_unique7,
};

static void test_cfunc_stress_group2()
{
  lua_State* L = luaL_newstate();
  const int ITERS = 500000;
  constexpr int NFUNCS = sizeof(g2_funcs) / sizeof(g2_funcs[0]);

  clock_t start = clock();

  for (int i = 0; i < ITERS; i++)
  {
    lua_pushcfuncinfo(L, g2_funcs[i % NFUNCS]);
    lua_pushinteger(L, 42);
    lua_call(L, 1, 1);
    lua_pop(L, 1);
  }

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group2 (many cfuncs called): %.1f ms\n", ms);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/*
** Group 3: Deep call stacks mixing cfunc and Lua calls.
** Measures overhead on function call/return paths.
*/
static int cf_recurse(lua_State* L);
static CFuncInfo cfi_recurse(cf_recurse);

static int cf_recurse(lua_State* L)
{
  int depth = (int)lua_tointeger(L, 1);
  if (depth <= 0)
    return 0;
  lua_checkstack(L, 5);
  lua_pushcfuncinfo(L, &cfi_recurse);
  lua_pushinteger(L, depth - 1);
  lua_call(L, 1, 0);
  return 0;
}

static void test_cfunc_stress_group3()
{
  lua_State* L = luaL_newstate();
  const int ITERS = 200000;
  const int DEPTH = 10;

  clock_t start = clock();

  for (int i = 0; i < ITERS; i++)
  {
    lua_pushcfuncinfo(L, &cfi_recurse);
    lua_pushinteger(L, DEPTH);
    lua_call(L, 1, 0);
  }

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group3 (deep cfunc stacks): %.1f ms\n", ms);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/*
** Group 4: Large tables full of cfunc values, iterated and overwritten.
** Stresses table slot writes with cfunc values.
*/
static CFuncInfo cfi_noop(cf_noop);
static CFuncInfo cfi_add1(cf_add1);
static CFuncInfo cfi_add2(cf_add2);
static CFuncInfo cfi_id(cf_id);

static void test_cfunc_stress_group4()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  const int TABLE_SIZE = 1000;
  const int ITERS = 2000;

  static CFuncInfo* tab_cfis[] = {
    &cfi_noop, &cfi_add1, &cfi_add2, &cfi_id,
    &cfi_unique0, &cfi_unique1, &cfi_unique2, &cfi_unique3,
  };
  constexpr int NTAB = sizeof(tab_cfis) / sizeof(tab_cfis[0]);

  clock_t start = clock();

  lua_createtable(L, TABLE_SIZE, 0);
  int tidx = lua_gettop(L);

  for (int iter = 0; iter < ITERS; iter++)
  {
    /* Fill table with cfuncs */
    for (int i = 1; i <= TABLE_SIZE; i++)
    {
      lua_pushcfuncinfo(L, tab_cfis[i % NTAB]);
      lua_rawseti(L, tidx, i);
    }
    /* Overwrite with integers */
    for (int i = 1; i <= TABLE_SIZE; i++)
    {
      lua_pushinteger(L, i);
      lua_rawseti(L, tidx, i);
    }
  }

  lua_pop(L, 1);

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group4 (table cfunc churn): %.1f ms\n", ms);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/*
** Group 5: GC pressure with many live cfunc references across threads.
** Tests marking cost when many live slots hold cfuncs.
*/
static void test_cfunc_stress_group5()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  const int NTHREADS = 50;
  const int SLOTS_PER_THREAD = 200;
  const int GC_CYCLES = 100;

  static CFuncInfo* gc_cfis[] = {
    &cfi_noop, &cfi_add1, &cfi_add2, &cfi_id,
    &cfi_unique0, &cfi_unique1, &cfi_unique2, &cfi_unique3,
    &cfi_unique4, &cfi_unique5, &cfi_unique6, &cfi_unique7,
  };
  constexpr int NGF = sizeof(gc_cfis) / sizeof(gc_cfis[0]);

  clock_t start = clock();

  /* Create threads with cfuncs on their stacks */
  lua_createtable(L, NTHREADS, 0); /* anchor table */
  for (int t = 0; t < NTHREADS; t++)
  {
    lua_State* T = lua_newthread(L);
    lua_rawseti(L, -2, t + 1);
    lua_checkstack(T, SLOTS_PER_THREAD + 5);
    for (int s = 0; s < SLOTS_PER_THREAD; s++)
    {
      lua_pushcfuncinfo(T, gc_cfis[s % NGF]);
    }
  }

  /* Force many GC cycles while all those cfuncs are live */
  for (int i = 0; i < GC_CYCLES; i++)
  {
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  }

  lua_pop(L, 1); /* pop anchor table */

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group5 (GC with live cfuncs): %.1f ms\n", ms);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/*
** Group 6: Full Lua test suite runtime (tests/all.lua).
** Measures overall VM performance under realistic mixed workloads.
*/
static void test_cfunc_stress_group6()
{
  lua_State* L = luaL_newstate();
#ifdef LUA_DEBUG
  luai_openlibstest(L);
#else
  luaL_openlibs(L);
#endif

  /* Set up environment like the lua binary does */
  const char* lua = std::getenv("PUBLICAPI_LUA");
  std::string setup = std::string("_TMP='_1'; arg={[0]='") + (lua != nullptr ? lua : "../cpp/lua") + "'}";
  CHECK(luaL_dostring(L, HostString(setup.c_str())) == LUA_OK);

  clock_t start = clock();

  int status = luaL_dofile(L, "all.lua"_hs);
  if (status != LUA_OK)
  {
    auto msg = lua_tostring(L, -1);
    if (msg)
      std::printf("  group6 ERROR: %s\n", std::string(msg->to_utf8().data()).c_str());
    lua_pop(L, 1);
  }
  CHECK(status == LUA_OK);

  clock_t end = clock();
  double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
  std::printf("  group6 (full test suite): %.0f ms\n", ms);

  lua_close(L);
}

static void test_cfunc_stress()
{
  std::printf("cfunc stress tests:\n");
  test_cfunc_stress_group1();
  test_cfunc_stress_group2();
  test_cfunc_stress_group3();
  test_cfunc_stress_group4();
  test_cfunc_stress_group5();
  test_cfunc_stress_group6();
}


/*
** API calls with continuations ('lua_getik' and the others): a metamethod
** they run may yield; when the coroutine is resumed and the metamethod
** returns, the continuation finds the stack as the plain call leaves it.
*/

/* the continuation of every test function: returns what is on the stack,
   after a marker saying whether it ran (true) and its context */
static int apik_cont(lua_State* L, int status, lua_KContext ctx)
{
  CHECK(status == LUA_YIELD);
  int n = lua_gettop(L);
  lua_pushboolean(L, true);
  lua_pushinteger(L, static_cast<lua_Integer>(ctx));
  lua_rotate(L, 1, 2);
  return n + 2;
}

/* direct path: the same results, with the marker false */
static int apik_done(lua_State* L)
{
  int n = lua_gettop(L);
  lua_pushboolean(L, false);
  lua_pushinteger(L, 0);
  lua_rotate(L, 1, 2);
  return n + 2;
}

/* each test function returns its whole stack after the call, on both
   paths, so the two must agree */
static int cf_getik(lua_State* L) /* (t, n) -> t, t[n] */
{
  lua_Integer n = lua_tointeger(L, 2);
  lua_settop(L, 1);
  lua_getik(L, 1, n, 11, apik_cont);
  return apik_done(L);
}

static int cf_getfieldk(lua_State* L) /* (t) -> t, t.field */
{
  lua_settop(L, 1);
  lua_getfieldk(L, 1, "field", 12, apik_cont);
  return apik_done(L);
}

static int cf_gettablek(lua_State* L) /* (t, k) -> t, k, t[k] */
{
  lua_settop(L, 2);
  lua_pushvalue(L, 2);
  lua_gettablek(L, 1, 13, apik_cont);
  return apik_done(L);
}

static int cf_setik(lua_State* L) /* (t, n, v) -> t, n (t[n] = v) */
{
  lua_Integer n = lua_tointeger(L, 2);
  lua_settop(L, 3);
  lua_setik(L, 1, n, 14, apik_cont);
  return apik_done(L);
}

static int cf_setfieldk(lua_State* L) /* (t, v) -> t (t.field = v) */
{
  lua_settop(L, 2);
  lua_setfieldk(L, 1, "field", 15, apik_cont);
  return apik_done(L);
}

static int cf_settablek(lua_State* L) /* (t, k, v) -> t, k, v (t[k] = v) */
{
  lua_settop(L, 3);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_settablek(L, 1, 16, apik_cont);
  return apik_done(L);
}

static int cf_comparek(lua_State* L) /* (a, b) -> a, b, a < b */
{
  lua_settop(L, 2);
  lua_comparek(L, 1, 2, LUA_OPLT, 17, apik_cont);
  return apik_done(L);
}

static int cf_lenk(lua_State* L) /* (v) -> v, #v */
{
  lua_settop(L, 1);
  lua_lenk(L, 1, 18, apik_cont);
  return apik_done(L);
}

static int cf_arithk(lua_State* L) /* (a, b) -> a + b */
{
  lua_settop(L, 2);
  lua_arithk(L, LUA_OPADD, 19, apik_cont);
  return apik_done(L);
}

static int cf_concatk(lua_State* L) /* (...) -> concatenation of all */
{
  lua_concatk(L, lua_gettop(L), 20, apik_cont);
  return apik_done(L);
}

static void test_api_continuations()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  const struct { const char* name; lua_CFunction f; } fs[] = {
      {"getik", cf_getik}, {"getfieldk", cf_getfieldk}, {"gettablek", cf_gettablek},
      {"setik", cf_setik}, {"setfieldk", cf_setfieldk}, {"settablek", cf_settablek},
      {"comparek", cf_comparek}, {"lenk", cf_lenk}, {"arithk", cf_arithk},
      {"concatk", cf_concatk}};
  for (const auto& f : fs)
  {
    lua_pushcfunction(L, f.f);
    lua_setglobal(L, f.name);
  }
  int st = luaL_dostring(L, R"(
    -- run 'f' in a coroutine, resuming every yield; returns its results
    -- and the number of yields
    local function run(f, ...)
      local co = coroutine.create(f)
      local r = table.pack(coroutine.resume(co, ...))
      local yields = 0
      while coroutine.status(co) ~= "dead" do
        assert(r[1], r[2])
        assert(r[2] == "pause")
        yields = yields + 1
        r = table.pack(coroutine.resume(co))
      end
      assert(r[1], r[2])
      return yields, table.unpack(r, 2, r.n)
    end
    local function pause() coroutine.yield("pause") coroutine.yield("pause") end
    local mt = {
      __index = function(t, k) pause() return "got " .. tostring(k) end,
      __newindex = function(t, k, v) pause() rawset(t, k, v .. "!") end,
      __lt = function(a, b) pause() return a.v < b.v end,
      __len = function(t) pause() return 99 end,
      __add = function(a, b) pause() return a.v + b end,
      __concat = function(a, b)
        pause()
        return (type(a) == "table" and a.v or a) .. (type(b) == "table" and b.v or b)
      end,
    }
    local function obj(v) return setmetatable({v = v}, mt) end

    local function check(yields, ran, ctx, ...)
      return {yields = yields, ran = ran, ctx = ctx, n = select("#", ...), ...}
    end

    -- through a yielding metamethod: the continuation sees the plain result
    local o = obj(1)
    local r = check(run(getik, o, 5))
    assert(r.yields == 2 and r.ran and r.ctx == 11 and r.n == 2 and r[1] == o and r[2] == "got 5")
    r = check(run(getfieldk, o))
    assert(r.yields == 2 and r.ran and r.ctx == 12 and r.n == 2 and r[2] == "got field")
    r = check(run(gettablek, o, "key"))
    assert(r.yields == 2 and r.ran and r.ctx == 13 and r.n == 3 and r[2] == "key" and r[3] == "got key")
    local t = obj(1)
    r = check(run(setik, t, 3, "x"))
    assert(r.yields == 2 and r.ran and r.ctx == 14 and r.n == 2 and rawget(t, 3) == "x!")
    r = check(run(setfieldk, t, "y"))
    assert(r.yields == 2 and r.ran and r.ctx == 15 and r.n == 1 and rawget(t, "field") == "y!")
    r = check(run(settablek, t, "k", "z"))
    assert(r.yields == 2 and r.ran and r.ctx == 16 and r.n == 3 and r[3] == "z" and rawget(t, "k") == "z!")
    r = check(run(comparek, obj(1), obj(2)))
    assert(r.yields == 2 and r.ran and r.ctx == 17 and r.n == 3 and r[3] == true)
    r = check(run(comparek, obj(2), obj(1)))
    assert(r.yields == 2 and r.ran and r.n == 3 and r[3] == false)
    r = check(run(lenk, o))
    assert(r.yields == 2 and r.ran and r.ctx == 18 and r.n == 2 and r[2] == 99)
    r = check(run(arithk, obj(40), 2))
    assert(r.yields == 2 and r.ran and r.ctx == 19 and r.n == 1 and r[1] == 42)
    -- concatenation goes on after each yield: two metamethod calls, four yields
    r = check(run(concatk, "a", obj("b"), "c", obj("d")))
    assert(r.yields == 4 and r.ran and r.ctx == 20 and r.n == 1 and r[1] == "abcd", r[1])

    -- without metamethods: the same stacks, no continuation
    r = check(run(getik, {10, 20}, 2))
    assert(r.yields == 0 and not r.ran and r.n == 2 and r[2] == 20)
    r = check(run(getfieldk, {field = "f"}))
    assert(r.yields == 0 and not r.ran and r.n == 2 and r[2] == "f")
    r = check(run(setik, {}, 1, "v"))
    assert(r.yields == 0 and not r.ran and r.n == 2)
    r = check(run(comparek, 1, 2))
    assert(r.yields == 0 and not r.ran and r.n == 3 and r[3] == true)
    r = check(run(concatk, "x", 1, "y"))
    assert(r.yields == 0 and not r.ran and r.n == 1 and r[1] == "x1y")
    r = check(run(arithk, 40, 2))
    assert(r.yields == 0 and not r.ran and r.n == 1 and r[1] == 42)

    -- outside a coroutine a metamethod cannot yield: the plain call
    local ok, err = pcall(getik, obj(1), 1)
    assert(not ok and string.find(err, "outside a coroutine"))
    local ran, ctx, _, v = getik(setmetatable({}, {__index = function() return "plain" end}), 1)
    assert(ran == false and v == "plain")

    -- an error raised by the metamethod after a resume propagates
    local bad = setmetatable({}, {__index = function() coroutine.yield("pause") error("late", 0) end})
    local co = coroutine.create(getik)
    assert(coroutine.resume(co, bad, 1))
    local ok2, err2 = coroutine.resume(co)
    assert(not ok2 and err2 == "late")
    return "ok"
  )");
  if (st != LUA_OK)
    std::printf("      %s\n", std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()).c_str());
  CHECK(st == LUA_OK);
  lua_close(L);
}

/*
** {======================================================
** The rest of the freeze-ready C API: lua_settopk, lua_closeslotk,
** lua_getglobalk, lua_setglobalk, the luaL_*k helpers and lua_resumek.
** Each test function returns its whole stack after the call on both
** paths (see 'apik_cont'); the Lua driver runs it plainly, through
** metamethods that yield, and under a budget that freezes at every step.
** =======================================================
*/

static int cf_settopk(lua_State* L) /* (a, b) -> closes b, then a (a, b closable) */
{
  lua_settop(L, 2);
  lua_toclose(L, 1);
  lua_toclose(L, 2);
  lua_pushinteger(L, 7);
  lua_settopk(L, 0, 31, apik_cont);
  CHECK(lua_gettop(L) == 0);
  return apik_done(L);
}

static int cf_settopk_keep(lua_State* L) /* (a, b) -> a, 7: closes only b */
{
  lua_settop(L, 2);
  lua_toclose(L, 2);
  lua_pushinteger(L, 7);
  lua_settopk(L, 1, 41, apik_cont);
  return apik_done(L);
}

static int cf_closeslotk(lua_State* L) /* (a) -> nil (a closed) */
{
  lua_settop(L, 1);
  lua_toclose(L, 1);
  lua_closeslotk(L, 1, 32, apik_cont);
  return apik_done(L);
}

static int cf_getglobalk(lua_State* L) /* () -> zzg */
{
  lua_settop(L, 0);
  lua_getglobalk(L, "zzg", 33, apik_cont);
  return apik_done(L);
}

static int cf_setglobalk(lua_State* L) /* (v) -> (zzs = v) */
{
  lua_settop(L, 1);
  lua_setglobalk(L, "zzs", 34, apik_cont);
  return apik_done(L);
}

static int cf_callmetak(lua_State* L) /* (o) -> o, o:__foo() if any */
{
  lua_settop(L, 1);
  luaL_callmetak(L, 1, "__foo", 35, apik_cont);
  return apik_done(L);
}

static int tostring_cont(lua_State* L, int status, lua_KContext ctx)
{
  luaL_tolstringresult(L);
  return apik_cont(L, status, ctx);
}

static int cf_tolstringk(lua_State* L) /* (v) -> v, tostring(v) */
{
  lua_settop(L, 1);
  luaL_tolstringk(L, 1, 36, tostring_cont);
  return apik_done(L);
}

static int len_cont(lua_State* L, int status, lua_KContext ctx)
{
  lua_pushinteger(L, luaL_lenresult(L));
  return apik_cont(L, status, ctx);
}

static int cf_luallenk(lua_State* L) /* (v) -> v, #v */
{
  lua_settop(L, 1);
  lua_pushinteger(L, luaL_lenk(L, 1, 37, len_cont));
  return apik_done(L);
}

static int cf_getsubtablek(lua_State* L) /* (t) -> t, t.sub, found */
{
  lua_settop(L, 1);
  luaL_getsubtablek(L, 1, "sub", 38, apik_cont);
  return apik_done(L);
}

static int openmod_cont(lua_State*, int, lua_KContext)
{
  return 1;
}

static int openmod(lua_State* L) /* (name) -> openmod(name) */
{
  lua_getglobal(L, "openmod");
  lua_pushvalue(L, 1);
  lua_callk(L, 1, 1, 0, openmod_cont);
  return 1;
}

static int cf_requirefk(lua_State* L) /* (name) -> name, module */
{
  lua_settop(L, 1);
  luaL_requirefk(L, "modx", openmod, true, 39, apik_cont);
  return apik_done(L);
}

static int dostring_cont(lua_State* L, int status, lua_KContext ctx)
{ /* (a pcall that ended in an error gets that status) */
  CHECK(status != LUA_OK);
  lua_pushinteger(L, status == LUA_YIELD ? LUA_OK : status);
  int n = lua_gettop(L);
  lua_pushboolean(L, true);
  lua_pushinteger(L, static_cast<lua_Integer>(ctx));
  lua_rotate(L, 1, 2);
  return n + 2;
}

static int cf_dostringk(lua_State* L) /* (s) -> s, results..., status */
{
  lua_settop(L, 1);
  std::string s(lua_tolstring(L, 1)->data(), lua_tolstring(L, 1)->size());
  int st = luaL_dostringk(L, s.c_str(), 40, dostring_cont);
  lua_pushinteger(L, st);
  return apik_done(L);
}

static int cf_dofilek(lua_State* L) /* (path) -> path, results..., status */
{
  lua_settop(L, 1);
  std::string s(lua_tolstring(L, 1)->data(), lua_tolstring(L, 1)->size());
  int st = luaL_dofilek(L, s.c_str(), 42, dostring_cont);
  lua_pushinteger(L, st);
  return apik_done(L);
}

/* lua_resumek: (co) -> true, co's results (or false, message) */
static int resumek_end(lua_State* L, lua_State* co, lua_ResumeResult rr)
{
  if (rr.status == LUA_OK || rr.status == LUA_YIELD)
  {
    lua_xmove(co, L, rr.nres);
    lua_pushboolean(L, true);
    lua_insert(L, -(rr.nres + 1));
    return rr.nres + 1;
  }
  lua_xmove(co, L, 1);
  lua_pushboolean(L, false);
  lua_insert(L, -2);
  return 2;
}

static int resumek_cont(lua_State* L, int status, lua_KContext ctx)
{
  CHECK(ctx == 43);
  lua_State* co = lua_tothread(L, 1);
  lua_pushinteger(L, 1);
  lua_setglobal(L, "resumek_ran");
  return resumek_end(L, co, {status, lua_resumenres(co, status)});
}

/* a C function that marks its argument to be closed and returns: the
   '__close' runs as it returns (and can freeze there) */
static int cf_tbcret(lua_State* L)
{
  lua_settop(L, 1);
  lua_toclose(L, 1);
  lua_pushinteger(L, 5);
  return 1;
}

static int cf_resumek(lua_State* L)
{
  lua_State* co = lua_tothread(L, 1);
  lua_settop(L, 1);
  return resumek_end(L, co, lua_resumek(co, L, 0, 43, resumek_cont));
}

static void test_api_freezable()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  const struct { const char* name; lua_CFunction f; } fs[] = {
      {"settopk", cf_settopk}, {"settopk_keep", cf_settopk_keep}, {"closeslotk", cf_closeslotk},
      {"getglobalk", cf_getglobalk}, {"setglobalk", cf_setglobalk}, {"callmetak", cf_callmetak},
      {"tolstringk", cf_tolstringk}, {"luallenk", cf_luallenk}, {"getsubtablek", cf_getsubtablek},
      {"requirefk", cf_requirefk}, {"dostringk", cf_dostringk}, {"dofilek", cf_dofilek},
      {"resumek", cf_resumek}, {"tbcret", cf_tbcret}};
  for (const auto& f : fs)
  {
    lua_pushcfunction(L, f.f);
    lua_setglobal(L, f.name);
  }
  int st = luaL_dostring(L, R"LUA(
    -- run 'f' in a coroutine, resuming every yield (the metamethods yield)
    local function run(f, ...)
      local co = coroutine.create(f)
      local r = table.pack(coroutine.resume(co, ...))
      local yields = 0
      while coroutine.status(co) ~= "dead" do
        assert(r[1], r[2])
        assert(r[2] == "pause")
        yields = yields + 1
        r = table.pack(coroutine.resume(co))
      end
      assert(r[1], r[2])
      return yields, table.unpack(r, 2, r.n)
    end
    -- run 'f' in a coroutine with a budget of one step that freezes it
    -- each time, thawing it until it ends: the number of thaws
    local function frozen(f, ...)
      local co = coroutine.create(f)
      debug.setbudget(co, 1, function() debug.freeze() end)
      local r = table.pack(coroutine.resume(co, ...))
      local thaws = 0
      while debug.isfrozen(co) do
        thaws = thaws + 1
        r = table.pack(debug.thaw(co))
      end
      assert(r[1], r[2])
      return thaws, table.unpack(r, 2, r.n)
    end
    local function pause() coroutine.yield("pause") end
    local function noop() local x = 0 for i = 1, 3 do x = x + i end return x end

    -- the stack a test function returns, as {ran, ctx, ...}
    local function res(...) return {n = select("#", ...), ...} end
    local log
    local function closable(name, p)
      return setmetatable({}, {__close = function() p() log[#log + 1] = name end})
    end

    for _, mode in ipairs{"yield", "freeze"} do
      local p = mode == "yield" and pause or noop
      local function go(f, ...)
        if mode == "yield" then return run(f, ...) else return frozen(f, ...) end
      end
      -- (under the budget, a freeze can come anywhere: the plain stacks are
      -- compared, not the markers of whether 'k' ran)
      local function plain(t) return mode ~= "yield" or (t[1] == 0 and t[2] == false) end
      local function expectsusp(n) -- the continuation ran
        if mode == "yield" then return n == 2 or n == 1 or n == 4 else return n > 0 end
      end

      -- lua_settopk: '__close' of each variable, in reverse order
      log = {}
      local n, ran, ctx, a = go(settopk, closable("a", p), closable("b", p))
      assert(expectsusp(n) and ran == true and ctx == 31 and a == nil, mode .. " settopk " .. tostring(n) .. tostring(ran) .. tostring(ctx) .. tostring(a))
      assert(#log == 2 and log[1] == "b" and log[2] == "a")
      log = {}
      n, ran, ctx, a = go(settopk_keep, closable("a", p), closable("b", p))
      assert(expectsusp(n) and ran == true and ctx == 41, mode .. " keep " .. tostring(n) .. tostring(ran) .. tostring(ctx))
      assert(#log == 1 and log[1] == "b")
      assert(a ~= nil) -- the first value is still there

      -- lua_closeslotk
      log = {}
      n, ran, ctx, a = go(closeslotk, closable("s", p))
      assert(expectsusp(n) and ran == true and ctx == 32 and a == nil, mode .. " closeslot " .. tostring(n) .. tostring(ran) .. tostring(ctx) .. tostring(a))
      assert(#log == 1 and log[1] == "s")

      -- an error in '__close' propagates from either path
      local bad = setmetatable({}, {__close = function() p() error("close failed", 0) end})
      local co = coroutine.create(settopk)
      if mode == "yield" then
        assert(coroutine.resume(co, bad, closable("x", p)))
        local ok, err = coroutine.resume(co)
        while ok and coroutine.status(co) ~= "dead" do ok, err = coroutine.resume(co) end
        assert(not ok and err == "close failed")
      else
        debug.setbudget(co, 1, function() debug.freeze() end)
        local ok, err = coroutine.resume(co, bad, closable("x", p))
        while debug.isfrozen(co) do ok, err = debug.thaw(co) end
        assert(not ok and err == "close failed")
      end

      -- lua_getglobalk / lua_setglobalk through _G's metamethods
      setmetatable(_G, {
        __index = function(t, k) p() return "g:" .. k end,
        __newindex = function(t, k, v) p() rawset(t, k, tostring(v) .. "!") end})
      n, ran, ctx, a = go(getglobalk)
      assert(expectsusp(n) and ran == true and ctx == 33 and a == "g:zzg", a)
      n, ran, ctx = go(setglobalk, 5)
      assert(expectsusp(n) and ran == true and ctx == 34 and rawget(_G, "zzs") == "5!")
      setmetatable(_G, nil)
      _G.zzs = nil

      -- luaL_callmetak: the metamethod's result; none if no metamethod
      local o = setmetatable({}, {__foo = function(self) p() return "foo" end})
      local t = res(go(callmetak, o))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 35 and t[4] == o and t[5] == "foo" and t.n == 5)
      t = res(go(callmetak, {}))
      assert(plain(t) and t.n == 4)

      -- luaL_tolstringk, luaL_lenk
      o = setmetatable({}, {__tostring = function() p() return "str" end,
                            __len = function() p() return 99 end,
                            __name = "N"})
      t = res(go(tolstringk, o))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 36 and t[4] == o and t[5] == "str" and t.n == 5)
      t = res(go(tolstringk, 12))
      assert(plain(t) and t[5] == "12")
      t = res(go(luallenk, o))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 37 and t[5] == 99 and t.n == 5)
      t = res(go(luallenk, {1, 2, 3}))
      assert(plain(t) and t[5] == 3)
      -- a '__tostring' that does not return a string, a '__len' that is no integer
      local ok, err = pcall(go, tolstringk, setmetatable({}, {__tostring = function() p() return {} end}))
      assert(not ok and string.find(err, "'__tostring' must return a string"), err)
      ok, err = pcall(go, luallenk, setmetatable({}, {__len = function() p() return "x" end}))
      assert(not ok and string.find(err, "object length is not an integer"), err)

      -- luaL_getsubtablek: found, created, and through metamethods
      t = res(go(getsubtablek, {sub = {1}}))
      assert(plain(t) and t[5][1] == 1 and t[6] == true and t.n == 6)
      local store = {}
      local lazy = setmetatable({}, {
        __index = function(_, k) p() return rawget(store, k) end,
        __newindex = function(_, k, v) p() rawset(store, k, v) end})
      t = res(go(getsubtablek, lazy))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 38 and t[6] == false and t.n == 6)
      assert(store.sub == t[5])
      t = res(go(getsubtablek, lazy))
      assert(t[5] == store.sub and t[6] == true)

      -- luaL_requirefk: opens through a function that pauses, once
      local opened = 0
      function openmod(name) p() opened = opened + 1 return {name = name} end
      t = res(go(requirefk, "who"))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 39 and t[5].name == "modx" and t.n == 5, t[1])
      assert(package.loaded.modx == t[5] and modx == t[5] and opened == 1)
      t = res(go(requirefk, "again")) -- already loaded: openmod is not called
      assert(t[5] == package.loaded.modx and opened == 1)
      package.loaded.modx, modx, openmod = nil, nil, nil

      -- luaL_dostringk, luaL_dofilek: pcall with a continuation
      t = res(go(dostringk, "local p = ... return 1, 2"))
      assert(plain(t) and t[5] == 1 and t[6] == 2 and t[7] == 0 and t.n == 7)
      _G.pauser = p
      t = res(go(dostringk, "pauser() pauser() return 'x', 'y'"))
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 40 and t[5] == "x" and t[6] == "y" and t[7] == 0, t[1])
      t = res(go(dostringk, "pauser() error('boom', 0)"))
      assert(t[5] == "boom" and t[6] == 2 and t.n == 6)
      t = res(go(dostringk, "x = = 1"))
      assert(plain(t) and t[6] == 3, t[6]) -- a syntax error
      local path = os.tmpname()
      local f = assert(io.open(path, "w"))
      f:write("pauser() return 'file', 3")
      f:close()
      t = res(go(dofilek, path))
      os.remove(path)
      assert(expectsusp(t[1]) and t[2] == true and t[3] == 42 and t[5] == "file" and t[6] == 3 and t[7] == 0, t[1])
      pauser = nil
    end

    -- the helpers that complete at once when what they call does not suspend
    -- (no coroutine, no budget): the plain results
    do
      local plain = setmetatable({}, {__foo = function() return "foo" end})
      local t = res(callmetak(plain))
      assert(t.n == 4 and t[1] == false and t[2] == 0 and t[3] == plain and t[4] == "foo", "callmetak " .. t.n .. tostring(t[1]) .. tostring(t[2]) .. tostring(t[4]) .. tostring(t[5]))
      local path = os.tmpname()
      local f = assert(io.open(path, "w")) f:write("return 7, 8") f:close()
      t = res(dofilek(path))
      assert(t.n == 6 and t[4] == 7 and t[5] == 8 and t[6] == 0, "dofilek " .. t.n .. " " .. tostring(t[3]) .. " " .. tostring(t[4]) .. " " .. tostring(t[5]) .. " " .. tostring(t[6]))
      os.remove(path)
      t = res(dofilek(path)) -- no such file: the load fails, the status is in the stack
      assert(t[t.n] ~= 0 and type(t[t.n - 1]) == "string", tostring(t[t.n])) -- the message, then the status
      t = res(dostringk("return 1"))
      assert(t.n == 5 and t[4] == 1 and t[5] == 0, t.n)
    end

    -- in a thread that cannot yield (a comparator of sort): a freeze goes through
    -- the helper, which then finishes without yielding
    do
      local function nny(f)
        return function(...)
          local args = table.pack(...)
          local r
          table.sort({2, 1}, function() r = table.pack(f(table.unpack(args, 1, args.n))) return false end)
          return table.unpack(r, 1, r.n)
        end
      end
      local p = noop
      log = {}
      local function closable(name)
        return setmetatable({}, {__close = function() p() log[#log + 1] = name end})
      end
      local n, ran, ctx, a = frozen(nny(settopk), closable("a"), closable("b"))
      assert(n > 0 and ran == true and ctx == 31 and a == nil and #log == 2 and log[1] == "b" and log[2] == "a")
      log = {}
      n, ran, ctx, a = frozen(nny(closeslotk), closable("s"))
      assert(n > 0 and ran == true and ctx == 32 and a == nil and #log == 1)
      log = {}
      -- a C function that returns with a variable to close, closed as it returns
      n, ran, ctx = frozen(tbcret, closable("r"))
      assert(n > 0 and #log == 1 and log[1] == "r")
    end

    -- in the callback, canfreeze from another thread: no freeze from there
    do
      local seen
      local co = coroutine.create(function() for i = 1, 40 do end end)
      debug.setbudget(co, 5, function()
        local ok, why = coroutine.wrap(debug.canfreeze)()
        seen = {ok, why}
        debug.freeze()
      end)
      coroutine.resume(co)
      while debug.isfrozen(co) do debug.thaw(co) end
      assert(seen[1] == false and seen[2] == "no budget in effect", tostring(seen[2]))
    end

    -- lua_resumek: a freeze of the coroutine resumed passes through the
    -- C function and goes on in the continuation
    local inner = function(a) local x = a for i = 1, 5 do x = x + i end return "done", x end
    local co = coroutine.create(function()
      local c = coroutine.create(inner)
      return resumek(c)
    end)
    -- without a budget: the plain result
    local t = res(coroutine.resume(co))
    assert(t[1] == true and t[2] == false) -- resume needs the argument 'a'; the coroutine got none
    assert(type(t[3]) == "string")
    local c2 = coroutine.create(function() return resumek(coroutine.create(function() return "done", 15 end)) end)
    t = res(coroutine.resume(c2))
    assert(t[1] == true and t[2] == true and t[3] == "done" and t[4] == 15 and t.n == 4)
    -- under the budget: freezes inside the resumed coroutine, many times
    resumek_ran = nil
    local c3 = coroutine.create(function()
      return resumek(coroutine.create(function() local x = 0 for i = 1, 20 do x = x + i end return "done", x end))
    end)
    debug.setbudget(c3, 1, function() debug.freeze() end)
    local r = table.pack(coroutine.resume(c3))
    local thaws = 0
    while debug.isfrozen(c3) do thaws = thaws + 1 r = table.pack(debug.thaw(c3)) end
    assert(thaws > 20 and r[1] == true and r[2] == true and r[3] == "done" and r[4] == 210 and r.n == 4, thaws)
    assert(resumek_ran == 1)
    -- an error in the resumed coroutine
    local c4 = coroutine.create(function()
      return resumek(coroutine.create(function() local x = 0 for i = 1, 5 do x = x + i end error("inner", 0) end))
    end)
    debug.setbudget(c4, 1, function() debug.freeze() end)
    r = table.pack(coroutine.resume(c4))
    while debug.isfrozen(c4) do r = table.pack(debug.thaw(c4)) end
    assert(r[1] == true and r[2] == false and r[3] == "inner" and r.n == 3)
    -- a yield of the resumed coroutine comes back as the plain result
    local c5 = coroutine.create(function()
      return resumek(coroutine.create(function() coroutine.yield("y", 1) end))
    end)
    t = res(coroutine.resume(c5))
    assert(t[1] == true and t[2] == true and t[3] == "y" and t[4] == 1)
    return "ok"
  )LUA");
  if (st != LUA_OK)
    std::printf("      %s\n", std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()).c_str());
  CHECK(st == LUA_OK);
  lua_close(L);
}

/*
** {======================================================
** Budget and freezing
** =======================================================
*/

/* the budget callback of these tests: counts its calls, and freezes
   (if 'fz_target' is set) from call 'fz_at' on (each time, if 'fz_every') */
static int fz_calls = 0;
static int fz_freezes = 0;
static int fz_at = 0;
static bool fz_every = false;
static lua_State* fz_target = nullptr;
static lua_State* fz_user = nullptr; /* the 'user_co' of the last call */
static int fz_cb_can = -1;           /* what 'lua_nofreeze' said in the last call: could it */
static int fz_cb_why = -1;           /* and why not */

static void fz_budget(lua_State* user_co, lua_State* spender)
{
  fz_calls++;
  fz_user = user_co;
  fz_cb_why = lua_nofreeze(spender);
  fz_cb_can = fz_cb_why == 0;
  if (fz_target != nullptr && fz_calls >= fz_at && (fz_every || fz_calls == fz_at))
  {
    fz_freezes++;
    lua_freeze(spender); /* (never returns) */
  }
}

static void fz_reset(lua_State* L, lua_Integer n, lua_State* target, int at, bool every)
{
  fz_calls = fz_freezes = 0;
  fz_target = target;
  fz_at = at;
  fz_every = every;
  lua_setbudget(L, n, n > 0 ? fz_budget : nullptr);
}

static std::string fz_str(lua_State* L, int idx)
{
  auto s = lua_tolstring(L, idx);
  return s ? std::string(s->data(), s->size()) : std::string("(not a string)");
}

/* the number of VM instructions of global 'name' called with integer 'arg' */
static int fz_count = 0;
static void fz_counthook(lua_State*, lua_Debug*) { fz_count++; }

static int fz_instructions(lua_State* L, const char* name, lua_Integer arg)
{
  fz_count = 0;
  lua_sethook(L, fz_counthook, LUA_MASKCOUNT, 1);
  lua_getglobal(L, name);
  lua_pushinteger(L, arg);
  /* (the hook counts from the first instruction of the function called) */
  int st = lua_pcall(L, 1, 1, 0);
  lua_sethook(L, nullptr, 0, 0);
  CHECK(st == LUA_OK);
  lua_pop(L, 1);
  return fz_count;
}

/* a C function calling its first argument with a continuation */
static int fz_callk_cont(lua_State* L, int status, lua_KContext ctx)
{
  CHECK(status == LUA_YIELD && ctx == 77);
  lua_pushboolean(L, true); /* (thawed through the continuation) */
  return 2;
}

static int fz_callk(lua_State* L)
{
  lua_settop(L, 2);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_callk(L, 1, 1, 77, fz_callk_cont);
  lua_pushboolean(L, false);
  return 2;
}

/* a C function calling its first argument without a continuation */
static int fz_call(lua_State* L)
{
  lua_settop(L, 2);
  lua_call(L, 1, 1);
  return 1;
}

/* a C function resuming its first argument (a thread) without a continuation:
   its first yielded value */
static int fz_plainresume(lua_State* L)
{
  lua_State* co = lua_tothread(L, 1);
  lua_ResumeResult r = lua_resume(co, L, 0);
  CHECK(r.status == LUA_YIELD && r.nres == 1);
  lua_xmove(co, L, 1);
  return 1;
}

/* a C function calling its first argument protected, with a continuation */
static int fz_pcallk_cont(lua_State* L, int status, lua_KContext)
{
  lua_pushboolean(L, status == LUA_OK || status == LUA_YIELD);
  lua_insert(L, -2);
  lua_pushinteger(L, status);
  return 3; /* success, its result or error, and the status */
}

static int fz_pcallk(lua_State* L)
{
  lua_settop(L, 2);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  int st = lua_pcallk(L, 1, 1, 0, 0, fz_pcallk_cont);
  lua_pushboolean(L, st == LUA_OK);
  lua_insert(L, -2);
  lua_pushinteger(L, -1);
  return 3;
}

/* reads t[k] with a continuation: the value, and whether it went on in
   the continuation */
static int fz_getik_cont(lua_State* L, int status, lua_KContext ctx)
{
  CHECK(status == LUA_YIELD && ctx == 55);
  lua_pushboolean(L, true);
  return 2;
}

static int fz_getik(lua_State* L)
{
  lua_getik(L, 1, lua_tointeger(L, 2), 55, fz_getik_cont);
  lua_pushboolean(L, false);
  return 2;
}

/* a host protected call with a message handler, frozen at every budget
   point (in the handler too), ends as the unfrozen call does (the handler
   frozen at base level used to end in success) */
static int mh_calls = 0, mh_at = 0;
static void mh_budget(lua_State*, lua_State* spender)
{
  mh_calls++;
  if (mh_calls == mh_at)
    lua_freeze(spender);
}

/* the handler of a host protected call is not in effect inside a protected
   call of the script that froze before: an error that call catches is not
   the handler's (the handler used to be put back at a thaw) */
static void test_handler_in_effect_after_thaw()
{
  for (int at = 0; at <= 40; at++)
  {
    lua_State* L = new_state();
    luaL_openlibs(L);
    CHECK(luaL_loadstring(L, R"(
      return function(m) return 'H:' .. tostring(m) end,
             function()
               local s = 0
               for i = 1, 10 do s = s + i end
               local ok, e = pcall(error, 99)
               for i = 1, 10 do s = s + i end
               local ok2, e2 = pcall(function() local x = nil; return x.y end)
               return ok, e, ok2, (tostring(e2):gsub("^.-:%d+: ", "")), s
             end
    )") == LUA_OK);
    lua_call(L, 0, 2); /* the handler at 1, the function at 2 */
    mh_calls = 0;
    mh_at = at;
    lua_setbudget(L, 1, at > 0 ? mh_budget : nullptr);
    lua_pushvalue(L, 2);
    int st = lua_pcall(L, 0, 5, 1);
    while (st == LUA_OK && lua_isfrozen(L))
      st = lua_thaw(L, nullptr).status;
    lua_setbudget(L, 0, nullptr);
    CHECK(st == LUA_OK && lua_gettop(L) == 7);
    CHECK(!lua_toboolean(L, 3) && lua_tointeger(L, 4) == 99);
    CHECK(!lua_toboolean(L, 5) && fz_str(L, 6) == "attempt to index a nil value (local 'x')");
    CHECK(lua_tointeger(L, 7) == 110);
    lua_close(L);
  }
}

/* closing a state whose thread has a budget that freezes at every point:
   the '__gc' of kernel objects all run (a freeze used to stop them and
   abort the close) */
static int cl_calls = 0;
static int cl_gc(lua_State*)
{
  cl_calls++;
  return 0;
}
static int cl_budget_calls = 0;
static void cl_budget(lua_State*, lua_State* spender)
{
  cl_budget_calls++;
  lua_freeze(spender);
}

static void test_close_with_budget()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  lua_pushcfunction(L, cl_gc);
  lua_setglobal(L, "clgc");
  CHECK(luaL_dostring(L, R"(
    for i = 1, 5 do
      setmetatable({}, {__gc = function() local s = 0 for k = 1, 20 do s = s + k end clgc() end})
    end
  )") == LUA_OK);
  lua_setbudget(L, 1, cl_budget);
  lua_close(L);
  CHECK(cl_calls == 5);
  CHECK(cl_budget_calls == 0); /* (no budget point once it is closing) */
}

static void test_message_handler_freeze()
{
  const char* handlers[] = {
      "function(m) local s=0 for i=1,10 do s=s+i end return 'H:'..tostring(m)..':'..s end",
      "function(m) local s=0 for i=1,10 do s=s+i end error('H:'..tostring(m)) end"};
  const char* callees[] = {"5", "nil", "function() error('boom') end", "function() local x = nil; return x.y end"};
  for (int h = 0; h < 2; h++)
    for (int c = 0; c < 4; c++)
    {
      std::string expect;
      int expectst = 0;
      for (int at = 0; at <= 14; at++)
      {
        lua_State* L = new_state();
        luaL_openlibs(L);
        std::string code = std::string("return ") + handlers[h] + ", " + callees[c];
        CHECK(luaL_loadstring(L, code.c_str()) == LUA_OK);
        lua_call(L, 0, 2); /* handler at 1, callee at 2 */
        mh_calls = 0;
        mh_at = at;
        lua_setbudget(L, 1, at > 0 ? mh_budget : nullptr);
        int st = lua_pcall(L, 0, 1, 1);
        bool froze = lua_isfrozen(L);
        int guard = 0;
        while (lua_isfrozen(L) && guard++ < 100)
        {
          lua_ResumeResult r = lua_thaw(L, nullptr);
          st = r.status;
        }
        (void)froze;
        lua_setbudget(L, 0, nullptr);
        std::string got = fz_str(L, -1);
        if (at == 0)
        { /* the unfrozen run is the reference */
          expect = got;
          expectst = st;
          CHECK(st != LUA_OK);
        }
        else
        {
          CHECK(st == expectst);
          CHECK(got == expect);
        }
        lua_close(L);
      }
    }
}

/* '__gc' of user objects wait for their group, and a closing state does
   not call them; those of kernel objects are called, as in stock */
static int gc_user_calls = 0;
static int gc_kernel_calls = 0;

static int gc_user(lua_State*)
{
  gc_user_calls++;
  return 0;
}

static int gc_kernel(lua_State*)
{
  gc_kernel_calls++;
  return 0;
}

static void gc_nobudgetf(lua_State*, lua_State*) {}

static void test_group_gc()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  lua_pushcfunction(L, gc_user);
  lua_setglobal(L, "gcuser");
  lua_pushcfunction(L, gc_kernel);
  lua_setglobal(L, "gckernel");
  int st = luaL_dostring(L, R"(
    local function make(f) setmetatable({}, {__gc = f}) end
    function makeuser() make(gcuser) end
    makekernel = function() make(gckernel) end
    makekernel()
    function waiting() return coroutine.yield("made") end
  )");
  CHECK(st == LUA_OK);
  lua_State* co = lua_newthread(L);
  lua_setbudget(co, 1000, gc_nobudgetf);
  lua_getglobal(co, "makeuser");
  CHECK(lua_resume(co, L, 0).status == LUA_OK);
  CHECK(gc_user_calls == 0);
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(gc_kernel_calls == 1); /* the collector calls a kernel object's at once */
  CHECK(gc_user_calls == 0);   /* a user object's waits for its group */
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  CHECK(gc_user_calls == 0);
  /* and when the state closes with it waiting, it is not called */
  lua_getglobal(L, "makekernel");
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_OK);
  lua_close(L);
  CHECK(gc_user_calls == 0 && gc_kernel_calls == 2);
}

/*
** Refusals of a frozen thread (a host's resume or close) push their message
** onto it, as stock does; a host that leaves the messages there changes
** nothing of the frozen thread: its thaw drops them (B14)
*/
static int b14_calls = 0;
static void b14_budget(lua_State*, lua_State* spender)
{
  if (++b14_calls == 2)
    lua_freeze(spender);
}

static void test_frozen_refusals_keep_top()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "function work(a, b) local s = 0 for i = 1, 200 do s = s + i end return s + a, b end") ==
        LUA_OK);
  lua_State* co = lua_newthread(L);
  lua_getglobal(co, "work");
  lua_pushinteger(co, 5);
  lua_pushstring(co, "x");
  b14_calls = 0;
  lua_setbudget(co, 50, b14_budget);
  lua_ResumeResult r = lua_resume(co, L, 2);
  CHECK(lua_isfrozen(co));
  for (int i = 0; i < 300; i++)
    CHECK(lua_closethread(co, L) == LUA_ERRRUN);
  for (int i = 0; i < 300; i++)
  { /* (with no arguments: a frozen thread is read-only, the host pushes nothing on it) */
    r = lua_resume(co, L, 0);
    CHECK(r.status == LUA_ERRRUN);
  }
  while (lua_isfrozen(co))
    r = lua_thaw(co, L);
  CHECK(r.status == LUA_OK && r.nres == 2);
  CHECK(lua_gettop(co) == 2);
  CHECK(lua_tointeger(co, 1) == 20105);
  auto x = lua_tolstring(co, 2);
  CHECK(x && *x == byte_view("x"));
  lua_close(L);
}

/*
** A host's budget callback given 'user_co' (the topmost budgeted thread of
** the chain) and a 'spender' that is another thread of the chain: asked of
** 'user_co', 'lua_nofreeze' answers for the chain, and 'lua_freeze'
** freezes it (B15)
*/
static int b15_can = -1;
static void b15_budget(lua_State* user_co, lua_State* spender)
{
  if (user_co == spender || b15_can != -1)
    return;
  b15_can = lua_nofreeze(user_co) == 0;
  if (b15_can)
    lua_freeze(user_co);
}

static void test_freeze_given_user_co()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "function outer() local co = coroutine.create(function() local s = 0 "
                         "for i = 1, 500 do s = s + i end return s end) "
                         "local ok, s = coroutine.resume(co) return s end") == LUA_OK);
  lua_State* co = lua_newthread(L);
  lua_getglobal(co, "outer");
  b15_can = -1;
  lua_setbudget(co, 20, b15_budget);
  lua_ResumeResult r = lua_resume(co, L, 0);
  CHECK(b15_can == 1);
  CHECK(lua_isfrozen(co));
  while (lua_isfrozen(co))
    r = lua_thaw(co, L);
  CHECK(r.status == LUA_OK && lua_tointeger(co, -1) == 125250);
  lua_close(L);
}

static void test_freeze()
{
  lua_State* L = new_state();
  luaL_openlibs(L);
  lua_pushcfunction(L, fz_callk);
  lua_setglobal(L, "callk");
  lua_pushcfunction(L, fz_call);
  lua_setglobal(L, "call");
  lua_pushcfunction(L, fz_plainresume);
  lua_setglobal(L, "plainresume");
  lua_pushcfunction(L, fz_pcallk);
  lua_setglobal(L, "pcallk");
  lua_pushcfunction(L, fz_getik);
  lua_setglobal(L, "getik");
  int st = luaL_dostring(L, R"(
    function sum(n) local s = 0 for i = 1, n do s = s + i * 3 % 7 end return s end
    function ccalls(n) local ty, s = type, 0 for i = 1, n do if ty(i) == "number" then s = s + 1 end end return s end
    function yieldable(n)
      local s = sum(n)
      return s, coroutine.isyieldable()
    end
    function viacallk(n) local r, thawed = callk(sum, n) return r * 10 + (thawed and 1 or 0) end
    function viacall(n) return call(sum, n) end
    function canf() return debug.canfreeze() end
    function can_viacallk() return (callk(canf, 0)) end
    function can_viacall() return call(canf, 0) end
    function can_viaresume()
      return plainresume(coroutine.create(function() local _, w = debug.canfreeze() coroutine.yield(w) end))
    end
    function can_viacall_why() local w call(function() local _ _, w = debug.canfreeze() end) return w end
    function late(n)
      local r = sum(n)
      error("late " .. r, 0)
    end
    function viapcallk(n)
      local ok, v, st = pcallk(late, n)
      return tostring(ok) .. " " .. tostring(v) .. " " .. tostring(st) .. " " .. tostring(coroutine.isyieldable())
    end
    proxy = setmetatable({}, {__index = function(t, k)
      inyield = coroutine.isyieldable()
      return k * 1000 + sum(20)
    end})
    function viagetik(k) local v, thawed = getik(proxy, k) return v, thawed end
    function libwork()
      local t = {5, 3, 9, 1, 7, 2}
      table.sort(t, function(a, b) local x = sum(3) return a < b end)
      local g = string.gsub("abc", "%w", function(c) return c:upper() .. sum(2) end)
      local s = tostring(setmetatable({}, {__tostring = function() return "T" .. sum(2) end}))
      local p = setmetatable({}, {__index = function(_, i) if i <= 3 then return i * sum(1) end end})
      local n = 0
      for _, v in ipairs(p) do n = n + v end
      local c = table.concat(setmetatable({}, {__index = function(_, i) return tostring(i * sum(1)) end,
                                               __len = function() return 4 end}), "-")
      local f = string.format("%s|%5s", s, setmetatable({}, {__tostring = function() return "F" end}))
      return table.concat(t, ",") .. "|" .. g .. "|" .. f .. "|" .. n .. "|" .. c .. "|" ..
             tostring(coroutine.isyieldable())
    end
  )");
  CHECK(st == LUA_OK);
  lua_Integer left = 0;

  /* off by default; on; off again */
  CHECK(lua_getbudget(L) == 0 && lua_budgetleft(L) == 0);
  lua_setbudget(L, 100, fz_budget);
  CHECK(lua_getbudget(L) == 100 && lua_budgetleft(L) == 100);
  CHECK(lua_gethookmask(L) == 0); /* (not a hook) */
  lua_setbudget(L, 100, nullptr);
  CHECK(lua_getbudget(L) == 0);
  lua_setbudget(L, 0, fz_budget);
  CHECK(lua_getbudget(L) == 0);

  /* steps: a hand-off for the host's call, the instructions, the C calls */
  const lua_Integer HUGE = lua_Integer(1) << 50;
  int isum = fz_instructions(L, "sum", 50);
  int iccalls = fz_instructions(L, "ccalls", 50);
  lua_Integer before = 0;
  fz_reset(L, HUGE, nullptr, 0, false);
  before = lua_budgetleft(L);
  lua_getglobal(L, "sum");
  lua_pushinteger(L, 50);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_pop(L, 1);
  left = lua_budgetleft(L);
  CHECK(before - left == 1 + isum);
  before = lua_budgetleft(L);
  lua_getglobal(L, "ccalls");
  lua_pushinteger(L, 50);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_pop(L, 1);
  left = lua_budgetleft(L);
  CHECK(before - left == 1 + iccalls + 50);
  before = lua_budgetleft(L);
  lua_pushcfunction(L, fz_call);
  lua_getglobal(L, "sum");
  lua_pushinteger(L, 50);
  CHECK(lua_pcall(L, 2, 1, 0) == LUA_OK); /* hand-off, C call, hand-off */
  lua_pop(L, 1);
  left = lua_budgetleft(L);
  CHECK(before - left == 1 + 1 + 1 + isum);
  CHECK(fz_calls == 0);

  /* a callback every 'n' steps */
  for (int n = 1; n <= 9; n++)
  {
    fz_reset(L, n, nullptr, 0, false);
    lua_getglobal(L, "sum");
    lua_pushinteger(L, 50);
    CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
    lua_pop(L, 1);
    CHECK(fz_calls == isum / n);
  }

  /* the main thread, frozen at every budget point of a host 'lua_pcall'
     and of a host 'lua_call', thaws to the same result */
  lua_getglobal(L, "sum");
  lua_pushinteger(L, 50);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_Integer plain = lua_tointeger(L, -1);
  lua_pop(L, 1);
  for (int callkind = 0; callkind < 2; callkind++)
  {
    int frozeat = 0;
    for (int at = 1; at <= isum; at++)
    {
      fz_reset(L, 1, L, at, false);
      lua_getglobal(L, "sum");
      lua_pushinteger(L, 50);
      if (callkind == 0)
        CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
      else
        lua_call(L, 1, 1);
      bool frozen = lua_isfrozen(L);
      if (frozen)
        frozeat++;
      int thaws = 0;
      lua_ResumeResult r{LUA_OK, 1};
      while (lua_isfrozen(L))
      {
        r = lua_thaw(L, nullptr);
        thaws++;
      }
      CHECK(r.status == LUA_OK && r.nres == 1);
      CHECK(lua_gettop(L) == 1 && lua_tointeger(L, 1) == plain);
      CHECK(thaws == (frozen ? 1 : 0));
      lua_settop(L, 0);
    }
    CHECK(frozeat == isum);
  }
  /* frozen at every point, again and again */
  fz_reset(L, 1, L, 1, true);
  lua_getglobal(L, "sum");
  lua_pushinteger(L, 50);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  int thaws = 0;
  while (lua_isfrozen(L))
  {
    lua_thaw(L, nullptr);
    thaws++;
  }
  CHECK(thaws == isum && fz_freezes == isum);
  CHECK(lua_gettop(L) == 1 && lua_tointeger(L, 1) == plain);
  lua_settop(L, 0);

  /* the main thread cannot yield after a thaw, as before */
  fz_reset(L, 3, L, 1, true);
  lua_getglobal(L, "yieldable");
  lua_pushinteger(L, 20);
  CHECK(lua_pcall(L, 1, 2, 0) == LUA_OK);
  while (lua_isfrozen(L))
    lua_thaw(L, nullptr);
  CHECK(lua_gettop(L) == 2 && !lua_toboolean(L, 2));
  CHECK(!lua_isyieldable(L));
  lua_settop(L, 0);

  /* through a C function with a continuation: its continuation goes on */
  lua_getglobal(L, "viacallk");
  lua_pushinteger(L, 30);
  fz_reset(L, 0, nullptr, 0, false);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_Integer viaplain = lua_tointeger(L, -1); /* (not thawed: ends in 0) */
  lua_settop(L, 0);
  CHECK(viaplain % 10 == 0);
  fz_reset(L, 4, L, 1, true);
  lua_getglobal(L, "viacallk");
  lua_pushinteger(L, 30);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  CHECK(lua_isfrozen(L));
  while (lua_isfrozen(L))
    lua_thaw(L, nullptr);
  CHECK(lua_gettop(L) == 1 && lua_tointeger(L, 1) == viaplain + 1);
  CHECK(fz_cb_can == 1 && fz_cb_why == 0); /* (and that it can) */
  lua_settop(L, 0);

  /* lua_nofreeze and debug.canfreeze: no budget in effect, none; under a
     budget, a plain call in the way makes it false, a call with a
     continuation does not */
  fz_reset(L, 0, nullptr, 0, false);
  CHECK(lua_nofreeze(L) == LUA_NOFREEZE_BUDGET);
  lua_getglobal(L, "canf");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK && !lua_toboolean(L, -1));
  lua_settop(L, 0);
  fz_reset(L, HUGE, nullptr, 0, false);
  lua_getglobal(L, "canf");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_toboolean(L, -1));
  lua_settop(L, 0);
  lua_getglobal(L, "can_viacallk");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_toboolean(L, -1));
  lua_settop(L, 0);
  lua_getglobal(L, "can_viacall");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK && !lua_toboolean(L, -1));
  lua_settop(L, 0);
  lua_getglobal(L, "can_viaresume");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(fz_str(L, -1) == "a resume that cannot freeze is in the way");
  lua_settop(L, 0);
  lua_getglobal(L, "can_viacall_why");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  CHECK(fz_str(L, -1) == "a call that cannot freeze is in the way");
  lua_settop(L, 0);
  fz_reset(L, 0, nullptr, 0, false);

  /* through a C function without one: the freeze cannot happen, the
     chain is destroyed (the thread is dead, with the message) and the
     host's call fails; closing the thread makes it usable again */
  fz_reset(L, 2, L, 1, true);
  lua_getglobal(L, "viacall");
  lua_pushinteger(L, 30);
  st = lua_pcall(L, 1, 1, 0);
  while (st == LUA_OK && lua_isfrozen(L))
    st = lua_thaw(L, nullptr).status;
  CHECK(st == LUA_ERRRUN && fz_str(L, -1) == "cannot freeze");
  CHECK(fz_cb_can == 0 && fz_cb_why == LUA_NOFREEZE_CALL); /* (the callback was told that the freeze cannot happen) */
  CHECK(!lua_isfrozen(L) && lua_status(L) == LUA_ERRRUN);
  CHECK(lua_closethread(L, nullptr) == LUA_ERRRUN);
  lua_settop(L, 0);
  CHECK(lua_status(L) == LUA_OK);

  /* a protected call with a continuation where the thread cannot yield:
     frozen inside, its error after the thaw is still caught by it */
  fz_reset(L, 0, nullptr, 0, false);
  lua_getglobal(L, "viapcallk");
  lua_pushinteger(L, 30);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  std::string pplain = fz_str(L, -1);
  lua_settop(L, 0);
  CHECK(pplain.rfind("false late ", 0) == 0 && pplain.find(" -1 false") != std::string::npos);
  fz_reset(L, 3, L, 1, true);
  lua_getglobal(L, "viapcallk");
  lua_pushinteger(L, 30);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  CHECK(lua_isfrozen(L));
  while (lua_isfrozen(L))
    lua_thaw(L, nullptr);
  CHECK(lua_gettop(L) == 1);
  std::string pthawed = fz_str(L, 1);
  /* (after a thaw, the continuation finishes the call: status LUA_ERRRUN) */
  CHECK(pthawed.substr(0, pthawed.find(" 2 false")) == pplain.substr(0, pplain.find(" -1 false")));
  CHECK(pthawed.rfind("false late ", 0) == 0 && pthawed.find(" 2 false") != std::string::npos);
  lua_settop(L, 0);

  /* an API call with a continuation where the thread cannot yield: its
     metamethod freezes, cannot yield, and the continuation goes on */
  fz_reset(L, 0, nullptr, 0, false);
  lua_getglobal(L, "viagetik");
  lua_pushinteger(L, 7);
  CHECK(lua_pcall(L, 1, 2, 0) == LUA_OK);
  lua_Integer gplain = lua_tointeger(L, 1);
  CHECK(gplain > 7000 && !lua_toboolean(L, 2));
  lua_settop(L, 0);
  fz_reset(L, 3, L, 1, true);
  lua_getglobal(L, "viagetik");
  lua_pushinteger(L, 7);
  CHECK(lua_pcall(L, 1, 2, 0) == LUA_OK);
  CHECK(lua_isfrozen(L));
  while (lua_isfrozen(L))
    lua_thaw(L, nullptr);
  CHECK(lua_gettop(L) == 2 && lua_tointeger(L, 1) == gplain && lua_toboolean(L, 2));
  lua_getglobal(L, "inyield");
  CHECK(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
  lua_settop(L, 0);

  /* library functions that call back, in the main thread (it cannot
     yield): frozen at each budget point, and at all of them, they end as a
     plain run does */
  fz_reset(L, 0, nullptr, 0, false);
  lua_getglobal(L, "libwork");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  std::string lplain = fz_str(L, -1);
  lua_settop(L, 0);
  CHECK(lplain == "1,2,3,5,7,9|A9B9C9|T9|    F|18|3-6-9-12|false"); /* (as stock Lua) */
  fz_reset(L, 1, nullptr, 0, false);
  lua_getglobal(L, "libwork");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_settop(L, 0);
  int lpoints = fz_calls;
  CHECK(lpoints > 100);
  int lfroze = 0;
  for (int at = 1; at <= lpoints; at++)
  {
    fz_reset(L, 1, L, at, false);
    lua_getglobal(L, "libwork");
    CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
    if (lua_isfrozen(L))
      lfroze++;
    while (lua_isfrozen(L))
      lua_thaw(L, nullptr);
    CHECK(lua_gettop(L) == 1 && fz_str(L, 1) == lplain);
    lua_settop(L, 0);
  }
  CHECK(lfroze == lpoints);
  fz_reset(L, 1, L, 1, true);
  lua_getglobal(L, "libwork");
  CHECK(lua_pcall(L, 0, 1, 0) == LUA_OK);
  while (lua_isfrozen(L))
    lua_thaw(L, nullptr);
  CHECK(lua_gettop(L) == 1 && fz_str(L, 1) == lplain);
  CHECK(fz_freezes > 100);
  lua_settop(L, 0);

  /* an error after a thaw that nothing catches: as 'lua_pcall' reports it */
  fz_reset(L, 3, L, 1, false);
  lua_getglobal(L, "late");
  lua_pushinteger(L, 30);
  CHECK(lua_pcall(L, 1, 1, 0) == LUA_OK);
  CHECK(lua_isfrozen(L));
  lua_ResumeResult er = lua_thaw(L, nullptr);
  CHECK(er.status == LUA_ERRRUN && er.nres == 1);
  CHECK(lua_gettop(L) == 1 && fz_str(L, 1).rfind("late ", 0) == 0);
  lua_settop(L, 0);
  /* ... and from a host 'lua_call' too (the thaw is protected) */
  lua_getglobal(L, "late");
  lua_pushinteger(L, 30);
  fz_reset(L, 3, L, 1, false);
  lua_call(L, 1, 1);
  CHECK(lua_isfrozen(L));
  er = lua_thaw(L, nullptr);
  CHECK(er.status == LUA_ERRRUN && lua_gettop(L) == 1);
  lua_settop(L, 0);

  /* coroutines through 'lua_resume': frozen at every budget point, also
     inside the coroutines they resume, with their own yields */
  st = luaL_dostring(L, R"(
    function body(a)
      local inner = coroutine.create(function(x)
        local y = coroutine.yield(x + sum(5))
        return y * 2
      end)
      local _, v = coroutine.resume(inner, a)
      local w = coroutine.yield(v)
      local _, z = coroutine.resume(inner, w)
      local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)
      return z + gen() + gen() + gen() + sum(10)
    end
  )");
  CHECK(st == LUA_OK);
  auto runbody = [&](lua_Integer n, bool freeze, int& freezes) -> std::string
  {
    lua_State* co = lua_newthread(L);
    lua_getglobal(co, "body");
    lua_pushinteger(co, 4);
    fz_reset(L, n, freeze ? co : nullptr, 1, true);
    lua_setbudget(co, n, n > 0 ? fz_budget : nullptr); /* (the thread was made before the budget) */
    std::string log;
    int nargs = 1;
    for (;;)
    {
      lua_ResumeResult rr = lua_isfrozen(co) ? lua_thaw(co, nullptr) : lua_resume(co, nullptr, nargs);
      nargs = 0;
      if (lua_isfrozen(co))
      {
        CHECK(rr.status == LUA_OK && rr.nres == 0);
        continue;
      }
      if (rr.status == LUA_YIELD)
      {
        CHECK(rr.nres == 1);
        lua_Integer v = lua_tointeger(co, -1);
        lua_pop(co, 1);
        log += "y" + std::to_string(v) + " ";
        lua_pushinteger(co, v + 1);
        nargs = 1;
        continue;
      }
      CHECK(rr.status == LUA_OK);
      log += "r" + std::to_string(lua_tointeger(co, -1));
      break;
    }
    freezes = fz_freezes;
    lua_setbudget(L, 0, nullptr);
    lua_pop(L, 1); /* the thread */
    return log;
  };
  int fzs = 0;
  std::string cplain = runbody(0, false, fzs);
  for (lua_Integer n : {1, 2, 3, 5, 8})
  {
    std::string clog = runbody(n, true, fzs);
    CHECK(clog == cplain);
    CHECK(fzs > 5);
  }

  /* the budget belongs to a thread and to the threads it creates */
  {
    lua_State* unbudgeted = lua_newthread(L); /* made with no budget in effect */
    lua_State* owner = lua_newthread(L);
    lua_setbudget(owner, 5, fz_budget);
    lua_State* child = lua_newthread(owner); /* made while the group is in effect */
    CHECK(lua_getbudget(owner) == 5 && lua_budgetleft(owner) == 5);
    CHECK(lua_getbudget(child) == 5 && lua_budgetleft(child) == 5);
    CHECK(lua_getbudget(unbudgeted) == 0);
    CHECK(lua_getbudget(L) == 0);
    lua_setbudget(owner, 0, nullptr);
    CHECK(lua_getbudget(owner) == 0);
    CHECK(lua_getbudget(child) == 5); /* it keeps its group */
    lua_State* later = lua_newthread(owner);
    CHECK(lua_getbudget(later) == 0);
    lua_pop(L, 2); /* the threads */
  }

  /* setting a budget that needs a new group, with no memory for it, gives
     LUA_ERRMEM and raises nothing, on the thread or elsewhere: the thread is
     as it was; with memory, it is set */
  {
    CapDeny cd;
    lua_State* S = lua_newstate(&cd, 0);
    luaL_openlibs(S);
    lua_State* co = lua_newthread(S);
    CHECK(luaL_loadstring(co, "coroutine.yield(1) return 2") == LUA_OK);
    CHECK(lua_resume(co, S, 0).status == LUA_YIELD);
    lua_pop(co, 1);
    lua_gc(S, LUA_GCCOLLECT, 0, 0, 0);
    cd.cap = cd.used + 1; /* (no room for a group) */
    CHECK(lua_setbudget(co, 100, fz_budget) == LUA_ERRMEM);
    cd.cap = 0;
    CHECK(lua_status(co) == LUA_YIELD && lua_getbudget(co) == 0);
    CHECK(lua_setbudget(co, 100, fz_budget) == LUA_OK && lua_getbudget(co) == 100);
    lua_ResumeResult rr = lua_resume(co, S, 0);
    CHECK(rr.status == LUA_OK && rr.nres == 1 && lua_tointeger(co, -1) == 2);
    lua_close(S);
    CHECK(cd.used == 0);
  }

  /* closing the state with the main thread frozen runs none of its pending
     '__close' (a frozen thread runs none), and frees everything */
  {
    static bool closed = false;
    closed = false;
    CapDeny cd;
    lua_State* S = lua_newstate(&cd, 0);
    luaL_openlibs(S);
    lua_pushcfunction(S, [](lua_State*) -> int { closed = true; return 0; });
    lua_setglobal(S, "mark");
    fz_reset(S, 50, S, 1, false);
    CHECK(luaL_loadstring(S, "local x <close> = setmetatable({}, {__close = mark}) while true do end") == LUA_OK);
    CHECK(lua_pcall(S, 0, 0, 0) == LUA_OK && lua_isfrozen(S));
    lua_close(S);
    CHECK(!closed && cd.used == 0);
  }

  /* a host call that fails closes its variables where a '__close' can
     freeze: a freeze lands at the host call (LUA_OK, frozen), and the thaw
     finishes the close and gives the error, as the call would have; also
     when the error comes after a thaw, and when the close freezes again */
  for (int every = 0; every < 2; every++)
  {
    lua_State* S = luaL_newstate();
    luaL_openlibs(S);
    CHECK(luaL_dostring(S, "log = {}") == LUA_OK);
    CHECK(luaL_loadstring(S, "for i = 1, 100 do end "
                             "local x <close> = setmetatable({}, {__close = function(_, e) "
                             "log[#log + 1] = 'start' for i = 1, 300 do end log[#log + 1] = 'end ' .. tostring(e) end}) "
                             "error('boom', 0)") == LUA_OK);
    fz_reset(S, 50, S, every ? 1 : 5, every != 0); /* (the 5th: in the close) */
    int pst = lua_pcall(S, 0, 0, 0);
    CHECK(pst == LUA_OK && lua_isfrozen(S));
    lua_ResumeResult r{LUA_OK, 0};
    int nthaws = 0;
    while (lua_isfrozen(S))
    {
      r = lua_thaw(S, nullptr);
      nthaws++;
    }
    fz_reset(S, 0, nullptr, 0, false);
    CHECK(r.status == LUA_ERRRUN && r.nres == 1 && fz_str(S, -1) == "boom");
    CHECK(every ? nthaws > 5 : nthaws == 1);
    lua_settop(S, 0);
    CHECK(luaL_dostring(S, "return table.concat(log, ',')") == LUA_OK && fz_str(S, -1) == "start,end boom");
    lua_close(S);
  }

  /* the host's close of a coroutine: its '__close' run under its budget,
     and a freeze there lands at lua_closethread (LUA_OK, frozen); the thaw
     finishes the close, with its status */
  for (int failed = 0; failed < 2; failed++)
  {
    lua_State* S = luaL_newstate();
    luaL_openlibs(S);
    CHECK(luaL_dostring(S, "log = {}") == LUA_OK);
    lua_State* co = lua_newthread(S);
    CHECK(luaL_loadstring(co, "local x <close> = setmetatable({}, {__close = function(_, e) "
                              "log[#log + 1] = 'start' for i = 1, 300 do end log[#log + 1] = 'end ' .. tostring(e) end}) "
                              "local fail = ... coroutine.yield() if fail then error('boom', 0) end") == LUA_OK);
    lua_pushboolean(co, failed);
    CHECK(lua_resume(co, S, 1).status == LUA_YIELD);
    lua_settop(co, 0);
    if (failed)
      CHECK(lua_resume(co, S, 0).status == LUA_ERRRUN); /* (dead, its variable not closed) */
    fz_reset(S, 0, nullptr, 0, false);
    lua_setbudget(co, 50, fz_budget);
    fz_target = co;
    fz_at = 2;
    int cst = lua_closethread(co, nullptr);
    CHECK(cst == LUA_OK && lua_isfrozen(co));
    lua_ResumeResult r{LUA_OK, 0};
    while (lua_isfrozen(co))
      r = lua_thaw(co, nullptr);
    CHECK(r.status == (failed ? LUA_ERRRUN : LUA_OK));
    if (failed)
      CHECK(fz_str(co, -1) == "boom");
    fz_reset(S, 0, nullptr, 0, false);
    CHECK(luaL_dostring(S, "return table.concat(log, ',')") == LUA_OK);
    CHECK(fz_str(S, -1) == (failed ? "start,end boom" : "start,end nil"));
    lua_close(S);
  }

  /* and where such a close cannot freeze (a C function without a
     continuation is in the way), the chain is destroyed: the host call
     fails, the thread is dead with the message; whether the close began at
     the host call (0), after a thaw (1) or was thawed itself (2) */
  for (int mode = 0; mode < 3; mode++)
  {
    lua_State* S = luaL_newstate();
    luaL_openlibs(S);
    lua_pushcfunction(S, fz_call);
    lua_setglobal(S, "call");
    CHECK(luaL_dostring(S, "log = {}") == LUA_OK);
    CHECK(luaL_loadstring(S, "local pre, mid = ... for i = 1, pre do end "
                             "local x <close> = setmetatable({}, {__close = function(_, e) "
                             "log[#log + 1] = 'start' for i = 1, mid do end "
                             "call(function() for i = 1, 300 do end end) log[#log + 1] = 'end' end}) "
                             "error('boom', 0)") == LUA_OK);
    lua_pushinteger(S, mode == 1 ? 200 : 0);
    lua_pushinteger(S, mode == 2 ? 200 : 0);
    fz_reset(S, 50, S, 1, true);
    int pst = lua_pcall(S, 2, 0, 0);
    int nthaws = 0;
    while (pst == LUA_OK && lua_isfrozen(S))
    {
      pst = lua_thaw(S, nullptr).status;
      nthaws++;
    }
    fz_reset(S, 0, nullptr, 0, false);
    CHECK(pst == LUA_ERRRUN && fz_str(S, -1) == "cannot freeze");
    CHECK(mode == 0 ? nthaws == 0 : nthaws > 0);
    CHECK(!lua_isfrozen(S) && lua_status(S) == LUA_ERRRUN);
    CHECK(lua_closethread(S, nullptr) == LUA_ERRRUN);
    lua_settop(S, 0);
    CHECK(lua_status(S) == LUA_OK);
    CHECK(luaL_dostring(S, "return table.concat(log, ',')") == LUA_OK && fz_str(S, -1) == "start");
    lua_close(S);
  }
  {
    lua_State* S = luaL_newstate();
    luaL_openlibs(S);
    lua_pushcfunction(S, fz_call);
    lua_setglobal(S, "call");
    CHECK(luaL_dostring(S, "log = {}") == LUA_OK);
    lua_State* co = lua_newthread(S);
    CHECK(luaL_loadstring(co, "local x <close> = setmetatable({}, {__close = function(_, e) "
                              "log[#log + 1] = 'start' call(function() for i = 1, 300 do end end) "
                              "log[#log + 1] = 'end' end}) coroutine.yield()") == LUA_OK);
    CHECK(lua_resume(co, S, 0).status == LUA_YIELD);
    lua_settop(co, 0);
    fz_reset(S, 0, nullptr, 0, false);
    lua_setbudget(co, 50, fz_budget);
    fz_target = co;
    fz_at = 1;
    fz_every = true;
    CHECK(lua_closethread(co, nullptr) == LUA_ERRRUN && fz_str(co, -1) == "cannot freeze");
    CHECK(!lua_isfrozen(co) && lua_status(co) == LUA_ERRRUN);
    fz_reset(S, 0, nullptr, 0, false);
    CHECK(luaL_dostring(S, "return table.concat(log, ',')") == LUA_OK && fz_str(S, -1) == "start");
    lua_close(S);
  }

  /* a continuation with no calling thread is not used: 'lua_resumek' with
     no 'from' is 'lua_resume' */
  {
    lua_State* co = lua_newthread(L);
    CHECK(luaL_loadstring(co, "coroutine.yield(1) return 2") == LUA_OK);
    static auto k = [](lua_State*, int, lua_KContext) -> int { CHECK(false); return 0; };
    lua_ResumeResult rr = lua_resumek(co, nullptr, 0, 0, k);
    CHECK(rr.status == LUA_YIELD && rr.nres == 1 && lua_tointeger(co, -1) == 1);
    lua_pop(co, 1);
    rr = lua_resumek(co, nullptr, 0, 0, k);
    CHECK(rr.status == LUA_OK && rr.nres == 1 && lua_tointeger(co, -1) == 2);
    lua_pop(L, 1); /* the thread */
  }

  /* 'lua_freeze' only in the callback */
  lua_pushcfunction(L, [](lua_State* T) -> int { lua_freeze(T); return 0; });
  CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  CHECK(fz_str(L, -1) == "freeze outside the budget callback");
  lua_pop(L, 1);

  /* an error in the callback destroys the chain; a protected call in it
     does not catch that */
  {
    static auto failing = [](lua_State*, lua_State* spender)
    {
      lua_checkstack(spender, 1);
      lua_pushstring(spender, "callback failed");
      lua_error(spender);
    };
    lua_State* co = lua_newthread(L);
    lua_setbudget(co, 5, failing);
    CHECK(luaL_loadstring(co, "local ok = pcall(function() for i = 1, 100 do end end) caught = true") == LUA_OK);
    lua_ResumeResult rr = lua_resume(co, L, 0);
    CHECK(rr.status == LUA_ERRRUN && fz_str(co, -1) == "callback failed");
    CHECK(lua_status(co) == LUA_ERRRUN && !lua_isfrozen(co));
    lua_getglobal(L, "caught");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 2); /* the thread */
  }

  /* only unbudgeted code can thaw */
  {
    lua_State* co = lua_newthread(L);
    lua_setbudget(co, 5, fz_budget);
    fz_reset(L, 0, nullptr, 0, false);
    lua_setbudget(co, 5, fz_budget);
    fz_target = co;
    fz_at = 1;
    fz_every = false;
    CHECK(luaL_loadstring(co, "for i = 1, 100 do end") == LUA_OK);
    lua_ResumeResult rr = lua_resume(co, L, 0);
    CHECK(rr.status == LUA_OK && lua_isfrozen(co));
    lua_State* budgeted = lua_newthread(L);
    lua_setbudget(budgeted, 100, fz_budget);
    CHECK(lua_thaw(co, budgeted).status == LUA_ERRRUN);
    CHECK(fz_str(co, -1) == "cannot thaw from budgeted code");
    lua_pop(co, 1);
    CHECK(lua_isfrozen(co));
    /* a frozen thread is read-only: closing it fails and changes nothing */
    CHECK(lua_closethread(co, L) == LUA_ERRRUN);
    CHECK(fz_str(co, -1) == "cannot close a frozen coroutine");
    lua_pop(co, 1);
    CHECK(lua_isfrozen(co));

    fz_target = nullptr;
    rr = lua_thaw(co, L);
    CHECK(rr.status == LUA_OK && !lua_isfrozen(co));
    lua_pop(L, 2);
  }

  /* 'lua_thaw' of a thread that is not frozen */
  lua_State* nf = lua_newthread(L);
  lua_ResumeResult tr = lua_thaw(nf, L);
  CHECK(tr.status == LUA_ERRRUN && fz_str(nf, -1) == "cannot thaw a coroutine that is not frozen");
  lua_pop(L, 1);

  /* hooks: the budget is not in the hook mask; setting a hook keeps the
     budget; a thread made after the budget counts too */
  fz_reset(L, 5, nullptr, 0, false);
  lua_sethook(L, fz_counthook, LUA_MASKCOUNT, 1000000);
  CHECK(lua_gethookmask(L) == LUA_MASKCOUNT);
  lua_sethook(L, nullptr, 0, 0);
  CHECK(lua_gethookmask(L) == 0);
  st = luaL_dostring(L, "for i = 1, 100 do end");
  CHECK(st == LUA_OK && fz_calls > 10);
  lua_State* later = lua_newthread(L);
  luaL_loadstring(later, "for i = 1, 100 do end");
  int calls0 = fz_calls;
  CHECK(lua_resume(later, L, 0).status == LUA_OK);
  CHECK(fz_calls > calls0 + 10);
  lua_setbudget(L, 0, nullptr);
  lua_settop(L, 0);
  lua_close(L);
}

/* }====================================================== */

/* with arguments, only the tests whose names they give run (under valgrind) */
int main(int argc, char** argv)
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
      {"error_handling", test_error_handling},
      {"load_dump", test_load_dump},
      {"coroutines", test_coroutines},
      {"api_continuations", test_api_continuations},
      {"api_freezable", test_api_freezable},
      {"freeze", test_freeze},
      {"frozen_refusals_keep_top", test_frozen_refusals_keep_top},
      {"freeze_given_user_co", test_freeze_given_user_co},
      {"group_gc", test_group_gc},
      {"message_handler_freeze", test_message_handler_freeze},
      {"handler_in_effect_after_thaw", test_handler_in_effect_after_thaw},
      {"close_with_budget", test_close_with_budget},
      {"warnings", test_warnings},
      {"warn_control", test_warn_control},
      {"db_gethook_mask", test_db_gethook_mask},
      {"debughook_identity", test_debughook_identity},
      {"db_debug", test_db_debug},
      {"gc", test_gc},
      {"zero_size_userdata", test_zero_size_userdata},
      {"userobject", test_userobject},
      {"oom_between_array_allocs", test_oom_between_array_allocs},
      {"oom_long_concat", test_oom_long_concat},
      {"strings_through_allocator", test_strings_through_allocator},
      {"long_strings_made_in_place", test_long_strings_made_in_place},
      {"string_emergency_collection", test_string_emergency_collection},
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
      
      {"continuation_chain", test_continuation_chain},
      {"thread_hooks", test_thread_hooks},
      {"io_handles", test_io_handles},
      {"auxlib_restored", test_auxlib_restored},
      {"num2str", test_num2str},
      {"string_coercion", test_string_coercion},
      {"arith_floats", test_arith_floats},
      {"fixed_chunks", test_fixed_chunks},
      {"userdata_uservalues_gc", test_userdata_uservalues_gc},
      {"c_upvalue_indices", test_c_upvalue_indices},
      {"hook_yield", test_hook_yield},
      {"selected_libs", test_selected_libs},
      {"intern_identity", test_intern_identity},
      {"intern_resize_stress", test_intern_resize_stress},
      {"intern_collisions", test_intern_collisions},
      {"intern_gc_churn", test_intern_gc_churn},
      {"intern_long_keys", test_intern_long_keys},
      {"newmetatable_oom", test_newmetatable_oom},
      {"cfunc_stress", test_cfunc_stress},
  };

  for (const TestEntry& t : tests)
  {
    bool selected = (argc <= 1);
    for (int i = 1; i < argc; i++)
      selected = selected || std::strcmp(argv[i], t.name) == 0;
    if (!selected)
      continue;
    g_current = t.name;
    t.fn();
  }

  std::printf("test_public_api: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
