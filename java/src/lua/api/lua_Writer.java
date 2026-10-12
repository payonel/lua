package lua.api;

import lua.bytes.ByteView;
import lua.runtime.lua_State;

public interface lua_Writer
{
  /* 'b' null (std::nullopt): the end */
  int write(lua_State L, ByteView b);
}
