package com.payonel.lungolua.api;

import com.payonel.lungolua.runtime.lua_State;

public interface lua_WarnFunction
{
  void warn(lua_State L, HostString msg, int tocont);
}
