package lua.api;

import lua.bytes.ByteView;

public final class LuaApi
{
  private LuaApi()
  {
  }

  public static final int LUA_VERSION_MAJOR_N = 5;
  public static final int LUA_VERSION_MINOR_N = 5;
  public static final int LUA_VERSION_RELEASE_N = 1;
  public static final int LUA_VERSION_NUM = LUA_VERSION_MAJOR_N * 100 + LUA_VERSION_MINOR_N;
  public static final int LUA_VERSION_RELEASE_NUM = LUA_VERSION_NUM * 100 + LUA_VERSION_RELEASE_N;
  public static final ByteView LUA_VERSION_MAJOR = ByteView.of("5");
  public static final ByteView LUA_VERSION_MINOR = ByteView.of("5");
  public static final ByteView LUA_VERSION_RELEASE = ByteView.of("1");
  public static final ByteView LUA_VERSION = ByteView.of("Lua 5.5");
  public static final ByteView LUA_RELEASE = ByteView.of("Lua 5.5.1");
  public static final ByteView LUA_COPYRIGHT = ByteView.of("Lua 5.5.1  Copyright (C) 1994-2026 Lua.org, PUC-Rio");
  public static final ByteView LUA_AUTHORS = ByteView.of("R. Ierusalimschy, L. H. de Figueiredo, W. Celes");
  public static final ByteView LUA_SIGNATURE = ByteView.of("\33Lua");
  public static final int LUA_MULTRET = -1;
  public static final int LUA_REGISTRYINDEX = -(Integer.MAX_VALUE / 2 + 1000);
  public static final int LUA_OK = 0;
  public static final int LUA_YIELD = 1;
  public static final int LUA_ERRRUN = 2;
  public static final int LUA_ERRSYNTAX = 3;
  public static final int LUA_ERRMEM = 4;
  public static final int LUA_ERRERR = 5;
  public static final int LUA_TNONE = -1;
  public static final int LUA_TNIL = 0;
  public static final int LUA_TBOOLEAN = 1;
  public static final int LUA_TLIGHTUSERDATA = 2;
  public static final int LUA_TNUMBER = 3;
  public static final int LUA_TSTRING = 4;
  public static final int LUA_TTABLE = 5;
  public static final int LUA_TFUNCTION = 6;
  public static final int LUA_TUSERDATA = 7;
  public static final int LUA_TTHREAD = 8;
  public static final int LUA_NUMTYPES = 9;
  public static final int LUA_MINSTACK = 20;
  public static final int LUA_RIDX_GLOBALS = 2;
  public static final int LUA_RIDX_MAINTHREAD = 3;
  public static final int LUA_RIDX_LAST = 3;
  public static final int LUA_OPADD = 0;
  public static final int LUA_OPSUB = 1;
  public static final int LUA_OPMUL = 2;
  public static final int LUA_OPMOD = 3;
  public static final int LUA_OPPOW = 4;
  public static final int LUA_OPDIV = 5;
  public static final int LUA_OPIDIV = 6;
  public static final int LUA_OPBAND = 7;
  public static final int LUA_OPBOR = 8;
  public static final int LUA_OPBXOR = 9;
  public static final int LUA_OPSHL = 10;
  public static final int LUA_OPSHR = 11;
  public static final int LUA_OPUNM = 12;
  public static final int LUA_OPBNOT = 13;
  public static final int LUA_OPEQ = 0;
  public static final int LUA_OPLT = 1;
  public static final int LUA_OPLE = 2;
  public static final int LUA_NOFREEZE_BUDGET = 1;
  public static final int LUA_NOFREEZE_CALL = 2;
  public static final int LUA_NOFREEZE_RESUME = 3;
  public static final int LUA_GCSTOP = 0;
  public static final int LUA_GCRESTART = 1;
  public static final int LUA_GCCOLLECT = 2;
  public static final int LUA_GCCOUNT = 3;
  public static final int LUA_GCCOUNTB = 4;
  public static final int LUA_GCSTEP = 5;
  public static final int LUA_GCISRUNNING = 6;
  public static final int LUA_GCGEN = 7;
  public static final int LUA_GCINC = 8;
  public static final int LUA_GCPARAM = 9;
  public static final int LUA_GCPMINORMUL = 0;
  public static final int LUA_GCPMAJORMINOR = 1;
  public static final int LUA_GCPMINORMAJOR = 2;
  public static final int LUA_GCPPAUSE = 3;
  public static final int LUA_GCPSTEPMUL = 4;
  public static final int LUA_GCPSTEPSIZE = 5;
  public static final int LUA_GCPN = 6;
  public static final int LUA_N2SBUFFSZ = 64;
  public static final int LUA_HOOKCALL = 0;
  public static final int LUA_HOOKRET = 1;
  public static final int LUA_HOOKLINE = 2;
  public static final int LUA_HOOKCOUNT = 3;
  public static final int LUA_HOOKTAILCALL = 4;
  public static final int LUA_MASKCALL = 1 << LUA_HOOKCALL;
  public static final int LUA_MASKRET = 1 << LUA_HOOKRET;
  public static final int LUA_MASKLINE = 1 << LUA_HOOKLINE;
  public static final int LUA_MASKCOUNT = 1 << LUA_HOOKCOUNT;
}
