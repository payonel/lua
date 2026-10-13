/*
** The freeze gate: runs a Lua script on the main thread with a step
** budget whose callback freezes at every budget point, and thaws it from
** the host until it ends. The script must end as it does unbudgeted (the
** upstream suite and the deep tests are run this way: 'make gate').
**
** usage: gate <steps> <script.lua> [args...]
*/
package com.payonel.lungolua.tests;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.lib.Linit.luaL_openlibs;
import static com.payonel.lungolua.object.Lnames.lua_registerbudget;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.platform.OsApi.exit;
import static com.payonel.lungolua.platform.OsApi.os_fwrite;
import static com.payonel.lungolua.platform.OsApi.os_getenv;
import static com.payonel.lungolua.platform.OsApi.os_stderr;
import static com.payonel.lungolua.platform.OsApi.os_stdout;
import static com.payonel.lungolua.runtime.Ldo.lua_freeze;
import static com.payonel.lungolua.runtime.Ldo.lua_isfrozen;
import static com.payonel.lungolua.runtime.Ldo.lua_thaw;
import static com.payonel.lungolua.runtime.Lstate.lua_close;
import static com.payonel.lungolua.vm.Ldebug.lua_setbudget;

import java.util.Locale;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.platform.OsFile;
import com.payonel.lungolua.runtime.lua_State;

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

  private static int main(int argc, String[] argv)
  {
    lua_registerbudget("gate.freezer", freezer);
    if (argc < 3)
    {
      fprintf(stderr, "usage: %s <steps> <script.lua> [args...]\n", argv[0]);
      return 2;
    }
    long steps = atol(argv[1]);
    String script = argv[2];
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, msghandler);
    lua_setfield(L, LUA_REGISTRYINDEX, new HostString("_GATEMSGH"));
    /* the 'arg' table, as the standalone interpreter makes it (the tests that
       run a child interpreter find it in arg[-1]: GATE_LUA, '../lua') */
    lua_createtable(L, argc - 3, 2);
    ByteView interp = os_getenv(ByteView.of("GATE_LUA"));
    lua_pushstring(L, interp != null ? new HostString(interp) : new HostString("../lua"));
    lua_rawseti(L, -2, -1);
    for (int i = 2; i < argc; i++)
    {
      lua_pushstring(L, new HostString(argv[i]));
      lua_rawseti(L, -2, i - 2);
    }
    lua_setglobal(L, new HostString("arg"));
    if (luaL_dostring(L, new HostString("_TMP = '_1'; GATE = true")) != LUA_OK)
      return 2;
    lua_warning(L, new HostString("@on"), 0);
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString("_GATEMSGH"));
    int base = lua_gettop(L);
    if (luaL_loadfile(L, new HostString(script)) != LUA_OK)
    {
      fprintf(stderr, "gate: %s\n", c_str(str(L, -1)));
      return 2;
    }
    for (int i = 3; i < argc; i++)
      lua_pushstring(L, new HostString(argv[i]));
    lua_setbudget(L, steps, freezer);
    int st = lua_pcall(L, argc - 3, 0, base);
    long thaws = 0;
    while (st == LUA_OK && lua_isfrozen(L))
    {
      st = lua_thaw(L, null).status;
      thaws++;
    }
    lua_setbudget(L, 0, null);
    if (st != LUA_OK)
    {
      fprintf(stderr, "gate: failed after %d freezes: %s\n", freezes, c_str(str(L, -1)));
      return 1;
    }
    printf("gate: ok (%d steps per budget point, %d freezes, %d thaws)\n", steps, freezes, thaws);
    lua_close(L);
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
