package com.payonel.lungolua.api;

import com.payonel.lungolua.runtime.lua_State;

@FunctionalInterface
public interface lua_KFunction
{
  int call(lua_State L, int status, long ctx);
}
