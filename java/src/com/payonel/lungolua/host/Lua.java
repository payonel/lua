package com.payonel.lungolua.host;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.api.LuaConf.*;
import static com.payonel.lungolua.lib.Lualib.*;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;
import static sun.misc.SignalHandler.SIG_DFL;

import java.io.IOException;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Optional;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_Debug;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteSpan;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.platform.OsLib;
import com.payonel.lungolua.platform.Readline.l_addhistT;
import com.payonel.lungolua.platform.Readline.l_readlineT;
import com.payonel.lungolua.runtime.lua_State;
import sun.misc.Signal;
import sun.misc.SignalHandler;

public final class Lua
{
  private Lua()
  {
  }

  private static final Signal SIGINT = new Signal("INT");

  private static final int EXIT_SUCCESS = 0;
  private static final int EXIT_FAILURE = 1;

  private static lua_State globalL = null;

  private static ByteView progname = Lt.LUA_PROGNAME;

  /*
  ** Use 'sigaction' when available.
  */
  private static void setsignal(Signal sig, SignalHandler handler)
  {
    try
    {
      Signal.handle(sig, handler); /* do not mask any signal */
    }
    catch (IllegalArgumentException e)
    {
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

  private static final lua_Hook lstop = Lua::lstop;

  /*
  ** Function to be called at a C signal. Because a C signal cannot
  ** just change a Lua state (as there is no proper synchronization),
  ** this function only sets a hook that, when called, will stop the
  ** interpreter.
  */
  private static void laction(Signal i)
  {
    int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
    setsignal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
    lua_sethook(globalL, lstop, flag, 1);
  }

  private static final SignalHandler laction = Lua::laction;

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

  private static final lua_CFunction msghandler = Lua::msghandler;

  /*
  ** Interface to 'lua_pcall', which sets appropriate message function
  ** and C-signal handler. Used to run all chunks.
  */
  private static int docall(lua_State L, int narg, int nres)
  {
    int status;
    int base = lua_gettop(L) - narg;  /* function index */
    lua_pushcfunction(L, msghandler); /* push message handler */
    lua_insert(L, base);              /* put it under function and args */
    globalL = L;                      /* to be available to 'laction' */
    setsignal(SIGINT, laction);       /* set C-signal handler */
    status = lua_pcall(L, narg, nres, base);
    setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
    lua_remove(L, base);        /* remove message handler from the stack */
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
  private static void createargtable(lua_State L, byte[][] argv, int argc, int script)
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

  private static int handle_script(lua_State L, byte[][] argv, int script)
  {
    int status;
    byte[] fname = argv[script];

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
  private static final int has_error = 1; /* bad option */
  private static final int has_i = 2; /* -i */
  private static final int has_v = 4; /* -v */
  private static final int has_e = 8; /* -e */
  private static final int has_E = 16; /* -E */

  /*
  ** Traverses all arguments from 'argv', returning a mask with those
  ** needed before running any Lua code or an error code if it finds any
  ** invalid argument. In case of error, 'first' is the index of the bad
  ** argument.  Otherwise, 'first' is -1 if there is no program name,
  ** 0 if there is no script name, or the index of the script name.
  */
  private static int collectargs(byte[][] argv, int[] first)
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
      switch (argv[i][1])
      {                         /* else check option */
      case '-':                 /* '--' */
        if (argv[i][2] != '\0') /* extra characters after '--'? */
          return has_error;     /* invalid option */
        /* if there is a script name, it comes after '--' */
        first[0] = (argv[i + 1] != null) ? i + 1 : 0;
        return args;
      case '\0':     /* '-' */
        return args; /* script "name" is '-' */
      case 'E':
        if (argv[i][2] != '\0') /* extra characters? */
          return has_error;     /* invalid option */
        args |= has_E;
        break;
      case 'W':
        if (argv[i][2] != '\0') /* extra characters? */
          return has_error;     /* invalid option */
        break;
      case 'i':
        args |= has_i; /* (-i implies -v) */ /* FALLTHROUGH */
      case 'v':
        if (argv[i][2] != '\0') /* extra characters? */
          return has_error;     /* invalid option */
        args |= has_v;
        break;
      case 'e':
        args |= has_e; /* FALLTHROUGH */
      case 'l':        /* both options need an argument */
        if (argv[i][2] == '\0')
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
  private static boolean runargs(lua_State L, byte[][] argv, int n)
  {
    int i;
    lua_warningi(L, Lt.off_1, 0); /* by default, Lua stand-alone has warnings off */
    for (i = 1; i < n; i++)
    {
      int option = argv[i][1];
      assert argv[i][0] == '-'; /* already checked */
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
          assert argv[i] != null;
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

  private static boolean l_noenv = false;

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
    InternalString einit = Optional.ofNullable(l_getenv(namev)).map(InternalString::new).orElse(null);
    if (einit == null)
    {
      namev = Lt.LUA_INIT;
      einit = Optional.ofNullable(l_getenv(namev)).map(InternalString::new).orElse(null); /* try alternative name */
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
  private static boolean lua_stdin_is_tty()
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

  private static boolean lua_readline(lua_State L, ByteView prompt)
  {
    if (l_readline != null)         /* is there a 'readline'? */
    {
      ByteView ret = l_readline.call(prompt); /* use it */
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
      l_addhist.call(line);  /* use it */
    /* else nothing to be done */
  }

  /* try to load 'readline' dynamically */


  private static void lua_initreadline(lua_State L)
  {
    InternalString erllib = Optional.ofNullable(l_getenv(Lt.LUA_RLLIB_VAR)).map(InternalString::new).orElse(null); /* name of readline library */
    ByteView rllib = (erllib != null) ? erllib.view() : Lt.LUA_READLINELIB;
    OsLib lib = os_dlopen(rllib, false);
    if (lib != null)
    {
      String[] name = (String[]) os_dlsym(lib, Lt.rl_readline_name);
      if (name != null)
        name[0] = "lua";
      l_readline = (l_readlineT) os_dlsym(lib, Lt.readline);
      l_addhist = (l_addhistT) os_dlsym(lib, Lt.add_history);
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
  private static InternalString get_prompt(lua_State L, boolean firstline)
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
  private static final ByteView EOFMARK = Lt.eof;

  /*
  ** Check whether 'status' signals a syntax error and the error
  ** message at the top of the stack ends with the above mark for
  ** incomplete statements.
  */
  private static boolean incomplete(lua_State L, int status)
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
  private static boolean pushline(lua_State L, boolean firstline)
  {
    InternalString prmt = get_prompt(L, firstline);
    boolean ready = lua_readline(L, prmt.view());
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

  private static final ByteView kw = Lt.local;
  private static final ByteView space = Lt.sp_tab;

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
    assert lua_gettop(L) == 1;
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
    byte[][] argv = (byte[][]) lua_touserdata(L, 2);
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

  private static final lua_CFunction pmain = Lua::pmain;

  public static int main(int argc, byte[][] argv)
  {
    int status;
    boolean result;
    lua_State L = luaL_newstate(); /* create state */
    if (L == null)
    {
      ByteView name = (argv[0] != null) ? ByteView.cstring(argv[0], 0) : Lt.empty_str;
      l_message(name, Lt.cannot_create_state_not_enough_memory);
      return EXIT_FAILURE;
    }
    lua_gc(L, LUA_GCSTOP, 0, 0, 0);          /* stop GC while building state */
    lua_pushcfunction(L, pmain);    /* to call 'pmain' in protected mode */
    lua_pushinteger(L, argc);       /* 1st argument */
    lua_pushlightuserdata(L, argv); /* 2nd argument */
    status = lua_pcall(L, 2, 1, 0); /* do the call */
    result = lua_toboolean(L, -1);  /* get result */
    report(L, status);
    lua_close(L);
    return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  private static byte[][] cmdline()
  {
    byte[] b;
    try
    {
      b = Files.readAllBytes(Path.of("/proc/self/cmdline"));
    }
    catch (IOException | RuntimeException e)
    {
      return null;
    }
    ArrayList<byte[]> entries = new ArrayList<>();
    int s = 0;
    for (int i = 0; i < b.length; i++)
    {
      if (b[i] == 0)
      {
        entries.add(Arrays.copyOfRange(b, s, i));
        s = i + 1;
      }
    }
    return entries.toArray(new byte[0][]);
  }

  private static Charset jnucharset()
  {
    try
    {
      return Charset.forName(System.getProperty("sun.jnu.encoding"));
    }
    catch (RuntimeException e)
    {
      return null;
    }
  }

  private static byte[][] rawargs(String progname, String[] args)
  {
    byte[][] cl = cmdline();
    Charset jnu = jnucharset();
    if (cl == null || jnu == null || cl.length <= args.length)
      return null;
    int first = cl.length - args.length;
    byte[][] raw = new byte[args.length + 1][];
    for (int i = 0; i < args.length; i++)
    {
      if (!new String(cl[first + i], jnu).equals(args[i]))
        return null;
      raw[i + 1] = cl[first + i];
    }
    byte[] option = "-Dlua.progname=".getBytes(StandardCharsets.US_ASCII);
    for (int i = 0; i < first; i++)
    {
      byte[] e = cl[i];
      if (e.length >= option.length && Arrays.equals(e, 0, option.length, option, 0, option.length) &&
          new String(e, option.length, e.length - option.length, jnu).equals(progname))
        raw[0] = Arrays.copyOfRange(e, option.length, e.length);
    }
    return raw;
  }

  public static void main(String[] args)
  {
    setsignal(SIGINT, SIG_DFL);
    String progname = System.getProperty("lua.progname", "lua");
    byte[][] raw = rawargs(progname, args);
    byte[][] argv = new byte[args.length + 2][];
    for (int i = 0; i <= args.length; i++)
    {
      ByteView a;
      if (raw != null && raw[i] != null)
        a = new ByteView(raw[i]);
      else
        a = new HostString((i == 0) ? progname : args[i - 1]).to_utf8();
      argv[i] = Arrays.copyOfRange(a.array(), a.offset(), a.offset() + a.size() + 1);
    }
    System.exit(main(args.length + 1, argv));
  }
}
