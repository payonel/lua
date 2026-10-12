/*
** Engine internals tests: operations of the engine that no public API
** reaches, tested directly (the test links the raw objects). Run from the
** top directory (part of `make test`; `make engine-test` runs just these).
*/

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lgc.h"
#include "lua_internal.h"
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

/* steps(a, b): how many hashes of the sequence lie from a's hash to b's */
static int l_steps(lua_State* L)
{
  unsigned int inv = HASHSTEP;
  for (int i = 0; i < 5; i++)
    inv *= 2u - HASHSTEP * inv;
  unsigned int ha = static_cast<const GCObject*>(lua_topointer(L, 1))->hash;
  unsigned int hb = static_cast<const GCObject*>(lua_topointer(L, 2))->hash;
  lua_pushinteger(L, static_cast<lua_Integer>((hb - ha) * inv));
  return 1;
}

/* takehash(o, from): 'o', which no table holds as a key, takes the hash of 'from' */
static int l_takehash(lua_State* L)
{
  GCObject* o = const_cast<GCObject*>(static_cast<const GCObject*>(lua_topointer(L, 1)));
  o->hash = static_cast<const GCObject*>(lua_topointer(L, 2))->hash;
  return 0;
}

/*
** chainsok(t): every key of t's hash part is in the chain that starts at
** its main position (where a lookup looks for it), and no chain loops
*/
static int l_chainsok(lua_State* L)
{
  Table* t = table_at(L, 1);
  bool ok = true;
  if (!isdummy(t))
  {
    int size = sizenode(t);
    for (int i = 0; i < size && ok; i++)
    {
      const Node* n = gnode(t, i);
      if (keyisnil(n) || keyisdead(n))
        continue;
      TValue key;
      getnodekey(L, &key, n);
      int j = t->luaH_mainposition(&key);
      for (int steps = 0; j != i; steps++)
      {
        int next = gnode(t, j)->next;
        if (next == 0 || steps == size)
        {
          ok = false;
          break;
        }
        j += next;
      }
    }
  }
  lua_pushboolean(L, ok);
  return 1;
}

/* misplace(t): a copy of t's layout whose first key is a new table with a
   hash whose main position is a free node with no chain (so no lookup
   reaches the slot it is put in) */
static int l_misplace(lua_State* L)
{
  Table* src = table_at(L, 1);
  lua_createtable(L, 0, 0);
  Table* t = table_at(L, -1);
  t->luaH_resize(L, 0, allocsizenode(src));
  bool done = false;
  for (int i = 0; i < sizenode(src); i++)
  {
    const Node* n = gnode(src, i);
    TValue key;
    bool has = !keyisnil(n);
    if (has)
      getnodekey(L, &key, n);
    if (has && !keyisdead(n) && !done)
    {
      lua_createtable(L, 0, 0);
      TValue nk = getslot(L, L->top - 1);
      Table* k = hvalue(&nk);
      for (unsigned int h = 1; h < 100000; h++)
      {
        k->hash = h;
        int mp = t->luaH_mainposition(&nk);
        const Node* m = gnode(src, mp);
        if (mp != i && keyisnil(m) && m->next == 0)
          break;
      }
      copyobj(&key, &nk); /* on the stack until 't' holds it */
      done = true;
    }
    t->luaH_setnode(L, i, has ? &key : nullptr, keyisdead(n), &n->val, n->next);
  }
  t->lastfree = src->lastfree;
  lua_pushvalue(L, 2);
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
  lua_pushcfunction(L, l_copywith);
  lua_setglobal(L, "copywith");
  lua_pushcfunction(L, l_chainsok);
  lua_setglobal(L, "chainsok");
  lua_pushcfunction(L, l_misplace);
  lua_setglobal(L, "misplace");
  lua_pushcfunction(L, l_hash);
  lua_setglobal(L, "hash");
  lua_pushcfunction(L, l_takehash);
  lua_setglobal(L, "takehash");
  lua_pushcfunction(L, l_steps);
  lua_setglobal(L, "steps");
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
    if not chainsok(a) or not chainsok(b) then return false end
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

/* object keys made among strings: their order does not depend on which
   strings a state already had (interning reuses them) */
static void test_order_independent_of_interned_strings()
{
  std::printf("    object keys made among strings keep their order whatever strings exist ... ");
  g_test_failures = 0;
  std::string order[2];
  for (int i = 0; i < 2; i++)
  {
    lua_State* L = seeded_state(77);
    /* the same program, but the second state has the strings already */
    run(L, i == 0 ? "keep = {} for i = 1, 300 do keep[i] = 'other' .. i end return 'ok'"
                  : "keep = {} for i = 1, 300 do keep[i] = 'name' .. i end return 'ok'");
    order[i] = run(L, "local t, names = {}, {} "
                      "for i = 1, 300 do local n = 'name' .. i local k = {} t[k] = i names[k] = n end "
                      "local o = {} for k in pairs(t) do o[#o + 1] = names[k] end return table.concat(o, ' ')");
    lua_close(L);
  }
  CHECK(order[0] == order[1] && order[0] != "<error>");
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

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

/* an engine C function hashes as a key by its name, not by its address
   (which another process has elsewhere): a copy of it at another address
   has the same main position */
static void test_cfunction_hash_by_name()
{
  std::printf("    an engine C function as a key hashes by its name, not its address ... ");
  g_test_failures = 0;
  lua_State* L = seeded_state(5);
  lua_createtable(L, 0, 4096);
  Table* t = table_at(L, -1);
  int n = 0, same = 0;
  for (const char* name : {"base.next", "base.assert", "base.collectgarbage", "base.load", "base.ipairsaux",
                           "table.tconcat", "table.tinsert", "table.sort"})
  {
    CFuncInfo* f = lua_cfibyname(byte_view(name));
    CHECK(f != nullptr);
    if (f == nullptr)
      continue;
    CFuncInfo copy = *f; /* (at another address) */
    TValue a, b;
    setfvalue(&a, f);
    setfvalue(&b, &copy);
    n++;
    if (t->luaH_mainposition(&a) == t->luaH_mainposition(&b))
      same++;
  }
  CHECK(n == 8 && same == n);
  lua_close(L);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/* a load takes one hash of the sequence, its function's, however many
   functions its chunk has: the parser's own tables (its anchor, a cache of
   constants for each function) are never a program's keys, and the
   sequence depends only on what the program makes */
static void test_load_takes_one_hash()
{
  std::printf("    a load takes one hash of the sequence (its function's) ... ");
  g_test_failures = 0;
  lua_State* L = seeded_state(9);
  for (const char* chunk : {"return 1", "local function a() return function() end end local b = function() end return a, b",
                            "local t = {f = function() end, g = function() return function() end end} return t"})
  {
    unsigned int before = L->l_G->nexthash;
    CHECK(luaL_loadstring(L, chunk) == LUA_OK);
    CHECK(L->l_G->nexthash - before == lua::gc::HASHSTEP);
    lua_pop(L, 1);
  }
  lua_close(L);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/* an allocator that refuses large growths when told to */
struct DenyLargeAlloc : lua_Alloc
{
  bool deny = false;
  bool permit(size_t o, size_t n, int) override { return !(deny && n > o && n - o >= 8192); }
};

/*
** Every engine C function is known by its name from the start, before the
** library function that hands it out is first called (a restore in a new
** process looks them up by name; before, utf8.codes' iterators and the
** C library handle's __gc were named at their first use, so a fresh process
** could not restore a state holding one)
*/
static void test_names_known_from_start()
{
  std::printf("    engine C functions are named before their first use ... ");
  g_test_failures = 0;
  for (const char* name : {"utf8.iter_auxlax", "utf8.iter_auxstrict", "package.lib_gc", "utf8.iter_codes"})
    CHECK(lua_cfibyname(byte_view(name)) != nullptr);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/*
** The C++ heap of this program, as a host's allocator would see it: every
** new and delete goes through here ('heap_peak' is the most bytes live
** since it was last set)
*/
static size_t heap_live = 0;
static size_t heap_peak = 0;

void* operator new(std::size_t n)
{
  void* p = std::malloc(n + 16);
  if (p == nullptr)
    throw std::bad_alloc();
  *static_cast<std::size_t*>(p) = n;
  heap_live += n;
  if (heap_live > heap_peak)
    heap_peak = heap_live;
  return static_cast<char*>(p) + 16;
}

void operator delete(void* p) noexcept
{
  if (p == nullptr)
    return;
  char* b = static_cast<char*>(p) - 16;
  heap_live -= *static_cast<std::size_t*>(static_cast<void*>(b));
  std::free(b);
}

void operator delete(void* p, std::size_t) noexcept
{
  operator delete(p);
}

/* counts what the engine asks for, and refuses growth past 'cap' (0: none) */
struct CountAlloc : lua_Alloc
{
  size_t used = 0;
  size_t peak = 0;
  size_t cap = 0;
  bool permit(size_t o, size_t n, int) override
  {
    if (n > o && cap != 0 && used + (n - o) > cap)
      return false;
    used = used + n - o;
    if (used > peak)
      peak = used;
    return true;
  }
};

/*
** A string's bytes are counted before they exist, and a result made in a
** buffer takes the buffer's bytes (B24): making a string holds of the heap
** what the allocator counts. Before, a result was copied out of its buffer
** (held twice, counted once), and a string's bytes were copied before they
** were counted (a string refused had been made first).
*/
static void test_string_bytes_counted()
{
  std::printf("    a string's bytes are counted before they are made ... ");
  g_test_failures = 0;
  const size_t N = 8 << 20;
  CountAlloc a;
  lua_State* L = lua_newstate(&a, 1);
  luaL_openlibs(L);
  CHECK(run(L, "s = string.rep('a', 8 << 20) return 'ok'") == "ok");
  const char* made[] = {"return #string.rep('x', 8 << 20)", "return #s:upper()", "return #s:reverse()"};
  for (const char* code : made)
  {
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
    size_t live0 = heap_live;
    size_t used0 = a.used;
    heap_peak = heap_live;
    a.peak = a.used;
    CHECK(run(L, code) == std::to_string(N));
    CHECK(heap_peak - live0 < a.peak - used0 + N / 8);
  }
  lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
  a.cap = a.used + N / 2; /* no room for a copy of 's' */
  size_t live0 = heap_live;
  heap_peak = heap_live;
  CHECK(run(L, "return tostring(pcall(string.sub, s, 2))") == "false");
  CHECK(heap_peak - live0 < N / 8);
  a.cap = 0;
  lua_close(L);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/*
** The budget thread keeps the room its callbacks need (B16): a reservation
** that found no memory is made again the next time a budget is set, and a
** close of the thread ('coroutine.close') keeps it
*/
static void test_budget_thread_room()
{
  std::printf("    the budget thread keeps the room of its callbacks ... ");
  g_test_failures = 0;
  DenyLargeAlloc a;
  lua_State* L = lua_newstate(&a, 1);
  luaL_openlibs(L);
  CHECK(run(L, "co = coroutine.create(function() end) return 'ok'") == "ok");
  a.deny = true;
  CHECK(run(L, "return tostring(pcall(debug.setbudget, co, 10, function() end))") == "false");
  a.deny = false;
  CHECK(run(L, "debug.setbudget(co, 10, function() end) return 'ok'") == "ok");
  lua_State* B = L->l_G->budgetth;
  CHECK(B != nullptr && stacksize(B) >= 1000 && B->nci >= 40);
  CHECK(run(L, "coroutine.close(debug.getregistry()._BUDGETTHREAD) return 'ok'") == "ok");
  CHECK(B != nullptr && B == L->l_G->budgetth && stacksize(B) >= 1000);
  lua_close(L);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/*
** luaC_fullgcdue: a whole cycle that makes no '__gc' call (neither those
** it finds due nor those of a cycle under way: the collector's next step
** makes them, in order), frees what is dead and clears weak tables; in
** both modes of the collector
*/
static void test_fullgc_due()
{
  std::printf("    a whole cycle leaves its __gc calls to the next step ... ");
  g_test_failures = 0;
  const char* modes[] = {"incremental", "generational"};
  for (const char* mode : modes)
    for (int underway = 0; underway < 2; underway++)
    {
      lua_State* L = luaL_newstate();
      luaL_openlibs(L);
      std::string prog = std::string("collectgarbage('") + mode + "') live = {} for i = 1, 20000 do live[i] = {} end "
                         "fins = {} for i = 1, 3 do setmetatable({}, {__gc = function() fins[#fins + 1] = i end}) end "
                         "weak = setmetatable({}, {__mode = 'k'}) weak[{}] = 1 " +
                         (underway ? "collectgarbage('step', 1) " : "") + "return 'ok'";
      CHECK(run(L, prog.c_str()) == "ok");
      lua_lock(L);
      luaC_fullgcdue(L);
      lua_unlock(L);
      lua_getglobal(L, "fins"); /* (read with no allocation: no step) */
      CHECK(lua_rawlen(L, -1) == 0);
      lua_getglobal(L, "weak");
      lua_pushnil(L);
      CHECK(lua_next(L, -2) == 0);
      lua_settop(L, 0);
      CHECK(run(L, "return table.concat(fins, ' ') .. ' ' .. collectgarbage('incremental')") ==
            std::string("3 2 1 ") + mode);
      lua_close(L);
    }
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

static const Case chain_cases[] = {
  {"chains hold after inserts, deletes and rehash, for keys of every kind",
   "local t = {} for i = 1, 3000 do local k = (i % 4 == 0) and {} or (i % 4 == 1) and ('s' .. i) "
   "or (i % 4 == 2) and i * 1.5 or function() end t[k] = i assert(chainsok(t), i) end "
   "for k in pairs(t) do if math.random() < 0.4 then t[k] = nil end end collectgarbage() assert(chainsok(t))"},
  {"a misplaced key is detected",
   "local t = {} for i = 1, 5 do t[{}] = i end local u = misplace(t) assert(chainsok(t) and not chainsok(u))"},
};

static const Case object_key_cases[] = {
  {"every object has a hash, distinct for objects made one after another",
   "local seen = {} for i = 1, 1000 do local h = hash({}) assert(not seen[h]) seen[h] = true end "
   "assert(hash(print) ~= nil and hash(coroutine.running()) ~= nil)"},
  {"an object given another's hash takes its place in a copied layout",
   "local ks, t = {}, {} for i = 1, 50 do ks[i] = {} t[ks[i]] = i end for i = 1, 50, 4 do t[ks[i]] = nil end "
   "local news = {} for i = 1, 50 do news[i] = {} takehash(news[i], ks[i]) end "
   "local map = {} for i = 1, 50 do map[ks[i]] = news[i] end "
   "local u = copywith(t, map) assert(chainsok(u)) "
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
  {"only objects that hash by identity take hashes: strings, new or reused, do not",
   "local a = {} local s1 = ('fresh'):rep(3) .. 'x' local s2 = 'reused' local f = function() end "
   "local b = {} assert(steps(a, f) == 1 and steps(f, b) == 1, steps(a, b)) "
   "local c = coroutine.create(print) local d = {} assert(steps(b, c) == 1 and steps(c, d) == 1)"},
  {"light C functions and light userdata still hash (by address)",
   "local t = {[print] = 1, [string.format] = 2} assert(t[print] == 1 and t[string.format] == 2)"},
};

/*
** Names of the functions the engine keeps in its state (lnames.cpp): they
** resolve both ways, and the engine's own continuations, light C functions
** and budget callback have them.
*/
static int test_cont(lua_State*, int, lua_KContext)
{
  return 0;
}
LUA_KNAME("test", test_cont);

static void test_budget(lua_State*, lua_State*) {}

static void test_function_names()
{
  CHECK(lua_kname(test_cont) != nullptr && std::string(lua_kname(test_cont)) == "test.test_cont");
  CHECK(lua_kbyname("test.test_cont") == test_cont);
  CHECK(lua_kbyname("test.nothing") == nullptr && lua_kname(nullptr) == nullptr);
  /* a host names its own through the public function */
  lua_registerk("host.my_cont", test_cont);
  CHECK(lua_kbyname("host.my_cont") == test_cont);
  lua_registerbudget("host.my_budget", test_budget);
  CHECK(lua_budgetbyname("host.my_budget") == test_budget && std::string(lua_budgetname(test_budget)) == "host.my_budget");
  /* the continuations of the libraries */
  for (const char* n : {"base.finishpcall", "base.printk", "base.load_cont", "coroutine.coresume_cont",
                        "coroutine.auxwrap_cont", "string.gsubk", "string.formatk", "table.sortk",
                        "table.tinsertk_loop", "math.min_cont", "gc.gcdrain_cont", "auxlib.requireff",
                        "debug.thaw_cont", "package.ll_requirek"})
  {
    lua_KFunction k = lua_kbyname(n);
    CHECK(k != nullptr);
    CHECK(k != nullptr && std::string(lua_kname(k)) == n);
  }
  /* the light C functions the engine pushes itself, and one of a library */
  CHECK(lua_cfibyname("base.next") != nullptr);
  CHECK(lua_cfibyname("gc.gcdrain") != nullptr);
  CHECK(lua_cfibyname("auxlib.requireff") != nullptr);
  CHECK(lua_cfibyname("nowhere.nothing") == nullptr);
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  /* a function of a library, as the script sees it, has its name */
  int bad = 0;
  for (const char* lib : {"string", "table", "math", "os", "io", "coroutine", "debug", "utf8"})
  {
    lua_getglobal(L, lib);
    lua_pushnil(L);
    while (lua_next(L, -2))
    {
      if (lua_iscfunction(L, -1) && lua_islightcfunction(L, -1))
      {
        const CFuncInfo* cfi = static_cast<const CFuncInfo*>(lua_topointer(L, -1));
        if (lua_cfiname(cfi) == nullptr)
          bad++;
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  CHECK(bad == 0);
  lua_close(L);
}

/*
** User objects that can be kept outside the process (lua_UserObject): a box
** saves its state and the factory of its kind makes an equal one.
*/
static void test_object_kinds()
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  luaL_Box* box = luaL_newbox(L);
  box->ints = {1, -2, 3, 1000000007};
  luaL_Buffer B;
  luaL_buffinit(L, &B);
  luaL_addlstring(&B, "bytes of a box", 0, 14);
  luaL_buffsave(&B, box);
  CHECK(std::string(box->persistkind()) == "box");
  CHECK(box->persistuservalues()); /* (by default, its user values are part of its state) */
  lua_Integer ints[8];
  byte_view bytes;
  int n = box->persistsave(ints, &bytes);
  CHECK(n == 5 && ints[0] == 1 && ints[1] == -2 && ints[3] == 1000000007 && ints[4] == 14);
  CHECK(bytes == byte_view("bytes of a box"));
  lua_ObjectFactory f = lua_objectkind("box");
  CHECK(f != nullptr && lua_objectkind("nothing") == nullptr);
  std::unique_ptr<lua_UserObject> copy = f(L, ints, n, bytes);
  CHECK(copy != nullptr);
  luaL_Box* c = dynamic_cast<luaL_Box*>(copy.get());
  CHECK(c != nullptr && c->ints == box->ints && c->used == 14);
  lua_Integer ints2[8];
  byte_view bytes2;
  CHECK(c != nullptr && c->persistsave(ints2, &bytes2) == 5 && bytes2 == bytes);
  ints[4] = 3; /* the count of the bytes must be the bytes' */
  CHECK(f(L, ints, n, bytes) == nullptr);
  CHECK(f(L, ints, 4, bytes) == nullptr);
  copy->freed(L); /* (made outside a userdata, it gives back what it holds itself) */
  copy.reset();
  lua_close(L);
}

/*
** Objects made due by hand (luaC_makedue): their '__gc' run in the order
** given, even though they are alive and the collector is stopped; one whose
** group is gone is ordinary and runs none.
*/
static void test_make_due(bool generational)
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  if (generational)
    CHECK(luaL_dostring(L, "collectgarbage('generational')") == LUA_OK);
  CHECK(luaL_dostring(L, "collectgarbage('stop') log = {} keep = {} local mt = {__gc = function(o) log[#log + 1] = o.name end} "
                         "for _, n in ipairs{'a', 'b', 'c', 'd'} do keep[n] = setmetatable({name = n}, mt) end") == LUA_OK);
  GCObject* objs[4];
  const char* names[4] = {"c", "a", "d", "b"};
  for (int i = 0; i < 4; i++)
  {
    lua_getglobal(L, "keep");
    lua_getfield(L, -1, names[i]);
    objs[i] = static_cast<GCObject*>(const_cast<void*>(lua_topointer(L, -1)));
    lua_pop(L, 2);
  }
  objs[3]->owner = 65535; /* its group is gone */
  /* (one more that is not marked for finalization: nothing happens to it) */
  CHECK(luaL_dostring(L, "plain = {name = 'plain'}") == LUA_OK);
  lua_getglobal(L, "plain");
  GCObject* notmarked = static_cast<GCObject*>(const_cast<void*>(lua_topointer(L, -1)));
  lua_pop(L, 1);
  GCObject* given[5] = {objs[0], objs[1], objs[2], objs[3], notmarked};
  lua::gc::luaC_makedue(L, given, 5);
  CHECK(luaL_dostring(L, "collectgarbage() return table.concat(log, ',')") == LUA_OK);
  CHECK(std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()) == "c,a,d");
  lua_close(L);
}

/*
** Objects a restore makes due for a group's '__gc' (luaC_makedue) are queued
** white, as the collector keeps its queues (B30): the steps a restore runs
** can have marked them black, and a queued object that is not white is not
** traversed by the next cycle, which collected its metatable (its '__gc' was
** then looked up in freed memory)
*/
static void test_make_due_queued_white()
{
  std::printf("    objects made due for a group's __gc are queued white ... ");
  g_test_failures = 0;
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  CHECK(run(L, "collectgarbage('stop') log = {} keep = {} "
               "co = coroutine.create(function() "
               "  for i = 1, 3 do keep[i] = setmetatable({name = i}, {__gc = function(o) log[#log + 1] = o.name end}) end "
               "  coroutine.yield() local n = 0 for i = 1, 100 do n = n + i end "
               "  table.sort(log) return table.concat(log, ',') end) "
               "debug.setbudget(co, 1000000, function() end) coroutine.resume(co) return 'ok'") == "ok");
  GCObject* objs[3];
  for (int i = 0; i < 3; i++)
  {
    lua_getglobal(L, "keep");
    lua_geti(L, -1, i + 1);
    objs[i] = static_cast<GCObject*>(const_cast<void*>(lua_topointer(L, -1)));
    lua_pop(L, 2);
  }
  lua_lock(L);
  lua::gc::luaC_runtilstate(L, lua::gc::GCSenteratomic, 0); /* (all reachable is black) */
  lua_unlock(L);
  for (GCObject* o : objs)
    CHECK(!lua::gc::iswhite(o));
  CHECK(run(L, "keep = nil return 'ok'") == "ok");
  lua_lock(L);
  lua::gc::luaC_makedue(L, objs, 3);
  lua_unlock(L);
  for (GCObject* o : objs)
    CHECK(lua::gc::iswhite(o));
  lua_lock(L);
  lua::gc::luaC_fullgcdue(L);
  lua_unlock(L);
  CHECK(run(L, "return tostring(select(2, coroutine.resume(co)))") == "1,2,3");
  lua_close(L);
  std::printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
}

/*
** Objects marked for finalization put in another order by hand
** (luaC_ordermarked), as if they had been marked in it: when they die
** together, their '__gc' run in the reverse of that order, as stock runs
** them in the reverse of their marking. An object in the list that is not
** marked is left alone.
*/
static void test_order_marked(bool generational)
{
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  if (generational)
    CHECK(luaL_dostring(L, "collectgarbage('generational')") == LUA_OK);
  CHECK(luaL_dostring(L, "collectgarbage('stop') log = {} keep = {} local mt = {__gc = function(o) log[#log + 1] = o.name end} "
                         "for _, n in ipairs{'a', 'b', 'c', 'd'} do keep[n] = setmetatable({name = n}, mt) end "
                         "plain = {name = 'plain'}") == LUA_OK);
  GCObject* objs[5];
  const char* names[4] = {"c", "a", "d", "b"};
  for (int i = 0; i < 4; i++)
  {
    lua_getglobal(L, "keep");
    lua_getfield(L, -1, names[i]);
    objs[i] = static_cast<GCObject*>(const_cast<void*>(lua_topointer(L, -1)));
    lua_pop(L, 2);
  }
  lua_getglobal(L, "plain");
  objs[4] = static_cast<GCObject*>(const_cast<void*>(lua_topointer(L, -1)));
  lua_pop(L, 1);
  lua::gc::luaC_ordermarked(L, objs, 5);
  CHECK(luaL_dostring(L, "keep = nil plain = nil collectgarbage('restart') collectgarbage() collectgarbage() "
                         "return table.concat(log, ',')") == LUA_OK);
  CHECK(std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()) == "b,d,a,c");
  lua_close(L);
}

int main()
{
  std::printf("\n\tStarting engine internals tests\n\n");
  test_names_known_from_start(); /* (first: before any library function runs) */
  run_cases("Table::luaH_copyfrom (ltable.cpp)", table_copy_cases,
            sizeof(table_copy_cases) / sizeof(table_copy_cases[0]));
  run_cases("Table::luaH_mainposition: keys sit in their chains (ltable.cpp)", chain_cases,
            sizeof(chain_cases) / sizeof(chain_cases[0]));
  run_cases("object keys hash by the object's hash (ltable.cpp, lgc.cpp)", object_key_cases,
            sizeof(object_key_cases) / sizeof(object_key_cases[0]));
  test_same_order_in_two_states();
  test_order_independent_of_interned_strings();
  test_cfunction_hash_by_name();
  test_load_takes_one_hash();
  test_budget_thread_room();
  test_string_bytes_counted();
  test_fullgc_due();
  test_function_names();
  test_object_kinds();
  test_make_due(false);
  test_make_due(true);
  test_make_due_queued_white();
  test_order_marked(false);
  test_order_marked(true);
  if (g_failures == 0)
    std::printf("\n\tOK - engine: %d checks, 0 failures\n\n", g_checks);
  else
    std::printf("\n\tFAILED - engine: %d checks, %d failures\n\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
