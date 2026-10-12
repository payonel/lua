/*
** $Id: lua.c $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "lua_internal.h"
#include "os_api.h"
#include "literals.h"
#include "internal_string.h"

namespace lua::host
{

static lua_State* globalL = nullptr;

static byte_view progname = lt::LUA_PROGNAME;

/*
** Use 'sigaction' when available.
*/
static void setsignal(int sig, void (*handler)(int))
{
  struct sigaction sa;
  sa.sa_handler = handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask); /* do not mask any signal */
  sigaction(sig, &sa, nullptr);
}

/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop(lua_State* L, lua_Debug*)
{
  lua_sethook(L, nullptr, 0, 0); /* reset hook */
  luaL_errori(L, lt::interrupted);
}

/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction(int i)
{
  int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
  setsignal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
  lua_sethook(globalL, lstop, flag, 1);
}

static void print_usage(byte_view badoption)
{
  os_print_stderr(progname);
  os_print_stderr(lt::colon_sp);
  if (badoption.size() > 1 && (badoption[1] == 'e' || badoption[1] == 'l'))
  {
    os_print_stderr(lt::squote);
    os_print_stderr(badoption);
    os_print_stderr(lt::needs_argument_n);
  }
  else
  {
    os_print_stderr(lt::unrecognized_option);
    os_print_stderr(badoption);
    os_print_stderr(lt::n_1);
  }
  os_print_stderr(lt::usage);
  os_print_stderr(progname);
  os_print_stderr(lt::usage_options);
  os_flush_stderr();
}

static void l_message(byte_view pname, byte_view msg)
{
  if (!pname.empty())
  {
    os_print_stderr(pname);
    os_print_stderr(lt::colon_sp);
  }
  os_print_stderr(msg);
  os_print_stderr(lt::n_3);
  os_flush_stderr();
}

/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack.
*/
static int report(lua_State* L, int status)
{
  if (status != LUA_OK)
  {
    byte_view msg = lua_isstring(L, -1) ? lua_toistring(L, -1)->view() : lt::error_message_not_a_string;
    l_message(progname, msg);
    lua_pop(L, 1); /* remove message */
  }
  return status;
}

/*
** Message handler used to run all chunks
*/
static int msghandler(lua_State* L)
{
  byte_view msg;
  if (lua_isstring(L, 1))
    msg = lua_toistring(L, 1)->view();
  else
  {
    if (luaL_callmetai(L, 1, lt::mm_tostring) && /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)      /* that produces a string? */
      return 1;                              /* that is the message */
    else
    {
      lua_pushistringref(L, lua_typenamei(L, lua_type(L, 1)));
      msg = lua_pushfstringi(L, lt::error_object_is_a_s_value)->view();
    }
  }
  luaL_tracebacki(L, L, msg, 1); /* append a standard traceback */
  return 1;                             /* return the traceback */
}

/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall(lua_State* L, int narg, int nres)
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

static void print_version(void)
{
  os_print_stdout(LUA_COPYRIGHT);
  os_print_stdout(lt::n_3);
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
static void createargtable(lua_State* L, char** argv, int argc, int script)
{
  int i, narg;
  narg = argc - (script + 1); /* number of positive indices */
  lua_createtable(L, narg, script + 1);
  for (i = 0; i < argc; i++)
  {
    byte_view arg{argv[i]};
    lua_pushlstring(L, arg, 0, arg.size());
    lua_rawseti(L, -2, i - script);
  }
  lua_setglobali(L, lt::arg);
}

static int dochunk(lua_State* L, int status)
{
  if (status == LUA_OK)
    status = docall(L, 0, 0);
  return report(L, status);
}

static int dofile(lua_State* L, std::optional<byte_view> name)
{
  return dochunk(L, luaL_loadfilei(L, name, std::nullopt));
}

static int dostring(lua_State* L, byte_view s, byte_view name)
{
  return dochunk(L, luaL_loadbufferi(L, s, name, lt::t_char));
}

/*
** Receives 'globname[=modname]' and runs 'globname = require(modname)'.
** If there is no explicit modname and globname contains a '-', cut
** the suffix after '-' (the "version") to make the global name.
*/
static int dolibrary(lua_State* L, byte_view globname)
{
  int status;
  byte_view modname = globname;
  size_t suffix = byte_view::npos;
  size_t eq = globname.find('=');
  if (eq == byte_view::npos)
  {                 /* no explicit name? */
    /* module name is equal to global name */
    suffix = globname.find(lt::minus_ch); /* look for a suffix mark */
  }
  else
  {
    globname = globname.substr(0, eq); /* global name ends here */
    modname = modname.substr(eq + 1); /* module name starts after the '=' */
  }
  lua_getglobali(L, lt::require);
  lua_pushlstring(L, modname, 0, modname.size());
  status = docall(L, 1, 1); /* call 'require(modname)' */
  if (status == LUA_OK)
  {
    if (suffix != byte_view::npos)         /* is there a suffix mark? */
      globname = globname.substr(0, suffix); /* remove suffix from global name */
    lua_setglobali(L, globname);  /* globname = require(modname) */
  }
  return report(L, status);
}

/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs(lua_State* L)
{
  int i, n;
  if (lua_getglobali(L, lt::arg) != LUA_TTABLE)
    luaL_errori(L, lt::arg_is_not_a_table);
  n = (int) luaL_len(L, -1);
  luaL_checkstacki(L, n + 3, lt::too_many_arguments_to_script);
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i); /* remove table from the stack */
  return n;
}

static int handle_script(lua_State* L, char** argv)
{
  int status;
  const char* fname = argv[0];

  if (byte_view(fname) == lt::minus_ch && byte_view(argv[-1]) != lt::dashes)
    fname = nullptr; /* stdin */
  if (fname == nullptr)
    status = luaL_loadfilei(L, std::nullopt, std::nullopt); /* stdin */
  else
  {
    status = luaL_loadfilei(L, byte_view{fname}, std::nullopt);
  }
  if (status == LUA_OK)
  {
    int n = pushargs(L); /* push arguments to script */
    status = docall(L, n, LUA_MULTRET);
  }
  return report(L, status);
}

/* bits of various argument indicators in 'args' */
constexpr int has_error = 1; /* bad option */
constexpr int has_i = 2; /* -i */
constexpr int has_v = 4; /* -v */
constexpr int has_e = 8; /* -e */
constexpr int has_E = 16; /* -E */

/*
** Traverses all arguments from 'argv', returning a mask with those
** needed before running any Lua code or an error code if it finds any
** invalid argument. In case of error, 'first' is the index of the bad
** argument.  Otherwise, 'first' is -1 if there is no program name,
** 0 if there is no script name, or the index of the script name.
*/
static int collectargs(char** argv, int* first)
{
  int args = 0;
  int i;
  if (argv[0] != nullptr)
  {                       /* is there a program name? */
    if (argv[0][0])       /* not empty? */
      progname = argv[0]; /* save it */
  }
  else
  { /* no program name */
    *first = -1;
    return 0;
  }
  for (i = 1; argv[i] != nullptr; i++)
  { /* handle arguments */
    *first = i;
    if (argv[i][0] != '-') /* not an option? */
      return args;         /* stop handling options */
    switch (argv[i][1])
    {                         /* else check option */
    case '-':                 /* '--' */
      if (argv[i][2] != '\0') /* extra characters after '--'? */
        return has_error;     /* invalid option */
      /* if there is a script name, it comes after '--' */
      *first = (argv[i + 1] != nullptr) ? i + 1 : 0;
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
        if (argv[i] == nullptr || argv[i][0] == '-')
          return has_error; /* no next argument or it is another option */
      }
      break;
    default: /* invalid option */
      return has_error;
    }
  }
  *first = 0; /* no script name */
  return args;
}

/*
** Processes options 'e' and 'l', which involve running Lua code, and
** 'W', which also affects the state.
** Returns 0 if some code raises an error.
*/
static int runargs(lua_State* L, char** argv, int n)
{
  int i;
  lua_warningi(L, lt::off_1, 0); /* by default, Lua stand-alone has warnings off */
  for (i = 1; i < n; i++)
  {
    int option = argv[i][1];
    assert(argv[i][0] == '-'); /* already checked */
    switch (option)
    {
    case 'e':
    case 'l':
    {
      int status;
      HostString arg(argv[i]);
      byte_view argsv = arg.to_utf8();
      byte_view extra = argsv.substr(2); /* both options need an argument */
      if (extra.empty())
      {
        i++; /* try next 'argv' */
        assert(argv[i] != nullptr);
        extra = argv[i];
      }
      status = (option == 'e') ? dostring(L, extra, lt::command_line) : dolibrary(L, extra);
      if (status != LUA_OK)
        return 0;
      break;
    }
    case 'W':
      lua_warningi(L, lt::on_1, 0); /* warnings on */
      break;
    }
  }
  return 1;
}

static bool l_noenv = false;

static std::optional<byte_view> l_getenv(byte_view name)
{
  if (l_noenv)
    return std::nullopt;
  return os_getenv(name);
}

static int handle_luainit(lua_State* L)
{
  InternalString name = InternalString(lt::LUA_INIT).concat(LUA_VERSUFFIX);
  byte_view namev = name.view();
  std::optional<InternalString> einit = l_getenv(namev);
  if (!einit.has_value())
  {
    namev = lt::LUA_INIT;
    einit = l_getenv(namev);      /* try alternative name */
  }
  if (!einit.has_value())
    return LUA_OK;
  if (!einit->empty() && einit->byteAt(0) == '@')
  {
    InternalString skipAt = einit->substr(1);
    return dofile(L, skipAt.view());
  }
  else
  {
    InternalString chunkname = InternalString(lt::equals_ch).concat(namev);
    return dostring(L, einit->view(), chunkname.view());
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
static inline int lua_stdin_is_tty(void)
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
typedef char* (*l_readlineT)(const char* prompt);
static l_readlineT l_readline = nullptr;

/* pointer to 'add_history' function (if any) */
typedef void (*l_addhistT)(const char* string);
static l_addhistT l_addhist = nullptr;

static bool lua_readline(byte_span buff, byte_view prompt)
{
  if (l_readline != nullptr)         /* is there a 'readline'? */
  {
    char* ret = (*l_readline)(std::string(prompt.data(), prompt.size()).c_str()); /* use it */
    if (ret == nullptr)
    {
      return false;
    }
    byte_view sv(ret);
    buff.apply(sv);
    free(ret);
  }
  else
  {
    os_print_stdout(prompt);
    os_flush_stdout();                       /* show prompt */
    if (!os_readline(os_stdin(), buff))
    {
      return false;
    }
  }

  byte_view content = buff.str_view();
  if (content.ends_with('\n'))
  {
    buff[content.size() - 1] = '\0';
  }

  return true;
}

static void lua_saveline(byte_view line)
{
  if (l_addhist != nullptr) /* is there an 'add_history'? */
    (*l_addhist)(std::string(line.data(), line.size()).c_str());  /* use it */
  /* else nothing to be done */
}

/* try to load 'readline' dynamically */


static void lua_initreadline(lua_State* L)
{
  std::optional<InternalString> erllib = l_getenv(lt::LUA_RLLIB_VAR); /* name of readline library */
  byte_view rllib = erllib.has_value() ? erllib->view() : lt::LUA_READLINELIB;
  OsLib* lib = os_dlopen(rllib, false);
  if (lib != nullptr)
  {
    const char** name = static_cast<const char**>(os_dlsym(lib, lt::rl_readline_name));
    if (name != nullptr)
      *name = "lua";
    l_readline = reinterpret_cast<l_readlineT>(os_dlsym(lib, lt::readline));
    l_addhist = reinterpret_cast<l_addhistT>(os_dlsym(lib, lt::add_history));
    if (l_readline != nullptr) /* could load readline function? */
      return;               /* everything ok */
    /* else emit a warning */
  }
  lua_warningi(L, lt::unable_to_load_readline_library, 1);
  lua_warningi(L, rllib, 1);
  lua_warningi(L, lt::squote, 0);
}

/*
** Return the string to be used as a prompt by the interpreter. Leave
** the string (or nil, if using the default value) on the stack, to keep
** it anchored.
*/
static InternalString get_prompt(lua_State* L, int firstline)
{
  InternalString prompt;
  if (lua_getglobali(L, (firstline ? lt::_PROMPT : lt::_PROMPT2)) == LUA_TNIL)
  {
    prompt = firstline ? lt::LUA_PROMPT : lt::LUA_PROMPT2; /* use the default */
  }
  else
  { /* apply 'tostring' over the value */
    prompt = InternalString(luaL_tolstring(L, -1));
    lua_remove(L, -2); /* remove original value */
  }
  return prompt;
}

/* mark in error messages for incomplete statements */
const byte_view EOFMARK = lt::eof;

/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete(lua_State* L, int status)
{
  if (status == LUA_ERRSYNTAX)
  {
    byte_view msgv = *lua_tolstring(L, -1);
    if (msgv.ends_with(EOFMARK))
      return 1;
  }
  return 0; /* else... */
}

/*
** Prompt the user, read a line, and push it into the Lua stack.
*/
static int pushline(lua_State* L, int firstline)
{
  byte_owner buffer(LUA_MAXINPUT);
  InternalString prmt = get_prompt(L, firstline);
  bool ready = lua_readline(buffer, prmt.view());
  lua_pop(L, 1); /* remove prompt */
  if (!ready)
    return 0; /* no input */
  byte_view buffview = buffer.str_view();
  lua_pushlstring(L, buffview, 0, buffview.size());
  return 1;
}

/*
** Try to compile line on the stack as 'return <line>;'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn(lua_State* L)
{
  lua_pushvalue(L, -1);
  lua_pushfstringi(L, lt::return_s);
  byte_view retline = *lua_tolstring(L, -1);
  int status = luaL_loadbufferi(L, retline, lt::stdin_1, lt::t_char);
  if (status == LUA_OK)
    lua_remove(L, -2); /* remove modified line */
  else
    lua_pop(L, 2); /* pop result from 'luaL_loadbufferx' and modified line */
  return status;
}

static void checklocal(byte_view line)
{
  static const byte_view kw = lt::local;
  static const byte_view space = lt::sp_tab;
  size_t i = line.find_first_not_of(space); /* skip spaces */
  if (i == byte_view::npos)
    return;
  size_t j = i + kw.size();
  if (line.compare(i, kw.size(), kw) == 0 &&      /* "local"? */
      (j >= line.size() || line[j] == ' ' || line[j] == '\t'))
  {
    os_print_stderr(lt::warning_locals_do_not_survive_across_lines_in);
    os_flush_stderr();
  }
}

/*
** Read multiple lines until a complete Lua statement or an error not
** for an incomplete statement. Start with first line already read in
** the stack.
*/
static int multiline(lua_State* L)
{
  byte_view line = *lua_tolstring(L, 1); /* get first line */
  checklocal(line);
  for (;;)
  {                                                  /* repeat until gets a complete statement */
    int status = luaL_loadbufferi(L, line, lt::stdin_1, lt::t_char); /* try it */
    if (!incomplete(L, status) || !pushline(L, 0))
      return status;                  /* should not or cannot try to add continuation line */
    lua_remove(L, -2);                /* remove error message (from incomplete line) */
    lua_pushistringref(L, lt::n_3);         /* add newline... */
    lua_insert(L, -2);                /* ...between the two lines */
    lua_concat(L, 3);                 /* join them */
    line = *lua_tolstring(L, 1);       /* get what is has */
  }
}

/*
** Read a line and try to load (compile) it first as an expression (by
** adding "return " in front of it) and second as a statement. Return
** the final status of load/call with the resulting function (if any)
** in the top of the stack.
*/
static int loadline(lua_State* L)
{
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;                           /* no input */
  if ((status = addreturn(L)) != LUA_OK) /* 'return ...' did not work? */
    status = multiline(L);               /* try as command, maybe with continuation lines */
  std::optional<HostString> hline = lua_tostring(L, 1);
  byte_view line = hline->to_utf8();
  if (!line.empty())            /* non empty? */
    lua_saveline(line); /* keep history */
  lua_remove(L, 1);     /* remove line from the stack */
  assert(lua_gettop(L) == 1);
  return status;
}

/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print(lua_State* L)
{
  int n = lua_gettop(L);
  if (n > 0)
  { /* any result to be printed? */
    luaL_checkstacki(L, LUA_MINSTACK, lt::too_many_results_to_print);
    lua_getglobali(L, lt::print);
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
    {
      lua_pushvalue(L, -1);
      byte_view m = lua_pushfstringi(L, lt::error_calling_print_s)->view();
      l_message(progname, m);
    }
  }
}

/*
** Do the REPL: repeatedly read (load) a line, evaluate (call) it, and
** print any results.
*/
static void doREPL(lua_State* L)
{
  int status;
  byte_view oldprogname = progname;
  progname = lt::empty_str; /* no 'progname' on errors in interactive mode */
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
  os_print_stdout(lt::n_3);
  os_flush_stdout();
  progname = oldprogname;
}

/* }================================================================== */

/*
** Main body of stand-alone interpreter (to be called in protected mode).
** Reads the options and handles them all.
*/
static int pmain(lua_State* L)
{
  int argc = (int) lua_tointeger(L, 1);
  char** argv = static_cast<char**>(lua_touserdata(L, 2));
  int script;
  int args = collectargs(argv, &script);
  int optlim = (script > 0) ? script : argc; /* first argv not an option */
  luaL_checkversion(L);                      /* check that interpreter has correct version */
  if (args == has_error)
  {                            /* bad arg? */
    print_usage(argv[script]); /* 'script' has index of bad arg. */
    return 0;
  }
  if (args & has_v) /* option '-v'? */
    print_version();
  if (args & has_E)
  {                        /* option '-E'? */
    l_noenv = true; /* program will ignore environment variables */
    lua_pushboolean(L, 1); /* signal for libraries to ignore env. vars. */
    lua_setfieldi(L, LUA_REGISTRYINDEX, lt::LUA_NOENV);
  }
  luai_openlibs(L);                      /* open standard libraries */
  createargtable(L, argv, argc, script); /* create table 'arg' */
  lua_gc(L, LUA_GCRESTART, 0, 0, 0);              /* start GC... */
  lua_gc(L, LUA_GCGEN, 0, 0, 0);                  /* ...in generational mode */
  if (handle_luainit(L) != LUA_OK)       /* run LUA_INIT */
    return 0;                            /* error running LUA_INIT */
  if (!runargs(L, argv, optlim))         /* execute arguments -e, -l, and -W */
    return 0;                            /* something failed */
  if (script > 0)
  { /* execute main script (if there is one) */
    if (handle_script(L, argv + script) != LUA_OK)
      return 0; /* interrupt in case of error */
  }
  if (args & has_i) /* -i option? */
    doREPL(L);      /* do read-eval-print loop */
  else if (script < 1 && !(args & (has_e | has_v)))
  { /* no active option? */
    if (lua_stdin_is_tty())
    { /* running in interactive mode? */
      print_version();
      doREPL(L); /* do read-eval-print loop */
    }
    else
      dofile(L, std::nullopt); /* executes stdin as a file */
  }
  lua_pushboolean(L, 1); /* signal no errors */
  return 1;
}

}

int main(int argc, char** argv)
{
  int status, result;
  lua_State* L = luaL_newstate(); /* create state */
  if (L == nullptr)
  {
    byte_view name(argv[0] != nullptr ? argv[0] : lt::empty_str);
    l_message(name, lt::cannot_create_state_not_enough_memory);
    return EXIT_FAILURE;
  }
  lua_gc(L, LUA_GCSTOP, 0, 0, 0);          /* stop GC while building state */
  lua_pushcfunction(L, &pmain);   /* to call 'pmain' in protected mode */
  lua_pushinteger(L, argc);       /* 1st argument */
  lua_pushlightuserdata(L, argv); /* 2nd argument */
  status = lua_pcall(L, 2, 1, 0); /* do the call */
  result = lua_toboolean(L, -1);  /* get result */
  report(L, status);
  lua_close(L);
  return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}

