package com.payonel.lungolua.tests.libs;

import static com.payonel.lungolua.platform.OsApi.os_dlopen;
import static com.payonel.lungolua.platform.OsApi.os_dlfunc;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.runtime.lua_State;

/*
** lib_decls.h: the functions the libraries take from one another, bound
** as the dynamic linker binds them (to the first library loaded with
** global symbols that defines them: 'os_dlopen' of a library that takes
** them fails without one)
*/
public final class LibDecls
{
  private LibDecls()
  {
  }

  private static int call(String sym, lua_State L)
  {
    return os_dlfunc(os_dlopen(ByteView.EMPTY, false), ByteView.of(sym)).call(L);
  }

  public static int lib1_export(lua_State L)
  {
    return call("lib1_export", L);
  }

  public static int onefunction(lua_State L)
  {
    return call("onefunction", L);
  }

  public static int anotherfunc(lua_State L)
  {
    return call("anotherfunc", L);
  }

  public static int luaopen_lib1_sub(lua_State L)
  {
    return call("luaopen_lib1_sub", L);
  }

  public static int luaopen_lib11(lua_State L)
  {
    return call("luaopen_lib11", L);
  }

  public static int luaopen_lib2(lua_State L)
  {
    return call("luaopen_lib2", L);
  }

  public static int luaopen_lib21(lua_State L)
  {
    return call("luaopen_lib21", L);
  }
}
