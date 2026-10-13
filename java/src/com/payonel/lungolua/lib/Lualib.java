package com.payonel.lungolua.lib;

import com.payonel.lungolua.bytes.ByteView;

public final class Lualib
{
  private Lualib()
  {
  }

  /* version suffix for environment variable names */
  public static final ByteView LUA_VERSUFFIX = ByteView.of("_5_5");

  public static final int LUA_GLIBK = 1;

  public static final ByteView LUA_LOADLIBNAME = ByteView.of("package");
  public static final int LUA_LOADLIBK = LUA_GLIBK << 1;

  public static final ByteView LUA_COLIBNAME = ByteView.of("coroutine");
  public static final int LUA_COLIBK = LUA_LOADLIBK << 1;

  public static final ByteView LUA_DBLIBNAME = ByteView.of("debug");
  public static final int LUA_DBLIBK = LUA_COLIBK << 1;

  public static final ByteView LUA_IOLIBNAME = ByteView.of("io");
  public static final int LUA_IOLIBK = LUA_DBLIBK << 1;

  public static final ByteView LUA_MATHLIBNAME = ByteView.of("math");
  public static final int LUA_MATHLIBK = LUA_IOLIBK << 1;

  public static final ByteView LUA_OSLIBNAME = ByteView.of("os");
  public static final int LUA_OSLIBK = LUA_MATHLIBK << 1;

  public static final ByteView LUA_STRLIBNAME = ByteView.of("string");
  public static final int LUA_STRLIBK = LUA_OSLIBK << 1;

  public static final ByteView LUA_TABLIBNAME = ByteView.of("table");
  public static final int LUA_TABLIBK = LUA_STRLIBK << 1;

  public static final ByteView LUA_UTF8LIBNAME = ByteView.of("utf8");
  public static final int LUA_UTF8LIBK = LUA_TABLIBK << 1;
}
