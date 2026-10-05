/*
** Styx test host: C++ tests plus a runner for the Lua scripts in this
** directory. Run from tests/styx (part of `make test`).
**
** Each script runs once per GC mode, in a fresh state, with the shared
** harness (support/harness.lua) on the package path. Debug builds also open
** the test library, so the harness checks the heap invariants.
*/

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#ifdef LUA_DEBUG
#include "ltests.h"
#endif

static int g_checks = 0;
static int g_failures = 0;
static int g_test_failures = 0;
static const char* g_current = "";

static void check_impl(bool ok, const char* expr, int line)
{
  g_checks++;
  if (!ok)
  {
    g_failures++;
    g_test_failures++;
    std::printf("      FAIL [%s] line %d: %s\n", g_current, line, expr);
  }
}
#define CHECK(cond) check_impl((cond), #cond, __LINE__)

static lua_State* new_state()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
#ifdef LUA_DEBUG
  luaL_requiref(L, "T", luaB_opentests, 1);
  lua_pop(L, 1);
#endif
  return L;
}

static std::string to_string(lua_State* L, int idx)
{
  auto s = lua_tolstring(L, idx);
  return s ? std::string(s->data(), s->size()) : std::string();
}

static void push_string(lua_State* L, const std::string& s)
{
  lua_pushlstring(L, byte_view(s.data(), s.size()), 0, s.size());
}

/* pushes the module, as a script's `require "styx"` would */
static bool push_styx(lua_State* L)
{
  lua_getglobal(L, "require");
  lua_pushstring(L, "styx");
  return lua_pcall(L, 1, 1, 0) == LUA_OK;
}

/* [args...] -> [results...] (or [error]); returns the lua_pcall status */
static int call_styx(lua_State* L, const char* fn, int nargs, int nresults)
{
  if (!push_styx(L))
    return LUA_ERRRUN;
  lua_getfield(L, -1, fn);
  lua_remove(L, -2);
  lua_insert(L, -(nargs + 1));
  return lua_pcall(L, nargs, nresults, 0);
}

/* the value on top survives persist/unpersist; the stack is restored */
static bool roundtrips(lua_State* L)
{
  int base = lua_gettop(L) - 1;
  lua_pushvalue(L, -1);
  bool ok = call_styx(L, "persist", 1, 1) == LUA_OK &&
            call_styx(L, "unpersist", 1, 1) == LUA_OK &&
            lua_type(L, -1) == lua_type(L, -2) &&
            lua_isinteger(L, -1) == lua_isinteger(L, -2) &&
            lua_rawequal(L, -1, -2);
  lua_settop(L, base);
  return ok;
}

/* runs a chunk; returns its first result as a string ("" on error) */
static std::string eval(lua_State* L, const char* code)
{
  int top = lua_gettop(L);
  std::string r;
  if (luaL_loadstring(L, code) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK)
    r = to_string(L, -1);
  else
    std::printf("      error: %s\n", to_string(L, -1).c_str());
  lua_settop(L, top);
  return r;
}

static void test_requirable_not_global()
{
  lua_State* L = new_state();
  CHECK(lua_getglobal(L, "styx") == LUA_TNIL);
  lua_pop(L, 1);
  CHECK(push_styx(L));
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  lua_close(L);
}

static void test_api_surface()
{
  lua_State* L = new_state();
  CHECK(push_styx(L));
  CHECK(lua_getfield(L, -1, "persist") == LUA_TFUNCTION);
  lua_pop(L, 1);
  CHECK(lua_getfield(L, -1, "unpersist") == LUA_TFUNCTION);
  lua_pop(L, 1);
  CHECK(lua_getfield(L, -1, "settings") == LUA_TFUNCTION);
  lua_close(L);
}

static void test_primitives_roundtrip()
{
  lua_State* L = new_state();
  lua_pushnil(L);
  CHECK(roundtrips(L));
  lua_pushboolean(L, 1);
  CHECK(roundtrips(L));
  lua_pushboolean(L, 0);
  CHECK(roundtrips(L));
  lua_pushinteger(L, 42);
  CHECK(roundtrips(L));
  lua_pushinteger(L, LUA_MININTEGER);
  CHECK(roundtrips(L));
  lua_pushnumber(L, 3.5);
  CHECK(roundtrips(L));
  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

/* strings with every byte value, from C++ */
static void test_string_bytes()
{
  lua_State* L = new_state();
  std::string all;
  for (int i = 0; i < 256; i++)
    all.push_back(static_cast<char>(i));
  push_string(L, all);
  CHECK(call_styx(L, "persist", 1, 1) == LUA_OK);
  CHECK(call_styx(L, "unpersist", 1, 1) == LUA_OK);
  CHECK(to_string(L, -1) == all);
  lua_close(L);
}

/* persist and unpersist leave the caller's stack as it was, also on error */
static void test_stack_balance()
{
  lua_State* L = new_state();
  lua_pushinteger(L, 1);
  lua_pushinteger(L, 2);
  lua_newtable(L);
  CHECK(call_styx(L, "persist", 1, 1) == LUA_OK);
  CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TSTRING);
  CHECK(call_styx(L, "unpersist", 1, 1) == LUA_OK);
  CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TTABLE);
  lua_pop(L, 1);
  lua_getglobal(L, "io");
  lua_getfield(L, -1, "stdout");
  lua_remove(L, -2);
  CHECK(call_styx(L, "persist", 1, 1) == LUA_ERRRUN);
  CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TSTRING);
  lua_pop(L, 1);
  CHECK(lua_tointeger(L, 1) == 1 && lua_tointeger(L, 2) == 2 && lua_gettop(L) == 2);
  lua_close(L);
}

/* moves a blob between two states: persist in A, return the bytes */
static std::string persist_in_new_state(const char* code)
{
  lua_State* A = new_state();
  std::string blob = eval(A, code);
  lua_close(A);
  return blob;
}

/* restores 'blob' as global 'v' in a fresh state after running 'prelude' */
static lua_State* restore_in_new_state(const std::string& blob, const char* code)
{
  lua_State* B = new_state();
  push_string(B, blob);
  lua_setglobal(B, "blob");
  CHECK(luaL_dostring(B, code) == LUA_OK);
  return B;
}

/* persist in one state, restore and resume in a separate one */
static void test_thread_across_states()
{
  std::string blob = persist_in_new_state(
    "co = coroutine.create(function() local p = print; coroutine.yield(); p('hi', 42) end)\n"
    "coroutine.resume(co)\n"
    "return require('styx').persist({[_ENV]='env', [print]='print', [coroutine.yield]='yield'}, co)");
  CHECK(!blob.empty());

  lua_State* B = restore_in_new_state(blob,
    "perms = {env = _ENV, print = function(...) got = table.concat({...}, ',') end, yield = coroutine.yield}\n"
    "co = require('styx').unpersist(perms, blob)\n"
    "ok = coroutine.resume(co)\n"
    "status = coroutine.status(co)");
  CHECK(lua_getglobal(B, "ok") == LUA_TBOOLEAN && lua_toboolean(B, -1));
  CHECK(lua_getglobal(B, "got") == LUA_TSTRING && lua_tolstring(B, -1) == "hi,42");
  CHECK(lua_getglobal(B, "status") == LUA_TSTRING && lua_tolstring(B, -1) == "dead");
  lua_close(B);
}

/* a table graph with cycles and shared parts, persisted in one state */
static void test_tables_across_states()
{
  std::string blob = persist_in_new_state(
    "local shared = {'s'}\n"
    "local t = {1, 2, 3, name = 'n', a = shared, b = shared, [2.5] = true}\n"
    "t.self = t\n"
    "setmetatable(t, {__index = {extra = 'e'}})\n"
    "return require('styx').persist(t)");
  CHECK(!blob.empty());
  lua_State* B = restore_in_new_state(blob, "t = require('styx').unpersist(blob)");
  CHECK(eval(B, "return tostring(t.self == t and t.a == t.b and t.a[1] == 's')") == "true");
  CHECK(eval(B, "return #t .. t.name .. tostring(t[2.5]) .. t.extra") == "3ntruee");
  lua_close(B);
}

/* closures sharing an upvalue, persisted in one state, run in another */
static void test_closures_across_states()
{
  std::string blob = persist_in_new_state(
    "local function counter()\n"
    "  local n = 10\n"
    "  return {inc = function(k) n = n + k; return n end, get = function() return n end}\n"
    "end\n"
    "local c = counter(); c.inc(5)\n"
    "return require('styx').persist(c)");
  lua_State* B = restore_in_new_state(blob, "c = require('styx').unpersist(blob)");
  CHECK(eval(B, "return c.get()") == "15");
  CHECK(eval(B, "c.inc(1); return c.get()") == "16");
  lua_close(B);
}

/* every proper prefix of a stream is an error, and the state stays usable */
static void test_truncation_from_cpp()
{
  lua_State* L = new_state();
  std::string blob = eval(L,
    "local co = coroutine.create(function(a) local t = {a, 'x'}; coroutine.yield(t); return a end)\n"
    "coroutine.resume(co, 7)\n"
    "back = {_ENV, coroutine.yield}\n"
    "return require('styx').persist({[_ENV] = 1, [coroutine.yield] = 2}, {co, {1, 2, {3}}, 'str'})");
  CHECK(!blob.empty());
  int rejected = 0;
  for (size_t n = 0; n < blob.size(); n++)
  {
    lua_getglobal(L, "back");
    push_string(L, blob.substr(0, n));
    if (call_styx(L, "unpersist", 2, 1) != LUA_OK)
      rejected++;
    lua_pop(L, 1);
  }
  CHECK(rejected == static_cast<int>(blob.size()));
  CHECK(lua_gettop(L) == 0);
  CHECK(eval(L, "collectgarbage(); return 'alive'") == "alive");
  lua_close(L);
}

static void run_cpp_tests()
{
  struct TestEntry
  {
    const char* name;
    void (*fn)();
  };
  const TestEntry tests[] = {
      {"requirable_not_global", test_requirable_not_global},
      {"api_surface", test_api_surface},
      {"primitives_roundtrip", test_primitives_roundtrip},
      {"string_bytes", test_string_bytes},
      {"stack_balance", test_stack_balance},
      {"thread_across_states", test_thread_across_states},
      {"tables_across_states", test_tables_across_states},
      {"closures_across_states", test_closures_across_states},
      {"truncation_from_cpp", test_truncation_from_cpp},
  };
  std::printf("  styx_tests.cpp\n");
  for (const auto& t : tests)
  {
    g_current = t.name;
    g_test_failures = 0;
    t.fn();
    if (g_test_failures == 0)
      std::printf("    %s ... OK\n", t.name);
    else
      std::printf("    %s ... FAIL\n", t.name);
  }
}

/* GC mode a script runs under: name and the code that selects it */
struct GcMode
{
  const char* name;
  const char* setup;
};

static const GcMode gc_modes[] = {
    {"incremental", "collectgarbage('incremental')"},
    {"generational", "collectgarbage('generational')"},
    {"constant", "collectgarbage('incremental'); collectgarbage('param', 'pause', 0);"
                 "collectgarbage('param', 'stepsize', 64)"},
};

/* runs one script in a fresh state; returns false if it did not complete */
static bool run_script(const std::filesystem::path& path, const GcMode& mode,
                       bool verbose, int* cases, int* failures)
{
  lua_State* L = new_state();
  eval(L, "package.path = './support/?.lua;' .. package.path");
  lua_pushstring(L, mode.name);
  lua_setglobal(L, "STYX_MODE");
  lua_pushboolean(L, verbose);
  lua_setglobal(L, "STYX_VERBOSE");
  eval(L, mode.setup);
  bool ok = luaL_dofile(L, path.string().c_str()) == LUA_OK;
  if (!ok)
    std::printf("    FAIL [%s] script error: %s\n", mode.name, to_string(L, -1).c_str());
  if (lua_getglobal(L, "STYX_RESULT") == LUA_TTABLE)
  {
    lua_getfield(L, -1, "cases");
    lua_getfield(L, -2, "failures");
    *cases = static_cast<int>(lua_tointeger(L, -2));
    *failures = static_cast<int>(lua_tointeger(L, -1));
  }
  lua_close(L);
  return ok;
}

static void run_scripts()
{
  std::vector<std::filesystem::path> scripts;
  for (const auto& e : std::filesystem::directory_iterator("."))
    if (e.path().extension() == ".lua")
      scripts.push_back(e.path());
  std::sort(scripts.begin(), scripts.end());
  for (const auto& s : scripts)
  {
    std::printf("  %s\n", s.filename().string().c_str());
    bool first = true;
    for (const auto& mode : gc_modes)
    {
      int cases = 0, failures = 0;
      bool ok = run_script(s, mode, first, &cases, &failures);
      g_checks += cases + 1;
      g_failures += failures + (ok ? 0 : 1);
      if (!first && ok && failures == 0)
        std::printf("    (%s GC: %d cases OK)\n", mode.name, cases);
      first = false;
    }
  }
}

int main()
{
  std::printf("\n\tStarting styx tests\n\n");
  run_cpp_tests();
  run_scripts();
  if (g_failures == 0)
    std::printf("\n\tOK - styx: %d checks, 0 failures\n\n", g_checks);
  else
    std::printf("\n\tFAILED - styx: %d checks, %d failures\n\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
