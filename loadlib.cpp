/*
** $Id: loadlib.c $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/



#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lua_internal.h"
#include "lobject.h"
#include "os_api.h"
#include "lualib.h"
#include "literals.h"


/*
** system-dependent functions
*/

static void* lsys_load(lua_State* L, byte_view path, int seeglb);

static lua_CFunction lsys_sym(lua_State* L, void* lib, byte_view sym);

/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface,
** which is available in all POSIX systems.
** =========================================================================
*/

static void* lsys_load(lua_State* L, byte_view path, int seeglb)
{
  OsLib* lib = os_dlopen(path, seeglb != 0);
  if (l_unlikely(lib == nullptr))
    lua_pushistring(L, os_dlerror());
  return reinterpret_cast<void*>(lib);
}

static lua_CFunction lsys_sym(lua_State* L, void* lib, byte_view sym)
{
  lua_CFunction f = reinterpret_cast<lua_CFunction>(os_dlsym(reinterpret_cast<OsLib*>(lib), sym));
  if (l_unlikely(f == nullptr))
    lua_pushistring(L, os_dlerror());
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
constexpr char LUA_PATH_VAR[] = "LUA_PATH";

constexpr char LUA_CPATH_VAR[] = "LUA_CPATH";

/*
** return registry.LUA_NOENV as a boolean
*/
static int noenv(lua_State* L)
{
  int b;
  lua_getfieldi(L, LUA_REGISTRYINDEX, lt::LUA_NOENV);
  b = lua_toboolean(L, -1);
  lua_pop(L, 1); /* remove value */
  return b;
}

/*
** Set a path. (If using the default path, assume it is a string
** literal in C and create it as an external string.)
*/
static void setpath(lua_State* L, byte_view fieldname, const char* envname, byte_view dft)
{
  byte_view env{envname};
  lua_pushlstring(L, env, 0, env.size());
  lua_pushlstring(L, LUA_VERSUFFIX, 0, LUA_VERSUFFIX.size());
  lua_pushfstringi(L, lt::fmt_ss);
  std::optional<byte_view> epath = os_getenv(lua_toistring(L, -1)->view()); /* try versioned name */
  if (!epath.has_value())                                    /* no versioned environment variable? */
    epath = os_getenv(byte_view{envname});            /* try unversioned name */
  if (!epath.has_value() || noenv(L)) /* no environment variable? */
    lua_pushlstring(L, dft, 0, dft.size()); /* use default */
  else
  {
    byte_view path = epath.value();
    size_t dftmark = path.find(lt::dsemicolon);
    if (dftmark == byte_view::npos)
      lua_pushlstring(L, path, 0, path.size()); /* nothing to change */
    else
    { /* path contains a ";;": insert default path in its place */
      luaL_Buffer b;
      luaL_buffinit(L, &b);
      if (dftmark > 0)
      { /* is there a prefix before ';;'? */
        luaL_addlstring(&b, path, 0, dftmark); /* add it */
        luaL_addchar(&b, lt::LUA_PATH_SEP[0]);
      }
      luaL_addlstring(&b, dft, 0, dft.size()); /* add default */
      if (dftmark + 2 < path.size())
      { /* is there a suffix after ';;'? */
        luaL_addchar(&b, lt::LUA_PATH_SEP[0]);
        luaL_addlstring(&b, path, dftmark + 2, path.size() - (dftmark + 2));
      }
      luaL_pushresult(&b);
    }
  }
  lua_setfieldi(L, -3, fieldname); /* package[fieldname] = path value */
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
** return registry.lt::_CLIBS[path]
*/
static void* checkclib(lua_State* L, byte_view path)
{
  void* plib;
  lua_getfieldi(L, LUA_REGISTRYINDEX, lt::_CLIBS);
  lua_getfieldi(L, -1, path);
  plib = lua_touserdata(L, -1); /* plib = lt::_CLIBS[path] */
  lua_pop(L, 2);                /* pop lt::_CLIBS table and 'plib' */
  return plib;
}

static int lib_gc(lua_State* L)
{
  void** p = (void**) lua_touserdata(L, 1);
  if (*p)
  {
    os_dlclose(reinterpret_cast<OsLib*>(*p));
    *p = nullptr;
  }
  return 0;
}

static void createlibstr(lua_State* L, void* plib)
{
  void** p = (void**) lua_newuserdatauv(L, sizeof(void*), 0);
  *p = plib;
  static CFuncInfo cfi_lib_gc("loadlib.__gc"_hs, lib_gc);
  luaL_newmetatablei(L, lt::_LOADLIB);
  lua_pushcfuncinfo(L, &cfi_lib_gc);
  lua_setfieldi(L, -2, lt::mm_gc);
  lua_setmetatable(L, -2);
}

/*
** registry.lt::_CLIBS[path] = plib          -- for queries.
** Also create a reference to strlib, so that the library string will
** only be collected when registry.lt::_CLIBS is collected.
*/
static void addtoclib(lua_State* L, byte_view path, void* plib)
{
  lua_getfieldi(L, LUA_REGISTRYINDEX, lt::_CLIBS);
  lua_pushlightuserdata(L, plib);
  lua_setfieldi(L, -2, path); /* lt::_CLIBS[path] = plib */
  createlibstr(L, plib);
  luaL_ref(L, -2); /* keep library string in lt::_CLIBS */
  lua_pop(L, 1);   /* pop lt::_CLIBS table */
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
static int lookforfunc(lua_State* L, byte_view path, byte_view sym)
{
  int loadlibonly = (!sym.empty() && sym[0] == '*');
  void* reg = checkclib(L, path); /* check loaded C libraries */
  if (reg == nullptr)
  {                                          /* must load library? */
    reg = lsys_load(L, path, loadlibonly); /* global symbols if 'sym'=='*' */
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
  const InternalString* path = luaL_checkistring(L, 1);
  const InternalString* init = luaL_checkistring(L, 2);
  int stat = lookforfunc(L, path->view(), init->view());
  if (l_likely(stat == 0)) /* no errors? */
    return 1;              /* return the loaded function */
  else
  { /* error; error message is on stack top */
    luaL_pushfail(L);
    lua_insert(L, -2);
    lua_pushlstring(L, ((stat == ERRLIB) ? lt::open_kw : lt::init), 0, (((stat == ERRLIB) ? lt::open_kw : lt::init)).size());
    return 3; /* return fail, error message, and where */
  }
}

/*
** {======================================================
** 'require' function
** =======================================================
*/

static int readable(byte_view filename)
{
  OsOpenResult o = os_fopen(filename, lt::r_mode);
  if (o.f == nullptr)
    return 0;
  os_fclose(o.f);
  return 1;
}

/*
** Given a path such as ";blabla.so;blublu.so", pushes the string
**
** no file 'blabla.so'
**  no file 'blublu.so'
*/
static void pusherrornotfound(lua_State* L, byte_view path)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addlstring(&b, lt::no_file, 0, lt::no_file.size());
  luaL_addgsubi(&b, path, lt::LUA_PATH_SEP, lt::n_tno_file);
  luaL_addlstring(&b, lt::squote, 0, lt::squote.size());
  luaL_pushresult(&b);
}

static int searchpath(lua_State* L, byte_view name,
                      byte_view path,
                      byte_view sep,
                      byte_view dirsep)
{
  luaL_Buffer buff;
  /* separator is non-empty and appears in 'name'? */
  if (!sep.empty() && name.find(sep) != byte_view::npos)
  {
    name = luaL_gsubi(L, name, sep, dirsep);
  }
  luaL_buffinit(L, &buff);
  /* add path to the buffer, replacing marks ('?') with the file name */
  luaL_addgsubi(&buff, path, lt::LUA_PATH_MARK, name);
  {
    std::span<char> area = luaL_buffaddr(&buff);
    byte_view list{area.data(), luaL_bufflen(&buff)};
    size_t pos = 0;
    while (pos < list.size())
    {
      size_t sepmark = list.find(lt::LUA_PATH_SEP, pos);
      if (sepmark == byte_view::npos)          /* separator not found? */
        sepmark = list.size();                        /* name goes until the end */
      byte_view filename = byte_view(list).substr(pos, sepmark - pos);
      if (readable(filename))
      {                                    /* does file exist and is readable? */
        lua_pushistringref(L, filename);  /* save and return name */
        return 1;
      }
      pos = sepmark + 1;
    }
  }
  luaL_pushresult(&buff);                     /* push path to create error message */
  pusherrornotfound(L, *lua_tolstring(L, -1)); /* create error message */
  return 0; /* not found */
}

static int ll_searchpath(lua_State* L)
{
  int found = searchpath(L, luaL_checklstring(L, 1),
                         luaL_checklstring(L, 2),
                         luaL_optlstring(L, 3, lt::dot),
                         luaL_optlstring(L, 4, lt::LUA_DIRSEP));
  if (found)
    return 1;
  else
  { /* error message is on top of the stack */
    luaL_pushfail(L);
    lua_insert(L, -2);
    return 2; /* return fail + error message */
  }
}

static int findfile(lua_State* L, byte_view name, byte_view pname, byte_view dirsep)
{
  lua_getfieldi(L, lua_upvalueindex(1), pname);
  if (l_unlikely(!lua_isstring(L, -1)))
  {
    lua_pushlstring(L, pname, 0, pname.size());
    luaL_errori(L, lt::package_s_must_be_a_string);
  }
  byte_view path = *lua_tolstring(L, -1);
  return searchpath(L, name, path, lt::dot, dirsep);
}

static int checkload(lua_State* L, int stat, byte_view filename)
{
  if (l_likely(stat))
  {                                                /* module loaded successfully? */
    lua_pushlstring(L, filename, 0, filename.size()); /* will be 2nd argument to module */
    return 2;                    /* return open function and file name */
  }
  else
  {
    lua_pushvalue(L, 1);
    lua_pushlstring(L, filename, 0, filename.size());
    lua_pushvalue(L, -3);
    return luaL_errori(L, lt::error_loading_module_s_from_file_s_n_t_s);
  }
}

static int searcher_Lua(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  if (!findfile(L, name, lt::path, lt::LUA_DIRSEP))
    return 1;

  byte_view filename = lua_toistring(L, -1)->view();
  return checkload(L, (luaL_loadfilei(L, filename, std::nullopt) == LUA_OK), filename);
}

/*
** Try to find a load function for module 'modname' at file 'filename'.
** First, change '.' to '_' in 'modname'; then, if 'modname' has
** the form X-Y (that is, it has an "ignore mark"), build a function
** name "luaopen_X" and look for it. (For compatibility, if that
** fails, it also tries "luaopen_Y".) If there is no ignore mark,
** look for a function named "luaopen_modname".
*/
static int loadfunc(lua_State* L, byte_view filename, byte_view modname0)
{
  byte_view modname = luaL_gsubi(L, modname0, lt::dot, lt::underscore);
  size_t mark = modname.find(lt::minus_ch);
  if (mark != byte_view::npos)
  {
    int stat;
    lua_pushlstring(L, modname, 0, mark);
    lua_pushfstringi(L, lt::luaopen_s);
    const InternalString* openfunc = lua_toistring(L, -1);
    stat = lookforfunc(L, filename, openfunc->view());
    if (stat != ERRFUNC)
      return stat;
    modname = byte_view(modname).substr(mark + 1); /* else go ahead and try old-style name */
  }
  lua_pushlstring(L, modname, 0, modname.size());
  lua_pushfstringi(L, lt::luaopen_s);
  const InternalString* openfunc = lua_toistring(L, -1);
  return lookforfunc(L, filename, openfunc->view());
}

static int searcher_C(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  if (!findfile(L, name, lt::cpath, lt::LUA_DIRSEP))
    return 1;
  const InternalString* filename = lua_toistring(L, -1);
  return checkload(L, (loadfunc(L, filename->view(), name) == 0), filename->view());
}

static int searcher_Croot(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  size_t p = name.find('.');
  int stat;
  if (p == byte_view::npos)
    return 0; /* is root */
  byte_view prefix = name.substr(0, p);
  if (!findfile(L, prefix, lt::cpath, lt::LUA_DIRSEP))
    return 1;
  const InternalString* filename = lua_toistring(L, -1);
  if ((stat = loadfunc(L, filename->view(), name)) != 0)
  {
    if (stat != ERRFUNC)
      return checkload(L, 0, filename->view()); /* real error */
    else
    { /* open function not found */
      lua_pushlstring(L, name, 0, name.size());
      lua_pushistringref(L, filename->view());
      lua_pushfstringi(L, lt::no_module_s_in_file_s);
      return 1;
    }
  }
  lua_pushistringref(L, filename->view()); /* will be 2nd argument to module */
  return 2;
}

static int searcher_preload(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  if (lua_getfieldi(L, -1, name) == LUA_TNIL)
  { /* not found? */
    lua_pushlstring(L, name, 0, name.size());
    lua_pushfstringi(L, lt::no_field_package_preload_s);
    return 1;
  }
  else
  {
    lua_pushistringref(L, lt::preload_1);
    return 2;
  }
}

static void findloader(lua_State* L, byte_view name)
{
  int i;
  luaL_Buffer msg; /* to build error message */
  /* push 'package.searchers' to index 3 in the stack */
  if (l_unlikely(lua_getfieldi(L, lua_upvalueindex(1), lt::searchers) != LUA_TTABLE))
    luaL_errori(L, lt::package_searchers_must_be_a_table);
  luaL_buffinit(L, &msg);
  luaL_addlstring(&msg, lt::n_t, 0, lt::n_t.size()); /* error-message prefix for first message */
  /*  iterate over available searchers to find a loader */
  for (i = 1;; i++)
  {
    if (l_unlikely(lua_rawgeti(L, 3, i) == LUA_TNIL))
    {                        /* no more searchers? */
      lua_pop(L, 1);         /* remove nil */
      luaL_buffsub(&msg, 2); /* remove last prefix */
      luaL_pushresult(&msg); /* create error message */
      lua_pushlstring(L, name, 0, name.size());
      lua_pushvalue(L, -2);
      luaL_errori(L, lt::module_s_not_found_s);
    }
    lua_pushlstring(L, name, 0, name.size());
    lua_call(L, 1, 2);         /* call it */
    if (lua_isfunction(L, -2)) /* did it find a loader? */
      return;                  /* module loader found */
    else if (lua_isstring(L, -2))
    {                               /* searcher returned error message? */
      lua_pop(L, 1);                /* remove extra return */
      luaL_addvalue(&msg);          /* concatenate error message */
      luaL_addlstring(&msg, lt::n_t, 0, lt::n_t.size()); /* prefix for next message */
    }
    else             /* no error message */
      lua_pop(L, 2); /* remove both returns */
  }
}

static int ll_require(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  lua_settop(L, 1); /* LOADED table will be at index 2 */
  lua_getfieldi(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_getfieldi(L, 2, name); /* LOADED[name] */
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
  if (!lua_isnil(L, -1))       /* non-nil return? */
    lua_setfieldi(L, 2, name); /* LOADED[name] = returned value */
  else
    lua_pop(L, 1); /* pop nil */
  if (lua_getfieldi(L, 2, name) == LUA_TNIL)
  {                            /* module set no value? */
    lua_pushboolean(L, 1);     /* use true as result */
    lua_copy(L, -1, -2);       /* replace loader result */
    lua_setfieldi(L, 2, name); /* LOADED[name] = true */
  }
  lua_rotate(L, -2, 1); /* loader data <-> module result  */
  return 2;             /* return module result and loader data */
}

/* }====================================================== */

static CFuncInfo cfi_ll_loadlib("loadlib"_hs, ll_loadlib);
static CFuncInfo cfi_ll_searchpath("searchpath"_hs, ll_searchpath);

static const luaL_RegCFI pk_funcs[] = {
  {lt::loadlib, &cfi_ll_loadlib},
  {lt::searchpath, &cfi_ll_searchpath},
  {lt::preload_2, nullptr},
  {lt::cpath, nullptr},
  {lt::path, nullptr},
  {lt::searchers, nullptr},
  {lt::loaded, nullptr},
};

static const luaL_Regi ll_funcs[] = {
  {lt::require, ll_require},
};

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
  lua_setfieldi(L, -2, lt::searchers); /* put it in field 'searchers' */
}

LUAMOD_API int luaopen_package(lua_State* L)
{
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, lt::_CLIBS); /* create lt::_CLIBS table */
  lua_pop(L, 1);                                 /* will not use it now */
  luaL_newlibcfi(L, pk_funcs);
  createsearcherstable(L);
  /* set paths */
  setpath(L, lt::path, LUA_PATH_VAR, lt::LUA_PATH_DEFAULT);
  setpath(L, lt::cpath, LUA_CPATH_VAR, lt::LUA_CPATH_DEFAULT);
  /* store config information */
  lua_pushlstring(L, lt::CONFIG, 0, lt::CONFIG.size());
  lua_setfieldi(L, -2, lt::config);
  /* set field 'loaded' */
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_setfieldi(L, -2, lt::loaded);
  /* set field 'preload' */
  luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  lua_setfieldi(L, -2, lt::preload_2);
  lua_pushglobaltable(L);
  lua_pushvalue(L, -2);          /* set 'package' as upvalue for next lib */
  luaL_setfuncsi(L, ll_funcs, 1); /* open lib into global table */
  lua_pop(L, 1);                 /* pop global table */
  return 1;                      /* return 'package' table */
}
