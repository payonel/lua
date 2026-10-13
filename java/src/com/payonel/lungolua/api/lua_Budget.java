package com.payonel.lungolua.api;

import com.payonel.lungolua.runtime.lua_State;

/*
** Budget callback ('lua_setbudget'): called with 'user_co', the topmost
** budgeted thread of the running chain (the one a freeze freezes), and
** 'spender', the thread that spent the budget, which it must leave as it
** is (no room is made in its stack: to run Lua code, the callback uses a
** thread of its own)
*/
@FunctionalInterface
public interface lua_Budget
{
  void call(lua_State user_co, lua_State spender);
}
