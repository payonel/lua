package lua.api;

import lua.bytes.ByteView;
import lua.runtime.lua_State;

public interface lua_Reader
{
  /* the next piece, or null (std::nullopt) at the end */
  ByteView read(lua_State L);
}
