/*
** $Id: lua.c $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

#if !defined(LUA_PROGNAME)
#define LUA_PROGNAME "lua"
#endif

#if !defined(LUA_INIT_VAR)
#define LUA_INIT_VAR "LUA_INIT"
#endif

/* Name of the environment variable with the name of the readline library */
#if !defined(LUA_RLLIB_VAR)
#define LUA_RLLIB_VAR "LUA_READLINELIB"
#endif

static lua_State* globalL = nullptr;

/* program name for error prefixes; empty = absent (interactive mode) */
static std::string progname = LUA_PROGNAME; /* host program text (consumer side) */

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
  luaL_error(L, HostString(lit<"interrupted!">()->view()));
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
  lua_writestringerror("%s: ", progname.c_str());
  if (badoption.size() > 1 && (badoption[1] == 'e' || badoption[1] == 'l'))
    lua_writestringerror("'%s' needs argument\n", badoption.c_str());
  else
    lua_writestringerror("unrecognized option '%s'\n", badoption.c_str());
  lua_writestringerror(
      "usage: %s [options] [script [args]]\n"
      "Available options are:\n"
      "  -e stat   execute string 'stat'\n"
      "  -i        enter interactive mode after executing 'script'\n"
      "  -l mod    require library 'mod' into global 'mod'\n"
      "  -l g=mod  require library 'mod' into global 'g'\n"
      "  -v        show version information\n"
      "  -E        ignore environment variables\n"
      "  -W        turn warnings on\n"
      "  --        stop handling options\n"
      "  -         stop handling options and execute stdin\n",
      progname.c_str());
}

/*
** Prints an error message, adding the program name in front of it
** (if present; empty = absent)
*/
static void l_message(const std::string& pname, const std::string& msg)
{
  if (!pname.empty())
    lua_writestringerror("%s: ", pname.c_str());
  lua_writestringerror("%s\n", msg.c_str());
}

/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack.
*/
static int report(lua_State* L, int status)
{
  if (status != LUA_OK)
  {
    std::string msg = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)->view())
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
  HostString msg;
  if (lua_isstring(L, 1))
    msg = *lua_tostring(L, 1);
  else
  {                                          /* error object not a string */
    if (luaL_callmeta(L, 1, HostString(lit<"__tostring">()->view())) && /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)      /* that produces a string? */
      return 1;                              /* that is the message */
    else
    {
      lua_pushstring(L, luaL_typename(L, 1));
      msg = lua_pushfstring(L, HostString(lit<"(error object is a %s value)">()->view()));
    }
  }
  luaL_traceback(L, L, msg, 1); /* append a standard traceback */
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
  lua_writestring(LUA_COPYRIGHT.data(), LUA_COPYRIGHT.size());
  lua_writeline();
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
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - script);
  }
  lua_setglobal(L, HostString(lit<"arg">()->view()));
}

static int dochunk(lua_State* L, int status)
{
  if (status == LUA_OK)
    status = docall(L, 0, 0);
  return report(L, status);
}

static int dofile(lua_State* L, const std::optional<HostString>& name)
{
  return dochunk(L, luaL_loadfilex(L, name, std::nullopt)); /* nullopt = stdin */
}

static int dostring(lua_State* L, std::string_view s, const HostString& name)
{
  return dochunk(L, luaL_loadbufferx(L, s, name, HostString(lit<"t">()->view())));
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
    suffix = globname.find(*LUA_IGMARK); /* look for a suffix mark */
  }
  else
  {
    globname = arg.substr(0, eq); /* global name ends here */
    modname = arg.substr(eq + 1); /* module name starts after the '=' */
  }
  lua_getglobal(L, HostString(lit<"require">()->view()));
  lua_pushstring(L, HostString(modname));
  status = docall(L, 1, 1); /* call 'require(modname)' */
  if (status == LUA_OK)
  {
    if (suffix != std::string::npos)         /* is there a suffix mark? */
      globname = globname.substr(0, suffix); /* remove suffix from global name */
    lua_setglobal(L, HostString(globname));  /* globname = require(modname) */
  }
  return report(L, status);
}

/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs(lua_State* L)
{
  int i, n;
  if (lua_getglobal(L, HostString(lit<"arg">()->view())) != LUA_TTABLE)
    luaL_error(L, HostString(lit<"'arg' is not a table">()->view()));
  n = (int) luaL_len(L, -1);
  luaL_checkstack(L, n + 3, HostString(lit<"too many arguments to script">()->view()));
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i); /* remove table from the stack */
  return n;
}

static int handle_script(lua_State* L, char** argv)
{
  int status;
  const char* fname = argv[0];
  if (std::string_view(fname) == lit<"-">()->view() && std::string_view(argv[-1]) != lit<"--">()->view())
    fname = nullptr; /* stdin */
  if (fname == nullptr)
    status = luaL_loadfilex(L, std::nullopt, std::nullopt); /* stdin */
  else
  {
    HostString scriptname(fname); /* host boundary: adopt the argv text */
    status = luaL_loadfilex(L, scriptname, std::nullopt);
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
  lua_warning(L, HostString(lit<"@off">()->view()), 0); /* by default, Lua stand-alone has warnings off */
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
      std::string arg(argv[i]); /* adopt the argv text (host program side) */
      std::string extra = arg.substr(2); /* both options need an argument */
      if (extra.empty())
      {
        i++; /* try next 'argv' */
        assert(argv[i] != nullptr); /* already checked by 'collectargs' */
        extra = argv[i];
      }
      status = (option == 'e')
                   ? dostring(L, extra, "=(command line)")
                   : dolibrary(L, extra);
      if (status != LUA_OK)
        return 0;
      break;
    }
    case 'W':
      lua_warning(L, "@on", 0); /* warnings on */
      break;
    }
  }
  return 1;
}

static char* (*l_getenv)(const char* name);

/* Function to ignore environment variables, used by option -E */
static char* no_getenv(const char*)
{
  return nullptr;
}

static int handle_luainit(lua_State* L)
{
  std::string name = LUA_INIT_VAR;
  name.append(LUA_VERSUFFIX); /* versioned name: e.g. "LUA_INIT_5_5" */
  /* l_getenv is system ABI: char* in and out, converted at the call */
  const char* einit = l_getenv(name.c_str());
  if (einit == nullptr)
  {
    name = LUA_INIT_VAR;
    einit = l_getenv(name.c_str()); /* try alternative name */
  }
  if (einit == nullptr)
    return LUA_OK;
  std::string init(einit); /* adopt the env value (host program side) */
  if (!init.empty() && init[0] == '@')
  {
    HostString fname{init.substr(1)};
    return dofile(L, fname);
  }
  else
    return dostring(L, init, HostString("=" + name));
}

/*
** {==================================================================
** Read-Eval-Print Loop (REPL)
** ===================================================================
*/

#if !defined(LUA_PROMPT)
#define LUA_PROMPT  "> "
constexpr char LUA_PROMPT2[] = ">> ";
#endif

#if !defined(LUA_MAXINPUT)
#define LUA_MAXINPUT 512
#endif

/*
** lua_stdin_is_tty detects whether the standard input is a 'tty' (that
** is, whether we're running lua interactively).
*/
#if !defined(lua_stdin_is_tty) /* { */

#include <unistd.h>
#define lua_stdin_is_tty() isatty(0)

#endif /* } */

/*
** * lua_initreadline initializes the readline system.
** * lua_readline defines how to show a prompt and then read a line from
**   the standard input.
** * lua_saveline defines how to "save" a read line in a "history".
** * lua_freeline defines how to free a line read by lua_readline.
*/

#if !defined(lua_readline) /* { */
/* Otherwise, all previously listed functions should be defined. */

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
    fputs(prompt, stdout);
    fflush(stdout);                          /* show prompt */
    return fgets(buff, LUA_MAXINPUT, stdin); /* read line */
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

#if defined(LUA_USE_DLOPEN) && defined(LUA_READLINELIB) /* { */
/* try to load 'readline' dynamically */

#include <dlfcn.h>

static void lua_initreadline(lua_State* L)
{
  /* l_getenv is system ABI: adopt the value at the call */
  const char* erllib = l_getenv(LUA_RLLIB_VAR); /* name of readline library */
  const char* rllib = (erllib != nullptr) ? erllib
                                          : LUA_READLINELIB; /* default name */
  void* lib = dlopen(rllib, RTLD_NOW | RTLD_LOCAL); /* system ABI */
  if (lib != nullptr)
  {
    const char** name = ((const char**)(dlsym(lib, "rl_readline_name")));
    if (name != nullptr)
      *name = "lua";
    l_readline = ((l_readlineT)(cast_func(dlsym(lib, "readline"))));
    l_addhist = ((l_addhistT)(cast_func(dlsym(lib, "add_history"))));
    if (l_readline != nullptr) /* could load readline function? */
      return;               /* everything ok */
    /* else emit a warning */
  }
  lua_warning(L, "unable to load readline library '", 1);
  lua_warning(L, rllib, 1);
  lua_warning(L, "'", 0);
}

#else /* }{ */
/* no dlopen or LUA_READLINELIB undefined */

/* Leave pointers with nullptr */
#define lua_initreadline(L) ((void) L)

#endif /* } */

#endif /* } */

/*
** Return the string to be used as a prompt by the interpreter. Leave
** the string (or nil, if using the default value) on the stack, to keep
** it anchored.
*/
static std::string get_prompt(lua_State* L, int firstline)
{
  if (lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2") == LUA_TNIL)
    return (firstline ? LUA_PROMPT : LUA_PROMPT2); /* use the default */
  else
  { /* apply 'tostring' over the value */
    std::string p{luaL_tolstring(L, -1).view()}; /* copy (host program side) */
    lua_remove(L, -2); /* remove original value */
    return p;
  }
}

/* mark in error messages for incomplete statements */
constexpr char EOFMARK[] = "<eof>";

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
    if (msgv.ends_with(EOFMARK)) /* bounded suffix compare (Java endsWith) */
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
  char* b = lua_readline(buffer, prmt.c_str()); /* readline ABI boundary */
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
  lua_pushvalue(L, -1); /* duplicate original line */
  lua_pushfstring(L, "return %s;"); /* modified line (anchored on the stack) */
  std::string_view retline = lua_tolstring(L, -1); /* borrowed */
  int status = luaL_loadbufferx(L, retline, "=stdin", "t");
  if (status == LUA_OK)
    lua_remove(L, -2); /* remove modified line */
  else
    lua_pop(L, 2); /* pop result from 'luaL_loadbufferx' and modified line */
  return status;
}

static void checklocal(std::string_view line)
{
  static constexpr std::string_view kw = "local";
  static constexpr std::string_view space = " \t";
  size_t i = line.find_first_not_of(space); /* skip spaces */
  if (i == std::string_view::npos)
    return; /* only spaces */
  size_t j = i + kw.size();
  if (line.compare(i, kw.size(), kw) == 0 &&      /* "local"? */
      (j >= line.size() || line[j] == ' ' || line[j] == '\t'))
  { /* followed by a space or end of line? */
    lua_writestringerror("%s\n",
                         "warning: locals do not survive across lines in interactive mode");
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
    int status = luaL_loadbufferx(L, line, "=stdin", "t"); /* try it */
    if (!incomplete(L, status) || !pushline(L, 0))
      return status;                  /* should not or cannot try to add continuation line */
    lua_remove(L, -2);                /* remove error message (from incomplete line) */
    lua_pushliteral(L, "\n");         /* add newline... */
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
  std::string line{lua_tostring(L, 1)->view()}; /* host program copy */
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
    luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
    lua_getglobal(L, "print");
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
    {
      lua_pushvalue(L, -1); /* error message is the argument */
      const HostString m = lua_pushfstring(L, "error calling 'print' (%s)");
      l_message(progname, std::string(m.view()));
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
  lua_writeline();
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
    l_getenv = &no_getenv; /* program will ignore environment variables */
    lua_pushboolean(L, 1); /* signal for libraries to ignore env. vars. */
    lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  }
  else
    l_getenv = &getenv;
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
