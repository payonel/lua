/*
** Engine internals tests: operations of the engine that no public API
** reaches, tested directly (the test links the raw objects). Run from the
** top directory (part of `make test`; `make engine-test` runs just these).
*/

#include <cstdio>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#ifdef LUA_DEBUG
#include "ltests.h"
#endif

static int g_checks = 0;
static int g_failures = 0;
static int g_test_failures = 0;

static void check_impl(bool ok, const char* expr, int line)
{
  g_checks++;
  if (!ok)
  {
    g_failures++;
    g_test_failures++;
    std::printf("      FAIL line %d: %s\n", line, expr);
  }
}
#define CHECK(cond) check_impl((cond), #cond, __LINE__)

static Table* table_at(lua_State* L, int idx)
{
  return const_cast<Table*>(static_cast<const Table*>(lua_topointer(L, idx)));
}

/* copy(t): a new table with t's layout (Table::luaH_copyfrom) */
static int l_copy(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_createtable(L, 0, 0);
  table_at(L, -1)->luaH_copyfrom(L, table_at(L, 1));
  return 1;
}

/* setnodes(t): a new table with t's nodes set one by one (luaH_setnode) */
static int l_setnodes(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  Table* src = table_at(L, 1);
  lua_createtable(L, 0, 0);
  Table* t = table_at(L, -1);
  t->luaH_resize(L, 0, allocsizenode(src));
  if (!isdummy(src))
    for (int i = 0; i < sizenode(src); i++)
    {
      const Node* n = gnode(src, i);
      TValue key;
      bool has = !keyisnil(n) && !keyisdead(n);
      if (has)
        getnodekey(L, &key, n);
      t->luaH_setnode(L, i, has ? &key : nullptr, keyisdead(n), &n->val, n->next);
    }
  t->lastfree = src->lastfree;
  return 1;
}

/* layout(t): array size, node count, lastfree and length hint */
static int l_layout(lua_State* L)
{
  Table* t = table_at(L, 1);
  lua_pushinteger(L, t->asize());
  lua_pushinteger(L, allocsizenode(t));
  lua_pushinteger(L, t->lastfree);
  lua_pushinteger(L, t->lenhint);
  return 4;
}

static lua_State* new_state()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
#ifdef LUA_DEBUG
  luaL_requiref(L, "T", luaB_opentests, 1);
  lua_pop(L, 1);
#endif
  lua_pushcfunction(L, l_copy);
  lua_setglobal(L, "copy");
  lua_pushcfunction(L, l_layout);
  lua_setglobal(L, "layout");
  lua_pushcfunction(L, l_setnodes);
  lua_setglobal(L, "setnodes");
  return L;
}

/* helpers available to every case */
static const char* const prelude = R"(
  function order(t)
    local o = {}
    for k in pairs(t) do o[#o + 1] = tostring(k) end
    return table.concat(o, " ")
  end
  function same(a, b)
    if order(a) ~= order(b) or #a ~= #b then return false end
    for k, v in pairs(a) do
      if rawget(b, k) ~= v then return false end
      if next(a, k) ~= next(b, k) then return false end
    end
    local la = {layout(a)}
    local lb = {layout(b)}
    for i = 1, 4 do if la[i] ~= lb[i] then return false end end
    return getmetatable(a) == getmetatable(b)
  end
  function heap() if T then T.checkmemory() end end
  function churned(n, seed)
    math.randomseed(seed)
    local t = {}
    for i = 1, n do t["key" .. math.random(n * 10)] = i end
    for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end
    for i = 1, n // 4 do t["new" .. math.random(n * 10)] = i end
    return t
  end
)";

struct Case
{
  const char* name;
  const char* code;
};

static const Case table_copy_cases[] = {
  {"a churned table keeps its order",
   "for seed = 1, 50 do local t = churned(64, seed) local c = copy(t) "
   "assert(same(t, c), 'seed ' .. seed) end"},
  {"integer, float and boolean keys, and an array part with holes",
   "local t = {1, 2, nil, 4, [1.5] = 'f', [true] = 't', [false] = 'f', [-3] = 'n', [100] = 'h'} "
   "t[2] = nil local c = copy(t) assert(same(t, c)) assert(#c == #t)"},
  {"dead keys keep the chains",
   "for seed = 1, 20 do local t = churned(100, seed) "
   "for k in pairs(t) do if math.random() < 0.5 then t[k] = nil end end "
   "collectgarbage() collectgarbage() local c = copy(t) assert(same(t, c), 'seed ' .. seed) end"},
  {"object keys (same state, same addresses)",
   "local ks = {} local t = {} for i = 1, 50 do ks[i] = {} t[ks[i]] = i end "
   "for i = 1, 50, 3 do t[ks[i]] = nil end local c = copy(t) assert(same(t, c))"},
  {"the metatable is copied",
   "local mt = {__index = function(_, k) return k .. '!' end} local t = setmetatable({a = 1}, mt) "
   "local c = copy(t) assert(getmetatable(c) == mt and c.zzz == 'zzz!' and c.a == 1)"},
  {"a table filled by luaH_setnode works as a metatable",
   "local mt = setnodes({__index = function(_, k) return k .. '!' end, __mode = 'k', __len = function() return 7 end}) "
   "local x = setmetatable({}, mt) assert(x.abc == 'abc!' and #x == 7) "
   "local w = setmetatable({}, setnodes({__mode = 'k'})) w[{}] = true collectgarbage() assert(next(w) == nil)"},
  {"empty and array-only tables",
   "local e = copy({}) assert(next(e) == nil) local a = {} for i = 1, 100 do a[i] = i end "
   "local c = copy(a) assert(same(a, c) and #c == 100)"},
  {"a copy keeps working: inserts, deletes, rehash",
   "local t = churned(64, 7) local c = copy(t) "
   "for i = 1, 500 do c['more' .. i] = i end for i = 1, 500, 2 do c['more' .. i] = nil end "
   "for k, v in pairs(t) do assert(c[k] == v) end for i = 2, 500, 2 do assert(c['more' .. i] == i) end "
   "collectgarbage() heap()"},
  {"a large churned table",
   "local t = churned(5000, 3) local c = copy(t) assert(same(t, c)) collectgarbage() heap()"},
  {"copies survive collections in every mode",
   "for _, mode in ipairs{'incremental', 'generational'} do collectgarbage(mode) "
   "local keep = {} for i = 1, 200 do local t = churned(32, i) keep[i] = {t, copy(t)} "
   "if i % 20 == 0 then collectgarbage('step') end end collectgarbage() heap() "
   "for i = 1, 200 do assert(same(keep[i][1], keep[i][2])) end end collectgarbage('incremental')"},
};

static void run_cases(const char* group, const Case* cases, size_t n)
{
  std::printf("  %s\n", group);
  for (size_t i = 0; i < n; i++)
  {
    lua_State* L = new_state();
    g_test_failures = 0;
    std::string code = std::string(prelude) + cases[i].code;
    if (luaL_dostring(L, code.c_str()) != LUA_OK)
    {
      auto m = lua_tolstring(L, -1);
      std::printf("      error: %s\n", m ? std::string(m->data(), m->size()).c_str() : "?");
      CHECK(false);
    }
    else
      CHECK(true);
    std::printf("    %s ... %s\n", cases[i].name, g_test_failures == 0 ? "OK" : "FAIL");
    lua_close(L);
  }
}

int main()
{
  std::printf("\n\tStarting engine internals tests\n\n");
  run_cases("Table::luaH_copyfrom (ltable.cpp)", table_copy_cases,
            sizeof(table_copy_cases) / sizeof(table_copy_cases[0]));
  if (g_failures == 0)
    std::printf("\n\tOK - engine: %d checks, 0 failures\n\n", g_checks);
  else
    std::printf("\n\tFAILED - engine: %d checks, %d failures\n\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
