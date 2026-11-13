/*
** The freeze gate: runs a Lua script on the main thread with a step
** budget whose callback freezes at every budget point, and thaws it from
** the host until it ends. The script must end as it does unbudgeted (the
** upstream suite and the deep tests are run this way: 'make gate').
**
** usage: gate <steps> <script.lua> [args...]
*/
#pragma warning disable CS0162, CS0164, CS0168, CS0219, CS0649, CS0169, CS0414, CS8321
using System;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using lua_Budget = Payonel.LungoLua.Api.lua_Budget;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using OsFile = Payonel.LungoLua.Platform.OsFile;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;
using static Payonel.LungoLua.Lib.Linit;
using static Payonel.LungoLua.Object.Lnames;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lstate;
using static Payonel.LungoLua.Vm.Ldebug;

namespace Payonel.LungoLua.Tests;

public sealed class Gate
{
  private Gate()
  {
  }

  private static readonly OsFile stdout = os_stdout();
  private static readonly OsFile stderr = os_stderr();

  private static void fprintf(OsFile f, string fmt, params object[] args)
  {
    os_fwrite(f, ByteView.of(JString.format(Locale.ROOT, fmt, args)));
  }

  private static void printf(string fmt, params object[] args)
  {
    fprintf(stdout, fmt, args);
  }

  private static string c_str(string s)
  {
    int z = s.indexOf('\u0000');
    return z < 0 ? s : s.substring(0, z);
  }

  private static long atol(string s)
  {
    int i = 0;
    while (i < s.length() && (s.charAt(i) == ' ' || (s.charAt(i) >= '\t' && s.charAt(i) <= '\r')))
      i++;
    bool neg = false;
    if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-'))
      neg = s.charAt(i++) == '-';
    long v = 0;
    while (i < s.length() && s.charAt(i) >= '0' && s.charAt(i) <= '9')
      v = v * 10 + (s.charAt(i++) - '0');
    return neg ? -v : v;
  }

  private static long freezes = 0;

  private static string str(lua_State L, int idx)
  {
    ByteView s = lua_tolstring(L, idx);
    return s != null ? s.toString() : "(no message)";
  }

  private static void freezer(lua_State user_co, lua_State spender)
  {
    freezes++;
    lua_freeze(spender);
  }

  private static readonly lua_Budget freezer_fn = Gate.freezer;

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

  private static readonly lua_CFunction msghandler_fn = Gate.msghandler;

  private static int main(int argc, string[] argv)
  {
    lua_registerbudget("gate.freezer", freezer_fn);
    if (argc < 3)
    {
      fprintf(stderr, "usage: %s <steps> <script.lua> [args...]\n", argv[0]);
      return 2;
    }
    long steps = atol(argv[1]);
    string script = argv[2];
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, msghandler_fn);
    lua_setfield(L, LUA_REGISTRYINDEX, new HostString("_GATEMSGH"));
    /* the 'arg' table, as the standalone interpreter makes it (the tests that
       run a child interpreter find it in arg[-1]: GATE_LUA, '../java/lua') */
    lua_createtable(L, argc - 3, 2);
    ByteView interp = os_getenv(ByteView.of("GATE_LUA"));
    lua_pushstring(L, interp != null ? new HostString(interp) : new HostString("../java/lua"));
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
    int @base = lua_gettop(L);
    if (luaL_loadfile(L, new HostString(script)) != LUA_OK)
    {
      fprintf(stderr, "gate: %s\n", c_str(str(L, -1)));
      return 2;
    }
    for (int i = 3; i < argc; i++)
      lua_pushstring(L, new HostString(argv[i]));
    lua_setbudget(L, steps, freezer_fn);
    int st = lua_pcall(L, argc - 3, 0, @base);
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

  public static void main(string[] args)
  {
    string[] argv = new string[args.Length + 2];
    argv[0] = JSystem.getProperty("lua.progname", "test_gate");
    JSystem.arraycopy(args, 0, argv, 1, args.Length);
    exit(main(args.Length + 1, argv));
  }
}
