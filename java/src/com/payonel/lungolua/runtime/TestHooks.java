package com.payonel.lungolua.runtime;

public interface TestHooks
{
  void luai_tracegctest(lua_State L, int first);

  lua_State luai_newstate();

  void luai_openlibstest(lua_State L);
}
