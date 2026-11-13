using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Object.Lnames;

namespace Payonel.LungoLua.Api;

public sealed class BudgetNamer
{
  public BudgetNamer(string name, lua_Budget f) : this(name, f, null)
  {

  }

  public BudgetNamer(string name, lua_Budget f, Consumer<lua_State> prepare)
  {
    lua_registerbudget(name, f, prepare);
  }
}
