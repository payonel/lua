using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

/*
** Budget callback ('lua_setbudget'): called with 'user_co', the topmost
** budgeted thread of the running chain (the one a freeze freezes), and
** 'spender', the thread that spent the budget, which it must leave as it
** is (no room is made in its stack: to run Lua code, the callback uses a
** thread of its own)
*/
public delegate void lua_Budget(lua_State user_co, lua_State spender);

