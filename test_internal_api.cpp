/*
** test_internal_api.cpp
** Tests for the host-facing internal API (lua_internal.h) — the extensions
** this port adds beyond the formal Lua 5.5 surface (memory limits,
** lua_newuserdata, lua_resetthread, ...).
**
** Deliberately separate from test_public_api.cpp, which includes only the
** official consumer headers (lua.h, lualib.h, lauxlib.h) and tests the
** formal API. This file is the home for everything that requires
** lua_internal.h.
*/

#include <cstdio>
#include <string_view>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_internal.h"

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

static lua_State* new_state()
{
  lua_State* L = luaL_newstate();
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

static int load_string(lua_State* L, std::string_view code)
{
  StringReader r{code, false};
  return lua_load(L, whole_reader, &r, "=tia"_hs, std::nullopt);
}

static int do_string(lua_State* L, std::string_view code)
{
  int st = load_string(L, code);
  if (st != LUA_OK)
    return st;
  return lua_pcall(L, 0, LUA_MULTRET, 0);
}

/* ================= tests ================= */

/* lua_newuserdata: one uservalue by default */
static void test_newuserdata()
{
  lua_State* L = new_state();

  void* p = lua_newuserdata(L, 32);
  CHECK(p != nullptr);
  CHECK(lua_type(L, -1) == LUA_TUSERDATA);
  CHECK(lua_getiuservalue(L, -1, 1) == LUA_TNIL); /* has slot 1 */
  lua_pop(L, 1);
  CHECK(lua_getiuservalue(L, -1, 2) == LUA_TNONE); /* but not slot 2 */
  lua_pop(L, 2);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* memory limit: get/set round-trip, allocation keeps working under a
   generous limit */
static void test_memlimit()
{
  lua_State* L = new_state();
  luaL_openselectedlibs(L, LUA_GLIBK, 0);

  size_t orig = lua_getmemlimit(L);
  lua_setmemlimit(L, 512u * 1024u * 1024u);
  CHECK(lua_getmemlimit(L) == 512u * 1024u * 1024u);
  CHECK(do_string(L, "local t = {} for i = 1, 1000 do t[i] = i end "
                     "return #t") == LUA_OK);
  CHECK(lua_tointeger(L, -1) == 1000);
  lua_pop(L, 1);
  lua_setmemlimit(L, orig);
  CHECK(lua_getmemlimit(L) == orig);

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* lua_resetthread: a suspended coroutine returns to a pristine state */
static void test_resetthread()
{
  lua_State* L = new_state();

  lua_State* co = lua_newthread(L);
  /* give the thread the coroutine lib so yield resolves */
  luaL_openselectedlibs(co, LUA_GLIBK | LUA_COLIBK, 0);
  CHECK(load_string(co, "coroutine.yield(1) return 2") == LUA_OK);
  lua_ResumeResult rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_YIELD);
  CHECK(lua_status(co) == LUA_YIELD);

  CHECK(lua_resetthread(co) == LUA_OK);
  CHECK(lua_status(co) == LUA_OK);
  CHECK(lua_gettop(co) == 0);

  /* the reset thread is reusable */
  CHECK(load_string(co, "return 'reused'") == LUA_OK);
  rr = lua_resume(co, L, 0);
  CHECK(rr.status == LUA_OK);
  CHECK(lua_tolstring(co, -1) == "reused");
  lua_pop(L, 1); /* the thread */

  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* ================= runner ================= */

struct TestEntry
{
  const char* name;
  void (*fn)();
};

int main()
{
  const TestEntry tests[] = {
      {"newuserdata", test_newuserdata},
      {"memlimit", test_memlimit},
      {"resetthread", test_resetthread},
  };

  for (const TestEntry& t : tests)
  {
    g_current = t.name;
    t.fn();
  }

  std::printf("test_internal_api: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
