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
#include <cstdlib>
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
/* sets _PERMS: every C function and userdata reachable from _G, the
   registry and the string metatable, named by its path; keys are visited
   in sorted order, so two states with the same libraries agree on names;
   _BACK is the inverse */
static const char* const build_perms =
  "local perms = {} local seen = {} "
  "local function upv(f) local _, x = debug.getupvalue(f, 1) return x end "
  "local function walk(t, name) if seen[t] then return end seen[t] = true "
  "local keys = {} for k in pairs(t) do keys[#keys + 1] = k end "
  "table.sort(keys, function(a, b) return tostring(a) < tostring(b) end) "
  "for _, k in ipairs(keys) do local v = t[k] local n = name .. '.' .. tostring(k) "
  "if (type(v) == 'function' and debug.getinfo(v, 'S').what == 'C' and type(upv(v)) ~= 'table') or type(v) == 'userdata' then "
  "if perms[v] == nil then perms[v] = n end "
  "elseif type(v) == 'table' then walk(v, n) end end "
  "local mt = getmetatable(t) if mt then walk(mt, name .. '<mt>') end end "
  "walk(getmetatable(''), 'smt') walk(_G, '_G') walk(debug.getregistry(), 'R') "
  "_PERMS = perms _BACK = {} for v, n in pairs(perms) do _BACK[n] = v end return 'ok'";

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
  "local co = coroutine.create(function() local function f() error('boom') end f() end) "
  "coroutine.resume(co) return co, {[error] = 'error', [_ENV] = 'env'}",
  "local function counter() local n = 0 "
  "return function() n = n + 1 return n end, function() return n end end "
  "local inc, get = counter() return {inc, get}, {[debug.upvalueid(inc, 1)] = 'count'}",
  "local log = {} local co = coroutine.create(function() for i = 1, 3 do coroutine.yield(i) end end) "
  "debug.sethook(co, function(e, l) log[#log + 1] = l end, 'l', 4) coroutine.resume(co) "
  "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield', [log] = 'log'}",
  "local t = {} for i = 1, 50 do t['k' .. i] = i end "
  "for i = 1, 50, 2 do t['k' .. i] = nil end return t, {}",
  "local mt = {__gc = function() end} local t = {} "
  "for i = 1, 4 do t[i] = setmetatable({i}, mt) end t[5] = {t[3], t[1]} return t, {}",
  "local t = {} for i = 1, 30 do t['d' .. i] = i end local gone = {} "
  "for i = 1, 30, 3 do t['d' .. i] = nil gone[#gone + 1] = 'd' .. i end "
  "collectgarbage() collectgarbage() return {t, gone}, {}",
  "local t, gone = {}, {} for i = 1, 40 do t[(i % 2 == 0) and {} or function() end] = i end "
  "for k, v in pairs(t) do if v % 3 == 0 then t[k] = nil gone[#gone + 1] = k end end "
  "collectgarbage() collectgarbage() return {t, gone}, {}",
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
  CHECK(eval(L, "local mt = {__gc = function() end} marked = {} "
                "for i = 1, 5 do marked[i] = setmetatable({}, mt) end "
                "dk = {} for i = 1, 20 do dk['k' .. i] = i end dkeys = {} "
                "for i = 1, 20, 2 do dk['k' .. i] = nil dkeys[#dkeys + 1] = 'k' .. i end "
                "collectgarbage() collectgarbage() "
                "for i = 1, 3 do setmetatable({}, mt) end return 'ok'") == "ok");
  CHECK(eval(L, build_perms) == "ok");
  CHECK(push_value_and_perms(L, "return {marked, dk, dkeys}, {}"));
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
  lua_settop(L, 0);
  CHECK(luaL_unpersistvalue(L, 0, byte_view(w.out.data(), w.out.size()), &msg) == LUA_OK);
  lua_setglobal(L, "r");
  CHECK(eval(L, "local n, cur = 0, r while cur.n do n = n + 1 cur = cur.n end "
                "return tostring(n == 200000 and cur.last)") == "true");
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

/*
** The host persists one child coroutine (not the state): what it captured
** from outside (a table, a callback, a userdata) is in the permanents, so
** a second state restores it connected to its own objects.
*/
static void test_host_persists_child_thread()
{
  lua_State* A = new_state();
  CHECK(push_value_and_perms(A,
    "local config = {prefix = 'a:'} "
    "local callback = function(s) end "
    "local co = coroutine.create(function() "
    "  local out = io.stdout "
    "  while true do local m = coroutine.yield() callback(config.prefix .. m, out) end "
    "end) "
    "coroutine.resume(co) "
    "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield', [config] = 'config', "
    "            [callback] = 'callback', [io.stdout] = 'out'}"));
  StringWriter w;
  HostString msg;
  int st = luaL_persistvalue(A, 2, 1, &w, &msg);
  if (st != LUA_OK)
    std::printf("      host persist error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  lua_close(A);

  lua_State* B = restore_in_new_state(w.out,
    "got = {} "
    "local cfg = {prefix = 'b:'} "
    "local cb = function(s, out) got[#got + 1] = s; got.out = out end "
    "local back = {env = _ENV, yield = coroutine.yield, config = cfg, callback = cb, out = io.stderr} "
    "local co = require('styx').unpersist(back, blob) "
    "coroutine.resume(co, 'one') cfg.prefix = 'c:' coroutine.resume(co, 'two') "
    "ok = got[1] == 'b:one' and got[2] == 'c:two' and got.out == io.stderr");
  CHECK(eval(B, "return tostring(ok)") == "true");
  lua_close(B);
}

/*
** Host side restore (luaL_unpersiststate, luaL_unpersistvalue): the host
** creates a state (with the persisted seed), opens what it provides, and
** restores into it; what is restored is charged through its allocator.
*/

/* a state made for 'blob' (luaL_newstatefor), with the libraries open */
static lua_State* new_state_for(CountingAlloc* a, const std::string& blob)
{
  lua_State* L = luaL_newstatefor(a, byte_view(blob.data(), blob.size()));
  luaL_openlibs(L);
  return L;
}

static int restore_state(lua_State* L, const std::string& blob, HostString* msg)
{
  eval(L, build_perms);
  lua_getglobal(L, "_BACK");
  int st = luaL_unpersiststate(L, lua_gettop(L), byte_view(blob.data(), blob.size()), msg);
  lua_pop(L, 1);
  return st;
}

/* a running program, persisted whole by the host and restored in a new state */
static const char* const program =
  "data = {list = {1, 2, 3}, name = 'state'} "
  "local function counter() local n = 0 "
  "  return function() n = n + 1 return n end, function() return n end end "
  "inc, get = counter() inc() "
  "local C = {__close = function(v) closed = (closed or '') .. v.name end} "
  "worker = coroutine.create(function(a) "
  "  local total = a "
  "  local res <close> = setmetatable({name = 'res'}, C) "
  "  peek = function() return total end "
  "  while true do total = total + coroutine.yield(total) end "
  "end) "
  "coroutine.resume(worker, 10) "
  "getmetatable('').__index.shout = function(s) return s:upper() .. '!' end "
  "debug.setmetatable(0, {__index = {double = function(n) return n * 2 end}}) "
  "main_ref = coroutine.running() "
  "return 'ok'";

static void test_host_state_roundtrip()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, program) == "ok");
  CHECK(eval(A, build_perms) == "ok");
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, 1, &w, &msg) == LUA_OK);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, w.out);
  int st = restore_state(B, w.out, &msg);
  if (st != LUA_OK)
    std::printf("      host restore error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  CHECK(lua_gettop(B) == 0);
  CHECK(eval(B, "return data.name .. #data.list") == "state3");
  CHECK(eval(B, "inc() return tostring(get())") == "2");
  CHECK(eval(B, "local _, t = coroutine.resume(worker, 5) return tostring(t) .. ',' .. peek()") == "15,15");
  CHECK(eval(B, "return ('hey'):shout()") == "HEY!");
  CHECK(eval(B, "return tostring((21):double())") == "42");
  CHECK(eval(B, "return tostring(main_ref == coroutine.running() and debug.getregistry()[3] == main_ref)") == "true");
  CHECK(eval(B, "coroutine.close(worker) return closed") == "res");
  CHECK(eval(B, "return tostring(print == _BACK and true or type(print))") == "function");
  lua_close(B);
}

/* a state persisted at its memory cap is restored and runs on */
static void test_host_restore_of_capped_state()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  lua_getglobal(A, "_PERMS");
  a1.cap = a1.used + 256 * 1024;
  CHECK(eval(A,
    "hog = {} "
    "local ok, err = pcall(function() for i = 1, 1e9 do hog[i] = ('x'):rep(100) .. i end end) "
    "return tostring(err)").find("not enough memory") != std::string::npos);
  int count_a = lua_gc(A, LUA_GCCOUNT, 0, 0, 0);
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, 1, &w, &msg) == LUA_OK);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, w.out);
  CHECK(restore_state(B, w.out, &msg) == LUA_OK);
  lua_gc(B, LUA_GCCOLLECT, 0, 0, 0);
  int count_b = lua_gc(B, LUA_GCCOUNT, 0, 0, 0);
  CHECK(count_b > count_a / 2 && count_b < count_a * 2);
  CHECK(eval(B, "return tostring(#hog > 100 and hog[1] == ('x'):rep(100) .. 1)") == "true");
  /* the restored program runs under the same cap again */
  a2.cap = a2.used + 64 * 1024;
  CHECK(eval(B,
    "local ok, err = pcall(function() for i = 1, 1e9 do hog[#hog + 1] = ('y'):rep(100) .. i end end) "
    "return tostring(err)").find("not enough memory") != std::string::npos);
  a2.cap = 0;
  lua_close(B);
}

/* a restore that runs out of memory fails cleanly and leaves the state as it was */
static void test_host_restore_out_of_memory()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, "big = {} for i = 1, 20000 do big[i] = 'item' .. i end return 'ok'") == "ok");
  CHECK(eval(A, build_perms) == "ok");
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, 1, &w, &msg) == LUA_OK);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, w.out);
  CHECK(eval(B, "mine = 'still here' return 'ok'") == "ok");
  CHECK(eval(B, build_perms) == "ok");
  lua_getglobal(B, "_BACK");
  a2.cap = a2.used + 64 * 1024;
  int st = luaL_unpersiststate(B, 1, byte_view(w.out.data(), w.out.size()), &msg);
  a2.cap = 0;
  CHECK(st != LUA_OK);
  CHECK(std::string(msg.to_os()).find("not enough memory") != std::string::npos);
  CHECK(lua_gettop(B) == 1);
  lua_settop(B, 0);
  CHECK(eval(B, "return mine") == "still here");
  CHECK(eval(B, "return tostring(big)") == "nil");
  lua_close(B);
}

/* values: host restore takes the inverse permanents and may restore the main thread */
static void test_host_value_restore()
{
  lua_State* A = new_state();
  CHECK(push_value_and_perms(A,
    "local t = {list = {1, 2}, f = function() return 7 end} t.self = t "
    "return {t, coroutine.running(), print}, {[print] = 'print'}"));
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(A, 2, 1, &w, &msg) == LUA_OK);
  lua_close(A);

  lua_State* B = new_state();
  CHECK(eval(B, "_BACK = {print = tostring} return 'ok'") == "ok");
  lua_getglobal(B, "_BACK");
  CHECK(luaL_unpersistvalue(B, 1, byte_view(w.out.data(), w.out.size()), &msg) == LUA_OK);
  CHECK(lua_gettop(B) == 2 && lua_type(B, 2) == LUA_TTABLE);
  lua_setglobal(B, "v");
  lua_settop(B, 0);
  CHECK(eval(B, "return tostring(v[1].self == v[1] and v[1].f() == 7 and #v[1].list == 2)") == "true");
  CHECK(eval(B, "return tostring(v[2] == coroutine.running() and v[3] == tostring)") == "true");
  lua_close(B);
}

/* the formats keep apart: scripts cannot read state streams, values are not states */
static void test_host_stream_kinds()
{
  lua_State* L = new_state();
  CHECK(eval(L, build_perms) == "ok");
  lua_getglobal(L, "_PERMS");
  StringWriter ws;
  HostString msg;
  CHECK(luaL_persiststate(L, 1, &ws, &msg) == LUA_OK);
  CountingAlloc a;
  lua_State* made = luaL_newstatefor(&a, byte_view(ws.out.data(), ws.out.size()));
  CHECK(made != nullptr);
  lua_close(made);
  push_string(L, ws.out);
  CHECK(call_styx(L, "unpersist", 1, 1) == LUA_ERRRUN);
  lua_settop(L, 0);
  std::string value = persist_in_new_state("return require('styx').persist({1, 2})");
  made = luaL_newstatefor(&a, byte_view(value.data(), value.size()));
  CHECK(made != nullptr);
  lua_close(made);
  CHECK(luaL_newstatefor(&a, byte_view("garbage", 7)) == nullptr);
  CHECK(luaL_unpersiststate(L, 0, byte_view(value.data(), value.size()), &msg) != LUA_OK);
  CHECK(luaL_unpersistvalue(L, 0, byte_view(ws.out.data(), ws.out.size()), &msg) != LUA_OK);
  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

static void external_hook(lua_State*, lua_Debug*) {}

/* a hook set from C (not by the debug library) is not supported yet */
static void test_external_hook_not_supported()
{
  lua_State* L = new_state();
  CHECK(eval(L, "co = coroutine.create(function() coroutine.yield() end) coroutine.resume(co) return 'ok'") == "ok");
  lua_getglobal(L, "co");
  lua_sethook(lua_tothread(L, -1), external_hook, LUA_MASKLINE, 0);
  lua_pop(L, 1);
  CHECK(eval(L, "local ok, err = pcall(require('styx').persist, "
                "{[_ENV] = 'env', [coroutine.yield] = 'y'}, {co}) return err")
        == "a hook not set by the debug library is not supported yet (at root[1])");
  CHECK(eval(L, "_P = {[_ENV] = 'env', [coroutine.yield] = 'y'} return 'ok'") == "ok");
  lua_getglobal(L, "_P");
  lua_getglobal(L, "co");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(L, 1, 2, &w, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()) == "a hook not set by the debug library is not supported yet (at root)");
  lua_close(L);
}

/*
** Table order across states: restored with the persisting state's seed, a
** table iterates in the same order; with another seed its entries are all
** there (inserted again, in another order).
*/
static const char* const churn =
  "math.randomseed(42) t = {} "
  "for i = 1, 200 do t['key' .. math.random(2000)] = i end "
  "for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end "
  "for i = 1, 50 do t['new' .. math.random(2000)] = i end "
  "order = {} for k in pairs(t) do order[#order + 1] = k end "
  "order = table.concat(order, ' ') return 'ok'";

static void test_table_order_across_states()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 1234);
  luaL_openlibs(A);
  CHECK(eval(A, churn) == "ok");
  std::string expected = eval(A, "return order");
  std::string script_blob = eval(A, "return require('styx').persist(t)");
  lua_getglobal(A, "t");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(A, 0, lua_gettop(A), &w, &msg) == LUA_OK);
  lua_close(A);

  for (const std::string* blob : {&w.out, &script_blob})
  {
    CountingAlloc a2;
    lua_State* B = luaL_newstatefor(&a2, byte_view(blob->data(), blob->size()));
    luaL_openlibs(B);
    CHECK(luaL_unpersistvalue(B, 0, byte_view(blob->data(), blob->size()), &msg) == LUA_OK);
    lua_setglobal(B, "t");
    CHECK(eval(B, "local o = {} for k in pairs(t) do o[#o + 1] = k end return table.concat(o, ' ')") == expected);
    lua_close(B);

    CountingAlloc a3;
    lua_State* C = lua_newstate(&a3, 1235); /* another seed than the stream's 1234 */
    luaL_openlibs(C);
    CHECK(luaL_unpersistvalue(C, 0, byte_view(blob->data(), blob->size()), &msg) == LUA_OK);
    lua_setglobal(C, "t");
    push_string(C, expected);
    lua_setglobal(C, "expected");
    CHECK(eval(C, "local n = 0 for k, v in pairs(t) do n = n + 1 end "
                  "local m = 0 for k in expected:gmatch('%S+') do assert(t[k] ~= nil, k) m = m + 1 end "
                  "return tostring(n == m)") == "true");
    lua_close(C);
  }
}


/*
** Finalization and the collector in a whole state (ADR-28): objects marked
** for finalization are part of the state until their finalizers run, so
** garbage waiting for its finalizer, and finalizers already due, run after
** a restore, once each, in the original's order; the collector's mode,
** parameters and running state are restored.
*/

static std::string persist_state(lua_State* A)
{
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  int st = luaL_persiststate(A, lua_gettop(A), &w, &msg);
  if (st != LUA_OK)
    std::printf("      host persist error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  lua_pop(A, 1);
  return w.out;
}

static lua_State* restored_state(CountingAlloc* a, const std::string& blob)
{
  lua_State* B = new_state_for(a, blob);
  HostString msg;
  int st = restore_state(B, blob, &msg);
  if (st != LUA_OK)
    std::printf("      host restore error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  return B;
}

static void test_host_state_marked_garbage()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "collectgarbage('stop') finalized = {} "
                "local mt = {__gc = function(o) finalized[#finalized + 1] = o.name end} "
                "for i = 1, 5 do setmetatable({name = 'g' .. i}, mt) end "
                "kept = setmetatable({name = 'kept'}, mt) "
                "collectgarbage('restart') return 'ok'") == "ok");
  std::string blob = persist_state(A);
  std::string order = eval(A, "collectgarbage() collectgarbage() return table.concat(finalized, ' ')");
  CHECK(order == "g5 g4 g3 g2 g1");
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = restored_state(&a2, blob);
  CHECK(eval(B, "return tostring(#finalized)") == "0");
  CHECK(eval(B, "collectgarbage() collectgarbage() return table.concat(finalized, ' ')") == order);
  CHECK(eval(B, "kept = nil collectgarbage() collectgarbage() return finalized[#finalized]") == "kept");
  lua_close(B);
}

static void test_host_state_pending_finalizers()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "collectgarbage('incremental') count = 0 "
                "local mt = {__gc = function() count = count + 1 end} "
                "for i = 1, 3000 do setmetatable({}, mt) end return 'ok'") == "ok");
  /* step the collector until some finalizers ran and the others are due */
  int ran = 0;
  for (int i = 0; i < 100000 && ran == 0; i++)
  {
    lua_gc(A, LUA_GCSTEP, 0, 0, 0);
    lua_getglobal(A, "count");
    ran = static_cast<int>(lua_tointeger(A, -1));
    lua_pop(A, 1);
  }
  CHECK(ran > 0 && ran < 3000);
  std::string blob = persist_state(A);
  CHECK(eval(A, "collectgarbage() return tostring(count)") == "3000");
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = restored_state(&a2, blob);
  lua_getglobal(B, "count");
  CHECK(lua_tointeger(B, -1) >= ran);
  lua_pop(B, 1);
  CHECK(eval(B, "collectgarbage() collectgarbage() return tostring(count)") == "3000");
  lua_close(B);
}

static const char* const read_settings =
  "local p = {} for _, n in ipairs{'minormul', 'majorminor', 'minormajor', 'pause', 'stepmul', 'stepsize'} "
  "do p[#p + 1] = collectgarbage('param', n) end "
  "local running = collectgarbage('isrunning') "
  "local mode = collectgarbage('incremental') collectgarbage(mode) "
  "return table.concat(p, ' ') .. ' ' .. tostring(running) .. ' ' .. mode";

static void test_host_state_collector_settings()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "collectgarbage('generational') collectgarbage('param', 'minormul', 33) "
                "collectgarbage('param', 'pause', 150) collectgarbage('param', 'stepmul', 300) "
                "collectgarbage('stop') return 'ok'") == "ok");
  std::string settings = eval(A, read_settings);
  CHECK(settings.find("false generational") != std::string::npos);
  std::string blob = persist_state(A);
  lua_close(A);
  CountingAlloc a2;
  lua_State* B = restored_state(&a2, blob);
  CHECK(eval(B, read_settings) == settings);
  lua_close(B);

  /* and back: a running incremental state restored over a stopped one */
  CountingAlloc a3;
  lua_State* C = new_counted_state(&a3);
  CHECK(eval(C, build_perms) == "ok");
  std::string defaults = eval(C, read_settings);
  CHECK(defaults.find("true incremental") != std::string::npos);
  blob = persist_state(C);
  lua_close(C);
  CountingAlloc a4;
  lua_State* D = new_state_for(&a4, blob);
  CHECK(eval(D, "collectgarbage('generational') collectgarbage('param', 'pause', 400) "
                "collectgarbage('stop') return 'ok'") == "ok");
  HostString msg;
  CHECK(restore_state(D, blob, &msg) == LUA_OK);
  CHECK(eval(D, read_settings) == defaults);
  lua_close(D);
}

static int noop(lua_State*) { return 0; }

/* garbage with a finalizer is part of the state: a userdata is not supported yet */
static void test_host_state_garbage_userdata()
{
  lua_State* A = new_state();
  CHECK(eval(A, build_perms) == "ok");
  lua_newuserdatauv(A, 8, 0);
  lua_createtable(A, 0, 1);
  lua_pushcfunction(A, noop);
  lua_setfield(A, -2, "__gc");
  lua_setmetatable(A, -2);
  lua_pop(A, 1);
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, 1, &w, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()) == "persisting a userdata value is not supported yet (at finalizer list)");
  lua_gc(A, LUA_GCCOLLECT, 0, 0, 0); /* finalized: no longer marked */
  StringWriter w2;
  CHECK(luaL_persiststate(A, 1, &w2, &msg) == LUA_OK);
  lua_close(A);
}

/* next from a deleted key: with the seed kept, as before; with another, the contents */
static void test_dead_keys_across_states()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 77);
  luaL_openlibs(A);
  CHECK(eval(A, "t = {} for i = 1, 40 do t['k' .. i] = i end gone = {} "
                "for i = 1, 40, 3 do t['k' .. i] = nil gone[#gone + 1] = 'k' .. i end "
                "collectgarbage() collectgarbage() "
                "local e = {} for i, k in ipairs(gone) do e[i] = tostring(next(t, k)) end "
                "expected = table.concat(e, ' ') return 'ok'") == "ok");
  std::string expected = eval(A, "return expected");
  std::string blob = eval(A, "return require('styx').persist({t = t, gone = gone})");
  lua_close(A);
  HostString msg;
  for (unsigned int seed : {77u, 78u})
  {
    CountingAlloc a2;
    lua_State* B = lua_newstate(&a2, seed);
    luaL_openlibs(B);
    CHECK(luaL_unpersistvalue(B, 0, byte_view(blob.data(), blob.size()), &msg) == LUA_OK);
    lua_setglobal(B, "v");
    CHECK(eval(B, "local n = 0 for _ in pairs(v.t) do n = n + 1 end return tostring(n == 26 and v.t.k2 == 2)") == "true");
    if (seed == 77)
      CHECK(eval(B, "local e = {} for i, k in ipairs(v.gone) do e[i] = tostring(next(v.t, k)) end "
                    "return table.concat(e, ' ')") == expected);
    lua_close(B);
  }
}


/*
** Object keys across states (ADR-30): a table, function or thread keeps the
** hash it was given when created, so a restored table with object keys
** iterates in the same order in a state with the same seed; keys that are
** the restoring state's own objects (permanents) keep it when they were
** made the same way (the libraries, with the same seed).
*/
static const char* const object_keys_program =
  "math.randomseed(7) t, names = {}, {} "
  "for i = 1, 200 do local r = math.random(3) "
  "  local k = (r == 1) and {} or (r == 2) and function() return i end or coroutine.create(print) "
  "  t[k] = i names[k] = 'o' .. i end "
  "for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end "
  "collectgarbage() collectgarbage() "
  "function order(tt, nn) local o = {} for k in pairs(tt) do o[#o + 1] = nn[k] or '?' end "
  "  return table.concat(o, ' ') end "
  "return 'ok'";

static void test_object_key_order_across_states()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 4321);
  luaL_openlibs(A);
  CHECK(eval(A, object_keys_program) == "ok");
  std::string expected = eval(A, "return order(t, names)");
  CHECK(eval(A, "_P = {[print] = 'print'} _V = {t = t, names = names} return 'ok'") == "ok");
  std::string script_blob = eval(A, "return require('styx').persist(_P, _V)");
  lua_getglobal(A, "_P");
  lua_getglobal(A, "_V");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persistvalue(A, 1, 2, &w, &msg) == LUA_OK);
  CHECK(w.out == script_blob);
  lua_close(A);

  for (unsigned int seed : {4321u, 4322u})
  {
    CountingAlloc a2;
    lua_State* B = lua_newstate(&a2, seed);
    luaL_openlibs(B);
    CHECK(eval(B, "_BACK = {print = print} "
                  "function order(tt, nn) local o = {} for k in pairs(tt) do o[#o + 1] = nn[k] or '?' end "
                  "  return table.concat(o, ' ') end return 'ok'") == "ok");
    lua_getglobal(B, "_BACK");
    CHECK(luaL_unpersistvalue(B, 1, byte_view(script_blob.data(), script_blob.size()), &msg) == LUA_OK);
    lua_setglobal(B, "v");
    lua_settop(B, 0);
    push_string(B, expected);
    lua_setglobal(B, "expected");
    if (seed == 4321u)
      CHECK(eval(B, "return order(v.t, v.names)") == expected);
    /* with another seed: the same entries */
    CHECK(eval(B, "local n = 0 for k, i in pairs(v.t) do assert(v.names[k] == 'o' .. i) n = n + 1 end "
                  "local m = 0 for _ in expected:gmatch('%S+') do m = m + 1 end return tostring(n == m)")
          == "true");
    lua_close(B);
  }
}

/* library objects as keys: made the same way with the same seed, they hash the same */
static void test_library_keys_across_states()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 99);
  luaL_openlibs(A);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "t = {} local i = 0 "
                "for _, k in ipairs{io.stdout, io.stderr, io.stdin, print, string.format, math.floor, "
                "                   coroutine.yield, table.insert} do i = i + 1 t[k] = i end "
                "for i = 1, 30 do t[{}] = 100 + i end "
                "local o = {} for k, v in pairs(t) do o[#o + 1] = v end expected = table.concat(o, ' ') "
                "return 'ok'") == "ok");
  std::string expected = eval(A, "return expected");
  lua_settop(A, 0);
  lua_getglobal(A, "_PERMS");
  lua_getglobal(A, "t");
  StringWriter w;
  HostString msg;
  int st = luaL_persistvalue(A, 1, 2, &w, &msg);
  if (st != LUA_OK)
    std::printf("      host persist error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, w.out);
  CHECK(eval(B, build_perms) == "ok");
  lua_getglobal(B, "_BACK");
  CHECK(luaL_unpersistvalue(B, 1, byte_view(w.out.data(), w.out.size()), &msg) == LUA_OK);
  lua_setglobal(B, "r");
  lua_settop(B, 0);
  CHECK(eval(B, "local o = {} for k, v in pairs(r) do o[#o + 1] = v end return table.concat(o, ' ')") == expected);
  CHECK(eval(B, "return tostring(r[io.stdout] == 1 and r[print] == 4 and r[table.insert] == 8)") == "true");
  lua_close(B);
}

/* a whole state goes on giving new objects the hashes the original would */
static void test_state_next_hash()
{
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "function make() local u = {} for i = 1, 60 do u[(i % 2 == 0) and {} or function() end] = i end "
                "  local o = {} for k, v in pairs(u) do o[#o + 1] = v end return table.concat(o, ' ') end "
                "return 'ok'") == "ok");
  std::string blob = persist_state(A);
  lua_getglobal(A, "make");
  CHECK(lua_pcall(A, 0, 1, 0) == LUA_OK);
  std::string expected = to_string(A, -1);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = restored_state(&a2, blob);
  lua_getglobal(B, "make");
  CHECK(lua_pcall(B, 0, 1, 0) == LUA_OK);
  CHECK(to_string(B, -1) == expected);
  lua_close(B);
}


/* a whole state is restored only into a state made for its stream */
static void test_state_restore_needs_its_state()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 31337);
  luaL_openlibs(A);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, "data = {x = 1, y = 2} return 'ok'") == "ok");
  std::string blob = persist_state(A);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = lua_newstate(&a2, 31338);
  luaL_openlibs(B);
  CHECK(eval(B, "mine = 'kept' return 'ok'") == "ok");
  HostString msg;
  CHECK(restore_state(B, blob, &msg) == LUA_ERRRUN);
  CHECK(std::string(msg.to_os()) ==
        "the state does not hash as the persisted one (create it with luaL_newstatefor)");
  CHECK(eval(B, "return mine") == "kept");
  lua_close(B);

  CountingAlloc a3;
  lua_State* C = new_state_for(&a3, blob);
  CHECK(restore_state(C, blob, &msg) == LUA_OK);
  CHECK(eval(C, "return tostring(data.x + data.y)") == "3");
  lua_close(C);
}

/*
** A VM persisted and restored anywhere goes on exactly as it would have:
** the original and a restored copy run the same code after the snapshot
** and must produce the same transcript (what is deterministic within one
** run: table order, object hashes, new keys, suspended loops; ADR-31).
*/
static const char* const vm_part1 =
  "P = {} "
  "P.s = {} for i = 1, 300 do P.s['key' .. (i * 7 % 500)] = i end "
  "for k in pairs(P.s) do if #k % 2 == 0 then P.s[k] = nil end end "
  "P.o, P.names = {}, {} "
  "for i = 1, 200 do local k = (i % 3 == 0) and {} or (i % 3 == 1) and function() return i end "
  "  or coroutine.create(print) P.o[k] = i P.names[k] = 'o' .. i end "
  "for k, v in pairs(P.o) do if v % 4 == 0 then P.o[k] = nil end end "
  "P.gone = {} for k, v in pairs(P.s) do if v % 5 == 0 then P.s[k] = nil P.gone[#P.gone + 1] = k end end "
  "for i = 1, 100 do local junk = 'later' .. i end "
  "P.walk_s = coroutine.create(function() for k, v in pairs(P.s) do coroutine.yield(k .. '=' .. v) end end) "
  "P.walk_o = coroutine.create(function() for k in pairs(P.o) do P.o[k] = nil coroutine.yield(P.names[k]) end end) "
  "for i = 1, 10 do coroutine.resume(P.walk_s) coroutine.resume(P.walk_o) end "
  "local n = 0 P.inc = function() n = n + 1 return n end P.get = function() return n end "
  "P.list = {} for i = 1, 20 do P.list[i] = i * 3 end P.list[25] = 'far' "
  "P.closer = coroutine.create(function() "
  "  local res <close> = setmetatable({}, {__close = function() P.closed = 'yes' end}) "
  "  coroutine.yield('opened') return 'done' end) "
  "coroutine.resume(P.closer) "
  "return 'ok'";

static const char* const vm_part2 =
  "local out = {} local function w(x) out[#out + 1] = tostring(x) end "
  "for i = 1, 20 do w(select(2, coroutine.resume(P.walk_s))) w(select(2, coroutine.resume(P.walk_o))) end "
  "for k in pairs(P.s) do w(k) end "
  "for k in pairs(P.o) do w(P.names[k]) end "
  "for _, k in ipairs(P.gone) do w(next(P.s, k)) end "
  "for i = 1, 100 do P.s['later' .. i] = i end "
  "for i = 1, 60 do local k = (i % 2 == 0) and {} or function() end P.o[k] = 1000 + i P.names[k] = 'n' .. i end "
  "for k in pairs(P.s) do w(k) end "
  "for k in pairs(P.o) do w(P.names[k]) end "
  "local t = {} for i = 1, 40 do t[{}] = i end for _, v in pairs(t) do w(v) end "
  "w(P.inc()) w(P.inc()) w(P.get()) "
  "w(#P.list) table.insert(P.list, 1, 'first') w(#P.list) w(table.remove(P.list)) "
  "local arr = {} for i = 1, 50 do arr[i] = (i * 37) % 101 end table.sort(arr) w(table.concat(arr, ',')) "
  "w(select(2, coroutine.resume(P.closer))) w(P.closed) "
  "w(select(2, pcall(function() local x = nil return x.field end))) "
  "w(string.format('%5.2f %d %q', 3.14159, 42, 'q')) "
  "for k in pairs(_G) do w(k) end for k in pairs(package.loaded) do w(k) end "
  "for k in pairs(string) do w(k) end for k in pairs(debug.getregistry()) do w(type(k)) end "
  "return table.concat(out, ' ')";

static void test_snapshot_continues_identically()
{
  CountingAlloc a1;
  lua_State* A = lua_newstate(&a1, 2024);
  luaL_openlibs(A);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, vm_part1) == "ok");
  std::string blob = persist_state(A);
  std::string expected = eval(A, vm_part2);
  CHECK(expected.size() > 1000);
  lua_close(A);

  CountingAlloc a2;
  lua_State* B = luaL_newstatefor(&a2, byte_view(blob.data(), blob.size()));
  CHECK(B != nullptr);
  luaL_openlibs(B);
  HostString msg;
  int st = restore_state(B, blob, &msg);
  if (st != LUA_OK)
    std::printf("      host restore error: %s\n", msg.to_os());
  CHECK(st == LUA_OK);
  std::string got = eval(B, vm_part2);
  if (got != expected)
  {
    size_t i = 0;
    while (i < got.size() && i < expected.size() && got[i] == expected[i])
      i++;
    std::printf("      first difference at %zu: '%s' vs '%s'\n", i, expected.substr(i, 40).c_str(),
                got.substr(i, 40).c_str());
  }
  CHECK(got == expected);
  lua_close(B);
}


/*
** Frozen threads in a whole state (ADR-34): a user program frozen by its
** budget at a point, persisted with the state and restored in a new one,
** goes on exactly as the original does. The cases are the library callbacks
** of tests/deep/libcases.lua (what the standard library calls back from C
** frames); each is frozen at every one of its budget points of 17 steps.
*/
static const char* const frozen_prelude = R"LUA(
cases = dofile(CASES_FILE)
busyn = 0
function busy() busyn = busyn + 1 for i = 1, 3 do end end
-- a deterministic text of results
local function show(v, seen)
  local t = type(v)
  if t == "string" then return string.format("%q", v) end
  if t == "table" then
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(keys) do parts[#parts + 1] = show(k) .. "=" .. show(v[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  if t == "function" or t == "thread" or t == "userdata" then return t end
  return tostring(v)
end
function render(...)
  local parts = {}
  for i = 1, select("#", ...) do parts[i] = show((select(i, ...))) end
  return table.concat(parts, " ")
end
function start(i, n, k)
  local c = cases[i]
  fcount = 0
  co = coroutine.create(function() return render(c.f(busy)) end)
  if n > 0 then
    debug.setbudget(co, n, function() fcount = fcount + 1 if fcount == k then debug.freeze() end end)
  end
  local ok, r = coroutine.resume(co)
  if not ok then result = "ERR " .. tostring(r) return "done" end
  if debug.isfrozen(co) then return "frozen" end
  result = r
  return "done"
end
function finish()
  while debug.isfrozen(co) do
    local ok, r = debug.thaw(co)
    if not ok then result = "ERR " .. tostring(r) return end
    if not debug.isfrozen(co) then result = r end
  end
end
)LUA";

static const char* g_cases_file = "../deep/libcases.lua";

static lua_State* frozen_state(CountingAlloc* a)
{
  lua_State* L = new_counted_state(a);
  CHECK(eval(L, build_perms) == "ok");
  CHECK(eval(L, (std::string("CASES_FILE = '") + g_cases_file + "'").c_str()) == "");
  CHECK(eval(L, frozen_prelude) == "");
  return L;
}

static void frozen_cases(int mincases, int minpoints)
{
  CountingAlloc a0;
  lua_State* P = frozen_state(&a0);
  int ncases = std::atoi(eval(P, "return #cases").c_str());
  CHECK(ncases >= mincases);
  int points = 0;
  for (int i = 1; i <= ncases; i++)
  {
    std::string plain = eval(P, ("start(" + std::to_string(i) + ", 0, 0) finish() return result").c_str());
    std::string name = eval(P, ("return cases[" + std::to_string(i) + "].name").c_str());
    CHECK(plain.rfind("ERR", 0) != 0);
    for (int k = 1; k < 400; k++)
    {
      CountingAlloc a1;
      lua_State* A = frozen_state(&a1);
      std::string st = eval(A, ("return start(" + std::to_string(i) + ", 17, " + std::to_string(k) + ")").c_str());
      if (st != "frozen")
      {
        lua_close(A);
        break;
      }
      points++;
      std::string blob = persist_state(A);
      if (blob.empty())
      { /* (it failed: said so; the first point of the case is enough) */
        std::printf("      case %d (%s), point %d: not persisted\n", i, name.c_str(), k);
        lua_close(A);
        break;
      }
      CountingAlloc a2;
      int before = g_test_failures;
      lua_State* B = restored_state(&a2, blob);
      if (g_test_failures != before)
        std::printf("      (case %d (%s), point %d)\n", i, name.c_str(), k);
      CHECK(eval(B, "if debug.isfrozen(co) then return 'yes' end return 'no'") == "yes");
      std::string ra = eval(A, "finish() return result");
      std::string rb = eval(B, "finish() return result");
      if (ra != plain || rb != plain || eval(A, "return busyn") != eval(B, "return busyn"))
        std::printf("      case %d (%s), point %d: plain [%s] original [%s] restored [%s] busy %s/%s\n", i, name.c_str(), k,
                    plain.c_str(), ra.c_str(), rb.c_str(), eval(A, "return busyn").c_str(), eval(B, "return busyn").c_str());
      CHECK(ra == plain);
      CHECK(rb == plain);
      CHECK(eval(A, "return busyn") == eval(B, "return busyn"));
      lua_close(A);
      lua_close(B);
    }
  }
  CHECK(points >= minpoints);
  lua_close(P);
}

static void test_frozen_library_cases()
{
  g_cases_file = "../deep/libcases.lua";
  frozen_cases(40, 500);
}

static void test_frozen_engine_cases()
{
  g_cases_file = "frozencases.lua";
  frozen_cases(8, 200);
}


/*
** Frozen threads with what the host provides (ADR-34): a continuation and a
** budget callback of its own persist when they have a name, and not before;
** and a restore that runs out of memory fails cleanly.
*/
static int host_k(lua_State* L, int status, lua_KContext ctx)
{
  CHECK(status == LUA_YIELD || status == LUA_OK);
  lua_pushinteger(L, static_cast<lua_Integer>(ctx));
  return 2; /* (the result of the call, and the context) */
}

static int host_callk(lua_State* L)
{
  lua_settop(L, 2);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_callk(L, 1, 1, 77, host_k);
  return host_k(L, LUA_OK, 77);
}

static int host_budget_calls = 0;
static void host_budget(lua_State*, lua_State* spender)
{
  host_budget_calls++;
  if (host_budget_calls == 2)
    lua_freeze(spender);
}

/* a state with the host's function 'callk' and the permanents */
static lua_State* host_state(CountingAlloc* a)
{
  lua_State* L = new_counted_state(a);
  lua_pushcfunction(L, host_callk);
  lua_setglobal(L, "callk");
  CHECK(eval(L, build_perms) == "ok");
  CHECK(eval(L, "CASES_FILE = 'frozencases.lua'") == "");
  CHECK(eval(L, frozen_prelude) == "");
  return L;
}

static const char* const host_program =
  "function work(n) local s = 0 for i = 1, n do s = s + i end return s end "
  "log = {} "
  "co = coroutine.create(function() local r, ctx = callk(work, 40) log[#log + 1] = r .. '/' .. ctx return r, ctx end) "
  "fcount = 0 "
  "debug.setbudget(co, 7, function() fcount = fcount + 1 if fcount == 3 then debug.freeze() end end) "
  "coroutine.resume(co) "
  "return debug.isfrozen(co) and 'frozen' or 'done'";

static void test_frozen_host_continuation()
{
  CountingAlloc a1;
  lua_State* A = host_state(&a1);
  CHECK(eval(A, host_program) == "frozen");
  /* the continuation has no name: the state is not persisted */
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, lua_gettop(A), &w, &msg) != LUA_OK);
  CHECK(std::string(msg.to_os()).find("a continuation that has no name") != std::string::npos);
  lua_pop(A, 1);
  /* named, it is */
  lua_registerk("styxtest.host_k", host_k);
  std::string blob = persist_state(A);
  CHECK(!blob.empty());
  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, blob);
  lua_pushcfunction(B, host_callk);
  lua_setglobal(B, "callk");
  HostString msg2;
  CHECK(restore_state(B, blob, &msg2) == LUA_OK);
  CHECK(eval(B, "if debug.isfrozen(co) then return 'yes' end return 'no'") == "yes");
  CHECK(eval(A, "finish() return table.concat(log, ',')") == "820/77");
  CHECK(eval(B, "finish() return table.concat(log, ',')") == "820/77");
  CHECK(eval(A, "return coroutine.status(co)") == "dead" && eval(B, "return coroutine.status(co)") == "dead");
  lua_close(A);
  lua_close(B);
}

static void test_frozen_host_budget_callback()
{
  CountingAlloc a1;
  lua_State* A = host_state(&a1);
  CHECK(eval(A, "co = coroutine.create(function() local s = 0 for i = 1, 60 do s = s + i end return s end) return 'ok'") == "ok");
  lua_getglobal(A, "co");
  lua_State* th = lua_tothread(A, -1);
  host_budget_calls = 0;
  lua_setbudget(th, 9, host_budget);
  lua_pop(A, 1);
  CHECK(eval(A, "coroutine.resume(co) return debug.isfrozen(co) and 'frozen' or 'done'") == "frozen");
  lua_getglobal(A, "_PERMS");
  StringWriter w;
  HostString msg;
  CHECK(luaL_persiststate(A, lua_gettop(A), &w, &msg) != LUA_OK);
  CHECK(std::string(msg.to_os()).find("a budget callback that has no name") != std::string::npos);
  lua_pop(A, 1);
  lua_registerbudget("styxtest.host_budget", host_budget);
  std::string blob = persist_state(A);
  CHECK(!blob.empty());
  CountingAlloc a2;
  lua_State* B = new_state_for(&a2, blob);
  lua_pushcfunction(B, host_callk);
  lua_setglobal(B, "callk");
  HostString msg2;
  CHECK(restore_state(B, blob, &msg2) == LUA_OK);
  int before = host_budget_calls;
  CHECK(eval(B, "while debug.isfrozen(co) do debug.thaw(co) end return coroutine.status(co)") == "dead");
  CHECK(host_budget_calls > before); /* (the callback of the restored group ran) */
  lua_close(A);
  lua_close(B);
}

static void test_frozen_restore_out_of_memory()
{
  CountingAlloc a1;
  lua_State* A = host_state(&a1);
  CHECK(eval(A, "co = coroutine.create(function() "
                "  local inner = coroutine.create(function() local t = {} for i = 1, 300 do t[i] = i end "
                "    for i = 1, 100 do local x = string.format('%d', i) end return #t end) "
                "  return coroutine.resume(inner) end) "
                "fcount = 0 "
                "debug.setbudget(co, 40, function() fcount = fcount + 1 if fcount == 3 then debug.freeze() end end) "
                "coroutine.resume(co) return debug.isfrozen(co) and 'frozen' or 'done'") == "frozen");
  std::string blob = persist_state(A);
  CHECK(!blob.empty());
  std::string expected = eval(A, "finish() return tostring(coroutine.status(co))");
  /* with less and less room, a restore fails with a memory error and leaves the state usable; with enough, it works */
  bool failed = false, worked = false;
  for (size_t extra = 2 * 1024; extra <= 400 * 1024; extra += 6 * 1024)
  {
    CountingAlloc a2;
    lua_State* B = new_state_for(&a2, blob);
    lua_pushcfunction(B, host_callk);
    lua_setglobal(B, "callk");
    CHECK(eval(B, build_perms) == "ok");
    lua_getglobal(B, "_BACK");
    a2.cap = a2.used + extra;
    HostString msg;
    int st = luaL_unpersiststate(B, 1, byte_view(blob.data(), blob.size()), &msg);
    a2.cap = 0;
    if (st != LUA_OK)
    {
      failed = true;
      CHECK(std::string(msg.to_os()).find("not enough memory") != std::string::npos);
      CHECK(lua_gettop(B) == 1);
      lua_settop(B, 0);
      CHECK(eval(B, "return 'usable'") == "usable");
    }
    else
    {
      worked = true;
      lua_settop(B, 0);
      CHECK(eval(B, "finish() return tostring(coroutine.status(co))") == expected);
    }
    lua_close(B);
  }
  CHECK(failed && worked);
  lua_close(A);
}


/*
** The '__gc' that waits for a budget group (ADR-34): objects of a user
** program that died while no thread of its group ran wait in the group's
** queue, and run, once each and in the same order, when a thread of the
** group goes on after a restore; the objects marked and not yet dead keep
** their group; an object whose group is gone does not run its '__gc'.
*/
static void test_frozen_deferred_gc()
{
  const char* const script =
    "log = {} "
    "co = coroutine.create(function() "
    "  do local mt = {__gc = function(o) log[#log + 1] = 'gc' .. o.id end} "
    "     for i = 1, 3 do setmetatable({id = i}, mt) end "
    "     alive = setmetatable({id = 9}, mt) end "
    "  coroutine.yield('made') "
    "  for i = 1, 5 do end "
    "  coroutine.yield('ran') "
    "  alive = nil "
    "  return 'end' "
    "end) "
    "debug.setbudget(co, 1000000, function() end) "
    "assert(coroutine.resume(co)) "
    "kernel = setmetatable({}, {__gc = function() log[#log + 1] = 'kernel' end}) "
    "kernel = nil "
    "collectgarbage() " /* (the kernel runs: the dead objects of the group wait in its queue) */
    "return #log";
  CountingAlloc a1;
  lua_State* A = new_counted_state(&a1);
  CHECK(eval(A, build_perms) == "ok");
  CHECK(eval(A, script) == "1");              /* only the kernel's ran */
  CHECK(eval(A, "return log[1]") == "kernel");
  std::string blob = persist_state(A);
  CHECK(!blob.empty());
  CountingAlloc a2;
  lua_State* B = restored_state(&a2, blob);
  CHECK(eval(B, "return #log") == "1");
  /* the group goes on: its queue is drained when it runs */
  CHECK(eval(A, "assert(coroutine.resume(co)) return table.concat(log, ',')") ==
        eval(B, "assert(coroutine.resume(co)) return table.concat(log, ',')"));
  CHECK(eval(A, "return #log") == "4");
  CHECK(eval(A, "return table.concat(log, ',', 2)").find("gc") != std::string::npos);
  /* the live one dies with the program, in both alike: the thread is dead,
     so nobody drains its group's queue */
  CHECK(eval(A, "assert(coroutine.resume(co)) collectgarbage() return table.concat(log, ',')") ==
        eval(B, "assert(coroutine.resume(co)) collectgarbage() return table.concat(log, ',')"));
  CHECK(eval(A, "return #log") == eval(B, "return #log"));
  lua_close(A);
  lua_close(B);
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
      {"host_persists_child_thread", test_host_persists_child_thread},
      {"host_state_roundtrip", test_host_state_roundtrip},
      {"host_restore_of_capped_state", test_host_restore_of_capped_state},
      {"host_restore_out_of_memory", test_host_restore_out_of_memory},
      {"host_value_restore", test_host_value_restore},
      {"host_stream_kinds", test_host_stream_kinds},
      {"external_hook_not_supported", test_external_hook_not_supported},
      {"table_order_across_states", test_table_order_across_states},
      {"host_state_marked_garbage", test_host_state_marked_garbage},
      {"host_state_pending_finalizers", test_host_state_pending_finalizers},
      {"host_state_collector_settings", test_host_state_collector_settings},
      {"host_state_garbage_userdata", test_host_state_garbage_userdata},
      {"dead_keys_across_states", test_dead_keys_across_states},
      {"object_key_order_across_states", test_object_key_order_across_states},
      {"library_keys_across_states", test_library_keys_across_states},
      {"state_next_hash", test_state_next_hash},
      {"state_restore_needs_its_state", test_state_restore_needs_its_state},
      {"snapshot_continues_identically", test_snapshot_continues_identically},
      {"frozen_library_cases", test_frozen_library_cases},
      {"frozen_engine_cases", test_frozen_engine_cases},
      {"frozen_deferred_gc", test_frozen_deferred_gc},
      {"frozen_host_continuation", test_frozen_host_continuation},
      {"frozen_host_budget_callback", test_frozen_host_budget_callback},
      {"frozen_restore_out_of_memory", test_frozen_restore_out_of_memory},
  };
  std::printf("  styx_tests.cpp\n");
  const char* only = std::getenv("STYX_ONLY"); /* (to run one C++ test, under valgrind: no scripts then) */
  for (const auto& t : tests)
  {
    if (only != nullptr && std::string(only) != t.name)
      continue;
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
  if (std::getenv("STYX_ONLY") == nullptr)
    run_scripts();
  if (g_failures == 0)
    std::printf("\n\tOK - styx: %d checks, 0 failures\n\n", g_checks);
  else
    std::printf("\n\tFAILED - styx: %d checks, %d failures\n\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
