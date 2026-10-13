package com.payonel.lungolua.bytes;

import com.payonel.lungolua.api.lua_Reader;
import com.payonel.lungolua.runtime.lua_State;

/* --------- Private Part ------------------ */

public final class ZIO
{
  public ByteView chunk = Lt.empty_str;
  public int pos = 0;
  public lua_Reader reader = null;
  public lua_State L = null;
  public LoadResume rz = null;       /* a load that a freeze can stop (see lload.h) */
  public boolean resuming = false;   /* (its frames are being made again) */
}
