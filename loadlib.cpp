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
#include "lstate.h"
#include "os_api.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{


/*
** system-dependent functions
*/

static OsLib* lsys_load(lua_State* L, byte_view path, int seeglb);

static lua_CFunction lsys_sym(lua_State* L, OsLib* lib, byte_view sym);

/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface,
** which is available in all POSIX systems.
** =========================================================================
*/

static OsLib* lsys_load(lua_State* L, byte_view path, int seeglb)
{
  OsLib* lib = os_dlopen(path, seeglb != 0);
  if (l_unlikely(lib == nullptr))
    lua_pushistring(L, os_dlerror());
  return lib;
}

static lua_CFunction lsys_sym(lua_State* L, OsLib* lib, byte_view sym)
{
  lua_CFunction f = os_dlfunc(lib, sym);
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
static void setpath(lua_State* L, byte_view fieldname, byte_view envname, byte_view dft)
{
  lua_pushlstring(L, envname, 0, envname.size());
  lua_pushlstring(L, LUA_VERSUFFIX, 0, LUA_VERSUFFIX.size());
  lua_pushfstringi(L, lt::fmt_ss);
  std::optional<byte_view> epath = os_getenv(lua_toistring(L, -1)->view()); /* try versioned name */
  if (!epath.has_value())                                    /* no versioned environment variable? */
    epath = os_getenv(envname);            /* try unversioned name */
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
static OsLib* checkclib(lua_State* L, byte_view path)
{
  OsLib* plib;
  lua_getfieldi(L, LUA_REGISTRYINDEX, lt::_CLIBS);
  lua_getfieldi(L, -1, path);
  plib = static_cast<OsLib*>(lua_touserdata(L, -1)); /* plib = lt::_CLIBS[path] */
  lua_pop(L, 2);                /* pop lt::_CLIBS table and 'plib' */
  return plib;
}

struct LibHandle : lua_UserObject
{
  OsLib* lib;
};

constexpr size_t LIBHANDLE_SIZE = 8;

static int lib_gc(lua_State* L)
{
  LibHandle* p = static_cast<LibHandle*>(lua_touserobject(L, 1));
  if (p->lib)
  {
    os_dlclose(p->lib);
    p->lib = nullptr;
  }
  return 0;
}

static CFuncInfo cfi_lib_gc(lib_gc, "package.lib_gc");

static void createlibstr(lua_State* L, OsLib* plib)
{
  std::unique_ptr<LibHandle> o = std::make_unique<LibHandle>();
  o->lib = plib;
  lua_newuserobject(L, std::move(o), LIBHANDLE_SIZE, 0);
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
static void addtoclib(lua_State* L, byte_view path, OsLib* plib)
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
  OsLib* reg = checkclib(L, path); /* check loaded C libraries */
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
    lua_pushlstring(L, ((stat == ERRLIB) ? lt::open_kw : lt::init), 0, ((stat == ERRLIB) ? lt::open_kw : lt::init).size());
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
    byte_span area = luaL_buffaddr(&buff);
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

/*
** The searchers read 'package.path' or 'package.cpath', which may call
** back ('__index' of 'package'), with a continuation: their work goes on
** from the value read (on the top), with the name at 1.
*/
static int findfile(lua_State* L, byte_view name, byte_view pname, byte_view dirsep)
{
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

/*
** (with 'package.path' read: 'ctx' 0; or a load of the file stopped by a
** freeze goes on: 'ctx' is the slot of its chunk name, see
** 'luaL_loadfileik', with the file name below)
*/
static int searcher_Luak(lua_State* L, int status, lua_KContext ctx)
{
  if (ctx != 0)
  {
    int base = (int)ctx;
    byte_view filename = lua_toistring(L, base - 1)->view();
    return checkload(L, luaL_loadfileikresume(L, status, std::nullopt, base, ctx, searcher_Luak) == LUA_OK, filename);
  }
  byte_view name = luaL_checklstring(L, 1);
  if (!findfile(L, name, lt::path, lt::LUA_DIRSEP))
    return 1;
  byte_view filename = lua_toistring(L, -1)->view();
  int base = lua_gettop(L) + 1;
  return checkload(L, luaL_loadfileik(L, filename, std::nullopt, base, searcher_Luak) == LUA_OK, filename);
}

LUA_KNAME("package", searcher_Luak);

static int searcher_Lua(lua_State* L)
{
  luaL_checklstring(L, 1);
  luaE_freezeonly(L);
  lua_getfieldik(L, lua_upvalueindex(1), lt::path, 0, searcher_Luak);
  return searcher_Luak(L, LUA_OK, 0);
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

/* (with 'package.cpath' read) */
static int searcher_Ck(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  byte_view name = luaL_checklstring(L, 1);
  if (!findfile(L, name, lt::cpath, lt::LUA_DIRSEP))
    return 1;
  const InternalString* filename = lua_toistring(L, -1);
  return checkload(L, loadfunc(L, filename->view(), name) == 0, filename->view());
}

LUA_KNAME("package", searcher_Ck);

static int searcher_C(lua_State* L)
{
  luaL_checklstring(L, 1);
  luaE_freezeonly(L);
  lua_getfieldik(L, lua_upvalueindex(1), lt::cpath, 0, searcher_Ck);
  return searcher_Ck(L, LUA_OK, 0);
}

/* (with 'package.cpath' read) */
static int searcher_Crootk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  byte_view name = luaL_checklstring(L, 1);
  byte_view prefix = name.substr(0, name.find('.'));
  int stat;
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

LUA_KNAME("package", searcher_Crootk);

static int searcher_Croot(lua_State* L)
{
  byte_view name = luaL_checklstring(L, 1);
  if (name.find('.') == byte_view::npos)
    return 0; /* is root */
  luaE_freezeonly(L);
  lua_getfieldik(L, lua_upvalueindex(1), lt::cpath, 0, searcher_Crootk);
  return searcher_Crootk(L, LUA_OK, 0);
}

/* (with the preload table read: 'ctx' 1; with its field: 2) */
static int searcher_preloadk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  byte_view name = luaL_checklstring(L, 1);
  if (ctx == 1)
    lua_getfieldik(L, -1, name, 2, searcher_preloadk);
  if (lua_isnil(L, -1))
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

LUA_KNAME("package", searcher_preloadk);

static int searcher_preload(lua_State* L)
{
  luaL_checklstring(L, 1);
  luaE_freezeonly(L);
  lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE, 1, searcher_preloadk);
  return searcher_preloadk(L, LUA_OK, 1);
}

/*
** 'require' calls back (the searchers, the loader, and the metamethods of
** 'package', 'package.loaded' and the registry), each with a
** continuation that goes on at a step of it ('REQ_*', with the searcher
** being tried). The name is at 1, LOADED at 2; while it looks for a
** loader, 'package.searchers' is at 3 and a box with the error message
** at 4.
*/
enum
{
  REQ_LOADED,    /* LOADED read */
  REQ_GOT,       /* LOADED[name] read */
  REQ_SEARCHERS, /* 'package.searchers' read */
  REQ_SEARCHED,  /* a searcher returned */
  REQ_LOADER,    /* the loader returned */
  REQ_SET,       /* LOADED[name] set to the loader's result */
  REQ_RESULT,    /* LOADED[name] read again */
  REQ_DONE       /* LOADED[name] set to true */
};

static int ll_requirek(lua_State* L, int status, lua_KContext ctx);

/* (the continuation context: the step, and the searcher) */
static lua_KContext reqctx(int step, lua_Integer i)
{
  return (lua_KContext)(i * 8 + step);
}

static int require_from(lua_State* L, int step, lua_Integer i)
{
  byte_view name = luaL_checklstring(L, 1);
  luaL_Buffer msg; /* to build error message */
  luaL_buffinit(L, &msg);
  for (;;)
  {
    switch (step)
    {
    case REQ_LOADED:
      lua_getfieldik(L, 2, name, reqctx(REQ_GOT, 0), ll_requirek); /* LOADED[name] */
      [[fallthrough]];
    case REQ_GOT:
      if (lua_toboolean(L, -1)) /* is it there? */
        return 1;               /* package is already loaded */
      /* else must load package */
      lua_pop(L, 1); /* remove 'getfield' result */
      /* push 'package.searchers' to index 3 in the stack */
      lua_getfieldik(L, lua_upvalueindex(1), lt::searchers, reqctx(REQ_SEARCHERS, 0), ll_requirek);
      [[fallthrough]];
    case REQ_SEARCHERS:
    {
      if (l_unlikely(lua_type(L, 3) != LUA_TTABLE))
        luaL_errori(L, lt::package_searchers_must_be_a_table);
      luaL_newbox(L); /* at 4 */
      luaL_addlstring(&msg, lt::n_t, 0, lt::n_t.size()); /* error-message prefix for first message */
      i = 1;
      break; /* iterate over available searchers to find a loader */
    }
    case REQ_SEARCHED:
      luaL_buffload(&msg, luaL_tobox(L, 4));
      if (lua_isfunction(L, -2)) /* did it find a loader? */
      {                          /* module loader found */
        lua_remove(L, 4);        /* (the box) */
        lua_rotate(L, -2, 1); /* function <-> loader data */
        lua_pushvalue(L, 1);  /* name is 1st argument to module loader */
        lua_pushvalue(L, -3); /* loader data is 2nd argument */
        /* stack: ...; loader data; loader function; mod. name; loader data */
        lua_callk(L, 2, 1, reqctx(REQ_LOADER, 0), ll_requirek); /* run loader to load module */
        step = REQ_LOADER;
        continue;
      }
      else if (lua_isstring(L, -2))
      {                               /* searcher returned error message? */
        lua_pop(L, 1);                /* remove extra return */
        luaL_addvalue(&msg);          /* concatenate error message */
        luaL_addlstring(&msg, lt::n_t, 0, lt::n_t.size()); /* prefix for next message */
      }
      else             /* no error message */
        lua_pop(L, 2); /* remove both returns */
      i++;
      break;
    case REQ_LOADER:
      /* stack: ...; loader data; result from loader */
      if (!lua_isnil(L, -1)) /* non-nil return? */
        lua_setfieldik(L, 2, name, reqctx(REQ_SET, 0), ll_requirek); /* LOADED[name] = returned value */
      else
        lua_pop(L, 1); /* pop nil */
      [[fallthrough]];
    case REQ_SET:
      lua_getfieldik(L, 2, name, reqctx(REQ_RESULT, 0), ll_requirek);
      [[fallthrough]];
    case REQ_RESULT:
      if (lua_isnil(L, -1))
      {                        /* module set no value? */
        lua_pushboolean(L, 1); /* use true as result */
        lua_copy(L, -1, -2);   /* replace loader result */
        lua_setfieldik(L, 2, name, reqctx(REQ_DONE, 0), ll_requirek); /* LOADED[name] = true */
      }
      [[fallthrough]];
    default: /* REQ_DONE */
      lua_rotate(L, -2, 1); /* loader data <-> module result  */
      return 2;             /* return module result and loader data */
    }
    /* try searcher 'i' */
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
    luaL_buffsave(&msg, luaL_tobox(L, 4));
    lua_callk(L, 1, 2, reqctx(REQ_SEARCHED, i), ll_requirek); /* call it */
    step = REQ_SEARCHED;
  }
}

static int ll_requirek(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  return require_from(L, (int)(ctx % 8), (lua_Integer)(ctx / 8));
}

LUA_KNAME("package", ll_requirek);

static int ll_require(lua_State* L)
{
  luaL_checklstring(L, 1);
  lua_settop(L, 1); /* LOADED table will be at index 2 */
  luaE_freezeonly(L);
  lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE, reqctx(REQ_LOADED, 0), ll_requirek);
  return require_from(L, REQ_LOADED, 0);
}

/* }====================================================== */

static CFuncInfo cfi_ll_loadlib(ll_loadlib, "package.ll_loadlib");
static CFuncInfo cfi_ll_searchpath(ll_searchpath, "package.ll_searchpath");

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

static CClosureNamer searcher_preload_kind(lua_CClosureKind{"package.searcher_preload", searcher_preload, 1, nullptr, nullptr});
static CClosureNamer searcher_Lua_kind(lua_CClosureKind{"package.searcher_Lua", searcher_Lua, 1, nullptr, nullptr});
static CClosureNamer searcher_C_kind(lua_CClosureKind{"package.searcher_C", searcher_C, 1, nullptr, nullptr});
static CClosureNamer searcher_Croot_kind(lua_CClosureKind{"package.searcher_Croot", searcher_Croot, 1, nullptr, nullptr});

static CClosureNamer require_kind(lua_CClosureKind{"package.ll_require", ll_require, 1, nullptr, nullptr});
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
  lua_createtable(L, std::size(searchers) - 1, 0);
  /* fill it with predefined searchers */
  for (i = 0; searchers[i] != nullptr; i++)
  {
    lua_pushvalue(L, -2); /* set 'package' as upvalue for all searchers */
    lua_pushcclosure(L, searchers[i], 1);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfieldi(L, -2, lt::searchers); /* put it in field 'searchers' */
}

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

