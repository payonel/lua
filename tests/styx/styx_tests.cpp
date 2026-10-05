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

/* light userdata are restored through the permanents table */
static void test_light_userdata()
{
  static int here, there;
  lua_State* L = new_state();
  lua_pushlightuserdata(L, &here);
  CHECK(call_styx(L, "persist", 1, 1) == LUA_ERRRUN);
  CHECK(to_string(L, -1).find("persisting a userdata value is not supported yet") != std::string::npos);
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushlightuserdata(L, &here);
  lua_pushstring(L, "here");
  lua_rawset(L, -3);
  lua_pushlightuserdata(L, &here);
  CHECK(call_styx(L, "persist", 2, 1) == LUA_OK);
  lua_newtable(L);
  lua_pushlightuserdata(L, &there);
  lua_setfield(L, -2, "here");
  lua_insert(L, -2);
  CHECK(call_styx(L, "unpersist", 2, 1) == LUA_OK);
  CHECK(lua_touserdata(L, -1) == &there);
  lua_close(L);
}

/*
** Host side persist (luaL_persistvalue, luaL_persiststate): called by the
** host on a state that is not running; it allocates nothing in the state.
*/

struct StringWriter : lua_Writer
{
  std::string out;
  int stop_after = -1; /* calls before reporting failure (-1: never) */
  int write(lua_State*, std::optional<byte_view> b) override
  {
    if (stop_after == 0)
      return 1;
    if (stop_after > 0)
      stop_after--;
    if (b.has_value())
      out.append(b->data(), b->size());
    return 0;
  }
};

/* counts requests to grow memory and can deny them (frees always pass) */
struct CountingAlloc : lua_Alloc
{
  long grows = 0;
  bool deny = false;
  size_t cap = 0;   /* when non zero, deny growth past this many bytes */
  size_t used = 0;
  bool permit(size_t osize, size_t nsize, int) override
  {
    if (nsize > osize)
    {
      grows++;
      if (deny || (cap != 0 && used + (nsize - osize) > cap))
        return false;
    }
    used = used + nsize - osize;
    return true;
  }
};

static lua_State* new_counted_state(CountingAlloc* a)
{
  lua_State* L = lua_newstate(a, 0);
  luaL_openlibs(L);
  return L;
}

/* runs 'code', which returns a value and its permanents table */
static bool push_value_and_perms(lua_State* L, const char* code)
{
  if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 2, 0) != LUA_OK)
  {
    std::printf("      error: %s\n", to_string(L, -1).c_str());
    lua_settop(L, 0);
    return false;
  }
  return true;
}

/* value at -2 and perms at -1: the script path's bytes */
static std::string script_persist(lua_State* L)
{
  lua_pushvalue(L, -1);
  lua_pushvalue(L, -3);
  if (call_styx(L, "persist", 2, 1) != LUA_OK)
  {
    std::printf("      script persist error: %s\n", to_string(L, -1).c_str());
    lua_pop(L, 1);
    return "";
  }
  std::string r = to_string(L, -1);
  lua_pop(L, 1);
  return r;
}

/* sets _PERMS: every C function and userdata reachable from _G, the
   registry and the string metatable, named by its path */
static const char* const build_perms =
  "local perms = {} local seen = {} "
  "local function walk(t, name) if seen[t] then return end seen[t] = true "
  "for k, v in pairs(t) do local n = name .. '.' .. tostring(k) "
  "if type(v) == 'function' or type(v) == 'userdata' then perms[v] = n "
  "elseif type(v) == 'table' then walk(v, n) end end "
  "local mt = getmetatable(t) if mt then walk(mt, name .. '<mt>') end end "
  "walk(_G, '_G') walk(debug.getregistry(), 'R') walk(getmetatable(''), 'smt') "
  "_PERMS = perms return 'ok'";

static const char* const graph_samples[] = {
  "return {1, 2.5, -0.0, true, false, 'str', ('long'):rep(40)}, {}",
  "local t = {a = 'x', [10] = 3.5, nested = {deep = {true}}} t.self = t "
  "return setmetatable(t, {__index = {k = 1}}), {}",
  "local t = {1, nil, 3} t[7] = 7 local k = {} t[k] = k return t, {}",
  "local function counter() local n = 0 "
  "return function() n = n + 1 return n end, function() return n end end "
  "local inc, get = counter() inc() return {inc, get, counter()}, {}",
  "local co = coroutine.create(function(a) local n = a "
  "coroutine.yield(function() n = n + 1 return n end) return n end) "
  "local _, f = coroutine.resume(co, 5) "
  "return {co, f}, {[_ENV] = 'env', [coroutine.yield] = 'yield'}",
  "local C = {__close = function() end} "
  "local co = coroutine.create(function() local x <close> = setmetatable({}, C) "
  "coroutine.yield() end) coroutine.resume(co) "
  "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield'}",
  "local function mk(i) return function() return i end end "
  "local t = {} for i = 1, 5 do t[i] = mk(i) end return t, {}",
  "return {print, string.format, io.stdout}, "
  "{[print] = 'print', [string.format] = 'fmt', [io.stdout] = 1}",
  "return {setmetatable({1}, {__persist = true}), 'x'}, {}",
  "local t = {} for i = 1, 50 do t['k' .. i] = i end "
  "for i = 1, 50, 2 do t['k' .. i] = nil end return t, {}",
};

/* host persist of a value is byte for byte the script path's stream */
static void test_host_value_matches_script()
{
  lua_State* L = new_state();
  for (const char* code : graph_samples)
  {
    if (!push_value_and_perms(L, code))
    {
      CHECK(false);
      continue;
    }
    std::string script = script_persist(L);
    StringWriter w;
    HostString msg;
    int st = luaL_persistvalue(L, lua_gettop(L), lua_gettop(L) - 1, &w, &msg);
    if (st != LUA_OK)
      std::printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    CHECK(!script.empty() && w.out == script);
    lua_settop(L, 0);
  }
  lua_close(L);
}

/* host persist asks for no memory at all, even when nothing is permitted */
static void test_host_persist_allocates_nothing()
{
  CountingAlloc a;
  lua_State* L = new_counted_state(&a);
  CHECK(eval(L, build_perms) == "ok");
  CHECK(push_value_and_perms(L, graph_samples[4]));
  lua_getglobal(L, "_PERMS");
  a.grows = 0;
  a.deny = true;
  StringWriter w1, w2;
  HostString msg;
  CHECK(luaL_persistvalue(L, 2, 1, &w1, &msg) == LUA_OK);
  CHECK(luaL_persiststate(L, 3, &w2, &msg) == LUA_OK);
  a.deny = false;
  CHECK(a.grows == 0);
  CHECK(!w1.out.empty() && !w2.out.empty());
  lua_close(L);
}

/* a state whose scripts have run out of memory can still be persisted */
static void test_host_persist_at_memory_cap()
{
  CountingAlloc a;
  lua_State* L = new_counted_state(&a);
  CHECK(eval(L, build_perms) == "ok");
  lua_getglobal(L, "_PERMS");  /* the permanents are ready before the cap */
  a.cap = a.used + 256 * 1024;
  CHECK(eval(L,
    "hog = {} "
    "local ok, err = pcall(function() for i = 1, 1e9 do hog[i] = ('x'):rep(100) .. i end end) "
    "return tostring(err)").find("not enough memory") != std::string::npos);
  StringWriter w;
  HostString msg;
  a.grows = 0;
  int st = luaL_persiststate(L, 1, &w, &msg);
  if (st != LUA_OK)
    std::printf("      host persist error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  CHECK(a.grows == 0);
  CHECK(w.out.size() > 100 * 1000);
  lua_close(L);
}

/* no Lua code runs: a __persist function is not called */
static void test_host_persist_runs_no_lua()
{
  lua_State* L = new_state();
  CHECK(push_value_and_perms(L,
    "calls = 0 "
    "local t = setmetatable({}, {__persist = function() calls = calls + 1 "
    "return function() return {} end end}) return {inner = t}, {}"));
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(L, 2, 1, &w, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()).find("is not supported yet in a host persist (at root.inner)") != std::string::npos);
  CHECK(eval(L, "return tostring(calls)") == "0");
  lua_close(L);
}

/* errors carry the path, and the stack is left as it was */
static void test_host_persist_errors()
{
  lua_State* L = new_state();
  CHECK(push_value_and_perms(L, "return {a = {file = io.stdout}}, {}"));
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(L, 2, 1, &w, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()) == "persisting a userdata value is not supported yet (at root.a.file)");
  CHECK(lua_gettop(L) == 2);
  lua_close(L);
}

/* no depth limit: the walk keeps its own stack */
static void test_host_persist_deep()
{
  lua_State* L = new_state();
  CHECK(push_value_and_perms(L,
    "local t = {} local cur = t for i = 1, 200000 do cur.n = {} cur = cur.n end "
    "cur.last = true return t, {}"));
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(L, 2, 1, &w, &msg) == LUA_OK);
  CHECK(w.out.size() > 200000);
  lua_close(L);
}

/* a writer that fails stops the persist */
static void test_host_persist_writer_stops()
{
  lua_State* L = new_state();
  CHECK(push_value_and_perms(L,
    "local t = {} for i = 1, 100000 do t[i] = i end return t, {}"));
  StringWriter w;
  w.stop_after = 1;
  HostString msg;
  CHECK(luaL_persistvalue(L, 2, 1, &w, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()).find("writer") != std::string::npos);
  lua_close(L);
}

static int call_host_persist(lua_State* L)
{
  StringWriter w;
  HostString msg;
  lua_pushinteger(L, luaL_persistvalue(L, 0, 1, &w, &msg));
  lua_pushstring(L, msg.to_os());
  return 2;
}

/* a state that is running Lua code is refused */
static void test_host_persist_running_state()
{
  lua_State* L = new_state();
  lua_pushcfunction(L, call_host_persist);
  lua_setglobal(L, "host_persist");
  CHECK(eval(L, "local st, msg = host_persist({}) return st .. ' ' .. msg")
        .find("the state is running") != std::string::npos);
  lua_close(L);
}

/* whole-state streams are deterministic and use their own root tag */
static void test_host_persist_state_stream()
{
  lua_State* L = new_state();
  CHECK(eval(L, "data = {list = {1, 2, 3}, name = 'state'} return 'ok'") == "ok");
  StringWriter w1, w2;
  HostString msg;
  CHECK(luaL_persiststate(L, 0, &w1, &msg) == LUA_ERRRUN);  /* C functions need perms */
  CHECK(std::string(msg.to_os()).find("cannot persist a C function") != std::string::npos);
  CHECK(eval(L, build_perms) == "ok");
  lua_settop(L, 0);
  lua_getglobal(L, "_PERMS");
  w1.out.clear();
  int st = luaL_persiststate(L, 1, &w1, &msg);
  if (st != LUA_OK)
    std::printf("      host persist error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  CHECK(luaL_persiststate(L, 1, &w2, &msg) == LUA_OK);
  CHECK(!w1.out.empty() && w1.out == w2.out);
  CHECK(w1.out.compare(0, 4, "Styx") == 0);
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
      {"light_userdata", test_light_userdata},
      {"host_value_matches_script", test_host_value_matches_script},
      {"host_persist_allocates_nothing", test_host_persist_allocates_nothing},
      {"host_persist_at_memory_cap", test_host_persist_at_memory_cap},
      {"host_persist_runs_no_lua", test_host_persist_runs_no_lua},
      {"host_persist_errors", test_host_persist_errors},
      {"host_persist_deep", test_host_persist_deep},
      {"host_persist_writer_stops", test_host_persist_writer_stops},
      {"host_persist_running_state", test_host_persist_running_state},
      {"host_persist_state_stream", test_host_persist_state_stream},
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
