using System;
using System.Runtime.InteropServices;
using Payonel.LungoLua.Compat;
using HostString = Payonel.LungoLua.Api.HostString;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_Debug = Payonel.LungoLua.Api.lua_Debug;
using lua_Hook = Payonel.LungoLua.Api.lua_Hook;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteSpan = Payonel.LungoLua.Bytes.ByteSpan;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using Lt = Payonel.LungoLua.Bytes.Lt;
using OsLib = Payonel.LungoLua.Platform.OsLib;
using l_addhistT = Payonel.LungoLua.Platform.Readline.l_addhistT;
using l_readlineT = Payonel.LungoLua.Platform.Readline.l_readlineT;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;
using static Payonel.LungoLua.Api.LuaConf;
using static Payonel.LungoLua.Lib.Lualib;
using static Payonel.LungoLua.Platform.OsApi;
using static Payonel.LungoLua.Runtime.Lhack;
using static Payonel.LungoLua.Runtime.Lstate;
using static Payonel.LungoLua.Vm.Ldebug;

namespace Payonel.LungoLua.Host;

public sealed class Lua
{
  private Lua()
  {
  }

  private const PosixSignal SIGINT = PosixSignal.SIGINT;

  /* the default action: no handler of the program's */
  private static readonly Action<PosixSignal> SIG_DFL = null;

  /* the handler set, if not the default */
  private static PosixSignalRegistration sigint = null;

  private const int EXIT_SUCCESS = 0;
  private const int EXIT_FAILURE = 1;

  private static lua_State globalL = null;

  private static ByteView progname = Lt.LUA_PROGNAME;

  /*
  ** Use 'sigaction' when available.
  */
  private static void setsignal(PosixSignal sig, Action<PosixSignal> handler)
  {
    lock (typeof(Lua))
    {
      sigint?.Dispose(); /* no registration: .NET takes the default action (the process ends by the signal) */
      sigint = null;
      try
      {
        if (handler != SIG_DFL)
          sigint = PosixSignalRegistration.Create(sig, c => { c.Cancel = true; handler(c.Signal); }); /* do not mask any signal */
      }
      catch (Exception e) when (e is PlatformNotSupportedException || e is System.IO.IOException)
      {
      }
    }
  }

  /*
  ** Hook set by signal function to stop the interpreter.
  */
  private static void lstop(lua_State L, lua_Debug ar)
  {
    lua_sethook(L, null, 0, 0); /* reset hook */
    luaL_errori(L, Lt.interrupted);
  }

  private static readonly lua_Hook lstop_fn = Lua.lstop;

  /*
  ** Function to be called at a C signal. Because a C signal cannot
  ** just change a Lua state (as there is no proper synchronization),
  ** this function only sets a hook that, when called, will stop the
  ** interpreter.
  */
  private static void laction(PosixSignal i)
  {
    int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
    setsignal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
    lua_sethook(globalL, lstop_fn, flag, 1);
  }

  private static readonly Action<PosixSignal> laction_fn = Lua.laction;

  private static void print_usage(ByteView badoption)
  {
    os_print_stderr(progname);
    os_print_stderr(Lt.colon_sp);
    if (badoption.size() > 1 && (badoption.at(1) == 'e' || badoption.at(1) == 'l'))
    {
      os_print_stderr(Lt.squote);
      os_print_stderr(badoption);
      os_print_stderr(Lt.needs_argument_n);
    }
    else
    {
      os_print_stderr(Lt.unrecognized_option);
      os_print_stderr(badoption);
      os_print_stderr(Lt.n_1);
    }
    os_print_stderr(Lt.usage);
    os_print_stderr(progname);
    os_print_stderr(Lt.usage_options);
    os_flush_stderr();
  }

  private static void l_message(ByteView pname, ByteView msg)
  {
    if (!pname.empty())
    {
      os_print_stderr(pname);
      os_print_stderr(Lt.colon_sp);
    }
    os_print_stderr(msg);
    os_print_stderr(Lt.n_3);
    os_flush_stderr();
  }

  /*
  ** Check whether 'status' is not OK and, if so, prints the error
  ** message on the top of the stack.
  */
  private static int report(lua_State L, int status)
  {
    if (status != LUA_OK)
    {
      ByteView msg = (lua_isstring(L, -1)) ? lua_toistring(L, -1).view() : Lt.error_message_not_a_string;
      l_message(progname, msg);
      lua_pop(L, 1); /* remove message */
    }
    return status;
  }

  /*
  ** Message handler used to run all chunks
  */
  private static int msghandler(lua_State L)
  {
    ByteView msg;
    if (lua_isstring(L, 1))
      msg = lua_toistring(L, 1).view();
    else
    {
      if (luaL_callmetai(L, 1, Lt.mm_tostring) && /* does it have a metamethod */
          lua_type(L, -1) == LUA_TSTRING)            /* that produces a string? */
        return 1;                                    /* that is the message */
      else
      {
        lua_pushistringref(L, lua_typenamei(L, lua_type(L, 1)));
        msg = lua_pushfstringi(L, Lt.error_object_is_a_s_value).view();
      }
    }
    luaL_tracebacki(L, L, msg, 1); /* append a standard traceback */
    return 1;                      /* return the traceback */
  }

  private static readonly lua_CFunction msghandler_fn = Lua.msghandler;

  /*
  ** Interface to 'lua_pcall', which sets appropriate message function
  ** and C-signal handler. Used to run all chunks.
  */
  private static int docall(lua_State L, int narg, int nres)
  {
    int status;
    int @base = lua_gettop(L) - narg;  /* function index */
    lua_pushcfunction(L, msghandler_fn); /* push message handler */
    lua_insert(L, @base);              /* put it under function and args */
    globalL = L;                      /* to be available to 'laction' */
    setsignal(SIGINT, laction_fn);       /* set C-signal handler */
    status = lua_pcall(L, narg, nres, @base);
    setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
    lua_remove(L, @base);        /* remove message handler from the stack */
    return status;
  }

  private static void print_version()
  {
    os_print_stdout(LUA_COPYRIGHT);
    os_print_stdout(Lt.n_3);
    os_flush_stdout();
  }

  /*
  ** Create the 'arg' table, which stores all arguments from the
  ** command line ('argv'). It should be aligned so that, at index 0,
  ** it has 'argv[script]', which is the script name. The arguments
  ** to the script (everything after 'script') go to positive indices;
  ** other arguments (before the script name) go to negative indices.
  ** If there is no script name, assume interpreter's name as base.
  ** (If there is no interpreter's name either, 'script' is -1, so
  ** table sizes are zero.)
  */
  private static void createargtable(lua_State L, sbyte[][] argv, int argc, int script)
  {
    int i, narg;
    narg = argc - (script + 1); /* number of positive indices */
    lua_createtable(L, narg, script + 1);
    for (i = 0; i < argc; i++)
    {
      ByteView arg = ByteView.cstring(argv[i], 0);
      lua_pushlstring(L, arg, 0, arg.size());
      lua_rawseti(L, -2, i - script);
    }
    lua_setglobali(L, Lt.arg);
  }

  private static int dochunk(lua_State L, int status)
  {
    if (status == LUA_OK)
      status = docall(L, 0, 0);
    return report(L, status);
  }

  private static int dofile(lua_State L, ByteView name)
  {
    return dochunk(L, luaL_loadfilei(L, name, null));
  }

  private static int dostring(lua_State L, ByteView s, ByteView name)
  {
    return dochunk(L, luaL_loadbufferi(L, s, name, Lt.t_char));
  }

  /*
  ** Receives 'globname[=modname]' and runs 'globname = require(modname)'.
  ** If there is no explicit modname and globname contains a '-', cut
  ** the suffix after '-' (the "version") to make the global name.
  */
  private static int dolibrary(lua_State L, ByteView globname)
  {
    int status;
    ByteView modname = globname;
    int suffix = ByteView.npos;
    int eq = globname.find('=');
    if (eq == ByteView.npos)
    {                 /* no explicit name? */
      /* module name is equal to global name */
      suffix = globname.find(Lt.minus_ch); /* look for a suffix mark */
    }
    else
    {
      globname = globname.substr(0, eq); /* global name ends here */
      modname = modname.substr(eq + 1); /* module name starts after the '=' */
    }
    lua_getglobali(L, Lt.require);
    lua_pushlstring(L, modname, 0, modname.size());
    status = docall(L, 1, 1); /* call 'require(modname)' */
    if (status == LUA_OK)
    {
      if (suffix != ByteView.npos)           /* is there a suffix mark? */
        globname = globname.substr(0, suffix); /* remove suffix from global name */
      lua_setglobali(L, globname);  /* globname = require(modname) */
    }
    return report(L, status);
  }

  /*
  ** Push on the stack the contents of table 'arg' from 1 to #arg
  */
  private static int pushargs(lua_State L)
  {
    int i, n;
    if (lua_getglobali(L, Lt.arg) != LUA_TTABLE)
      luaL_errori(L, Lt.arg_is_not_a_table);
    n = (int) luaL_len(L, -1);
    luaL_checkstacki(L, n + 3, Lt.too_many_arguments_to_script);
    for (i = 1; i <= n; i++)
      lua_rawgeti(L, -i, i);
    lua_remove(L, -i); /* remove table from the stack */
    return n;
  }

  private static int handle_script(lua_State L, sbyte[][] argv, int script)
  {
    int status;
    sbyte[] fname = argv[script];

    if (ByteView.cstring(fname, 0).equals(Lt.minus_ch) && !ByteView.cstring(argv[script - 1], 0).equals(Lt.dashes))
      fname = null; /* stdin */
    if (fname == null)
      status = luaL_loadfilei(L, null, null); /* stdin */
    else
    {
      status = luaL_loadfilei(L, ByteView.cstring(fname, 0), null);
    }
    if (status == LUA_OK)
    {
      int n = pushargs(L); /* push arguments to script */
      status = docall(L, n, LUA_MULTRET);
    }
    return report(L, status);
  }

  /* bits of various argument indicators in 'args' */
  private const int has_error = 1; /* bad option */
  private const int has_i = 2; /* -i */
  private const int has_v = 4; /* -v */
  private const int has_e = 8; /* -e */
  private const int has_E = 16; /* -E */

  /*
  ** Traverses all arguments from 'argv', returning a mask with those
  ** needed before running any Lua code or an error code if it finds any
  ** invalid argument. In case of error, 'first' is the index of the bad
  ** argument.  Otherwise, 'first' is -1 if there is no program name,
  ** 0 if there is no script name, or the index of the script name.
  */
  private static int collectargs(sbyte[][] argv, int[] first)
  {
    int args = 0;
    int i;
    if (argv[0] != null)
    {                       /* is there a program name? */
      if (argv[0][0] != 0)  /* not empty? */
        progname = ByteView.cstring(argv[0], 0); /* save it */
    }
    else
    { /* no program name */
      first[0] = -1;
      return 0;
    }
    for (i = 1; argv[i] != null; i++)
    { /* handle arguments */
      first[0] = i;
      if (argv[i][0] != '-') /* not an option? */
        return args;         /* stop handling options */
      switch ((int) argv[i][1])
      {                         /* else check option */
      case '-':                 /* '--' */
        if (argv[i][2] != '\u0000') /* extra characters after '--'? */
          return has_error;     /* invalid option */
        /* if there is a script name, it comes after '--' */
        first[0] = (argv[i + 1] != null) ? i + 1 : 0;
        return args;
      case '\u0000':     /* '-' */
        return args; /* script "name" is '-' */
      case 'E':
        if (argv[i][2] != '\u0000') /* extra characters? */
          return has_error;     /* invalid option */
        args |= has_E;
        break;
      case 'W':
        if (argv[i][2] != '\u0000') /* extra characters? */
          return has_error;     /* invalid option */
        break;
      case 'i':
        args |= has_i; /* (-i implies -v) */ /* FALLTHROUGH */
      goto case 'v'; case 'v':
        if (argv[i][2] != '\u0000') /* extra characters? */
          return has_error;     /* invalid option */
        args |= has_v;
        break;
      case 'e':
        args |= has_e; /* FALLTHROUGH */
      goto case 'l'; case 'l':        /* both options need an argument */
        if (argv[i][2] == '\u0000')
        {      /* no concatenated argument? */
          i++; /* try next 'argv' */
          if (argv[i] == null || argv[i][0] == '-')
            return has_error; /* no next argument or it is another option */
        }
        break;
      default: /* invalid option */
        return has_error;
      }
    }
    first[0] = 0; /* no script name */
    return args;
  }

  /*
  ** Processes options 'e' and 'l', which involve running Lua code, and
  ** 'W', which also affects the state.
  ** Returns 0 if some code raises an error.
  */
  private static bool runargs(lua_State L, sbyte[][] argv, int n)
  {
    int i;
    lua_warningi(L, Lt.off_1, 0); /* by default, Lua stand-alone has warnings off */
    for (i = 1; i < n; i++)
    {
      int option = argv[i][1];
      { if (JAssert.on && !( argv[i][0] == '-')) throw new AssertionError(); } /* already checked */
      switch (option)
      {
      case 'e':
      case 'l':
      {
        int status;
        HostString arg = new HostString(ByteView.cstring(argv[i], 0));
        ByteView argsv = arg.to_utf8();
        ByteView extra = argsv.substr(2); /* both options need an argument */
        if (extra.empty())
        {
          i++; /* try next 'argv' */
          { if (JAssert.on && !( argv[i] != null)) throw new AssertionError(); }
          extra = ByteView.cstring(argv[i], 0);
        }
        status = (option == 'e') ? dostring(L, extra, Lt.command_line) : dolibrary(L, extra);
        if (status != LUA_OK)
          return false;
        break;
      }
      case 'W':
        lua_warningi(L, Lt.on_1, 0); /* warnings on */
        break;
      }
    }
    return true;
  }

  private static bool l_noenv = false;

  private static ByteView l_getenv(ByteView name)
  {
    if (l_noenv)
      return null;
    return os_getenv(name);
  }

  private static int handle_luainit(lua_State L)
  {
    InternalString name = new InternalString(Lt.LUA_INIT).concat(LUA_VERSUFFIX);
    ByteView namev = name.view();
    InternalString einit = (l_getenv(namev) is ByteView v1) ? new InternalString(v1) : null;
    if (einit == null)
    {
      namev = Lt.LUA_INIT;
      einit = (l_getenv(namev) is ByteView v2) ? new InternalString(v2) : null; /* try alternative name */
    }
    if (einit == null)
      return LUA_OK;
    if (!einit.empty() && einit.byteAt(0) == '@')
    {
      InternalString skipAt = einit.substr(1);
      return dofile(L, skipAt.view());
    }
    else
    {
      InternalString chunkname = new InternalString(Lt.equals_ch).concat(namev);
      return dostring(L, einit.view(), chunkname.view());
    }
  }

  /*
  ** {==================================================================
  ** Read-Eval-Print Loop (REPL)
  ** ===================================================================
  */

  /*
  ** lua_stdin_is_tty detects whether the standard input is a 'tty' (that
  ** is, whether we're running lua interactively).
  */
  private static bool lua_stdin_is_tty()
  {
    return isatty(0);
  }

  /*
  ** * lua_initreadline initializes the readline system.
  ** * lua_readline defines how to show a prompt and then read a line from
  **   the standard input.
  ** * lua_saveline defines how to "save" a read line in a "history".
  ** * lua_freeline defines how to free a line read by lua_readline.
  */

  /* use dynamically loaded readline (or nothing) */

  /* pointer to 'readline' function (if any) */
  private static l_readlineT l_readline = null;

  /* pointer to 'add_history' function (if any) */
  private static l_addhistT l_addhist = null;

  private static bool lua_readline(lua_State L, ByteView prompt)
  {
    if (l_readline != null)         /* is there a 'readline'? */
    {
      ByteView ret = l_readline(prompt); /* use it */
      if (ret == null)
      {
        return false;
      }
      ByteView sv = ret;
      if (sv.ends_with('\n'))
        sv = sv.substr(0, sv.size() - 1);
      lua_pushlstring(L, sv, 0, sv.size());
      return true;
    }
    ByteOwner buff = new ByteOwner(LUA_MAXINPUT);
    os_print_stdout(prompt);
    os_flush_stdout();                       /* show prompt */
    if (!os_readline(os_stdin(), buff.span()))
    {
      return false;
    }
    ByteView content = buff.str_view();
    if (content.ends_with('\n'))
      content = content.substr(0, content.size() - 1);
    lua_pushlstring(L, content, 0, content.size());
    return true;
  }

  private static void lua_saveline(ByteView line)
  {
    if (l_addhist != null) /* is there an 'add_history'? */
      l_addhist(line);  /* use it */
    /* else nothing to be done */
  }

  /* try to load 'readline' dynamically */

  private static void lua_initreadline(lua_State L)
  {
    InternalString erllib = (l_getenv(Lt.LUA_RLLIB_VAR) is ByteView v3) ? new InternalString(v3) : null; /* name of readline library */
    ByteView rllib = (erllib != null) ? erllib.view() : Lt.LUA_READLINELIB;
    OsLib lib = os_dlopen(rllib, false);
    if (lib != null)
    {
      string[] name = (string[]) os_dlsym(lib, Lt.rl_readline_name);
      if (name != null)
        name[0] = "lua";
      l_readline = (l_readlineT) os_dlsym(lib, Lt.readline_fn);
      l_addhist = (l_addhistT) os_dlsym(lib, Lt.add_history_fn);
      if (l_readline != null) /* could load readline function? */
        return;               /* everything ok */
      /* else emit a warning */
    }
    lua_warningi(L, Lt.unable_to_load_readline_library, 1);
    lua_warningi(L, rllib, 1);
    lua_warningi(L, Lt.squote, 0);
  }

  /*
  ** Return the string to be used as a prompt by the interpreter. Leave
  ** the string (or nil, if using the default value) on the stack, to keep
  ** it anchored.
  */
  private static InternalString get_prompt(lua_State L, bool firstline)
  {
    InternalString prompt;
    if (lua_getglobali(L, ((firstline) ? Lt._PROMPT : Lt._PROMPT2)) == LUA_TNIL)
    {
      prompt = new InternalString((firstline) ? Lt.LUA_PROMPT : Lt.LUA_PROMPT2); /* use the default */
    }
    else
    { /* apply 'tostring' over the value */
      prompt = new InternalString(luaL_tolstring(L, -1));
      lua_remove(L, -2); /* remove original value */
    }
    return prompt;
  }

  /* mark in error messages for incomplete statements */
  private static readonly ByteView EOFMARK = Lt.eof;

  /*
  ** Check whether 'status' signals a syntax error and the error
  ** message at the top of the stack ends with the above mark for
  ** incomplete statements.
  */
  private static bool incomplete(lua_State L, int status)
  {
    if (status == LUA_ERRSYNTAX)
    {
      ByteView msgv = lua_tolstring(L, -1);
      if (msgv.ends_with(EOFMARK))
        return true;
    }
    return false; /* else... */
  }

  /*
  ** Prompt the user, read a line, and push it into the Lua stack.
  */
  private static bool pushline(lua_State L, bool firstline)
  {
    InternalString prmt = get_prompt(L, firstline);
    bool ready = lua_readline(L, prmt.view());
    if (!ready)
    {
      lua_pop(L, 1); /* remove prompt */
      return false;  /* no input */
    }
    lua_remove(L, -2); /* remove prompt */
    return true;
  }

  /*
  ** Try to compile line on the stack as 'return <line>;'; on return, stack
  ** has either compiled chunk or original line (if compilation failed).
  */
  private static int addreturn(lua_State L)
  {
    lua_pushvalue(L, -1);
    lua_pushfstringi(L, Lt.return_s);
    ByteView retline = lua_tolstring(L, -1);
    int status = luaL_loadbufferi(L, retline, Lt.stdin_1, Lt.t_char);
    if (status == LUA_OK)
      lua_remove(L, -2); /* remove modified line */
    else
      lua_pop(L, 2); /* pop result from 'luaL_loadbufferx' and modified line */
    return status;
  }

  private static readonly ByteView kw = Lt.local;
  private static readonly ByteView space = Lt.sp_tab;

  private static void checklocal(ByteView line)
  {
    int i = line.find_first_not_of(space); /* skip spaces */
    if (i == ByteView.npos)
      return;
    int j = i + kw.size();
    if (line.compare(i, kw.size(), kw) == 0 &&      /* "local"? */
        (j >= line.size() || line.at(j) == ' ' || line.at(j) == '\t'))
    {
      os_print_stderr(Lt.warning_locals_do_not_survive_across_lines_in);
      os_flush_stderr();
    }
  }

  /*
  ** Read multiple lines until a complete Lua statement or an error not
  ** for an incomplete statement. Start with first line already read in
  ** the stack.
  */
  private static int multiline(lua_State L)
  {
    ByteView line = lua_tolstring(L, 1); /* get first line */
    checklocal(line);
    for (;;)
    {                                                  /* repeat until gets a complete statement */
      int status = luaL_loadbufferi(L, line, Lt.stdin_1, Lt.t_char); /* try it */
      if (!incomplete(L, status) || !pushline(L, false))
        return status;                  /* should not or cannot try to add continuation line */
      lua_remove(L, -2);                /* remove error message (from incomplete line) */
      lua_pushistringref(L, Lt.n_3);    /* add newline... */
      lua_insert(L, -2);                /* ...between the two lines */
      lua_concat(L, 3);                 /* join them */
      line = lua_tolstring(L, 1);       /* get what is has */
    }
  }

  /*
  ** Read a line and try to load (compile) it first as an expression (by
  ** adding "return " in front of it) and second as a statement. Return
  ** the final status of load/call with the resulting function (if any)
  ** in the top of the stack.
  */
  private static int loadline(lua_State L)
  {
    int status;
    lua_settop(L, 0);
    if (!pushline(L, true))
      return -1;                           /* no input */
    if ((status = addreturn(L)) != LUA_OK) /* 'return ...' did not work? */
      status = multiline(L);               /* try as command, maybe with continuation lines */
    HostString hline = lua_tostring(L, 1);
    ByteView line = hline.to_utf8();
    if (!line.empty())    /* non empty? */
      lua_saveline(line); /* keep history */
    lua_remove(L, 1);     /* remove line from the stack */
    { if (JAssert.on && !( lua_gettop(L) == 1)) throw new AssertionError(); }
    return status;
  }

  /*
  ** Prints (calling the Lua 'print' function) any values on the stack
  */
  private static void l_print(lua_State L)
  {
    int n = lua_gettop(L);
    if (n > 0)
    { /* any result to be printed? */
      luaL_checkstacki(L, LUA_MINSTACK, Lt.too_many_results_to_print);
      lua_getglobali(L, Lt.print);
      lua_insert(L, 1);
      if (lua_pcall(L, n, 0, 0) != LUA_OK)
      {
        lua_pushvalue(L, -1);
        ByteView m = lua_pushfstringi(L, Lt.error_calling_print_s).view();
        l_message(progname, m);
      }
    }
  }

  /*
  ** Do the REPL: repeatedly read (load) a line, evaluate (call) it, and
  ** print any results.
  */
  private static void doREPL(lua_State L)
  {
    int status;
    ByteView oldprogname = progname;
    progname = Lt.empty_str; /* no 'progname' on errors in interactive mode */
    lua_initreadline(L);
    while ((status = loadline(L)) != -1)
    {
      if (status == LUA_OK)
        status = docall(L, 0, LUA_MULTRET);
      if (status == LUA_OK)
        l_print(L);
      else
        report(L, status);
    }
    lua_settop(L, 0); /* clear stack */
    os_print_stdout(Lt.n_3);
    os_flush_stdout();
    progname = oldprogname;
  }

  /* }================================================================== */

  /*
  ** Main body of stand-alone interpreter (to be called in protected mode).
  ** Reads the options and handles them all.
  */
  private static int pmain(lua_State L)
  {
    int argc = (int) lua_tointeger(L, 1);
    sbyte[][] argv = (sbyte[][]) lua_touserdata(L, 2);
    int[] script = new int[1];
    int args = collectargs(argv, script);
    int optlim = (script[0] > 0) ? script[0] : argc; /* first argv not an option */
    luaL_checkversion(L);                            /* check that interpreter has correct version */
    if (args == has_error)
    {                                                    /* bad arg? */
      print_usage(ByteView.cstring(argv[script[0]], 0)); /* 'script' has index of bad arg. */
      return 0;
    }
    if ((args & has_v) != 0) /* option '-v'? */
      print_version();
    if ((args & has_E) != 0)
    {                        /* option '-E'? */
      l_noenv = true; /* program will ignore environment variables */
      lua_pushboolean(L, true); /* signal for libraries to ignore env. vars. */
      lua_setfieldi(L, LUA_REGISTRYINDEX, Lt.LUA_NOENV);
    }
    luai_openlibs(L);                         /* open standard libraries */
    createargtable(L, argv, argc, script[0]); /* create table 'arg' */
    lua_gc(L, LUA_GCRESTART, 0, 0, 0);              /* start GC... */
    lua_gc(L, LUA_GCGEN, 0, 0, 0);                  /* ...in generational mode */
    if (handle_luainit(L) != LUA_OK)       /* run LUA_INIT */
      return 0;                            /* error running LUA_INIT */
    if (!runargs(L, argv, optlim))         /* execute arguments -e, -l, and -W */
      return 0;                            /* something failed */
    if (script[0] > 0)
    { /* execute main script (if there is one) */
      if (handle_script(L, argv, script[0]) != LUA_OK)
        return 0; /* interrupt in case of error */
    }
    if ((args & has_i) != 0) /* -i option? */
      doREPL(L);      /* do read-eval-print loop */
    else if (script[0] < 1 && (args & (has_e | has_v)) == 0)
    { /* no active option? */
      if (lua_stdin_is_tty())
      { /* running in interactive mode? */
        print_version();
        doREPL(L); /* do read-eval-print loop */
      }
      else
        dofile(L, null); /* executes stdin as a file */
    }
    lua_pushboolean(L, true); /* signal no errors */
    return 1;
  }

  private static readonly lua_CFunction pmain_fn = Lua.pmain;

  public static int main(int argc, sbyte[][] argv)
  {
    int status;
    bool result;
    lua_State L = luaL_newstate(); /* create state */
    if (L == null)
    {
      ByteView name = (argv[0] != null) ? ByteView.cstring(argv[0], 0) : Lt.empty_str;
      l_message(name, Lt.cannot_create_state_not_enough_memory);
      return EXIT_FAILURE;
    }
    lua_gc(L, LUA_GCSTOP, 0, 0, 0);          /* stop GC while building state */
    lua_pushcfunction(L, pmain_fn);    /* to call 'pmain' in protected mode */
    lua_pushinteger(L, argc);       /* 1st argument */
    lua_pushlightuserdata(L, argv); /* 2nd argument */
    status = lua_pcall(L, 2, 1, 0); /* do the call */
    result = lua_toboolean(L, -1);  /* get result */
    report(L, status);
    lua_close(L);
    return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  private static sbyte[][] cmdline()
  {
    sbyte[] b;
    try
    {
      b = (sbyte[]) (object) System.IO.File.ReadAllBytes("/proc/self/cmdline");
    }
    catch (Exception e)
    {
      return null;
    }
    ArrayList<sbyte[]> entries = new ();
    int s = 0;
    for (int i = 0; i < b.Length; i++)
    {
      if (b[i] == 0)
      {
        entries.add(JArrays.copyOfRange(b, s, i));
        s = i + 1;
      }
    }
    return entries.toArray(new sbyte[0][]);
  }

  private static System.Text.Encoding jnucharset()
  {
    return JCharsets.UTF_8; /* .NET decodes its arguments as UTF-8 */
  }

  private static sbyte[][] rawargs(string progname, string[] args)
  {
    sbyte[][] cl = cmdline();
    System.Text.Encoding jnu = jnucharset();
    if (cl == null || jnu == null || cl.Length <= args.Length)
      return null;
    int first = cl.Length - args.Length;
    sbyte[][] raw = new sbyte[args.Length + 1][];
    for (int i = 0; i < args.Length; i++)
    {
      if (! JString.of(cl[first + i], jnu).equals(args[i]))
        return null;
      raw[i + 1] = cl[first + i];
    }
    sbyte[] option = "-Dlua.progname=".getBytes(JCharsets.US_ASCII);
    for (int i = 0; i < first; i++)
    {
      sbyte[] e = cl[i];
      if (e.Length >= option.Length && JArrays.equals(e, 0, option.Length, option, 0, option.Length) &&
           JString.of(e, option.Length, e.Length - option.Length, jnu).equals(progname))
        raw[0] = JArrays.copyOfRange(e, option.Length, e.Length);
    }
    return raw;
  }

  public static void main(string[] args)
  {
    setsignal(SIGINT, SIG_DFL);
    string progname = JSystem.getProperty("lua.progname", "lua");
    sbyte[][] raw = rawargs(progname, args);
    sbyte[][] argv = new sbyte[args.Length + 2][];
    for (int i = 0; i <= args.Length; i++)
    {
      ByteView a;
      if (raw != null && raw[i] != null)
        a = new ByteView(raw[i]);
      else
        a = new HostString((i == 0) ? progname : args[i - 1]).to_utf8();
      argv[i] = JArrays.copyOfRange(a.array(), a.offset(), a.offset() + a.size() + 1);
    }
    JSystem.exit(main(args.Length + 1, argv));
  }
}
