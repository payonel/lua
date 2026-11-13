using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

public interface lua_Reader
{
  /* the next piece, or null (std::nullopt) at the end */
  ByteView read(lua_State L);
}
