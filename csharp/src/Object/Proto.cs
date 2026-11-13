using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using Cost = Payonel.LungoLua.Runtime.Cost;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Gc.Lgc;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Object;

/*
** Function Prototypes
*/
public sealed class Proto : GCListed
{
  public int numparams = 0; /* number of fixed (named) parameters */
  public sbyte flag = 0;
  public int maxstacksize = 0; /* number of registers needed by this function */
  public int linedefined = 0;     /* debug information  */
  public int lastlinedefined = 0; /* debug information  */
  public TValue[] k = new TValue[0];   /* constants used by the function */
  public int[] code = new int[0];      /* opcodes */
  public Proto[] p = new Proto[0];     /* functions defined inside the function */
  public Upvaldesc[] upvalues = new Upvaldesc[0]; /* upvalue information */
  public sbyte[] lineinfo = new sbyte[0];           /* information about source lines (debug information) */
  public AbsLineInfo[] abslineinfo = new AbsLineInfo[0]; /* idem */
  public LocVar[] locvars = new LocVar[0];        /* information about local variables (debug information) */
  public TString source = null;                   /* used for debug information */

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static Proto luaF_newproto(lua_State L)
  {
    Proto f = luaM_malloc_Proto(L);
    luaC_link(L, f, LUA_VPROTO);
    return f;
  }

  public long luaF_protosize()
  {
    long sz = Cost.Proto
                + (long) p.Length * Cost.Ref
                + (long) k.Length * Cost.TValue
                + (long) locvars.Length * Cost.LocVar
                + (long) upvalues.Length * Cost.Upvaldesc;
    if ((flag & PF_FIXED) == 0)
    {
      sz += (long) code.Length * Cost.Instruction;
      sz += (long) lineinfo.Length * Cost.Byte;
      sz += (long) abslineinfo.Length * Cost.AbsLineInfo;
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
    for (int i = 0; i < locvars.Length && locvars[i].startpc <= pc; i++)
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
