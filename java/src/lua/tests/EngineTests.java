/*
** Engine internals tests: operations of the engine that no public API
** reaches, tested directly (the test links the raw objects). Run from the
** top directory (part of `make test`; `make engine-test` runs just these).
*/
package lua.tests;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.gc.Lgc.*;
import static lua.lib.Linit.luaL_openlibs;
import static lua.object.Lnames.*;
import static lua.object.Lobject.*;
import static lua.object.Ltable.*;
import static lua.platform.OsApi.exit;
import static lua.platform.OsApi.os_print_stdout;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lstate.*;

import java.lang.management.ManagementFactory;
import java.util.Arrays;
import java.util.Locale;

import lua.api.HostString;
import lua.api.KNamer;
import lua.api.luaL_Box;
import lua.api.luaL_Buffer;
import lua.api.lua_Alloc;
import lua.api.lua_Budget;
import lua.api.lua_CFunction;
import lua.api.lua_KFunction;
import lua.api.lua_ObjectFactory;
import lua.api.lua_UserObject;
import lua.bytes.ByteView;
import lua.object.CFuncInfo;
import lua.object.GCObject;
import lua.object.Node;
import lua.object.TValue;
import lua.object.Table;
import lua.runtime.lua_State;

public final class EngineTests
{
  private EngineTests()
  {
  }

  private static void printf(String fmt, Object... args)
  {
    os_print_stdout(ByteView.of(String.format(Locale.ROOT, fmt, args)));
  }

  private static int g_checks = 0;
  private static int g_failures = 0;
  private static int g_test_failures = 0;

  private static void check_impl(boolean ok, String expr, int line)
  {
    g_checks++;
    if (!ok)
    {
      g_failures++;
      g_test_failures++;
      printf("      FAIL line %d: %s\n", line, expr);
    }
  }

  private static void CHECK(boolean cond, String expr)
  {
    check_impl((cond), expr, StackWalker.getInstance().walk(s -> s.skip(1).findFirst().get().getLineNumber()));
  }

  private static Table table_at(lua_State L, int idx)
  {
    return (Table) lua_topointer(L, idx);
  }

  /* copy(t): a new table with t's layout (Table::luaH_copyfrom) */
  private static int l_copy(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_createtable(L, 0, 0);
    table_at(L, -1).luaH_copyfrom(L, table_at(L, 1));
    return 1;
  }

  private static final lua_CFunction l_copy = EngineTests::l_copy;

  /* setnodes(t): a new table with t's nodes set one by one (luaH_setnode) */
  private static int l_setnodes(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    Table src = table_at(L, 1);
    lua_createtable(L, 0, 0);
    Table t = table_at(L, -1);
    t.luaH_resize(L, 0, allocsizenode(src));
    if (isdummy(src) == 0)
      for (int i = 0; i < sizenode(src); i++)
      {
        Node n = gnode(src, i);
        TValue key = new TValue();
        boolean has = !keyisnil(n);
        if (has)
          getnodekey(L, key, n);
        t.luaH_setnode(L, i, has ? key : null, keyisdead(n), n, n.next);
      }
    t.lastfree = src.lastfree;
    return 1;
  }

  private static final lua_CFunction l_setnodes = EngineTests::l_setnodes;

  /* copywith(t, map): t's layout, every key k that 'map' maps replaced by map[k] */
  private static int l_copywith(lua_State L)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    Table src = table_at(L, 1);
    lua_createtable(L, 0, 0);
    Table t = table_at(L, -1);
    t.luaH_resize(L, src.asize(), allocsizenode(src));
    if (isdummy(src) == 0)
      for (int i = 0; i < sizenode(src); i++)
      {
        Node n = gnode(src, i);
        TValue key = new TValue();
        boolean has = !keyisnil(n);
        if (has)
        {
          getnodekey(L, key, n);
          if (!keyisdead(n))
          {
            setslot(L, L.top, key);
            L.top++;
            if (lua_rawget(L, 2) != LUA_TNIL)
            {
              TValue m = getslot(L, L.top - 1);
              copyobj(key, m);
            }
            lua_pop(L, 1);
          }
        }
        t.luaH_setnode(L, i, has ? key : null, keyisdead(n), n, n.next);
      }
    t.lastfree = src.lastfree;
    return 1;
  }

  private static final lua_CFunction l_copywith = EngineTests::l_copywith;

  /* hash(o): the hash an object got when it was created */
  private static int l_hash(lua_State L)
  {
    luaL_checkany(L, 1);
    Object p = lua_topointer(L, 1);
    lua_pushinteger(L, Integer.toUnsignedLong(((GCObject) p).hash));
    return 1;
  }

  private static final lua_CFunction l_hash = EngineTests::l_hash;

  /* steps(a, b): how many hashes of the sequence lie from a's hash to b's */
  private static int l_steps(lua_State L)
  {
    int inv = HASHSTEP;
    for (int i = 0; i < 5; i++)
      inv *= 2 - HASHSTEP * inv;
    int ha = ((GCObject) lua_topointer(L, 1)).hash;
    int hb = ((GCObject) lua_topointer(L, 2)).hash;
    lua_pushinteger(L, Integer.toUnsignedLong((hb - ha) * inv));
    return 1;
  }

  private static final lua_CFunction l_steps = EngineTests::l_steps;

  /* takehash(o, from): 'o', which no table holds as a key, takes the hash of 'from' */
  private static int l_takehash(lua_State L)
  {
    GCObject o = (GCObject) lua_topointer(L, 1);
    o.hash = ((GCObject) lua_topointer(L, 2)).hash;
    return 0;
  }

  private static final lua_CFunction l_takehash = EngineTests::l_takehash;

  /*
  ** chainsok(t): every key of t's hash part is in the chain that starts at
  ** its main position (where a lookup looks for it), and no chain loops
  */
  private static int l_chainsok(lua_State L)
  {
    Table t = table_at(L, 1);
    boolean ok = true;
    if (isdummy(t) == 0)
    {
      int size = sizenode(t);
      for (int i = 0; i < size && ok; i++)
      {
        Node n = gnode(t, i);
        if (keyisnil(n) || keyisdead(n))
          continue;
        TValue key = new TValue();
        getnodekey(L, key, n);
        int j = t.luaH_mainposition(key);
        for (int steps = 0; j != i; steps++)
        {
          int next = gnode(t, j).next;
          if (next == 0 || steps == size)
          {
            ok = false;
            break;
          }
          j += next;
        }
      }
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  private static final lua_CFunction l_chainsok = EngineTests::l_chainsok;

  /* misplace(t): a copy of t's layout whose first key is a new table with a
     hash whose main position is a free node with no chain (so no lookup
     reaches the slot it is put in) */
  private static int l_misplace(lua_State L)
  {
    Table src = table_at(L, 1);
    lua_createtable(L, 0, 0);
    Table t = table_at(L, -1);
    t.luaH_resize(L, 0, allocsizenode(src));
    boolean done = false;
    for (int i = 0; i < sizenode(src); i++)
    {
      Node n = gnode(src, i);
      TValue key = new TValue();
      boolean has = !keyisnil(n);
      if (has)
        getnodekey(L, key, n);
      if (has && !keyisdead(n) && !done)
      {
        lua_createtable(L, 0, 0);
        TValue nk = getslot(L, L.top - 1);
        Table k = hvalue(nk);
        for (int h = 1; h < 100000; h++)
        {
          k.hash = h;
          int mp = t.luaH_mainposition(nk);
          Node m = gnode(src, mp);
          if (mp != i && keyisnil(m) && m.next == 0)
            break;
        }
        copyobj(key, nk); /* on the stack until 't' holds it */
        done = true;
      }
      t.luaH_setnode(L, i, has ? key : null, keyisdead(n), n, n.next);
    }
    t.lastfree = src.lastfree;
    lua_pushvalue(L, 2);
    return 1;
  }

  private static final lua_CFunction l_misplace = EngineTests::l_misplace;

  /* layout(t): array size, node count, lastfree and length hint */
  private static int l_layout(lua_State L)
  {
    Table t = table_at(L, 1);
    lua_pushinteger(L, t.asize());
    lua_pushinteger(L, allocsizenode(t));
    lua_pushinteger(L, t.lastfree);
    lua_pushinteger(L, t.lenhint);
    return 4;
  }

  private static final lua_CFunction l_layout = EngineTests::l_layout;

  private static lua_State new_state()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    if (LUA_DEBUG)
    {
      luaL_requiref(L, new HostString("T"), Ltests.luaB_opentests, 1);
      lua_pop(L, 1);
    }
    lua_pushcfunction(L, l_copy);
    lua_setglobal(L, new HostString("copy"));
    lua_pushcfunction(L, l_layout);
    lua_setglobal(L, new HostString("layout"));
    lua_pushcfunction(L, l_setnodes);
    lua_setglobal(L, new HostString("setnodes"));
    lua_pushcfunction(L, l_copywith);
    lua_setglobal(L, new HostString("copywith"));
    lua_pushcfunction(L, l_chainsok);
    lua_setglobal(L, new HostString("chainsok"));
    lua_pushcfunction(L, l_misplace);
    lua_setglobal(L, new HostString("misplace"));
    lua_pushcfunction(L, l_hash);
    lua_setglobal(L, new HostString("hash"));
    lua_pushcfunction(L, l_takehash);
    lua_setglobal(L, new HostString("takehash"));
    lua_pushcfunction(L, l_steps);
    lua_setglobal(L, new HostString("steps"));
    return L;
  }

  /* helpers available to every case */
  private static final String prelude = "\n" + """
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
""";

  private static final class Case
  {
    final String name;
    final String code;

    Case(String name, String code)
    {
      this.name = name;
      this.code = code;
    }
  }

  private static final Case[] table_copy_cases = {
    new Case("a churned table keeps its order",
             "for seed = 1, 50 do local t = churned(64, seed) local c = copy(t) " +
             "assert(same(t, c), 'seed ' .. seed) end"),
    new Case("integer, float and boolean keys, and an array part with holes",
             "local t = {1, 2, nil, 4, [1.5] = 'f', [true] = 't', [false] = 'f', [-3] = 'n', [100] = 'h'} " +
             "t[2] = nil local c = copy(t) assert(same(t, c)) assert(#c == #t)"),
    new Case("dead keys keep the chains",
             "for seed = 1, 20 do local t = churned(100, seed) " +
             "for k in pairs(t) do if math.random() < 0.5 then t[k] = nil end end " +
             "collectgarbage() collectgarbage() local c = copy(t) assert(same(t, c), 'seed ' .. seed) end"),
    new Case("next from a deleted key (dead after a collection) continues as on the original",
             "for seed = 1, 20 do local t = churned(64, seed) local keys = {} " +
             "for k in pairs(t) do keys[#keys + 1] = k end " +
             "local gone = {} for i = 1, #keys, 3 do t[keys[i]] = nil gone[#gone + 1] = keys[i] end " +
             "collectgarbage() collectgarbage() " +
             "for _, c in ipairs{copy(t), setnodes(t)} do for _, k in ipairs(gone) do " +
             "local ok, nk = pcall(next, c, k) assert(ok, 'seed ' .. seed .. ': ' .. tostring(nk)) " +
             "assert(nk == next(t, k), 'seed ' .. seed) end end end"),
    new Case("object keys (same state, same addresses)",
             "local ks = {} local t = {} for i = 1, 50 do ks[i] = {} t[ks[i]] = i end " +
             "for i = 1, 50, 3 do t[ks[i]] = nil end local c = copy(t) assert(same(t, c))"),
    new Case("the metatable is copied",
             "local mt = {__index = function(_, k) return k .. '!' end} local t = setmetatable({a = 1}, mt) " +
             "local c = copy(t) assert(getmetatable(c) == mt and c.zzz == 'zzz!' and c.a == 1)"),
    new Case("a table filled by luaH_setnode works as a metatable",
             "local mt = setnodes({__index = function(_, k) return k .. '!' end, __mode = 'k', __len = function() return 7 end}) " +
             "local x = setmetatable({}, mt) assert(x.abc == 'abc!' and #x == 7) " +
             "local w = setmetatable({}, setnodes({__mode = 'k'})) w[{}] = true collectgarbage() assert(next(w) == nil)"),
    new Case("empty and array-only tables",
             "local e = copy({}) assert(next(e) == nil) local a = {} for i = 1, 100 do a[i] = i end " +
             "local c = copy(a) assert(same(a, c) and #c == 100)"),
    new Case("a copy keeps working: inserts, deletes, rehash",
             "local t = churned(64, 7) local c = copy(t) " +
             "for i = 1, 500 do c['more' .. i] = i end for i = 1, 500, 2 do c['more' .. i] = nil end " +
             "for k, v in pairs(t) do assert(c[k] == v) end for i = 2, 500, 2 do assert(c['more' .. i] == i) end " +
             "collectgarbage() heap()"),
    new Case("a large churned table",
             "local t = churned(5000, 3) local c = copy(t) assert(same(t, c)) collectgarbage() heap()"),
    new Case("copies survive collections in every mode",
             "for _, mode in ipairs{'incremental', 'generational'} do collectgarbage(mode) " +
             "local keep = {} for i = 1, 200 do local t = churned(32, i) keep[i] = {t, copy(t)} " +
             "if i % 20 == 0 then collectgarbage('step') end end collectgarbage() heap() " +
             "for i = 1, 200 do assert(same(keep[i][1], keep[i][2])) end end collectgarbage('incremental')"),
  };

  private static void run_cases(String group, Case[] cases, int n)
  {
    printf("  %s\n", group);
    for (int i = 0; i < n; i++)
    {
      lua_State L = new_state();
      g_test_failures = 0;
      String code = prelude + cases[i].code;
      if (luaL_dostring(L, new HostString(code)) != LUA_OK)
      {
        ByteView m = lua_tolstring(L, -1);
        printf("      error: %s\n", m != null ? m.substr(0, m.find(0)) : "?");
        CHECK(false, "false");
      }
      else
        CHECK(true, "true");
      printf("    %s ... %s\n", cases[i].name, g_test_failures == 0 ? "OK" : "FAIL");
      lua_close(L);
    }
  }

  /*
  ** Object keys (tables, functions, threads, userdata) hash by the hash each
  ** object gets when it is created, not by its address.
  */

  private static final class PlainAlloc implements lua_Alloc
  {
    @Override
    public boolean permit(long osize, long nsize, int tag)
    {
      return true;
    }
  }

  private static final PlainAlloc plain_alloc = new PlainAlloc();

  /* a state with a given seed (the debug build's test library needs its own
     allocator, so it is not opened here) */
  private static lua_State seeded_state(int seed)
  {
    lua_State L = lua_newstate(plain_alloc, seed);
    luaL_openlibs(L);
    lua_pushcfunction(L, l_layout);
    lua_setglobal(L, new HostString("layout"));
    return L;
  }

  /* runs 'code' (prelude included) and returns its result as a string */
  private static String run(lua_State L, String code)
  {
    String chunk = prelude + code;
    if (luaL_dostring(L, new HostString(chunk)) != LUA_OK)
    {
      ByteView m = lua_tolstring(L, -1);
      printf("      error: %s\n", m != null ? m.substr(0, m.find(0)) : "?");
      return "<error>";
    }
    ByteView r = lua_gettop(L) > 0 ? lua_tolstring(L, -1) : null;
    String out = r != null ? r.toString() : "<nil>";
    lua_settop(L, 0);
    return out;
  }

  /* a table whose keys are objects of every kind, each named in 'names' */
  private static final String object_keys =
    "names = {} t = {} " +
    "for i = 1, 300 do " +
    "  local k = (i % 3 == 0) and {} or (i % 3 == 1) and function() return i end " +
    "            or coroutine.create(function() end) " +
    "  names[k] = 'k' .. i t[k] = i " +
    "end " +
    "for k in pairs(t) do if t[k] % 7 == 0 then t[k] = nil end end ";

  /* object keys made among strings: their order does not depend on which
     strings a state already had (interning reuses them) */
  private static void test_order_independent_of_interned_strings()
  {
    printf("    object keys made among strings keep their order whatever strings exist ... ");
    g_test_failures = 0;
    String[] order = new String[2];
    for (int i = 0; i < 2; i++)
    {
      lua_State L = seeded_state(77);
      /* the same program, but the second state has the strings already */
      String keep = i == 0 ? "keep = {} for i = 1, 300 do keep[i] = 'other' .. i end return 'ok'"
                           : "keep = {} for i = 1, 300 do keep[i] = 'name' .. i end return 'ok'";
      CHECK(run(L, keep).equals("ok"), "run(L, keep) == \"ok\"");
      order[i] = run(L, "local t, names = {}, {} " +
                        "for i = 1, 300 do local n = 'name' .. i local k = {} t[k] = i names[k] = n end " +
                        "local o = {} for k in pairs(t) do o[#o + 1] = names[k] end return table.concat(o, ' ')");
      lua_close(L);
    }
    CHECK(order[0].equals(order[1]) && !order[0].equals("<error>"), "order[0] == order[1] && order[0] != \"<error>\"");
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  private static void test_same_order_in_two_states()
  {
    printf("    the same program and seed iterate object keys in the same order ... ");
    g_test_failures = 0;
    String[] order = new String[2];
    for (int i = 0; i < 2; i++)
    {
      lua_State L = seeded_state(1234); /* the second state's objects live elsewhere */
      CHECK(!run(L, object_keys).equals("<error>"), "run(L, object_keys) != \"<error>\"");
      order[i] = run(L, "local o = {} for k in pairs(t) do o[#o + 1] = names[k] end return table.concat(o, ' ')");
      lua_close(L);
    }
    CHECK(order[0].equals(order[1]) && !order[0].equals("<error>"), "order[0] == order[1] && order[0] != \"<error>\"");
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /* an engine C function hashes as a key by its name, not by its address
     (which another process has elsewhere): a copy of it at another address
     has the same main position */
  private static void test_cfunction_hash_by_name()
  {
    printf("    an engine C function as a key hashes by its name, not its address ... ");
    g_test_failures = 0;
    lua_State L = seeded_state(5);
    lua_createtable(L, 0, 4096);
    Table t = table_at(L, -1);
    int n = 0, same = 0;
    for (String name : new String[] {"base.next", "base.assert", "base.collectgarbage", "base.load", "base.ipairsaux",
                                     "table.tconcat", "table.tinsert", "table.sort"})
    {
      CFuncInfo f = lua_cfibyname(ByteView.of(name));
      CHECK(f != null, "f != nullptr");
      if (f == null)
        continue;
      CFuncInfo copy = new CFuncInfo(); /* (at another address) */
      copy.next = f.next;
      copy.tt = f.tt;
      copy.marked = f.marked;
      copy.owner = f.owner;
      copy.hash = f.hash;
      copy.fptr = f.fptr;
      TValue a = new TValue(), b = new TValue();
      setfvalue(a, f);
      setfvalue(b, copy);
      n++;
      if (t.luaH_mainposition(a) == t.luaH_mainposition(b))
        same++;
    }
    CHECK(n == 8 && same == n, "n == 8 && same == n");
    lua_close(L);
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /* a load takes one hash of the sequence, its function's, however many
     functions its chunk has: the parser's own tables (its anchor, a cache of
     constants for each function) are never a program's keys, and the
     sequence depends only on what the program makes */
  private static void test_load_takes_one_hash()
  {
    printf("    a load takes one hash of the sequence (its function's) ... ");
    g_test_failures = 0;
    lua_State L = seeded_state(9);
    for (String chunk : new String[] {"return 1", "local function a() return function() end end local b = function() end return a, b",
                                      "local t = {f = function() end, g = function() return function() end end} return t"})
    {
      int before = L.l_G.nexthash;
      CHECK(luaL_loadstring(L, new HostString(chunk)) == LUA_OK, "luaL_loadstring(L, chunk) == LUA_OK");
      CHECK(L.l_G.nexthash - before == HASHSTEP, "L->l_G->nexthash - before == lua::gc::HASHSTEP");
      lua_pop(L, 1);
    }
    lua_close(L);
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /* an allocator that refuses large growths when told to */
  private static final class DenyLargeAlloc implements lua_Alloc
  {
    boolean deny = false;

    @Override
    public boolean permit(long o, long n, int tag)
    {
      return !(deny && n > o && n - o >= 8192);
    }
  }

  /*
  ** Every engine C function is known by its name from the start, before the
  ** library function that hands it out is first called (a restore in a new
  ** process looks them up by name; before, utf8.codes' iterators and the
  ** C library handle's __gc were named at their first use, so a fresh process
  ** could not restore a state holding one)
  */
  private static void test_names_known_from_start()
  {
    printf("    engine C functions are named before their first use ... ");
    g_test_failures = 0;
    for (String name : new String[] {"utf8.iter_auxlax", "utf8.iter_auxstrict", "package.lib_gc", "utf8.iter_codes"})
      CHECK(lua_cfibyname(ByteView.of(name)) != null, "lua_cfibyname(byte_view(name)) != nullptr");
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /*
  ** The C++ heap of this program, as a host's allocator would see it: every
  ** new and delete goes through here ('heap_peak' is the most bytes live
  ** since it was last set)
  */
  private static long heap_live()
  {
    return ((com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean()).getCurrentThreadAllocatedBytes();
  }

  private static long heap_peak = 0;

  private static long heap_peak()
  {
    long live = heap_live();
    if (live > heap_peak)
      heap_peak = live;
    return heap_peak;
  }

  /* counts what the engine asks for, and refuses growth past 'cap' (0: none) */
  private static final class CountAlloc implements lua_Alloc
  {
    long used = 0;
    long peak = 0;
    long cap = 0;

    @Override
    public boolean permit(long o, long n, int tag)
    {
      if (n > o && cap != 0 && used + (n - o) > cap)
        return false;
      used = used + n - o;
      if (used > peak)
        peak = used;
      return true;
    }
  }

  /*
  ** A string's bytes are counted before they exist, and a result made in a
  ** buffer takes the buffer's bytes (B24): making a string holds of the heap
  ** what the allocator counts. Before, a result was copied out of its buffer
  ** (held twice, counted once), and a string's bytes were copied before they
  ** were counted (a string refused had been made first).
  */
  private static void test_string_bytes_counted()
  {
    printf("    a string's bytes are counted before they are made ... ");
    g_test_failures = 0;
    final long N = 8 << 20;
    CountAlloc a = new CountAlloc();
    lua_State L = lua_newstate(a, 1);
    luaL_openlibs(L);
    CHECK(run(L, "s = string.rep('a', 8 << 20) return 'ok'").equals("ok"), "run(L, \"s = string.rep('a', 8 << 20) return 'ok'\") == \"ok\"");
    String[] made = {"return #string.rep('x', 8 << 20)", "return #s:upper()", "return #s:reverse()"};
    for (String code : made)
    {
      lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
      long live0 = heap_live();
      long used0 = a.used;
      heap_peak = heap_live();
      a.peak = a.used;
      CHECK(run(L, code).equals(Long.toString(N)), "run(L, code) == std::to_string(N)");
      CHECK(heap_peak() - live0 < a.peak - used0 + N / 8, "heap_peak - live0 < a.peak - used0 + N / 8");
    }
    lua_gc(L, LUA_GCCOLLECT, 0, 0, 0);
    a.cap = a.used + N / 2; /* no room for a copy of 's' */
    long live0 = heap_live();
    heap_peak = heap_live();
    CHECK(run(L, "return tostring(pcall(string.sub, s, 2))").equals("false"), "run(L, \"return tostring(pcall(string.sub, s, 2))\") == \"false\"");
    CHECK(heap_peak() - live0 < N / 8, "heap_peak - live0 < N / 8");
    a.cap = 0;
    lua_close(L);
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /*
  ** The budget thread keeps the room its callbacks need (B16): a reservation
  ** that found no memory is made again the next time a budget is set, and a
  ** close of the thread ('coroutine.close') keeps it
  */
  private static void test_budget_thread_room()
  {
    printf("    the budget thread keeps the room of its callbacks ... ");
    g_test_failures = 0;
    DenyLargeAlloc a = new DenyLargeAlloc();
    lua_State L = lua_newstate(a, 1);
    luaL_openlibs(L);
    CHECK(run(L, "co = coroutine.create(function() end) return 'ok'").equals("ok"), "run(L, \"co = coroutine.create(function() end) return 'ok'\") == \"ok\"");
    a.deny = true;
    CHECK(run(L, "return tostring(pcall(debug.setbudget, co, 10, function() end))").equals("false"), "run(L, \"return tostring(pcall(debug.setbudget, co, 10, function() end))\") == \"false\"");
    a.deny = false;
    CHECK(run(L, "debug.setbudget(co, 10, function() end) return 'ok'").equals("ok"), "run(L, \"debug.setbudget(co, 10, function() end) return 'ok'\") == \"ok\"");
    lua_State B = L.l_G.budgetth;
    CHECK(B != null && stacksize(B) >= 1000 && B.nci >= 40, "B != nullptr && stacksize(B) >= 1000 && B->nci >= 40");
    CHECK(run(L, "coroutine.close(debug.getregistry()._BUDGETTHREAD) return 'ok'").equals("ok"), "run(L, \"coroutine.close(debug.getregistry()._BUDGETTHREAD) return 'ok'\") == \"ok\"");
    CHECK(B != null && B == L.l_G.budgetth && stacksize(B) >= 1000, "B != nullptr && B == L->l_G->budgetth && stacksize(B) >= 1000");
    lua_close(L);
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /*
  ** luaC_fullgcdue: a whole cycle that makes no '__gc' call (neither those
  ** it finds due nor those of a cycle under way: the collector's next step
  ** makes them, in order), frees what is dead and clears weak tables; in
  ** both modes of the collector
  */
  private static void test_fullgc_due()
  {
    printf("    a whole cycle leaves its __gc calls to the next step ... ");
    g_test_failures = 0;
    String[] modes = {"incremental", "generational"};
    for (String mode : modes)
      for (int underway = 0; underway < 2; underway++)
      {
        lua_State L = luaL_newstate();
        luaL_openlibs(L);
        String prog = "collectgarbage('" + mode + "') live = {} for i = 1, 20000 do live[i] = {} end " +
                      "fins = {} for i = 1, 3 do setmetatable({}, {__gc = function() fins[#fins + 1] = i end}) end " +
                      "weak = setmetatable({}, {__mode = 'k'}) weak[{}] = 1 " +
                      (underway != 0 ? "collectgarbage('step', 1) " : "") + "return 'ok'";
        CHECK(run(L, prog).equals("ok"), "run(L, prog.c_str()) == \"ok\"");
        lua_lock(L);
        luaC_fullgcdue(L);
        lua_unlock(L);
        lua_getglobal(L, new HostString("fins")); /* (read with no allocation: no step) */
        CHECK(lua_rawlen(L, -1) == 0, "lua_rawlen(L, -1) == 0");
        lua_getglobal(L, new HostString("weak"));
        lua_pushnil(L);
        CHECK(lua_next(L, -2) == 0, "lua_next(L, -2) == 0");
        lua_settop(L, 0);
        CHECK(run(L, "return table.concat(fins, ' ') .. ' ' .. collectgarbage('incremental')").equals(
                "3 2 1 " + mode), "run(L, \"return table.concat(fins, ' ') .. ' ' .. collectgarbage('incremental')\") == std::string(\"3 2 1 \") + mode");
        lua_close(L);
      }
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  private static final Case[] chain_cases = {
    new Case("chains hold after inserts, deletes and rehash, for keys of every kind",
             "local t = {} for i = 1, 3000 do local k = (i % 4 == 0) and {} or (i % 4 == 1) and ('s' .. i) " +
             "or (i % 4 == 2) and i * 1.5 or function() end t[k] = i assert(chainsok(t), i) end " +
             "for k in pairs(t) do if math.random() < 0.4 then t[k] = nil end end collectgarbage() assert(chainsok(t))"),
    new Case("a misplaced key is detected",
             "local t = {} for i = 1, 5 do t[{}] = i end local u = misplace(t) assert(chainsok(t) and not chainsok(u))"),
  };

  private static final Case[] object_key_cases = {
    new Case("every object has a hash, distinct for objects made one after another",
             "local seen = {} for i = 1, 1000 do local h = hash({}) assert(not seen[h]) seen[h] = true end " +
             "assert(hash(print) ~= nil and hash(coroutine.running()) ~= nil)"),
    new Case("an object given another's hash takes its place in a copied layout",
             "local ks, t = {}, {} for i = 1, 50 do ks[i] = {} t[ks[i]] = i end for i = 1, 50, 4 do t[ks[i]] = nil end " +
             "local news = {} for i = 1, 50 do news[i] = {} takehash(news[i], ks[i]) end " +
             "local map = {} for i = 1, 50 do map[ks[i]] = news[i] end " +
             "local u = copywith(t, map) assert(chainsok(u)) " +
             "for i = 1, 50 do assert(u[news[i]] == t[ks[i]], i) end " +
             "local o1, o2 = {}, {} for k in pairs(t) do o1[#o1 + 1] = t[k] end for k in pairs(u) do o2[#o2 + 1] = u[k] end " +
             "assert(table.concat(o1, ' ') == table.concat(o2, ' ')) " +
             "for i = 1, 50 do u[news[i]] = i * 2 end for i = 1, 50 do assert(u[news[i]] == i * 2) end heap()"),
    new Case("object keys keep working: inserts, deletes, rehash, weak keys, every mode",
             "for _, mode in ipairs{'incremental', 'generational'} do collectgarbage(mode) " +
             "local ks, t = {}, {} for i = 1, 2000 do ks[i] = (i % 2 == 0) and {} or function() end t[ks[i]] = i end " +
             "for i = 1, 2000, 3 do t[ks[i]] = nil end for i = 1, 2000 do if i % 3 ~= 1 then assert(t[ks[i]] == i) end end " +
             "local w = setmetatable({}, {__mode = 'k'}) for i = 1, 500 do w[{}] = i end collectgarbage() assert(next(w) == nil) " +
             "local e = setmetatable({}, {__mode = 'k'}) for i = 1, 100 do e[ks[i]] = {ks[i]} end collectgarbage() " +
             "for i = 1, 100 do assert(e[ks[i]][1] == ks[i]) end heap() end collectgarbage('incremental')"),
    new Case("only objects that hash by identity take hashes: strings, new or reused, do not",
             "local a = {} local s1 = ('fresh'):rep(3) .. 'x' local s2 = 'reused' local f = function() end " +
             "local b = {} assert(steps(a, f) == 1 and steps(f, b) == 1, steps(a, b)) " +
             "local c = coroutine.create(print) local d = {} assert(steps(b, c) == 1 and steps(c, d) == 1)"),
    new Case("light C functions and light userdata still hash (by address)",
             "local t = {[print] = 1, [string.format] = 2} assert(t[print] == 1 and t[string.format] == 2)"),
  };

  /*
  ** Names of the functions the engine keeps in its state (lnames.cpp): they
  ** resolve both ways, and the engine's own continuations, light C functions
  ** and budget callback have them.
  */
  private static int test_cont(lua_State L, int status, long ctx)
  {
    return 0;
  }

  private static final lua_KFunction test_cont = EngineTests::test_cont;

  private static final KNamer knamer_test_cont = new KNamer("test.test_cont", test_cont);

  private static void test_budget(lua_State user_co, lua_State spender)
  {
  }

  private static final lua_Budget test_budget = EngineTests::test_budget;

  private static void test_function_names()
  {
    CHECK(lua_kname(test_cont) != null && lua_kname(test_cont).equals("test.test_cont"), "lua_kname(test_cont) != nullptr && std::string(lua_kname(test_cont)) == \"test.test_cont\"");
    CHECK(lua_kbyname(ByteView.of("test.test_cont")) == test_cont, "lua_kbyname(\"test.test_cont\") == test_cont");
    CHECK(lua_kbyname(ByteView.of("test.nothing")) == null && lua_kname(null) == null, "lua_kbyname(\"test.nothing\") == nullptr && lua_kname(nullptr) == nullptr");
    /* a host names its own through the public function */
    lua_registerk("host.my_cont", test_cont);
    CHECK(lua_kbyname(ByteView.of("host.my_cont")) == test_cont, "lua_kbyname(\"host.my_cont\") == test_cont");
    lua_registerbudget("host.my_budget", test_budget);
    CHECK(lua_budgetbyname(ByteView.of("host.my_budget")) == test_budget && lua_budgetname(test_budget).equals("host.my_budget"), "lua_budgetbyname(\"host.my_budget\") == test_budget && std::string(lua_budgetname(test_budget)) == \"host.my_budget\"");
    /* the continuations of the libraries */
    for (String n : new String[] {"base.finishpcall", "base.printk", "base.load_cont", "coroutine.coresume_cont",
                                  "coroutine.auxwrap_cont", "string.gsubk", "string.formatk", "table.sortk",
                                  "table.tinsertk_loop", "math.min_cont", "gc.gcdrain_cont", "auxlib.requireff",
                                  "debug.thaw_cont", "package.ll_requirek"})
    {
      lua_KFunction k = lua_kbyname(ByteView.of(n));
      CHECK(k != null, "k != nullptr");
      CHECK(k != null && lua_kname(k).equals(n), "k != nullptr && std::string(lua_kname(k)) == n");
    }
    /* the light C functions the engine pushes itself, and one of a library */
    CHECK(lua_cfibyname(ByteView.of("base.next")) != null, "lua_cfibyname(\"base.next\") != nullptr");
    CHECK(lua_cfibyname(ByteView.of("gc.gcdrain")) != null, "lua_cfibyname(\"gc.gcdrain\") != nullptr");
    CHECK(lua_cfibyname(ByteView.of("auxlib.requireff")) != null, "lua_cfibyname(\"auxlib.requireff\") != nullptr");
    CHECK(lua_cfibyname(ByteView.of("nowhere.nothing")) == null, "lua_cfibyname(\"nowhere.nothing\") == nullptr");
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    /* a function of a library, as the script sees it, has its name */
    int bad = 0;
    for (String lib : new String[] {"string", "table", "math", "os", "io", "coroutine", "debug", "utf8"})
    {
      lua_getglobal(L, new HostString(lib));
      lua_pushnil(L);
      while (lua_next(L, -2) != 0)
      {
        if (lua_iscfunction(L, -1) != 0 && lua_islightcfunction(L, -1) != 0)
        {
          CFuncInfo cfi = (CFuncInfo) lua_topointer(L, -1);
          if (lua_cfiname(cfi) == null)
            bad++;
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    CHECK(bad == 0, "bad == 0");
    lua_close(L);
  }

  /*
  ** User objects that can be kept outside the process (lua_UserObject): a box
  ** saves its state and the factory of its kind makes an equal one.
  */
  private static void test_object_kinds()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    luaL_Box box = luaL_newbox(L);
    box.ints = new long[] {1, -2, 3, 1000000007};
    luaL_Buffer B = new luaL_Buffer();
    luaL_buffinit(L, B);
    luaL_addlstring(B, ByteView.of("bytes of a box"), 0, 14);
    luaL_buffsave(B, box);
    CHECK(box.persistkind().equals("box"), "std::string(box->persistkind()) == \"box\"");
    CHECK(box.persistuservalues(), "box->persistuservalues()"); /* (by default, its user values are part of its state) */
    long[] ints = new long[8];
    int n = box.persistints(ints);
    ByteView bytes = box.persistbytes();
    CHECK(n == 5 && ints[0] == 1 && ints[1] == -2 && ints[3] == 1000000007 && ints[4] == 14, "n == 5 && ints[0] == 1 && ints[1] == -2 && ints[3] == 1000000007 && ints[4] == 14");
    CHECK(bytes.equals(ByteView.of("bytes of a box")), "bytes == byte_view(\"bytes of a box\")");
    lua_ObjectFactory f = lua_objectkind(ByteView.of("box"));
    CHECK(f != null && lua_objectkind(ByteView.of("nothing")) == null, "f != nullptr && lua_objectkind(\"nothing\") == nullptr");
    lua_UserObject copy = f.call(L, ints, n, bytes);
    CHECK(copy != null, "copy != nullptr");
    luaL_Box c = (copy instanceof luaL_Box) ? (luaL_Box) copy : null;
    CHECK(c != null && Arrays.equals(c.ints, box.ints) && c.used == 14, "c != nullptr && c->ints == box->ints && c->used == 14");
    long[] ints2 = new long[8];
    CHECK(c != null && c.persistints(ints2) == 5 && c.persistbytes().equals(bytes), "c != nullptr && c->persistints(ints2) == 5 && c->persistbytes() == bytes");
    ints[4] = 3; /* the count of the bytes must be the bytes' */
    CHECK(f.call(L, ints, n, bytes) == null, "f(L, ints, n, bytes) == nullptr");
    CHECK(f.call(L, ints, 4, bytes) == null, "f(L, ints, 4, bytes) == nullptr");
    copy.freed(L); /* (made outside a userdata, it gives back what it holds itself) */
    copy = null;
    lua_close(L);
  }

  /*
  ** Objects made due by hand (luaC_makedue): their '__gc' run in the order
  ** given, even though they are alive and the collector is stopped; one whose
  ** group is gone is ordinary and runs none.
  */
  private static void test_make_due(boolean generational)
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    if (generational)
      CHECK(luaL_dostring(L, new HostString("collectgarbage('generational')")) == LUA_OK, "luaL_dostring(L, \"collectgarbage('generational')\") == LUA_OK");
    CHECK(luaL_dostring(L, new HostString("collectgarbage('stop') log = {} keep = {} local mt = {__gc = function(o) log[#log + 1] = o.name end} " +
                                          "for _, n in ipairs{'a', 'b', 'c', 'd'} do keep[n] = setmetatable({name = n}, mt) end")) == LUA_OK, "luaL_dostring(L, \"collectgarbage('stop') log = {} keep = {} ...\") == LUA_OK");
    GCObject[] objs = new GCObject[4];
    String[] names = {"c", "a", "d", "b"};
    for (int i = 0; i < 4; i++)
    {
      lua_getglobal(L, new HostString("keep"));
      lua_getfield(L, -1, new HostString(names[i]));
      objs[i] = (GCObject) lua_topointer(L, -1);
      lua_pop(L, 2);
    }
    objs[3].owner = 65535; /* its group is gone */
    /* (one more that is not marked for finalization: nothing happens to it) */
    CHECK(luaL_dostring(L, new HostString("plain = {name = 'plain'}")) == LUA_OK, "luaL_dostring(L, \"plain = {name = 'plain'}\") == LUA_OK");
    lua_getglobal(L, new HostString("plain"));
    GCObject notmarked = (GCObject) lua_topointer(L, -1);
    lua_pop(L, 1);
    GCObject[] given = {objs[0], objs[1], objs[2], objs[3], notmarked};
    luaC_makedue(L, given, 5);
    CHECK(luaL_dostring(L, new HostString("collectgarbage() return table.concat(log, ',')")) == LUA_OK, "luaL_dostring(L, \"collectgarbage() return table.concat(log, ',')\") == LUA_OK");
    CHECK(lua_tolstring(L, -1).toString().equals("c,a,d"), "std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()) == \"c,a,d\"");
    lua_close(L);
  }

  /*
  ** Objects a restore makes due for a group's '__gc' (luaC_makedue) are queued
  ** white, as the collector keeps its queues (B30): the steps a restore runs
  ** can have marked them black, and a queued object that is not white is not
  ** traversed by the next cycle, which collected its metatable (its '__gc' was
  ** then looked up in freed memory)
  */
  private static void test_make_due_queued_white()
  {
    printf("    objects made due for a group's __gc are queued white ... ");
    g_test_failures = 0;
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    CHECK(run(L, "collectgarbage('stop') log = {} keep = {} " +
                 "co = coroutine.create(function() " +
                 "  for i = 1, 3 do keep[i] = setmetatable({name = i}, {__gc = function(o) log[#log + 1] = o.name end}) end " +
                 "  coroutine.yield() local n = 0 for i = 1, 100 do n = n + i end " +
                 "  table.sort(log) return table.concat(log, ',') end) " +
                 "debug.setbudget(co, 1000000, function() end) coroutine.resume(co) return 'ok'").equals("ok"), "run(L, \"collectgarbage('stop') log = {} keep = {} ...\") == \"ok\"");
    GCObject[] objs = new GCObject[3];
    for (int i = 0; i < 3; i++)
    {
      lua_getglobal(L, new HostString("keep"));
      lua_geti(L, -1, i + 1);
      objs[i] = (GCObject) lua_topointer(L, -1);
      lua_pop(L, 2);
    }
    lua_lock(L);
    luaC_runtilstate(L, GCSenteratomic, 0); /* (all reachable is black) */
    lua_unlock(L);
    for (GCObject o : objs)
      CHECK(iswhite(o) == 0, "!lua::gc::iswhite(o)");
    CHECK(run(L, "keep = nil return 'ok'").equals("ok"), "run(L, \"keep = nil return 'ok'\") == \"ok\"");
    lua_lock(L);
    luaC_makedue(L, objs, 3);
    lua_unlock(L);
    for (GCObject o : objs)
      CHECK(iswhite(o) != 0, "lua::gc::iswhite(o)");
    lua_lock(L);
    luaC_fullgcdue(L);
    lua_unlock(L);
    CHECK(run(L, "return tostring(select(2, coroutine.resume(co)))").equals("1,2,3"), "run(L, \"return tostring(select(2, coroutine.resume(co)))\") == \"1,2,3\"");
    lua_close(L);
    printf("%s\n", g_test_failures == 0 ? "OK" : "FAIL");
  }

  /*
  ** Objects marked for finalization put in another order by hand
  ** (luaC_ordermarked), as if they had been marked in it: when they die
  ** together, their '__gc' run in the reverse of that order, as stock runs
  ** them in the reverse of their marking. An object in the list that is not
  ** marked is left alone.
  */
  private static void test_order_marked(boolean generational)
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    if (generational)
      CHECK(luaL_dostring(L, new HostString("collectgarbage('generational')")) == LUA_OK, "luaL_dostring(L, \"collectgarbage('generational')\") == LUA_OK");
    CHECK(luaL_dostring(L, new HostString("collectgarbage('stop') log = {} keep = {} local mt = {__gc = function(o) log[#log + 1] = o.name end} " +
                                          "for _, n in ipairs{'a', 'b', 'c', 'd'} do keep[n] = setmetatable({name = n}, mt) end " +
                                          "plain = {name = 'plain'}")) == LUA_OK, "luaL_dostring(L, \"collectgarbage('stop') log = {} keep = {} ...\") == LUA_OK");
    GCObject[] objs = new GCObject[5];
    String[] names = {"c", "a", "d", "b"};
    for (int i = 0; i < 4; i++)
    {
      lua_getglobal(L, new HostString("keep"));
      lua_getfield(L, -1, new HostString(names[i]));
      objs[i] = (GCObject) lua_topointer(L, -1);
      lua_pop(L, 2);
    }
    lua_getglobal(L, new HostString("plain"));
    objs[4] = (GCObject) lua_topointer(L, -1);
    lua_pop(L, 1);
    luaC_ordermarked(L, objs, 5);
    CHECK(luaL_dostring(L, new HostString("keep = nil plain = nil collectgarbage('restart') collectgarbage() collectgarbage() " +
                                          "return table.concat(log, ',')")) == LUA_OK, "luaL_dostring(L, \"keep = nil plain = nil ...\") == LUA_OK");
    CHECK(lua_tolstring(L, -1).toString().equals("b,d,a,c"), "std::string(lua_tolstring(L, -1)->data(), lua_tolstring(L, -1)->size()) == \"b,d,a,c\"");
    lua_close(L);
  }

  private static int main()
  {
    printf("\n\tStarting engine internals tests\n\n");
    test_names_known_from_start(); /* (first: before any library function runs) */
    run_cases("Table::luaH_copyfrom (ltable.cpp)", table_copy_cases,
              table_copy_cases.length);
    run_cases("Table::luaH_mainposition: keys sit in their chains (ltable.cpp)", chain_cases,
              chain_cases.length);
    run_cases("object keys hash by the object's hash (ltable.cpp, lgc.cpp)", object_key_cases,
              object_key_cases.length);
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
      printf("\n\tOK - engine: %d checks, 0 failures\n\n", g_checks);
    else
      printf("\n\tFAILED - engine: %d checks, %d failures\n\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
  }

  public static void main(String[] args)
  {
    exit(main());
  }
}
