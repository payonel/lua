package com.payonel.lungolua.api;

import com.payonel.lungolua.runtime.lua_State;

@FunctionalInterface
public interface lua_CFunction
{
  int call(lua_State L);
}
