using System;
using Payonel.LungoLua.Compat;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

public sealed class CFuncInfo : GCObject
{
  public lua_CFunction fptr;

  public CFuncInfo() : base(null, Lobject.LUA_VLCF, (sbyte) ((1 << 5) | 4) /* BLACK | G_OLD */, 0, 0)
  {

    fptr = null;
  }

  public CFuncInfo(lua_CFunction f) : base(null, Lobject.LUA_VLCF, (sbyte) ((1 << 5) | 4) /* BLACK | G_OLD */, 0, 0)
  {

    fptr = f;
  }

  /* an engine function: it has a name (see 'lua_cfiname') */
  public CFuncInfo(lua_CFunction f, string name) : this(f)
  {

    Lnames.registercfi(this, name);
    /* as a table key, it hashes by its name: the same in every process (FNV-1a; never 0) */
    int h = unchecked((int)0x811C9DC5U);
    for (int i = 0; i < name.length(); i++)
      h = (h ^ (name.charAt(i) & 0xFF)) * 16777619;
    hash = (h != 0) ? h : 1;
  }

  public static CFuncInfo luaF_newCFuncInfo(lua_State L, lua_CFunction f)
  {
    CFuncInfo cfi = luaM_malloc_CFuncInfo(L);
    luaC_link(L, cfi, LUA_VLCF);
    cfi.fptr = f;
    return cfi;
  }
}
