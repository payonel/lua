package com.payonel.lungolua.api;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.runtime.lua_State;

public interface lua_Reader
{
  /* the next piece, or null (std::nullopt) at the end */
  ByteView read(lua_State L);
}
