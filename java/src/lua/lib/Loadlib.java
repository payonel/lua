package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.lib.Lualib.LUA_VERSUFFIX;
import static lua.platform.OsApi.*;
import static lua.runtime.Lstate.luaE_freezeonly;

import lua.api.CClosureNamer;
import lua.api.KNamer;
import lua.api.luaL_Buffer;
import lua.api.luaL_RegCFI;
import lua.api.luaL_Regi;
import lua.api.lua_CClosureKind;
import lua.api.lua_CFunction;
import lua.api.lua_KFunction;
import lua.api.lua_UserObject;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.object.CFuncInfo;
import lua.platform.OsLib;
import lua.platform.OsOpenResult;
import lua.runtime.lua_State;

/*
** $Id: loadlib.c $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/
public final class Loadlib
{
  private Loadlib()
  {
  }


  /*
  ** system-dependent functions
  */

  /*
  ** {========================================================================
  ** This is an implementation of loadlib based on the dlfcn interface,
  ** which is available in all POSIX systems.
  ** =========================================================================
  */

  private static OsLib lsys_load(lua_State L, ByteView path, int seeglb)
  {
    OsLib lib = os_dlopen(path, seeglb != 0);
    if (lib == null)
      lua_pushistring(L, os_dlerror());
    return lib;
  }

  private static lua_CFunction lsys_sym(lua_State L, OsLib lib, ByteView sym)
  {
    lua_CFunction f = os_dlfunc(lib, sym);
    if (f == null)
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
  private static final ByteView LUA_PATH_VAR = ByteView.of("LUA_PATH");

  private static final ByteView LUA_CPATH_VAR = ByteView.of("LUA_CPATH");

  /*
  ** return registry.LUA_NOENV as a boolean
  */
  private static int noenv(lua_State L)
  {
    int b;
    lua_getfieldi(L, LUA_REGISTRYINDEX, Lt.LUA_NOENV);
    b = lua_toboolean(L, -1);
    lua_pop(L, 1); /* remove value */
    return b;
  }

  /*
  ** Set a path. (If using the default path, assume it is a string
  ** literal in C and create it as an external string.)
  */
  private static void setpath(lua_State L, ByteView fieldname, ByteView envname, ByteView dft)
  {
    lua_pushlstring(L, envname, 0, envname.size());
    lua_pushlstring(L, LUA_VERSUFFIX, 0, LUA_VERSUFFIX.size());
    lua_pushfstringi(L, Lt.fmt_ss);
    ByteView epath = os_getenv(lua_toistring(L, -1).view()); /* try versioned name */
    if (epath == null)                                    /* no versioned environment variable? */
      epath = os_getenv(envname);            /* try unversioned name */
    if (epath == null || noenv(L) != 0) /* no environment variable? */
      lua_pushlstring(L, dft, 0, dft.size()); /* use default */
    else
    {
      ByteView path = epath;
      int dftmark = path.find(Lt.dsemicolon);
      if (dftmark == ByteView.npos)
        lua_pushlstring(L, path, 0, path.size()); /* nothing to change */
      else
      { /* path contains a ";;": insert default path in its place */
        luaL_Buffer b = new luaL_Buffer();
        try
        {
          luaL_buffinit(L, b);
          if (dftmark > 0)
          { /* is there a prefix before ';;'? */
            luaL_addlstring(b, path, 0, dftmark); /* add it */
            luaL_addchar(b, Lt.LUA_PATH_SEP.at(0));
          }
          luaL_addlstring(b, dft, 0, dft.size()); /* add default */
          if (dftmark + 2 < path.size())
          { /* is there a suffix after ';;'? */
            luaL_addchar(b, Lt.LUA_PATH_SEP.at(0));
            luaL_addlstring(b, path, dftmark + 2, path.size() - (dftmark + 2));
          }
          luaL_pushresult(b);
        }
        finally
        {
          b.release();
        }
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
  private static OsLib checkclib(lua_State L, ByteView path)
  {
    OsLib plib;
    lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._CLIBS);
    lua_getfieldi(L, -1, path);
    plib = (OsLib) lua_touserdata(L, -1); /* plib = lt::_CLIBS[path] */
    lua_pop(L, 2);                /* pop lt::_CLIBS table and 'plib' */
    return plib;
  }

  private static final class LibHandle implements lua_UserObject
  {
    OsLib lib;
  }

  private static final int LIBHANDLE_SIZE = 8;

  private static int lib_gc(lua_State L)
  {
    LibHandle p = (LibHandle) lua_touserobject(L, 1);
    if (p.lib != null)
    {
      os_dlclose(p.lib);
      p.lib = null;
    }
    return 0;
  }

  private static final lua_CFunction lib_gc = Loadlib::lib_gc;

  private static final CFuncInfo cfi_lib_gc = new CFuncInfo(lib_gc, "package.lib_gc");

  private static void createlibstr(lua_State L, OsLib plib)
  {
    LibHandle o = new LibHandle();
    o.lib = plib;
    lua_newuserobject(L, o, LIBHANDLE_SIZE, 0);
    luaL_newmetatablei(L, Lt._LOADLIB);
    lua_pushcfuncinfo(L, cfi_lib_gc);
    lua_setfieldi(L, -2, Lt.mm_gc);
    lua_setmetatable(L, -2);
  }

  /*
  ** registry.lt::_CLIBS[path] = plib          -- for queries.
  ** Also create a reference to strlib, so that the library string will
  ** only be collected when registry.lt::_CLIBS is collected.
  */
  private static void addtoclib(lua_State L, ByteView path, OsLib plib)
  {
    lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._CLIBS);
    lua_pushlightuserdata(L, plib);
    lua_setfieldi(L, -2, path); /* lt::_CLIBS[path] = plib */
    createlibstr(L, plib);
    luaL_ref(L, -2); /* keep library string in lt::_CLIBS */
    lua_pop(L, 1);   /* pop lt::_CLIBS table */
  }

  /* error codes for 'lookforfunc' */
  private static final int ERRLIB = 1;
  private static final int ERRFUNC = 2;

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
  private static int lookforfunc(lua_State L, ByteView path, ByteView sym)
  {
    int loadlibonly = (!sym.empty() && sym.at(0) == '*') ? 1 : 0;
    OsLib reg = checkclib(L, path); /* check loaded C libraries */
    if (reg == null)
    {                                          /* must load library? */
      reg = lsys_load(L, path, loadlibonly); /* global symbols if 'sym'=='*' */
      if (reg == null)
        return ERRLIB; /* unable to load library */
      addtoclib(L, path, reg);
    }
    if (loadlibonly != 0)
    {                        /* loading only library (no function)? */
      lua_pushboolean(L, 1); /* return 'true' */
      return 0;              /* no errors */
    }
    else
    {
      lua_CFunction f = lsys_sym(L, reg, sym);
      if (f == null)
        return ERRFUNC;        /* unable to find function */
      lua_pushcfunction(L, f); /* else create new function */
      return 0;                /* no errors */
    }
  }

  private static int ll_loadlib(lua_State L)
  {
    InternalString path = luaL_checkistring(L, 1);
    InternalString init = luaL_checkistring(L, 2);
    int stat = lookforfunc(L, path.view(), init.view());
    if (stat == 0) /* no errors? */
      return 1;              /* return the loaded function */
    else
    { /* error; error message is on stack top */
      luaL_pushfail(L);
      lua_insert(L, -2);
      lua_pushlstring(L, ((stat == ERRLIB) ? Lt.open_kw : Lt.init), 0, ((stat == ERRLIB) ? Lt.open_kw : Lt.init).size());
      return 3; /* return fail, error message, and where */
    }
  }

  private static final lua_CFunction ll_loadlib = Loadlib::ll_loadlib;

  /*
  ** {======================================================
  ** 'require' function
  ** =======================================================
  */

  private static int readable(ByteView filename)
  {
    OsOpenResult o = os_fopen(filename, Lt.r_mode);
    if (o.f == null)
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
  private static void pusherrornotfound(lua_State L, ByteView path)
  {
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      luaL_addlstring(b, Lt.no_file, 0, Lt.no_file.size());
      luaL_addgsubi(b, path, Lt.LUA_PATH_SEP, Lt.n_tno_file);
      luaL_addlstring(b, Lt.squote, 0, Lt.squote.size());
      luaL_pushresult(b);
    }
    finally
    {
      b.release();
    }
  }

  private static int searchpath(lua_State L, ByteView name,
                                ByteView path,
                                ByteView sep,
                                ByteView dirsep)
  {
    luaL_Buffer buff = new luaL_Buffer();
    try
    {
      /* separator is non-empty and appears in 'name'? */
      if (!sep.empty() && name.find(sep) != ByteView.npos)
      {
        name = luaL_gsubi(L, name, sep, dirsep);
      }
      luaL_buffinit(L, buff);
      /* add path to the buffer, replacing marks ('?') with the file name */
      luaL_addgsubi(buff, path, Lt.LUA_PATH_MARK, name);
      {
        ByteSpan area = luaL_buffaddr(buff);
        ByteView list = new ByteView(area.array(), area.offset(), luaL_bufflen(buff));
        int pos = 0;
        while (pos < list.size())
        {
          int sepmark = list.find(Lt.LUA_PATH_SEP, pos);
          if (sepmark == ByteView.npos)          /* separator not found? */
            sepmark = list.size();                        /* name goes until the end */
          ByteView filename = list.substr(pos, sepmark - pos);
          if (readable(filename) != 0)
          {                                    /* does file exist and is readable? */
            lua_pushistringref(L, filename);  /* save and return name */
            return 1;
          }
          pos = sepmark + 1;
        }
      }
      luaL_pushresult(buff);                     /* push path to create error message */
      pusherrornotfound(L, lua_tolstring(L, -1)); /* create error message */
      return 0; /* not found */
    }
    finally
    {
      buff.release();
    }
  }

  private static int ll_searchpath(lua_State L)
  {
    int found = searchpath(L, luaL_checklstring(L, 1),
                           luaL_checklstring(L, 2),
                           luaL_optlstring(L, 3, Lt.dot),
                           luaL_optlstring(L, 4, Lt.LUA_DIRSEP));
    if (found != 0)
      return 1;
    else
    { /* error message is on top of the stack */
      luaL_pushfail(L);
      lua_insert(L, -2);
      return 2; /* return fail + error message */
    }
  }

  private static final lua_CFunction ll_searchpath = Loadlib::ll_searchpath;

  /*
  ** The searchers read 'package.path' or 'package.cpath', which may call
  ** back ('__index' of 'package'), with a continuation: their work goes on
  ** from the value read (on the top), with the name at 1.
  */
  private static int findfile(lua_State L, ByteView name, ByteView pname, ByteView dirsep)
  {
    if (lua_isstring(L, -1) == 0)
    {
      lua_pushlstring(L, pname, 0, pname.size());
      luaL_errori(L, Lt.package_s_must_be_a_string);
    }
    ByteView path = lua_tolstring(L, -1);
    return searchpath(L, name, path, Lt.dot, dirsep);
  }

  private static int checkload(lua_State L, int stat, ByteView filename)
  {
    if (stat != 0)
    {                                                /* module loaded successfully? */
      lua_pushlstring(L, filename, 0, filename.size()); /* will be 2nd argument to module */
      return 2;                    /* return open function and file name */
    }
    else
    {
      lua_pushvalue(L, 1);
      lua_pushlstring(L, filename, 0, filename.size());
      lua_pushvalue(L, -3);
      return luaL_errori(L, Lt.error_loading_module_s_from_file_s_n_t_s);
    }
  }

  /*
  ** (with 'package.path' read: 'ctx' 0; or a load of the file stopped by a
  ** freeze goes on: 'ctx' is the slot of its chunk name, see
  ** 'luaL_loadfileik', with the file name below)
  */
  private static int searcher_Luak(lua_State L, int status, long ctx)
  {
    if (ctx != 0)
    {
      int base = (int) ctx;
      ByteView filename = lua_toistring(L, base - 1).view();
      return checkload(L, (luaL_loadfileikresume(L, status, null, base, ctx, searcher_Luak) == LUA_OK) ? 1 : 0, filename);
    }
    ByteView name = luaL_checklstring(L, 1);
    if (findfile(L, name, Lt.path, Lt.LUA_DIRSEP) == 0)
      return 1;
    ByteView filename = lua_toistring(L, -1).view();
    int base = lua_gettop(L) + 1;
    return checkload(L, (luaL_loadfileik(L, filename, null, base, searcher_Luak) == LUA_OK) ? 1 : 0, filename);
  }

  private static final lua_KFunction searcher_Luak = Loadlib::searcher_Luak;

  private static final KNamer knamer_searcher_Luak = new KNamer("package.searcher_Luak", searcher_Luak);

  private static int searcher_Lua(lua_State L)
  {
    luaL_checklstring(L, 1);
    luaE_freezeonly(L);
    lua_getfieldik(L, lua_upvalueindex(1), Lt.path, 0, searcher_Luak);
    return searcher_Luak(L, LUA_OK, 0);
  }

  private static final lua_CFunction searcher_Lua = Loadlib::searcher_Lua;

  /*
  ** Try to find a load function for module 'modname' at file 'filename'.
  ** First, change '.' to '_' in 'modname'; then, if 'modname' has
  ** the form X-Y (that is, it has an "ignore mark"), build a function
  ** name "luaopen_X" and look for it. (For compatibility, if that
  ** fails, it also tries "luaopen_Y".) If there is no ignore mark,
  ** look for a function named "luaopen_modname".
  */
  private static int loadfunc(lua_State L, ByteView filename, ByteView modname0)
  {
    ByteView modname = luaL_gsubi(L, modname0, Lt.dot, Lt.underscore);
    int mark = modname.find(Lt.minus_ch);
    if (mark != ByteView.npos)
    {
      int stat;
      lua_pushlstring(L, modname, 0, mark);
      lua_pushfstringi(L, Lt.luaopen_s);
      InternalString openfunc = lua_toistring(L, -1);
      stat = lookforfunc(L, filename, openfunc.view());
      if (stat != ERRFUNC)
        return stat;
      modname = modname.substr(mark + 1); /* else go ahead and try old-style name */
    }
    lua_pushlstring(L, modname, 0, modname.size());
    lua_pushfstringi(L, Lt.luaopen_s);
    InternalString openfunc = lua_toistring(L, -1);
    return lookforfunc(L, filename, openfunc.view());
  }

  /* (with 'package.cpath' read) */
  private static int searcher_Ck(lua_State L, int status, long ctx)
  {
    ByteView name = luaL_checklstring(L, 1);
    if (findfile(L, name, Lt.cpath, Lt.LUA_DIRSEP) == 0)
      return 1;
    InternalString filename = lua_toistring(L, -1);
    return checkload(L, (loadfunc(L, filename.view(), name) == 0) ? 1 : 0, filename.view());
  }

  private static final lua_KFunction searcher_Ck = Loadlib::searcher_Ck;

  private static final KNamer knamer_searcher_Ck = new KNamer("package.searcher_Ck", searcher_Ck);

  private static int searcher_C(lua_State L)
  {
    luaL_checklstring(L, 1);
    luaE_freezeonly(L);
    lua_getfieldik(L, lua_upvalueindex(1), Lt.cpath, 0, searcher_Ck);
    return searcher_Ck(L, LUA_OK, 0);
  }

  private static final lua_CFunction searcher_C = Loadlib::searcher_C;

  /* (with 'package.cpath' read) */
  private static int searcher_Crootk(lua_State L, int status, long ctx)
  {
    ByteView name = luaL_checklstring(L, 1);
    ByteView prefix = name.substr(0, name.find('.'));
    int stat;
    if (findfile(L, prefix, Lt.cpath, Lt.LUA_DIRSEP) == 0)
      return 1;
    InternalString filename = lua_toistring(L, -1);
    if ((stat = loadfunc(L, filename.view(), name)) != 0)
    {
      if (stat != ERRFUNC)
        return checkload(L, 0, filename.view()); /* real error */
      else
      { /* open function not found */
        lua_pushlstring(L, name, 0, name.size());
        lua_pushistringref(L, filename.view());
        lua_pushfstringi(L, Lt.no_module_s_in_file_s);
        return 1;
      }
    }
    lua_pushistringref(L, filename.view()); /* will be 2nd argument to module */
    return 2;
  }

  private static final lua_KFunction searcher_Crootk = Loadlib::searcher_Crootk;

  private static final KNamer knamer_searcher_Crootk = new KNamer("package.searcher_Crootk", searcher_Crootk);

  private static int searcher_Croot(lua_State L)
  {
    ByteView name = luaL_checklstring(L, 1);
    if (name.find('.') == ByteView.npos)
      return 0; /* is root */
    luaE_freezeonly(L);
    lua_getfieldik(L, lua_upvalueindex(1), Lt.cpath, 0, searcher_Crootk);
    return searcher_Crootk(L, LUA_OK, 0);
  }

  private static final lua_CFunction searcher_Croot = Loadlib::searcher_Croot;

  /* (with the preload table read: 'ctx' 1; with its field: 2) */
  private static int searcher_preloadk(lua_State L, int status, long ctx)
  {
    ByteView name = luaL_checklstring(L, 1);
    if (ctx == 1)
      lua_getfieldik(L, -1, name, 2, searcher_preloadk);
    if (lua_isnil(L, -1) != 0)
    { /* not found? */
      lua_pushlstring(L, name, 0, name.size());
      lua_pushfstringi(L, Lt.no_field_package_preload_s);
      return 1;
    }
    else
    {
      lua_pushistringref(L, Lt.preload_1);
      return 2;
    }
  }

  private static final lua_KFunction searcher_preloadk = Loadlib::searcher_preloadk;

  private static final KNamer knamer_searcher_preloadk = new KNamer("package.searcher_preloadk", searcher_preloadk);

  private static int searcher_preload(lua_State L)
  {
    luaL_checklstring(L, 1);
    luaE_freezeonly(L);
    lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE, 1, searcher_preloadk);
    return searcher_preloadk(L, LUA_OK, 1);
  }

  private static final lua_CFunction searcher_preload = Loadlib::searcher_preload;

  /*
  ** 'require' calls back (the searchers, the loader, and the metamethods of
  ** 'package', 'package.loaded' and the registry), each with a
  ** continuation that goes on at a step of it ('REQ_*', with the searcher
  ** being tried). The name is at 1, LOADED at 2; while it looks for a
  ** loader, 'package.searchers' is at 3 and a box with the error message
  ** at 4.
  */
  private static final int REQ_LOADED = 0;    /* LOADED read */
  private static final int REQ_GOT = 1;       /* LOADED[name] read */
  private static final int REQ_SEARCHERS = 2; /* 'package.searchers' read */
  private static final int REQ_SEARCHED = 3;  /* a searcher returned */
  private static final int REQ_LOADER = 4;    /* the loader returned */
  private static final int REQ_SET = 5;       /* LOADED[name] set to the loader's result */
  private static final int REQ_RESULT = 6;    /* LOADED[name] read again */
  private static final int REQ_DONE = 7;      /* LOADED[name] set to true */

  /* (the continuation context: the step, and the searcher) */
  private static long reqctx(int step, long i)
  {
    return i * 8 + step;
  }

  private static int require_from(lua_State L, int step, long i)
  {
    ByteView name = luaL_checklstring(L, 1);
    luaL_Buffer msg = new luaL_Buffer(); /* to build error message */
    try
    {
      luaL_buffinit(L, msg);
      for (;;)
      {
        switch (step)
        {
        case REQ_LOADED:
          lua_getfieldik(L, 2, name, reqctx(REQ_GOT, 0), ll_requirek); /* LOADED[name] */
        case REQ_GOT:
          if (lua_toboolean(L, -1) != 0) /* is it there? */
            return 1;               /* package is already loaded */
          /* else must load package */
          lua_pop(L, 1); /* remove 'getfield' result */
          /* push 'package.searchers' to index 3 in the stack */
          lua_getfieldik(L, lua_upvalueindex(1), Lt.searchers, reqctx(REQ_SEARCHERS, 0), ll_requirek);
        case REQ_SEARCHERS:
        {
          if (lua_type(L, 3) != LUA_TTABLE)
            luaL_errori(L, Lt.package_searchers_must_be_a_table);
          luaL_newbox(L); /* at 4 */
          luaL_addlstring(msg, Lt.n_t, 0, Lt.n_t.size()); /* error-message prefix for first message */
          i = 1;
          break; /* iterate over available searchers to find a loader */
        }
        case REQ_SEARCHED:
          luaL_buffload(msg, luaL_tobox(L, 4));
          if (lua_isfunction(L, -2) != 0) /* did it find a loader? */
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
          else if (lua_isstring(L, -2) != 0)
          {                               /* searcher returned error message? */
            lua_pop(L, 1);                /* remove extra return */
            luaL_addvalue(msg);          /* concatenate error message */
            luaL_addlstring(msg, Lt.n_t, 0, Lt.n_t.size()); /* prefix for next message */
          }
          else             /* no error message */
            lua_pop(L, 2); /* remove both returns */
          i++;
          break;
        case REQ_LOADER:
          /* stack: ...; loader data; result from loader */
          if (lua_isnil(L, -1) == 0) /* non-nil return? */
            lua_setfieldik(L, 2, name, reqctx(REQ_SET, 0), ll_requirek); /* LOADED[name] = returned value */
          else
            lua_pop(L, 1); /* pop nil */
        case REQ_SET:
          lua_getfieldik(L, 2, name, reqctx(REQ_RESULT, 0), ll_requirek);
        case REQ_RESULT:
          if (lua_isnil(L, -1) != 0)
          {                        /* module set no value? */
            lua_pushboolean(L, 1); /* use true as result */
            lua_copy(L, -1, -2);   /* replace loader result */
            lua_setfieldik(L, 2, name, reqctx(REQ_DONE, 0), ll_requirek); /* LOADED[name] = true */
          }
        default: /* REQ_DONE */
          lua_rotate(L, -2, 1); /* loader data <-> module result  */
          return 2;             /* return module result and loader data */
        }
        /* try searcher 'i' */
        if (lua_rawgeti(L, 3, i) == LUA_TNIL)
        {                        /* no more searchers? */
          lua_pop(L, 1);         /* remove nil */
          luaL_buffsub(msg, 2); /* remove last prefix */
          luaL_pushresult(msg); /* create error message */
          lua_pushlstring(L, name, 0, name.size());
          lua_pushvalue(L, -2);
          luaL_errori(L, Lt.module_s_not_found_s);
        }
        lua_pushlstring(L, name, 0, name.size());
        luaL_buffsave(msg, luaL_tobox(L, 4));
        lua_callk(L, 1, 2, reqctx(REQ_SEARCHED, i), ll_requirek); /* call it */
        step = REQ_SEARCHED;
      }
    }
    finally
    {
      msg.release();
    }
  }

  private static int ll_requirek(lua_State L, int status, long ctx)
  {
    return require_from(L, (int) (ctx % 8), ctx / 8);
  }

  private static final lua_KFunction ll_requirek = Loadlib::ll_requirek;

  private static final KNamer knamer_ll_requirek = new KNamer("package.ll_requirek", ll_requirek);

  private static int ll_require(lua_State L)
  {
    luaL_checklstring(L, 1);
    lua_settop(L, 1); /* LOADED table will be at index 2 */
    luaE_freezeonly(L);
    lua_getfieldik(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE, reqctx(REQ_LOADED, 0), ll_requirek);
    return require_from(L, REQ_LOADED, 0);
  }

  private static final lua_CFunction ll_require = Loadlib::ll_require;

  /* }====================================================== */

  private static final CFuncInfo cfi_ll_loadlib = new CFuncInfo(ll_loadlib, "package.ll_loadlib");
  private static final CFuncInfo cfi_ll_searchpath = new CFuncInfo(ll_searchpath, "package.ll_searchpath");

  private static final luaL_RegCFI[] pk_funcs = {
    new luaL_RegCFI(Lt.loadlib, cfi_ll_loadlib),
    new luaL_RegCFI(Lt.searchpath, cfi_ll_searchpath),
    new luaL_RegCFI(Lt.preload_2, null),
    new luaL_RegCFI(Lt.cpath, null),
    new luaL_RegCFI(Lt.path, null),
    new luaL_RegCFI(Lt.searchers, null),
    new luaL_RegCFI(Lt.loaded, null),
  };

  private static final luaL_Regi[] ll_funcs = {
    new luaL_Regi(Lt.require, ll_require),
  };

  private static final CClosureNamer searcher_preload_kind = new CClosureNamer(new lua_CClosureKind("package.searcher_preload", searcher_preload, 1, null, null));
  private static final CClosureNamer searcher_Lua_kind = new CClosureNamer(new lua_CClosureKind("package.searcher_Lua", searcher_Lua, 1, null, null));
  private static final CClosureNamer searcher_C_kind = new CClosureNamer(new lua_CClosureKind("package.searcher_C", searcher_C, 1, null, null));
  private static final CClosureNamer searcher_Croot_kind = new CClosureNamer(new lua_CClosureKind("package.searcher_Croot", searcher_Croot, 1, null, null));

  private static final CClosureNamer require_kind = new CClosureNamer(new lua_CClosureKind("package.ll_require", ll_require, 1, null, null));

  private static void createsearcherstable(lua_State L)
  {
    lua_CFunction[] searchers = {
        searcher_preload,
        searcher_Lua,
        searcher_C,
        searcher_Croot,
        null};
    int i;
    /* create 'searchers' table */
    lua_createtable(L, searchers.length - 1, 0);
    /* fill it with predefined searchers */
    for (i = 0; searchers[i] != null; i++)
    {
      lua_pushvalue(L, -2); /* set 'package' as upvalue for all searchers */
      lua_pushcclosure(L, searchers[i], 1);
      lua_rawseti(L, -2, i + 1);
    }
    lua_setfieldi(L, -2, Lt.searchers); /* put it in field 'searchers' */
  }

  public static int luaopen_package(lua_State L)
  {
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, Lt._CLIBS); /* create lt::_CLIBS table */
    lua_pop(L, 1);                                 /* will not use it now */
    luaL_newlibcfi(L, pk_funcs);
    createsearcherstable(L);
    /* set paths */
    setpath(L, Lt.path, LUA_PATH_VAR, Lt.LUA_PATH_DEFAULT);
    setpath(L, Lt.cpath, LUA_CPATH_VAR, Lt.LUA_CPATH_DEFAULT);
    /* store config information */
    lua_pushlstring(L, Lt.CONFIG, 0, Lt.CONFIG.size());
    lua_setfieldi(L, -2, Lt.config);
    /* set field 'loaded' */
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_setfieldi(L, -2, Lt.loaded);
    /* set field 'preload' */
    luaL_getsubtablei(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_setfieldi(L, -2, Lt.preload_2);
    lua_pushglobaltable(L);
    lua_pushvalue(L, -2);          /* set 'package' as upvalue for next lib */
    luaL_setfuncsi(L, ll_funcs, 1); /* open lib into global table */
    lua_pop(L, 1);                 /* pop global table */
    return 1;                      /* return 'package' table */
  }

  public static final lua_CFunction luaopen_package = Loadlib::luaopen_package;
}
