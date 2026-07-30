/*
** $Id: loadlib.c $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/


#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

/*
** LUA_CSUBSEP is the character that replaces dots in submodule names
** when searching for a C loader.
** LUA_LSUBSEP is the character that replaces dots in submodule names
** when searching for a Lua loader.
*/
#if !defined(LUA_CSUBSEP)
#define LUA_CSUBSEP LUA_DIRSEP
#endif

#if !defined(LUA_LSUBSEP)
#define LUA_LSUBSEP LUA_DIRSEP
#endif

/* prefix for open functions in C libraries */

/* separator for open functions in C libraries */
constexpr char LUA_OFSEP[] = "_";

/*
** key for table in the registry that keeps handles
** for all loaded C libraries
*/
static const char* const CLIBS = "_CLIBS";

constexpr char LIB_FAIL[] = "open";

#define setprogdir(L) ((void) 0)

/* cast void* to a Lua function */

/*
** system-dependent functions
*/

/*
** load C library in file 'path'. If 'seeglb', load with all names in
** the library global.
** Returns the library; in case of error, returns nullptr plus an
** error string in the stack.
*/
static void* lsys_load(lua_State* L, const HostString& path, int seeglb);

/*
** Try to find a function named 'sym' in library 'lib'.
** Returns the function; in case of error, returns nullptr plus an
** error string in the stack.
*/
static lua_CFunction lsys_sym(lua_State* L, void* lib, const HostString& sym);

/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface,
** which is available in all POSIX systems.
** =========================================================================
*/

#include <dlfcn.h>

static void* lsys_load(lua_State* L, const HostString& path, int seeglb)
{
  /* dlopen is system ABI: char* materializes only at the call */
  void* lib = dlopen(path.c_str(), RTLD_NOW | (seeglb ? RTLD_GLOBAL : RTLD_LOCAL));
  if (l_unlikely(lib == nullptr))
    lua_pushstring(L, dlerror());
  return lib;
}

static lua_CFunction lsys_sym(lua_State* L, void* lib, const HostString& sym)
{
  /* dlsym is system ABI: char* materializes only at the call */
  lua_CFunction f = ((lua_CFunction)(cast_func(dlsym(lib, sym.c_str()))));
  if (l_unlikely(f == nullptr))
    lua_pushstring(L, dlerror());
  return f;
}

/* }====================================================== */

/*
** {==================================================================
** Set Paths
** ===================================================================
*/

/*
** LUA_PATH_VAR and LUA_CPATH_VAR are the names of the environment
** variables that Lua check to set its paths.
*/
#if !defined(LUA_PATH_VAR)
#define LUA_PATH_VAR "LUA_PATH"
#endif

#if !defined(LUA_CPATH_VAR)
#define LUA_CPATH_VAR "LUA_CPATH"
#endif

/*
** return registry.LUA_NOENV as a boolean
*/
static int noenv(lua_State* L)
{
  int b;
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  b = lua_toboolean(L, -1);
  lua_pop(L, 1); /* remove value */
  return b;
}

/*
** Set a path. (If using the default path, assume it is a string
** literal in C and create it as an external string.)
*/
static void setpath(lua_State* L, const HostString& fieldname,
                    const HostString& envname,
                    const HostString& dft)
{
  lua_pushstring(L, envname);
  lua_pushstring(L, LUA_VERSUFFIX.data());
  const InternalString* nver = lua_pushfstring(L, "%s%s");
  /* getenv is system ABI: char* in and out, converted at the call */
  const char* epath = getenv(nver->c_str());                 /* try versioned name */
  if (epath == nullptr)                                      /* no versioned environment variable? */
    epath = getenv(envname.c_str());                         /* try unversioned name */
  if (epath == nullptr || noenv(L)) /* no environment variable? */
    lua_pushstring(L, dft);         /* use default */
  else
  {
    HostString path(epath); /* host boundary: adopt the env value */
    size_t dftmark = path.find(LUA_PATH_SEP LUA_PATH_SEP);
    if (dftmark == HostString::npos)
      lua_pushstring(L, path); /* nothing to change */
    else
    { /* path contains a ";;": insert default path in its place */
      luaL_Buffer b;
      luaL_buffinit(L, &b);
      if (dftmark > 0)
      { /* is there a prefix before ';;'? */
        luaL_addlstring(&b, path, 0, dftmark); /* add it */
        luaL_addchar(&b, *LUA_PATH_SEP);
      }
      luaL_addstring(&b, dft); /* add default */
      if (dftmark + 2 < path.size())
      { /* is there a suffix after ';;'? */
        luaL_addchar(&b, *LUA_PATH_SEP);
        luaL_addlstring(&b, path, dftmark + 2, path.size() - (dftmark + 2));
      }
      luaL_pushresult(&b);
    }
  }
  setprogdir(L);
  lua_setfield(L, -3, fieldname); /* package[fieldname] = path value */
  lua_pop(L, 1);                  /* pop versioned variable name ('nver') */
}

/* }================================================================== */

/*
** External strings created by DLLs may need the DLL code to be
** deallocated. This implies that a DLL can only be unloaded after all
** its strings were deallocated. To ensure that, we create a 'library
** string' to represent each DLL, and when this string is deallocated
** it closes its corresponding DLL.
** (The string itself is irrelevant; its userdata is the DLL pointer.)
*/

/*
** return registry.CLIBS[path]
*/
static void* checkclib(lua_State* L, const HostString& path)
{
  void* plib;
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_getfield(L, -1, path);
  plib = lua_touserdata(L, -1); /* plib = CLIBS[path] */
  lua_pop(L, 2);                /* pop CLIBS table and 'plib' */
  return plib;
}

/*
** __gc for library userdata — unloads the DLL.
*/
static int lib_gc(lua_State* L)
{
  void** p = (void**) lua_touserdata(L, 1);
  if (*p)
  {
    dlclose(*p);
    *p = nullptr;
  }
  return 0;
}

/*
** Create a userdata that, when collected, will unload 'plib'
*/
static void createlibstr(lua_State* L, void* plib)
{
  void** p = (void**) lua_newuserdatauv(L, sizeof(void*), 0);
  *p = plib;
  if (luaL_newmetatable(L, "_LOADLIB"))
  {
    lua_pushcfunction(L, lib_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
}

/*
** registry.CLIBS[path] = plib          -- for queries.
** Also create a reference to strlib, so that the library string will
** only be collected when registry.CLIBS is collected.
*/
static void addtoclib(lua_State* L, const HostString& path, void* plib)
{
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_pushlightuserdata(L, plib);
  lua_setfield(L, -2, path); /* CLIBS[path] = plib */
  createlibstr(L, plib);
  luaL_ref(L, -2); /* keep library string in CLIBS */
  lua_pop(L, 1);   /* pop CLIBS table */
}

/* error codes for 'lookforfunc' */
constexpr int ERRLIB = 1;
constexpr int ERRFUNC = 2;

/*
** Look for a C function named 'sym' in a dynamically loaded library
** 'path'.
** First, check whether the library is already loaded; if not, try
** to load it.
** Then, if 'sym' is '*', return true (as library has been loaded).
** Otherwise, look for symbol 'sym' in the library and push a
** C function with that symbol.
** Return 0 with 'true' or a function in the stack; in case of
** errors, return an error code with an error message in the stack.
*/
static int lookforfunc(lua_State* L, const HostString& path, const HostString& sym)
{
  int loadlibonly = (!sym.empty() && sym[0] == '*');
  void* reg = checkclib(L, path); /* check loaded C libraries */
  if (reg == nullptr)
  {                                          /* must load library? */
    reg = lsys_load(L, path, loadlibonly);   /* global symbols if 'sym'=='*' */
    if (reg == nullptr)
      return ERRLIB; /* unable to load library */
    addtoclib(L, path, reg);
  }
  if (loadlibonly)
  {                        /* loading only library (no function)? */
    lua_pushboolean(L, 1); /* return 'true' */
    return 0;              /* no errors */
  }
  else
  {
    lua_CFunction f = lsys_sym(L, reg, sym);
    if (f == nullptr)
      return ERRFUNC;        /* unable to find function */
    lua_pushcfunction(L, f); /* else create new function */
    return 0;                /* no errors */
  }
}

static int ll_loadlib(lua_State* L)
{
  HostString path = luaL_checkstring(L, 1);
  HostString init = luaL_checkstring(L, 2);
  int stat = lookforfunc(L, path, init);
  if (l_likely(stat == 0)) /* no errors? */
    return 1;              /* return the loaded function */
  else
  { /* error; error message is on stack top */
    luaL_pushfail(L);
    lua_insert(L, -2);
    lua_pushstring(L, (stat == ERRLIB) ? LIB_FAIL : "init");
    return 3; /* return fail, error message, and where */
  }
}

/*
** {======================================================
** 'require' function
** =======================================================
*/

static int readable(const HostString& filename)
{
  /* fopen is system ABI: char* materializes only at the call */
  FILE* f = fopen(filename.c_str(), "r"); /* try to open file */
  if (f == nullptr)
    return 0; /* open failed */
  fclose(f);
  return 1;
}

/*
** Given a path such as ";blabla.so;blublu.so", pushes the string
**
** no file 'blabla.so'
**  no file 'blublu.so'
*/
static void pusherrornotfound(lua_State* L, const HostString& path)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "no file '");
  luaL_addgsub(&b, path, LUA_PATH_SEP, "'\n\tno file '");
  luaL_addstring(&b, "'");
  luaL_pushresult(&b);
}

/*
** Returns 1 with the found file name pushed on the stack, or 0 with
** the error message pushed.
*/
static int searchpath(lua_State* L, HostString name,
                      const HostString& path,
                      const HostString& sep,
                      const HostString& dirsep)
{
  luaL_Buffer buff;
  /* separator is non-empty and appears in 'name'? */
  if (!sep.empty() && name.find(sep) != HostString::npos)
  { /* replace it by 'dirsep' (result stays anchored on the stack) */
    const InternalString* r = luaL_gsub(L, name, sep, dirsep);
    name = HostString(r->bytes(), r->size());
  }
  luaL_buffinit(L, &buff);
  /* add path to the buffer, replacing marks ('?') with the file name */
  luaL_addgsub(&buff, path, LUA_PATH_MARK, name);
  {
    /* walk the list 'name1;name2;name3;...' by offsets over a view of
       the buffer (Java: byte[] + int cursors); each candidate file
       name materializes as host text only for the fopen probe */
    std::span<char> area = luaL_buffarea(&buff);
    std::string_view list{area.data(), luaL_bufflen(&buff)};
    size_t pos = 0;
    while (pos < list.size())
    {
      size_t sepmark = list.find(*LUA_PATH_SEP, pos); /* next separator */
      if (sepmark == std::string_view::npos)          /* separator not found? */
        sepmark = list.size();                        /* name goes until the end */
      HostString filename(list, pos, sepmark - pos);  /* deep copy of the sub-range */
      if (readable(filename))
      {                              /* does file exist and is readable? */
        lua_pushstring(L, filename); /* save and return name */
        return 1;
      }
      pos = sepmark + 1; /* continue after the separator */
    }
  }
  luaL_pushresult(&buff);                    /* push path to create error message */
  pusherrornotfound(L, lua_tostring(L, -1)); /* create error message */
  return 0; /* not found */
}

static int ll_searchpath(lua_State* L)
{
  int found = searchpath(L, luaL_checkstring(L, 1),
                         luaL_checkstring(L, 2),
                         luaL_optstring(L, 3, "."),
                         luaL_optstring(L, 4, LUA_DIRSEP));
  if (found)
    return 1;
  else
  { /* error message is on top of the stack */
    luaL_pushfail(L);
    lua_insert(L, -2);
    return 2; /* return fail + error message */
  }
}

/* same contract as 'searchpath': 1 = file name pushed, 0 = error pushed */
static int findfile(lua_State* L, const HostString& name,
                    const HostString& pname,
                    const HostString& dirsep)
{
  lua_getfield(L, lua_upvalueindex(1), pname);
  if (l_unlikely(!lua_isstring(L, -1)))
  {
    lua_pushstring(L, pname);
    luaL_error(L, "'package.%s' must be a string");
  }
  HostString path = lua_tostring(L, -1);
  return searchpath(L, name, path, ".", dirsep);
}

static int checkload(lua_State* L, int stat, const HostString& filename)
{
  if (l_likely(stat))
  {                              /* module loaded successfully? */
    lua_pushstring(L, filename); /* will be 2nd argument to module */
    return 2;                    /* return open function and file name */
  }
  else
  {
    lua_pushvalue(L, 1);
    lua_pushstring(L, filename);
    lua_pushvalue(L, -3); /* original error message */
    return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s");
  }
}

static int searcher_Lua(lua_State* L)
{
  HostString name = luaL_checkstring(L, 1);
  if (!findfile(L, name, "path", LUA_LSUBSEP))
    return 1; /* module not found in this path (error message pushed) */
  HostString filename = lua_tostring(L, -1);
  return checkload(L, (luaL_loadfilex(L, &filename, nullptr) == LUA_OK), filename);
}

/*
** Try to find a load function for module 'modname' at file 'filename'.
** First, change '.' to '_' in 'modname'; then, if 'modname' has
** the form X-Y (that is, it has an "ignore mark"), build a function
** name "luaopen_X" and look for it. (For compatibility, if that
** fails, it also tries "luaopen_Y".) If there is no ignore mark,
** look for a function named "luaopen_modname".
*/
static int loadfunc(lua_State* L, const HostString& filename, const HostString& modname0)
{
  const InternalString* r = luaL_gsub(L, modname0, ".", LUA_OFSEP);
  HostString modname(r->bytes(), r->size());
  size_t mark = modname.find(*LUA_IGMARK);
  if (mark != HostString::npos)
  {
    int stat;
    lua_pushlstring(L, modname, 0, mark);
    const InternalString* openfunc = lua_pushfstring(L, "luaopen_%s");
    /* symbol is host text at the dlsym boundary: deep copy */
    stat = lookforfunc(L, filename, HostString(openfunc->bytes(), openfunc->size()));
    if (stat != ERRFUNC)
      return stat;
    modname = modname.substr(mark + 1); /* else go ahead and try old-style name */
  }
  lua_pushstring(L, modname);
  const InternalString* openfunc = lua_pushfstring(L, "luaopen_%s");
  return lookforfunc(L, filename, HostString(openfunc->bytes(), openfunc->size()));
}

static int searcher_C(lua_State* L)
{
  HostString name = luaL_checkstring(L, 1);
  if (!findfile(L, name, "cpath", LUA_CSUBSEP))
    return 1; /* module not found in this path (error message pushed) */
  HostString filename = lua_tostring(L, -1);
  return checkload(L, (loadfunc(L, filename, name) == 0), filename);
}

static int searcher_Croot(lua_State* L)
{
  HostString name = luaL_checkstring(L, 1);
  size_t p = name.find('.');
  int stat;
  if (p == HostString::npos)
    return 0; /* is root */
  if (!findfile(L, name.substr(0, p), "cpath", LUA_CSUBSEP))
    return 1; /* root not found (error message pushed) */
  HostString filename = lua_tostring(L, -1);
  if ((stat = loadfunc(L, filename, name)) != 0)
  {
    if (stat != ERRFUNC)
      return checkload(L, 0, filename); /* real error */
    else
    { /* open function not found */
      lua_pushstring(L, name);
      lua_pushstring(L, filename);
      lua_pushfstring(L, "no module '%s' in file '%s'");
      return 1;
    }
  }
  lua_pushstring(L, filename); /* will be 2nd argument to module */
  return 2;
}

static int searcher_preload(lua_State* L)
{
  HostString name = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  if (lua_getfield(L, -1, name) == LUA_TNIL)
  { /* not found? */
    lua_pushstring(L, name);
    lua_pushfstring(L, "no field package.preload['%s']");
    return 1;
  }
  else
  {
    lua_pushliteral(L, ":preload:");
    return 2;
  }
}

static void findloader(lua_State* L, const HostString& name)
{
  int i;
  luaL_Buffer msg; /* to build error message */
  /* push 'package.searchers' to index 3 in the stack */
  if (l_unlikely(lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE))
    luaL_error(L, "'package.searchers' must be a table");
  luaL_buffinit(L, &msg);
  luaL_addstring(&msg, "\n\t"); /* error-message prefix for first message */
  /*  iterate over available searchers to find a loader */
  for (i = 1;; i++)
  {
    if (l_unlikely(lua_rawgeti(L, 3, i) == LUA_TNIL))
    {                        /* no more searchers? */
      lua_pop(L, 1);         /* remove nil */
      luaL_buffsub(&msg, 2); /* remove last prefix */
      luaL_pushresult(&msg); /* create error message */
      lua_pushstring(L, name);
      lua_pushvalue(L, -2); /* error message from the buffer */
      luaL_error(L, "module '%s' not found:%s");
    }
    lua_pushstring(L, name);
    lua_call(L, 1, 2);         /* call it */
    if (lua_isfunction(L, -2)) /* did it find a loader? */
      return;                  /* module loader found */
    else if (lua_isstring(L, -2))
    {                               /* searcher returned error message? */
      lua_pop(L, 1);                /* remove extra return */
      luaL_addvalue(&msg);          /* concatenate error message */
      luaL_addstring(&msg, "\n\t"); /* prefix for next message */
    }
    else             /* no error message */
      lua_pop(L, 2); /* remove both returns */
  }
}

static int ll_require(lua_State* L)
{
  HostString name = luaL_checkstring(L, 1);
  lua_settop(L, 1); /* LOADED table will be at index 2 */
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_getfield(L, 2, name); /* LOADED[name] */
  if (lua_toboolean(L, -1)) /* is it there? */
    return 1;               /* package is already loaded */
  /* else must load package */
  lua_pop(L, 1); /* remove 'getfield' result */
  findloader(L, name);
  lua_rotate(L, -2, 1); /* function <-> loader data */
  lua_pushvalue(L, 1);  /* name is 1st argument to module loader */
  lua_pushvalue(L, -3); /* loader data is 2nd argument */
  /* stack: ...; loader data; loader function; mod. name; loader data */
  lua_call(L, 2, 1); /* run loader to load module */
  /* stack: ...; loader data; result from loader */
  if (!lua_isnil(L, -1))      /* non-nil return? */
    lua_setfield(L, 2, name); /* LOADED[name] = returned value */
  else
    lua_pop(L, 1); /* pop nil */
  if (lua_getfield(L, 2, name) == LUA_TNIL)
  {                           /* module set no value? */
    lua_pushboolean(L, 1);    /* use true as result */
    lua_copy(L, -1, -2);      /* replace loader result */
    lua_setfield(L, 2, name); /* LOADED[name] = true */
  }
  lua_rotate(L, -2, 1); /* loader data <-> module result  */
  return 2;             /* return module result and loader data */
}

/* }====================================================== */

static const luaL_Reg pk_funcs[] = {
    {"loadlib", ll_loadlib},
    {"searchpath", ll_searchpath},
    /* placeholders */
    {"preload", nullptr},
    {"cpath", nullptr},
    {"path", nullptr},
    {"searchers", nullptr},
    {"loaded", nullptr},
    {{}, nullptr}};

static const luaL_Reg ll_funcs[] = {
    {"require", ll_require},
    {{}, nullptr}};

static void createsearcherstable(lua_State* L)
{
  static const lua_CFunction searchers[] = {
      searcher_preload,
      searcher_Lua,
      searcher_C,
      searcher_Croot,
      nullptr};
  int i;
  /* create 'searchers' table */
  lua_createtable(L, sizeof(searchers) / sizeof(searchers[0]) - 1, 0);
  /* fill it with predefined searchers */
  for (i = 0; searchers[i] != nullptr; i++)
  {
    lua_pushvalue(L, -2); /* set 'package' as upvalue for all searchers */
    lua_pushcclosure(L, searchers[i], 1);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "searchers"); /* put it in field 'searchers' */
}

LUAMOD_API int luaopen_package(lua_State* L)
{
  luaL_getsubtable(L, LUA_REGISTRYINDEX, CLIBS); /* create CLIBS table */
  lua_pop(L, 1);                                 /* will not use it now */
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(pk_funcs) / sizeof((pk_funcs)[0]) - 1), luaL_setfuncs(L, pk_funcs, 0));                      /* create 'package' table */
  createsearcherstable(L);
  /* set paths */
  setpath(L, "path", LUA_PATH_VAR, LUA_PATH_DEFAULT);
  setpath(L, "cpath", LUA_CPATH_VAR, LUA_CPATH_DEFAULT);
  /* store config information */
  lua_pushliteral(L, LUA_DIRSEP "\n" LUA_PATH_SEP "\n" LUA_PATH_MARK "\n" LUA_EXEC_DIR "\n" LUA_IGMARK "\n");
  lua_setfield(L, -2, "config");
  /* set field 'loaded' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_setfield(L, -2, "loaded");
  /* set field 'preload' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  lua_setfield(L, -2, "preload");
  lua_pushglobaltable(L);
  lua_pushvalue(L, -2);          /* set 'package' as upvalue for next lib */
  luaL_setfuncs(L, ll_funcs, 1); /* open lib into global table */
  lua_pop(L, 1);                 /* pop global table */
  return 1;                      /* return 'package' table */
}
