package com.payonel.lungolua.object;

import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Lmem.*;

import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.runtime.Cost;
import com.payonel.lungolua.runtime.lua_State;

/*
** Function Prototypes
*/
public final class Proto extends GCListed
{
  public int numparams = 0; /* number of fixed (named) parameters */
  public byte flag = 0;
  public int maxstacksize = 0; /* number of registers needed by this function */
  public int linedefined = 0;     /* debug information  */
  public int lastlinedefined = 0; /* debug information  */
  public TValue[] k = new TValue[0];   /* constants used by the function */
  public int[] code = new int[0];      /* opcodes */
  public Proto[] p = new Proto[0];     /* functions defined inside the function */
  public Upvaldesc[] upvalues = new Upvaldesc[0]; /* upvalue information */
  public byte[] lineinfo = new byte[0];           /* information about source lines (debug information) */
  public AbsLineInfo[] abslineinfo = new AbsLineInfo[0]; /* idem */
  public LocVar[] locvars = new LocVar[0];        /* information about local variables (debug information) */
  public TString source = null;                   /* used for debug information */

  public static Proto luaF_newproto(lua_State L)
  {
    Proto f = luaM_malloc_Proto(L);
    luaC_link(L, f, LUA_VPROTO);
    return f;
  }

  public long luaF_protosize()
  {
    long sz = Cost.Proto
                + (long) p.length * Cost.Ref
                + (long) k.length * Cost.TValue
                + (long) locvars.length * Cost.LocVar
                + (long) upvalues.length * Cost.Upvaldesc;
    if ((flag & PF_FIXED) == 0)
    {
      sz += (long) code.length * Cost.Instruction;
      sz += (long) lineinfo.length * Cost.Byte;
      sz += (long) abslineinfo.length * Cost.AbsLineInfo;
    }
    return sz;
  }

  public void luaF_freeproto(lua_State L)
  {
    if ((flag & PF_FIXED) == 0)
    {
      luaM_alloc_Instruction_span(L.l_G, L, code, 0, false);
      luaM_alloc_ls_byte_span(L.l_G, L, lineinfo, 0, false);
      luaM_alloc_AbsLineInfo_span(L.l_G, L, abslineinfo, 0, false);
    }
    luaM_alloc_Proto_ptr_span(L.l_G, L, p, 0, false);
    luaM_alloc_TValue_span(L.l_G, L, k, 0, false);
    luaM_alloc_LocVar_span(L.l_G, L, locvars, 0, false);
    luaM_alloc_Upvaldesc_span(L.l_G, L, upvalues, 0, false);
    luaM_free_Proto(L, this);
  }

  public InternalString luaF_getlocalname(int local_number, int pc)
  {
    for (int i = 0; i < locvars.length && locvars[i].startpc <= pc; i++)
    {
      if (pc < locvars[i].endpc)
      { /* is variable active? */
        local_number--;
        if (local_number == 0)
          return locvars[i].varname.data;
      }
    }
    return null; /* not found */
  }
}
