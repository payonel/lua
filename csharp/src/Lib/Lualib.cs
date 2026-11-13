using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Lib;

public sealed class Lualib
{
  private Lualib()
  {
  }

  /* version suffix for environment variable names */
  public static readonly ByteView LUA_VERSUFFIX = ByteView.of("_5_5");

  public const int LUA_GLIBK = 1;

  public static readonly ByteView LUA_LOADLIBNAME = ByteView.of("package");
  public const int LUA_LOADLIBK = LUA_GLIBK << 1;

  public static readonly ByteView LUA_COLIBNAME = ByteView.of("coroutine");
  public const int LUA_COLIBK = LUA_LOADLIBK << 1;

  public static readonly ByteView LUA_DBLIBNAME = ByteView.of("debug");
  public const int LUA_DBLIBK = LUA_COLIBK << 1;

  public static readonly ByteView LUA_IOLIBNAME = ByteView.of("io");
  public const int LUA_IOLIBK = LUA_DBLIBK << 1;

  public static readonly ByteView LUA_MATHLIBNAME = ByteView.of("math");
  public const int LUA_MATHLIBK = LUA_IOLIBK << 1;

  public static readonly ByteView LUA_OSLIBNAME = ByteView.of("os");
  public const int LUA_OSLIBK = LUA_MATHLIBK << 1;

  public static readonly ByteView LUA_STRLIBNAME = ByteView.of("string");
  public const int LUA_STRLIBK = LUA_OSLIBK << 1;

  public static readonly ByteView LUA_TABLIBNAME = ByteView.of("table");
  public const int LUA_TABLIBK = LUA_STRLIBK << 1;

  public static readonly ByteView LUA_UTF8LIBNAME = ByteView.of("utf8");
  public const int LUA_UTF8LIBK = LUA_TABLIBK << 1;
}
