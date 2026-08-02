/*
** $Id: lua.c $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "lua_internal.h"
#include "os_api.h"

constexpr char LUA_PROGNAME[] = "lua";

static const InternalString* const LUA_INIT_VAR = lit<"LUA_INIT">();

/* Name of the environment variable with the name of the readline library */
constexpr char LUA_RLLIB_VAR[] = "LUA_READLINELIB";

static lua_State* globalL = nullptr;

static std::string progname = LUA_PROGNAME;

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
  luaL_errori(L, lit<"interrupted!">()->view());
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

static void print_usage(const std::string& badoption)
{
  os_print_stderr(progname);
  os_print_stderr(lit<": ">()->view());
  if (badoption.size() > 1 && (badoption[1] == 'e' || badoption[1] == 'l'))
  {
    os_print_stderr(lit<"'">()->view());
    os_print_stderr(badoption);
    os_print_stderr(lit<"' needs argument\n">()->view());
  }
  else
  {
    os_print_stderr(lit<"unrecognized option '">()->view());
    os_print_stderr(badoption);
    os_print_stderr(lit<"'\n">()->view());
  }
  os_print_stderr(lit<"usage: ">()->view());
  os_print_stderr(progname);
  os_print_stderr(lit<" [options] [script [args]]\n"
      "Available options are:\n"
      "  -e stat   execute string 'stat'\n"
      "  -i        enter interactive mode after executing 'script'\n"
      "  -l mod    require library 'mod' into global 'mod'\n"
      "  -l g=mod  require library 'mod' into global 'g'\n"
      "  -v        show version information\n"
      "  -E        ignore environment variables\n"
      "  -W        turn warnings on\n"
      "  --        stop handling options\n"
      "  -         stop handling options and execute stdin\n">()->view());
  os_flush_stderr();
}

/*
** Prints an error message, adding the program name in front of it
** (if present; empty = absent)
*/
static void l_message(const std::string& pname, const std::string& msg)
{
  if (!pname.empty())
  {
    os_print_stderr(pname);
    os_print_stderr(lit<": ">()->view());
  }
  os_print_stderr(msg);
  os_print_stderr(lit<"\n">()->view());
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
    std::string msg = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)->to_utf8())
                                          : std::string("(error message not a string)");
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
  std::string_view msg;
  if (lua_isstring(L, 1))
    msg = lua_toistring(L, 1)->view();
  else
  {                                          /* error object not a string */
    if (luaL_callmetai(L, 1, lit<"__tostring">()->view()) && /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)      /* that produces a string? */
      return 1;                              /* that is the message */
    else
    {
      lua_pushistringref(L, lua_typenamei(L, lua_type(L, 1))->view());
      msg = lua_pushfstringi(L, lit<"(error object is a %s value)">()->view())->view();
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
  os_print_stdout(lit<"\n">()->view());
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
    std::string_view arg{argv[i]};
    lua_pushlstring(L, arg, 0, arg.size());
    lua_rawseti(L, -2, i - script);
  }
  lua_setglobali(L, lit<"arg">()->view());
}

static int dochunk(lua_State* L, int status)
{
  if (status == LUA_OK)
    status = docall(L, 0, 0);
  return report(L, status);
}

static int dofile(lua_State* L, std::string_view name)
{
  return dochunk(L, luaL_loadfilei(L, name, std::string_view{}));
}

static int dostring(lua_State* L, std::string_view s, std::string_view name)
{
  return dochunk(L, luaL_loadbufferi(L, s, name, lit<"t">()->view()));
}

/*
** Receives 'globname[=modname]' and runs 'globname = require(modname)'.
** If there is no explicit modname and globname contains a '-', cut
** the suffix after '-' (the "version") to make the global name.
*/
static int dolibrary(lua_State* L, const std::string& arg)
{
  int status;
  std::string globname, modname;
  size_t suffix = std::string::npos;
  size_t eq = arg.find('=');
  if (eq == std::string::npos)
  {                 /* no explicit name? */
    globname = arg; /* module name is equal to global name */
    modname = arg;
    suffix = globname.find(LUA_IGMARK->view()); /* look for a suffix mark */
  }
  else
  {
    globname = arg.substr(0, eq); /* global name ends here */
    modname = arg.substr(eq + 1); /* module name starts after the '=' */
  }
  lua_getglobali(L, lit<"require">()->view());
  lua_pushlstring(L, modname, 0, (modname).size());
  status = docall(L, 1, 1); /* call 'require(modname)' */
  if (status == LUA_OK)
  {
    if (suffix != std::string::npos)         /* is there a suffix mark? */
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
  if (lua_getglobali(L, lit<"arg">()->view()) != LUA_TTABLE)
    luaL_errori(L, lit<"'arg' is not a table">()->view());
  n = (int) luaL_len(L, -1);
  luaL_checkstacki(L, n + 3, lit<"too many arguments to script">()->view());
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i); /* remove table from the stack */
  return n;
}

static int handle_script(lua_State* L, char** argv)
{
  int status;
  const char* fname = argv[0];
  std::string_view nullsv(nullptr, 0);
  if (std::string_view(fname) == lit<"-">()->view() && std::string_view(argv[-1]) != lit<"--">()->view())
    fname = nullptr; /* stdin */
  if (fname == nullptr)
    status = luaL_loadfilei(L, nullsv, nullsv); /* stdin */
  else
  {
    status = luaL_loadfilei(L, std::string_view{fname}, nullsv);
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
  lua_warningi(L, lit<"@off">()->view(), 0); /* by default, Lua stand-alone has warnings off */
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
      std::string arg(argv[i]);
      std::string extra = arg.substr(2); /* both options need an argument */
      if (extra.empty())
      {
        i++; /* try next 'argv' */
        assert(argv[i] != nullptr);
        extra = argv[i];
      }
      status = (option == 'e')
                   ? dostring(L, extra, lit<"=(command line)">()->view())
                   : dolibrary(L, extra);
      if (status != LUA_OK)
        return 0;
      break;
    }
    case 'W':
      lua_warningi(L, lit<"@on">()->view(), 0); /* warnings on */
      break;
    }
  }
  return 1;
}

static bool l_noenv = false;

static std::optional<InternalString> l_getenv(std::string_view name)
{
  if (l_noenv)
    return std::nullopt;
  return os_getenv(name);
}

static int handle_luainit(lua_State* L)
{
  InternalString name = LUA_INIT_VAR->concat(LUA_VERSUFFIX->view());
  std::string_view namev = name.view();
  std::optional<InternalString> einit = l_getenv(namev);
  if (!einit.has_value())
  {
    namev = LUA_INIT_VAR->view();
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
    InternalString chunkname = lit<"=">()->concat(namev);
    return dostring(L, einit->view(), chunkname.view());
  }
}

/*
** {==================================================================
** Read-Eval-Print Loop (REPL)
** ===================================================================
*/

constexpr char LUA_PROMPT[] = "> ";
constexpr char LUA_PROMPT2[] = ">> ";

constexpr int LUA_MAXINPUT = 512;

/*
** lua_stdin_is_tty detects whether the standard input is a 'tty' (that
** is, whether we're running lua interactively).
*/
#include <unistd.h>
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

static char* lua_readline(char* buff, const char* prompt)
{
  if (l_readline != nullptr)         /* is there a 'readline'? */
    return (*l_readline)(prompt); /* use it */
  else
  { /* emulate 'readline' over 'buff' */
    os_print_stdout(std::string_view{prompt});
    os_flush_stdout();                       /* show prompt */
    return os_readline(os_stdin(), std::span<char>{buff, LUA_MAXINPUT}) ? buff : nullptr; /* read line */
  }
}

static void lua_saveline(const char* line)
{
  if (l_addhist != nullptr) /* is there an 'add_history'? */
    (*l_addhist)(line);  /* use it */
  /* else nothing to be done */
}

static void lua_freeline(char* line)
{
  if (l_readline != nullptr) /* is there a 'readline'? */
    free(line);           /* free line created by it */
  /* else 'lua_readline' used an automatic buffer; nothing to free */
}

/* try to load 'readline' dynamically */


static void lua_initreadline(lua_State* L)
{
  std::optional<InternalString> erllib = l_getenv(std::string_view{LUA_RLLIB_VAR}); /* name of readline library */
  const char* rllib = erllib.has_value() ? erllib->c_str()
                                         : LUA_READLINELIB; /* default name */
  OsLib* lib = os_dlopen(std::string_view{rllib}, false);
  if (lib != nullptr)
  {
    const char** name = ((const char**)(os_dlsym(lib, lit<"rl_readline_name">()->view())));
    if (name != nullptr)
      *name = "lua";
    l_readline = ((l_readlineT)(cast_func(os_dlsym(lib, lit<"readline">()->view()))));
    l_addhist = ((l_addhistT)(cast_func(os_dlsym(lib, lit<"add_history">()->view()))));
    if (l_readline != nullptr) /* could load readline function? */
      return;               /* everything ok */
    /* else emit a warning */
  }
  lua_warningi(L, lit<"unable to load readline library '">()->view(), 1);
  lua_warning(L, rllib, 1);
  lua_warningi(L, lit<"'">()->view(), 0);
}

/*
** Return the string to be used as a prompt by the interpreter. Leave
** the string (or nil, if using the default value) on the stack, to keep
** it anchored.
*/
static std::string get_prompt(lua_State* L, int firstline)
{
  if (lua_getglobali(L, (firstline ? lit<"_PROMPT">() : lit<"_PROMPT2">())->view()) == LUA_TNIL)
    return (firstline ? LUA_PROMPT : LUA_PROMPT2); /* use the default */
  else
  { /* apply 'tostring' over the value */
    std::string p{luaL_tolstring(L, -1).to_utf8()};
    lua_remove(L, -2); /* remove original value */
    return p;
  }
}

/* mark in error messages for incomplete statements */
const std::string_view EOFMARK = lit<"<eof>">()->view();

/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete(lua_State* L, int status)
{
  if (status == LUA_ERRSYNTAX)
  {
    std::string_view msgv = lua_tolstring(L, -1);
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
  char buffer[LUA_MAXINPUT];
  size_t l;
  std::string prmt = get_prompt(L, firstline);
  char* b = lua_readline(buffer, prmt.c_str());
  lua_pop(L, 1); /* remove prompt */
  if (b == nullptr)
    return 0; /* no input */
  l = strlen(b);
  if (l > 0 && b[l - 1] == '\n') /* line ends with newline? */
    b[--l] = '\0';               /* remove it */
  lua_pushlstring(L, std::string_view{b, l}, 0, l);
  lua_freeline(b);
  return 1;
}

/*
** Try to compile line on the stack as 'return <line>;'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn(lua_State* L)
{
  lua_pushvalue(L, -1);
  lua_pushfstringi(L, lit<"return %s;">()->view());
  std::string_view retline = lua_tolstring(L, -1);
  int status = luaL_loadbufferi(L, retline, lit<"=stdin">()->view(), lit<"t">()->view());
  if (status == LUA_OK)
    lua_remove(L, -2); /* remove modified line */
  else
    lua_pop(L, 2); /* pop result from 'luaL_loadbufferx' and modified line */
  return status;
}

static void checklocal(std::string_view line)
{
  static const std::string_view kw = lit<"local">()->view();
  static const std::string_view space = lit<" \t">()->view();
  size_t i = line.find_first_not_of(space); /* skip spaces */
  if (i == std::string_view::npos)
    return;
  size_t j = i + kw.size();
  if (line.compare(i, kw.size(), kw) == 0 &&      /* "local"? */
      (j >= line.size() || line[j] == ' ' || line[j] == '\t'))
  {
    os_print_stderr(lit<"warning: locals do not survive across lines in interactive mode\n">()->view());
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
  std::string_view line = lua_tolstring(L, 1); /* get first line */
  checklocal(line);
  for (;;)
  {                                                  /* repeat until gets a complete statement */
    int status = luaL_loadbufferi(L, line, lit<"=stdin">()->view(), lit<"t">()->view()); /* try it */
    if (!incomplete(L, status) || !pushline(L, 0))
      return status;                  /* should not or cannot try to add continuation line */
    lua_remove(L, -2);                /* remove error message (from incomplete line) */
    lua_pushistringref(L, lit<"\n">()->view());         /* add newline... */
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
static int loadline(lua_State* L)
{
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;                           /* no input */
  if ((status = addreturn(L)) != LUA_OK) /* 'return ...' did not work? */
    status = multiline(L);               /* try as command, maybe with continuation lines */
  std::string line{lua_tostring(L, 1)->to_utf8()};
  if (!line.empty())            /* non empty? */
    lua_saveline(line.c_str()); /* keep history */
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
    luaL_checkstacki(L, LUA_MINSTACK, lit<"too many results to print">()->view());
    lua_getglobali(L, lit<"print">()->view());
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
    {
      lua_pushvalue(L, -1);
      const InternalString* m = lua_pushfstringi(L, lit<"error calling 'print' (%s)">()->view());
      l_message(progname, std::string(m->view()));
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
  std::string oldprogname = progname;
  progname = ""; /* no 'progname' on errors in interactive mode */
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
  os_print_stdout(lit<"\n">()->view());
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
  char** argv = (char**) lua_touserdata(L, 2);
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
    lua_setfieldi(L, LUA_REGISTRYINDEX, lit<"LUA_NOENV">()->view());
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
      dofile(L, std::string_view{}); /* executes stdin as a file */
  }
  lua_pushboolean(L, 1); /* signal no errors */
  return 1;
}

int main(int argc, char** argv)
{
  int status, result;
  lua_State* L = luaL_newstate(); /* create state */
  if (L == nullptr)
  {
    l_message(argv[0] != nullptr ? argv[0] : "",
              "cannot create state: not enough memory");
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
