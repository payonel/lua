package com.payonel.lungolua.api;

import static com.payonel.lungolua.object.Lnames.*;

import java.util.function.Consumer;

import com.payonel.lungolua.runtime.lua_State;

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
