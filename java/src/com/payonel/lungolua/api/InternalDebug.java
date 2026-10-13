package com.payonel.lungolua.api;

import static com.payonel.lungolua.api.LuaConf.*;

import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.runtime.CallInfo;

public final class InternalDebug
{
  public int event = 0;
  public ByteView name = null;
  public ByteView namewhat = null;
  public ByteView what = ByteView.EMPTY;
  public ByteView source = ByteView.EMPTY;
  public int currentline = 0;
  public int linedefined = 0;
  public int lastlinedefined = 0;
  public int nups = 0;
  public int nparams = 0;
  public byte isvararg = 0;
  public int extraargs = 0;
  public byte istailcall = 0;
  public int ftransfer = 0;
  public int ntransfer = 0;
  public ByteOwner short_src_out = new ByteOwner(LUA_IDSIZE); /* (S) */
  public CallInfo i_ci = null;

  public InternalDebug()
  {
  }

  public InternalDebug(InternalDebug o)
  {
    event = o.event;
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

  public InternalDebug copy()
  {
    return new InternalDebug(this);
  }
}
