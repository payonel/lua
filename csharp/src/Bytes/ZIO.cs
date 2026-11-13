using System;
using Payonel.LungoLua.Compat;
using lua_Reader = Payonel.LungoLua.Api.lua_Reader;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Bytes;

/* --------- Private Part ------------------ */

public sealed class ZIO
{
  public ByteView chunk = Lt.empty_str;
  public int pos = 0;
  public lua_Reader reader = null;
  public lua_State L = null;
  public LoadResume rz = null;       /* a load that a freeze can stop (see lload.h) */
  public bool resuming = false;   /* (its frames are being made again) */
}
