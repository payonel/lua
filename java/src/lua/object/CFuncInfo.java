package lua.object;

import static lua.gc.Lgc.*;
import static lua.object.Lobject.*;
import static lua.runtime.Lmem.*;

import lua.api.lua_CFunction;
import lua.runtime.lua_State;

public final class CFuncInfo extends GCObject
{
  public lua_CFunction fptr;

  public CFuncInfo()
  {
    super(null, Lobject.LUA_VLCF, (byte) ((1 << 5) | 4) /* BLACK | G_OLD */, 0, 0);
    fptr = null;
  }

  public CFuncInfo(lua_CFunction f)
  {
    super(null, Lobject.LUA_VLCF, (byte) ((1 << 5) | 4) /* BLACK | G_OLD */, 0, 0);
    fptr = f;
  }

  /* an engine function: it has a name (see 'lua_cfiname') */
  public CFuncInfo(lua_CFunction f, String name)
  {
    this(f);
    Lnames.registercfi(this, name);
    /* as a table key, it hashes by its name: the same in every process (FNV-1a; never 0) */
    int h = 0x811c9dc5;
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
