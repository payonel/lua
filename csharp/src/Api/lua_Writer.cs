using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

public interface lua_Writer
{
  /* 'b' null (std::nullopt): the end */
  int write(lua_State L, ByteView b);
}
