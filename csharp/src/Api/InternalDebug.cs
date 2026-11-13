using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using CallInfo = Payonel.LungoLua.Runtime.CallInfo;
using static Payonel.LungoLua.Api.LuaConf;

namespace Payonel.LungoLua.Api;

public sealed class InternalDebug
{
  public int @event = 0;
  public ByteView name = null;
  public ByteView namewhat = null;
  public ByteView what = ByteView.EMPTY;
  public ByteView source = ByteView.EMPTY;
  public int currentline = 0;
  public int linedefined = 0;
  public int lastlinedefined = 0;
  public int nups = 0;
  public int nparams = 0;
  public bool isvararg = false;
  public int extraargs = 0;
  public bool istailcall = false;
  public int ftransfer = 0;
  public int ntransfer = 0;
  public ByteOwner short_src_out = new ByteOwner(LUA_IDSIZE); /* (S) */
  public CallInfo i_ci = null;

  public InternalDebug()
  {
  }

  public InternalDebug(InternalDebug o)
  {
    @event = o.@event;
    name = o.name;
    namewhat = o.namewhat;
    what = o.what;
    source = o.source;
    currentline = o.currentline;
    linedefined = o.linedefined;
    lastlinedefined = o.lastlinedefined;
    nups = o.nups;
    nparams = o.nparams;
    isvararg = o.isvararg;
    extraargs = o.extraargs;
    istailcall = o.istailcall;
    ftransfer = o.ftransfer;
    ntransfer = o.ntransfer;
    short_src_out = new ByteOwner(o.short_src_out.view());
    i_ci = o.i_ci;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public InternalDebug copy()
  {
    return new InternalDebug(this);
  }
}
