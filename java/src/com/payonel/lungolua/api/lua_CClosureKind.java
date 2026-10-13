package com.payonel.lungolua.api;

import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.runtime.lua_State;

/*
** Kinds of C closures that can be kept outside the process: the function,
** the number of upvalues, and for one that keeps a state object in its last
** upvalue, how to save the object as integers (the number, up to 8, or -1
** if the closure cannot be kept as it is) and how to make it again from
** them and the other upvalues (false if they are not valid). The other
** upvalues are values.
*/
public final class lua_CClosureKind
{
  @FunctionalInterface
  public interface Save
  {
    int call(CClosure cl, long[] ints);
  }

  @FunctionalInterface
  public interface Make
  {
    boolean call(lua_State L, CClosure cl, long[] ints, int nints);
  }

  public String name;
  public lua_CFunction f;
  public int nup;
  public Save save;
  public Make make;

  public lua_CClosureKind()
  {
  }

  public lua_CClosureKind(String name, lua_CFunction f, int nup, Save save, Make make)
  {
    this.name = name;
    this.f = f;
    this.nup = nup;
    this.save = save;
    this.make = make;
  }

  public void assign(lua_CClosureKind o)
  {
    name = o.name;
    f = o.f;
    nup = o.nup;
    save = o.save;
    make = o.make;
  }
}
