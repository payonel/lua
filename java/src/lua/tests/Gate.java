/*
** The freeze gate: runs a Lua script on the main thread with a step
** budget whose callback freezes at every budget point, and thaws it from
** the host until it ends. The script must end as it does unbudgeted (the
** upstream suite and the deep tests are run this way: 'make gate').
**
** With '--styx <n>', the Styx gate: at every n-th freeze the host also
** persists the whole state (luaL_persiststate, from its other thread),
** restores it into a new state (luaL_restorestate), closes the old one
** and goes on in the new one. A freeze where the state holds what cannot
** be persisted (an open file) is not persisted: it is counted and the next
** freeze is tried. The script must end as it does with no persist ('make
** styx-gate'); the upstream suite runs in its portable mode ('_port').
** Each restore must keep every object: a census of the original's objects
** and of the restored state's, by type, must match (see 'census').
**
** usage: gate <steps> [--styx <n>] <script.lua> [args...]
*/
package lua.tests;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.lib.Linit.luaL_openlibs;
import static lua.lib.Lstyx.luaL_persiststate;
import static lua.lib.Lstyx.luaL_restorestate;
import static lua.object.Lnames.lua_registerbudget;
import static lua.object.Lobject.*;
import static lua.platform.OsApi.exit;
import static lua.platform.OsApi.os_fclose;
import static lua.platform.OsApi.os_fopen;
import static lua.platform.OsApi.os_fwrite;
import static lua.platform.OsApi.os_getenv;
import static lua.platform.OsApi.os_stderr;
import static lua.platform.OsApi.os_stdout;
import static lua.runtime.Ldo.lua_freeze;
import static lua.runtime.Ldo.lua_isfrozen;
import static lua.runtime.Ldo.lua_thaw;
import static lua.runtime.Lstate.lua_close;
import static lua.runtime.Lstate.lua_newthread;
import static lua.vm.Ldebug.lua_setbudget;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.function.Consumer;

import lua.api.HostString;
import lua.api.lua_Budget;
import lua.api.lua_CFunction;
import lua.api.lua_Writer;
import lua.bytes.ByteView;
import lua.lib.luaL_RestoreSetup;
import lua.lib.luaL_Restored;
import lua.lib.luaL_StyxResult;
import lua.object.BudgetGroup;
import lua.object.GCObject;
import lua.object.LClosure;
import lua.object.TValue;
import lua.object.UpVal;
import lua.object.Udata;
import lua.platform.OsFile;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class Gate
{
  private Gate()
  {
  }

  private static final OsFile stdout = os_stdout();
  private static final OsFile stderr = os_stderr();

  private static void fprintf(OsFile f, String fmt, Object... args)
  {
    os_fwrite(f, ByteView.of(String.format(Locale.ROOT, fmt, args)));
  }

  private static void printf(String fmt, Object... args)
  {
    fprintf(stdout, fmt, args);
  }

  private static String c_str(String s)
  {
    int z = s.indexOf('\0');
    return z < 0 ? s : s.substring(0, z);
  }

  private static long atol(String s)
  {
    int i = 0;
    while (i < s.length() && (s.charAt(i) == ' ' || (s.charAt(i) >= '\t' && s.charAt(i) <= '\r')))
      i++;
    boolean neg = false;
    if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-'))
      neg = s.charAt(i++) == '-';
    long v = 0;
    while (i < s.length() && s.charAt(i) >= '0' && s.charAt(i) <= '9')
      v = v * 10 + (s.charAt(i++) - '0');
    return neg ? -v : v;
  }

  private static long freezes = 0;

  private static String str(lua_State L, int idx)
  {
    ByteView s = lua_tolstring(L, idx);
    return s != null ? s.toString() : "(no message)";
  }

  private static void freezer(lua_State user_co, lua_State spender)
  {
    freezes++;
    lua_freeze(spender);
  }

  private static final lua_Budget freezer = Gate::freezer;

  /* the permanents: every C function and userdata reachable from the
     registry and the string metatable when the libraries are open, and the
     metatable of each such userdata (a library checks its userdata against
     it: 'luaL_checkudata'), named by its path (keys in sorted order, so that
     two states with the same libraries agree); kept in the registry
     (_GATEPERMS, object -> name), and the inverse (name -> object) is
     returned */
  private static final String perms_walk =
    "local perms, seen = {}, {} " +
    "local function upv(f) local _, x = debug.getupvalue(f, 1) return x end " +
    "local function walk(t, name) if seen[t] then return end seen[t] = true " +
    "  local keys = {} for k in pairs(t) do keys[#keys + 1] = k end " +
    "  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end) " +
    "  for _, k in ipairs(keys) do local v = t[k] local n = name .. '.' .. tostring(k) " +
    "    if (type(v) == 'function' and debug.getinfo(v, 'S').what == 'C' and type(upv(v)) ~= 'table') " +
    "       or type(v) == 'userdata' then if perms[v] == nil then perms[v] = n end " +
    "      local mt = type(v) == 'userdata' and debug.getmetatable(v) " +
    "      if mt and perms[mt] == nil then perms[mt] = n .. '<mt>' end " +
    "    elseif type(v) == 'table' then walk(v, n) end end " +
    "  local mt = getmetatable(t) if mt then walk(mt, name .. '<mt>') end end " +
    "walk(getmetatable(''), 'smt') walk(debug.getregistry(), 'R') " +
    "local back = {} for v, n in pairs(perms) do back[n] = v end " +
    "debug.getregistry()._GATEPERMS = perms return back";

  /* the message handler of the standalone interpreter: a traceback */
  private static int msghandler(lua_State L)
  {
    ByteView msg = lua_tolstring(L, 1);
    if (msg == null)
    {
      lua_pushstring(L, new HostString("(error object is not a string)"));
      return 1;
    }
    luaL_traceback(L, L, new HostString(msg.substr(0, msg.find(0))), 1);
    return 1;
  }

  private static final lua_CFunction msghandler = Gate::msghandler;

  /* what the host provides a state: the libraries, its message handler (in
     the registry, so that it has a name), and the permanents (the inverse
     left on the stack) */
  private static final class Provide implements luaL_RestoreSetup
  {
    @Override
    public void setup(lua_State L)
    {
      luaL_openlibs(L);
      lua_pushcfunction(L, msghandler);
      lua_setfield(L, LUA_REGISTRYINDEX, new HostString("_GATEMSGH"));
      if (luaL_loadstring(L, new HostString(perms_walk)) != LUA_OK)
        lua_error(L);
      lua_call(L, 0, 1);
    }
  }

  private static final class StringWriter implements lua_Writer
  {
    ByteArrayOutputStream out = new ByteArrayOutputStream();

    @Override
    public int write(lua_State L, ByteView b)
    {
      if (b != null)
        out.write(b.array(), b.offset(), b.size());
      return 0;
    }
  }

  /*
  ** The objects a state's collector keeps, by type (a census): live objects
  ** and dead ones waiting for their '__gc'. Taken after a whole cycle that
  ** calls no '__gc': the restore ends with one, and the original gets one
  ** (it is closed next). Not counted, as not persisted: an open upvalue no
  ** closure refers to any more (its thread keeps it until its frame
  ** returns), and in the original, the half-made parse of a stopped load,
  ** held by its box (the restored load makes it again as it parses its
  ** kept input again).
  */
  private static TreeMap<Integer, Long> census(lua_State L, boolean original)
  {
    global_State g = L.l_G;
    if (original)
    {
      for (GCObject o = g.allgc.next; o != null; o = o.next)
        if (o.tt == LUA_VUSERDATA)
        {
          Udata u = (Udata) o;
          String k = u.obj != null ? u.obj.persistkind() : null;
          if (k != null && k.equals("load"))
            for (TValue v : u.uv)
              setnilvalue(v);
        }
      lua.gc.Lgc.luaC_fullgcdue(L);
    }
    ArrayList<GCObject> all = new ArrayList<>();
    Consumer<GCObject> collect = o -> {
      for (; o != null; o = o.next)
        all.add(o);
    };
    collect.accept(g.allgc.next);
    collect.accept(g.finobj.next);
    collect.accept(g.tobefnz.next);
    collect.accept(g.fixedgc);
    for (BudgetGroup b : g.groupbyid)
      if (b != null)
        collect.accept(b.fnz.next);
    Set<GCObject> used = Collections.newSetFromMap(new IdentityHashMap<>()); /* (the upvalues the closures refer to) */
    for (GCObject o : all)
      if (o.tt == LUA_VLCL)
        for (UpVal u : ((LClosure) o).upvals)
          used.add(u);
    TreeMap<Integer, Long> n = new TreeMap<>();
    for (GCObject o : all)
      if (o.tt != LUA_VUPVAL || used.contains(o))
        n.merge((int) o.tt, 1L, Long::sum);
    return n;
  }

  private static long censuses = 0;
  private static long persists = 0;
  private static long unpersistable = 0;
  private static TreeMap<String, Long> why = new TreeMap<>(); /* what could not be persisted, and how often */
  private static TreeMap<String, String> first = new TreeMap<>(); /* the first such message in full */
  private static long largest = 0;

  /*
  ** Persist the state of 'L' (its main thread frozen) from the host's other
  ** thread '*H', restore it into a new state and close the old one: '*L' and
  ** '*H' are then the new state's. False if it could not be persisted.
  */
  private static boolean persist_restore(lua_State[] L, lua_State[] H)
  {
    StringWriter w = new StringWriter();
    lua_getfield(H[0], LUA_REGISTRYINDEX, new HostString("_GATEPERMS"));
    luaL_StyxResult res = luaL_persiststate(H[0], lua_gettop(H[0]), w);
    lua_pop(H[0], 1);
    if (res.status != LUA_OK)
    {
      String m = c_str(res.msg.to_utf8().toString());
      int at = m.indexOf(" (at ");
      String kind = at == -1 ? m : m.substring(0, at);
      long count = why.getOrDefault(kind, 0L);
      why.put(kind, count + 1);
      if (count == 0)
        first.put(kind, m);
      unpersistable++;
      return false;
    }
    if (w.out.size() > largest)
      largest = w.out.size();
    Provide provide = new Provide();
    luaL_Restored rd = luaL_restorestate(lua_getallocf(L[0]), new ByteView(w.out.toByteArray()), provide);
    lua_State H2 = rd.L;
    if (H2 == null)
    {
      fprintf(stderr, "gate: restore failed after %d freezes: %s\n", freezes, c_str(rd.msg.to_utf8().toString()));
      ByteView dump = os_getenv(ByteView.of("GATE_DUMP"));
      if (dump != null) /* (the stream, to look into) */
      {
        OsFile f = os_fopen(dump, ByteView.of("wb")).f;
        if (f != null)
        {
          os_fwrite(f, new ByteView(w.out.toByteArray()));
          os_fclose(f);
        }
      }
      exit(1);
    }
    if (lua_gc(H2, LUA_GCISRUNNING, 0, 0, 0) != 0)
    { /* (the same objects in the original and in the restored state) */
      TreeMap<Integer, Long> before = census(L[0], true), after = census(H2, false);
      if (!before.equals(after))
      {
        fprintf(stderr, "gate: a restore lost or gained objects after %d freezes (type: before after)\n", freezes);
        for (Map.Entry<Integer, Long> e : before.entrySet())
          if (after.getOrDefault(e.getKey(), 0L).longValue() != e.getValue().longValue())
            fprintf(stderr, "gate:   %d: %d %d\n", e.getKey(), e.getValue(), after.getOrDefault(e.getKey(), 0L));
        for (Map.Entry<Integer, Long> e : after.entrySet())
          if (!before.containsKey(e.getKey()))
            fprintf(stderr, "gate:   %d: 0 %d\n", e.getKey(), e.getValue());
        exit(1);
      }
      censuses++;
    }
    lua_close(L[0]);
    H[0] = H2;
    lua_pop(H2, 1); /* (its stack is as it was persisted: the permanents pushed for the persist too) */
    lua_rawgeti(H2, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    L[0] = lua_tothread(H2, -1);
    lua_pop(H2, 1);
    persists++;
    return true;
  }

  private static int main(int argc, String[] argv)
  {
    lua_registerbudget("gate.freezer", freezer);
    long every = 0; /* (persist and restore at every n-th freeze) */
    if (argc >= 5 && argv[2].equals("--styx"))
    {
      every = atol(argv[3]);
      for (int i = 4; i <= argc; i++) /* (drop the option) */
        argv[i - 2] = argv[i];
      argc -= 2;
    }
    if (argc < 3)
    {
      fprintf(stderr, "usage: %s <steps> [--styx <n>] <script.lua> [args...]\n", argv[0]);
      return 2;
    }
    long steps = atol(argv[1]);
    String script = argv[2];
    lua_State[] L = {luaL_newstate()};
    new Provide().setup(L[0]);
    lua_pop(L[0], 1);
    lua_State[] H = {lua_newthread(L[0])}; /* (the host's other thread, for a frozen main thread) */
    lua_setfield(L[0], LUA_REGISTRYINDEX, new HostString("_GATEHOST"));
    /* the 'arg' table, as the standalone interpreter makes it (the tests that
       run a child interpreter find it in arg[-1]: GATE_LUA, '../lua') */
    lua_createtable(L[0], argc - 3, 2);
    ByteView interp = os_getenv(ByteView.of("GATE_LUA"));
    lua_pushstring(L[0], interp != null ? new HostString(interp) : new HostString("../lua"));
    lua_rawseti(L[0], -2, -1);
    for (int i = 2; i < argc; i++)
    {
      lua_pushstring(L[0], new HostString(argv[i]));
      lua_rawseti(L[0], -2, i - 2);
    }
    lua_setglobal(L[0], new HostString("arg"));
    if (luaL_dostring(L[0], new HostString("_TMP = '_1'; GATE = true")) != LUA_OK)
      return 2;
    /* (the Styx gate runs the upstream suite in its portable mode: the C
       libraries its other tests load hold C functions that have no names,
       so no persist would be made after them; the freeze gate runs those) */
    if (every > 0 && luaL_dostring(L[0], new HostString("_port = true")) != LUA_OK)
      return 2;
    lua_warning(L[0], new HostString("@on"), 0);
    lua_getfield(L[0], LUA_REGISTRYINDEX, new HostString("_GATEMSGH"));
    int base = lua_gettop(L[0]);
    if (luaL_loadfile(L[0], new HostString(script)) != LUA_OK)
    {
      fprintf(stderr, "gate: %s\n", c_str(str(L[0], -1)));
      return 2;
    }
    for (int i = 3; i < argc; i++)
      lua_pushstring(L[0], new HostString(argv[i]));
    lua_setbudget(L[0], steps, freezer);
    int st = lua_pcall(L[0], argc - 3, 0, base);
    long thaws = 0;
    long next = every;
    while (st == LUA_OK && lua_isfrozen(L[0]) != 0)
    {
      if (every > 0 && freezes >= next)
      {
        next = freezes + (persist_restore(L, H) ? every : 1);
        if (lua_gettop(H[0]) != 0)
        { /* (what the host leaves on its thread is persisted with it, every time) */
          fprintf(stderr, "gate: the host's thread holds %d values after %d persists\n", lua_gettop(H[0]), persists);
          return 1;
        }
      }
      st = lua_thaw(L[0], null).status;
      thaws++;
    }
    lua_setbudget(L[0], 0, null);
    if (st != LUA_OK)
    {
      fprintf(stderr, "gate: failed after %d freezes: %s\n", freezes, c_str(str(L[0], -1)));
      return 1;
    }
    printf("gate: ok (%d steps per budget point, %d freezes, %d thaws)\n", steps, freezes, thaws);
    if (every > 0)
    {
      printf("gate: %d persists and restores (every %d freezes; largest %d bytes), " +
             "%d freezes not persisted\n", persists, every, largest, unpersistable);
      for (Map.Entry<String, Long> e : why.entrySet())
        printf("gate:   %d x %s (first: %s)\n", e.getValue(), e.getKey(), first.get(e.getKey()));
      printf("gate: census: %d of %d restores, every object kept\n", censuses, persists);
    }
    lua_close(L[0]);
    return 0;
  }

  public static void main(String[] args)
  {
    String[] argv = new String[args.length + 2];
    argv[0] = System.getProperty("lua.progname", "test_gate");
    System.arraycopy(args, 0, argv, 1, args.length);
    exit(main(args.length + 1, argv));
  }
}
