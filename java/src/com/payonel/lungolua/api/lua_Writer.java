package com.payonel.lungolua.api;

import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.runtime.lua_State;

public interface lua_Writer
{
  /* 'b' null (std::nullopt): the end */
  int write(lua_State L, ByteView b);
}
