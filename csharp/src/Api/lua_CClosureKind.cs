using System;
using Payonel.LungoLua.Compat;
using CClosure = Payonel.LungoLua.Object.CClosure;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

/*
** Kinds of C closures that can be kept outside the process: the function,
** the number of upvalues, and for one that keeps a state object in its last
** upvalue, how to save the object as integers (the number, up to 8, or -1
** if the closure cannot be kept as it is) and how to make it again from
** them and the other upvalues (false if they are not valid). The other
** upvalues are values.
*/
public sealed class lua_CClosureKind
{

  public delegate int Save(CClosure cl, long[] ints);

  public delegate bool Make(lua_State L, CClosure cl, long[] ints, int nints);

  public string name;
  public lua_CFunction f;
  public int nup;
  public Save save;
  public Make make;

  public lua_CClosureKind()
  {
  }

  public lua_CClosureKind(string name, lua_CFunction f, int nup, Save save, Make make)
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
