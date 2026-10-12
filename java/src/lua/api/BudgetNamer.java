package lua.api;

import static lua.object.Lnames.*;

import java.util.function.Consumer;

import lua.runtime.lua_State;

public final class BudgetNamer
{
  public BudgetNamer(String name, lua_Budget f)
  {
    this(name, f, null);
  }

  public BudgetNamer(String name, lua_Budget f, Consumer<lua_State> prepare)
  {
    lua_registerbudget(name, f, prepare);
  }
}
