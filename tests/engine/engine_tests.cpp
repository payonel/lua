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
      bool has = !keyisnil(n);
      if (has)
        getnodekey(L, &key, n);
      t->luaH_setnode(L, i, has ? &key : nullptr, keyisdead(n), &n->val, n->next);
    }
  t->lastfree = src->lastfree;
  return 1;
}

/* copywith(t, map): t's layout, every key k that 'map' maps replaced by map[k] */
static int l_copywith(lua_State* L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  Table* src = table_at(L, 1);
  lua_createtable(L, 0, 0);
  Table* t = table_at(L, -1);
  t->luaH_resize(L, src->asize(), allocsizenode(src));
  if (!isdummy(src))
    for (int i = 0; i < sizenode(src); i++)
    {
      const Node* n = gnode(src, i);
      TValue key;
      bool has = !keyisnil(n);
      if (has)
      {
        getnodekey(L, &key, n);
        if (!keyisdead(n))
        {
          setslot(L, L->top, &key);
          L->top++;
          if (lua_rawget(L, 2) != LUA_TNIL)
          {
            TValue m = getslot(L, L->top - 1);
            copyobj(&key, &m);
          }
          lua_pop(L, 1);
        }
      }
      t->luaH_setnode(L, i, has ? &key : nullptr, keyisdead(n), &n->val, n->next);
    }
  t->lastfree = src->lastfree;
  return 1;
}

/* hash(o): the hash an object got when it was created */
static int l_hash(lua_State* L)
{
  luaL_checkany(L, 1);
  const void* p = lua_topointer(L, 1);
  lua_pushinteger(L, static_cast<const GCObject*>(p)->hash);
  return 1;
}

/* takehash(o, from): 'o', which no table holds as a key, takes the hash of 'from' */
static int l_takehash(lua_State* L)
{
  GCObject* o = const_cast<GCObject*>(static_cast<const GCObject*>(lua_topointer(L, 1)));
  o->hash = static_cast<const GCObject*>(lua_topointer(L, 2))->hash;
  return 0;
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
  lua_pushcfunction(L, l_copywith);
  lua_setglobal(L, "copywith");
  lua_pushcfunction(L, l_hash);
  lua_setglobal(L, "hash");
  lua_pushcfunction(L, l_takehash);
  lua_setglobal(L, "takehash");
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
  {"next from a deleted key (dead after a collection) continues as on the original",
   "for seed = 1, 20 do local t = churned(64, seed) local keys = {} "
   "for k in pairs(t) do keys[#keys + 1] = k end "
   "local gone = {} for i = 1, #keys, 3 do t[keys[i]] = nil gone[#gone + 1] = keys[i] end "
   "collectgarbage() collectgarbage() "
   "for _, c in ipairs{copy(t), setnodes(t)} do for _, k in ipairs(gone) do "
   "local ok, nk = pcall(next, c, k) assert(ok, 'seed ' .. seed .. ': ' .. tostring(nk)) "
   "assert(nk == next(t, k), 'seed ' .. seed) end end end"},
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

/*
** Object keys (tables, functions, threads, userdata) hash by the hash each
** object gets when it is created, not by its address.
*/

struct PlainAlloc : lua_Alloc
{
  bool permit(size_t, size_t, int) override { return true; }
};

static PlainAlloc plain_alloc;

/* a state with a given seed (the debug build's test library needs its own
   allocator, so it is not opened here) */
static lua_State* seeded_state(unsigned int seed)
{
  lua_State* L = lua_newstate(&plain_alloc, seed);
  luaL_openlibs(L);
  lua_pushcfunction(L, l_layout);
  lua_setglobal(L, "layout");
  return L;
}

/* runs 'code' (prelude included) and returns its result as a string */
static std::string run(lua_State* L, const char* code)
{
  std::string chunk = std::string(prelude) + code;
  if (luaL_dostring(L, chunk.c_str()) != LUA_OK)
  {
    auto m = lua_tolstring(L, -1);
    std::printf("      error: %s\n", m ? std::string(m->data(), m->size()).c_str() : "?");
    return "<error>";
  }
  auto r = lua_gettop(L) > 0 ? lua_tolstring(L, -1) : std::nullopt;
  std::string out = r ? std::string(r->data(), r->size()) : "<nil>";
  lua_settop(L, 0);
  return out;
}

/* a table whose keys are objects of every kind, each named in 'names' */
static const char* const object_keys =
  "names = {} t = {} "
  "for i = 1, 300 do "
  "  local k = (i % 3 == 0) and {} or (i % 3 == 1) and function() return i end "
  "            or coroutine.create(function() end) "
  "  names[k] = 'k' .. i t[k] = i "
  "end "
  "for k in pairs(t) do if t[k] % 7 == 0 then t[k] = nil end end ";

static void test_same_order_in_two_states()
{
  std::printf("    the same program and seed iterate object keys in the same order ... ");
  g_test_failures = 0;
  std::string order[2];
  for (int i = 0; i < 2; i++)
  {
    lua_State* L = seeded_state(1234); /* the second state's objects live elsewhere */
    run(L, object_keys);
    order[i] = run(L, "local o = {} for k in pairs(t) do o[#o + 1] = names[k] end return table.concat(o, ' ')");
    lua_close(L);
  }
  CHECK(order[0] == order[1] && order[0] != "<error>");
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

static const Case object_key_cases[] = {
  {"every object has a hash, distinct for objects made one after another",
   "local seen = {} for i = 1, 1000 do local h = hash({}) assert(not seen[h]) seen[h] = true end "
   "assert(hash(print) ~= nil and hash(coroutine.running()) ~= nil)"},
  {"an object given another's hash takes its place in a copied layout",
   "local ks, t = {}, {} for i = 1, 50 do ks[i] = {} t[ks[i]] = i end for i = 1, 50, 4 do t[ks[i]] = nil end "
   "local news = {} for i = 1, 50 do news[i] = {} takehash(news[i], ks[i]) end "
   "local map = {} for i = 1, 50 do map[ks[i]] = news[i] end "
   "local u = copywith(t, map) "
   "for i = 1, 50 do assert(u[news[i]] == t[ks[i]], i) end "
   "local o1, o2 = {}, {} for k in pairs(t) do o1[#o1 + 1] = t[k] end for k in pairs(u) do o2[#o2 + 1] = u[k] end "
   "assert(table.concat(o1, ' ') == table.concat(o2, ' ')) "
   "for i = 1, 50 do u[news[i]] = i * 2 end for i = 1, 50 do assert(u[news[i]] == i * 2) end heap()"},
  {"object keys keep working: inserts, deletes, rehash, weak keys, every mode",
   "for _, mode in ipairs{'incremental', 'generational'} do collectgarbage(mode) "
   "local ks, t = {}, {} for i = 1, 2000 do ks[i] = (i % 2 == 0) and {} or function() end t[ks[i]] = i end "
   "for i = 1, 2000, 3 do t[ks[i]] = nil end for i = 1, 2000 do if i % 3 ~= 1 then assert(t[ks[i]] == i) end end "
   "local w = setmetatable({}, {__mode = 'k'}) for i = 1, 500 do w[{}] = i end collectgarbage() assert(next(w) == nil) "
   "local e = setmetatable({}, {__mode = 'k'}) for i = 1, 100 do e[ks[i]] = {ks[i]} end collectgarbage() "
   "for i = 1, 100 do assert(e[ks[i]][1] == ks[i]) end heap() end collectgarbage('incremental')"},
  {"light C functions and light userdata still hash (by address)",
   "local t = {[print] = 1, [string.format] = 2} assert(t[print] == 1 and t[string.format] == 2)"},
};

int main()
{
  std::printf("\n\tStarting engine internals tests\n\n");
  run_cases("Table::luaH_copyfrom (ltable.cpp)", table_copy_cases,
            sizeof(table_copy_cases) / sizeof(table_copy_cases[0]));
  run_cases("object keys hash by the object's hash (ltable.cpp, lgc.cpp)", object_key_cases,
            sizeof(object_key_cases) / sizeof(object_key_cases[0]));
  test_same_order_in_two_states();
  if (g_failures == 0)
    std::printf("\n\tOK - engine: %d checks, 0 failures\n\n", g_checks);
  else
    std::printf("\n\tFAILED - engine: %d checks, %d failures\n\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
