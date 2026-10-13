package com.payonel.lungolua.runtime;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Lfunc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;
import static com.payonel.lungolua.vm.Lopcodes.*;

import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.ByteWrites;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.compiler.Labeldesc;
import com.payonel.lungolua.compiler.Vardesc;
import com.payonel.lungolua.object.AbsLineInfo;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.object.FmtArgs;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.LocVar;
import com.payonel.lungolua.object.Node;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.object.Udata;
import com.payonel.lungolua.object.UpVal;
import com.payonel.lungolua.object.Upvaldesc;

public final class Lmem
{
  private Lmem()
  {
  }

  /*
  ** When an allocation fails, it will try again after an emergency
  ** collection, except when it cannot run a collection.  The GC should
  ** not be called while the state is not fully built, as the collector
  ** is not yet fully initialized. Also, it should not be called when
  ** 'gcstopem' is true, because then the interpreter is in the middle of
  ** a collection step.
  */
  private static boolean cantryagain(global_State g)
  {
    return completestate(g) && !g.gcstopem;
  }

  private static boolean luaM_permit_(global_State g, lua_State L, long osize, long nsize, int tag)
  {
    if (osize == 0 && nsize == 0)
      return true;
    if (!g.frealloc.permit(osize, nsize, tag))
    {
      if (L == null)
        return false;
      if (!cantryagain(g))
        return false;
      luaC_fullgc(L, true); /* try to free some memory... */
      if (!g.frealloc.permit(osize, nsize, tag))
        return false;
    }
    return true;
  }

  private static boolean _alloc_span_request(global_State g, lua_State L, long oldCount, long newCount, long esize, boolean bthrow)
  {
    if (Long.compareUnsigned(newCount + 1, Long.divideUnsigned(MAX_SIZET, esize)) > 0)
      luaM_toobig(L);
    if (newCount > MAX_JARRAY)
    {
      if (bthrow && L != null)
        luaD_throw(L, (byte) LUA_ERRMEM);
      return false;
    }
    long osz = oldCount * esize;
    long nsz = newCount * esize;
    if (luaM_permit_(g, L, osz, nsz, 0) || newCount == 0) // always allow release
    {
      if (L != null)
        g.GCdebt -= nsz - osz;
    }
    else
    {
      if (bthrow && L != null)
      {
        luaD_throw(L, (byte) LUA_ERRMEM);
      }
      return false;
    }
    return true;
  }

  private static void _alloc_span_undo(global_State g, lua_State L, long oldCount, long newCount, long esize, boolean bthrow)
  {
    long osz = oldCount * esize;
    long nsz = newCount * esize;
    g.frealloc.permit(nsz, osz, 0);
    if (L != null)
    {
      g.GCdebt += nsz - osz;
      if (bthrow)
        luaD_throw(L, (byte) LUA_ERRMEM);
    }
  }

  private static ByteOwner copy_char_span(ByteWrites block, long count)
  {
    ByteOwner nb = new ByteOwner();
    if (count > 0)
    {
      nb = new ByteOwner((int) count);
      if (!block.empty())
        for (int ci = 0; ci < Math.min(count, block.size()); ci++)
          nb.set(ci, block.get(ci));
    }
    return nb;
  }

  public static ByteOwner luaM_alloc_char_span(global_State g, lua_State L, ByteWrites block, long count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.size(), count, Cost.Byte, bthrow)) return new ByteOwner();
    try
    {
      return copy_char_span(block, count);
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.size(), count, Cost.Byte, bthrow);
      return new ByteOwner();
    }
  }

  public static ByteOwner luaM_alloc_host_span(lua_State L, ByteWrites block, long count)
  {
    if (count > MAX_JARRAY || (!luaM_permit_(L.l_G, L, block.size(), count, 0) && count > 0))
      return new ByteOwner();
    try
    {
      return copy_char_span(block, count);
    }
    catch (OutOfMemoryError e)
    {
      L.l_G.frealloc.permit(count, block.size(), 0);
      return new ByteOwner();
    }
  }

  private static void free_debt(lua_State L, long size, int tag)
  {
    global_State g = L.l_G;
    g.frealloc.permit(size, 0, tag);
    g.GCdebt += size;
  }

  private static void take_debt(lua_State L, long size, int tag)
  {
    global_State g = L.l_G;
    if (!luaM_permit_(g, L, 0, size, tag))
      luaD_throw(L, (byte) LUA_ERRMEM);
    g.GCdebt -= size;
  }

  /*
  ** {==================================================================
  ** Functions to allocate/deallocate arrays for the Parser
  ** ===================================================================
  */

  /*
  ** Minimum size for arrays during parsing, to avoid overhead of
  ** reallocating to size 1, then 2, and then 4. All these arrays
  ** will be reallocated to exact sizes or erased when parsing ends.
  */
  private static final int MINSIZEARRAY = 4;

  private static int luaM_growsize_(lua_State L, int size, int nelems, int limit, ByteView what)
  {
    if (nelems + 1 <= size)
      return size;
    if (size >= limit / 2)
    {                    /* cannot double it? */
      if (size >= limit) /* cannot grow even a little? */
        luaG_runerror(L, Lt.too_many_s_limit_is_d, new FmtArgs().str(what).num(limit));
      size = limit; /* still have at least one free place */
    }
    else
    {
      size *= 2;
      if (size < MINSIZEARRAY)
        size = MINSIZEARRAY; /* minimum size */
    }
    assert nelems + 1 <= size && size <= limit;
    return size;
  }

  /* }================================================================== */

  public static RuntimeException luaM_toobig(lua_State L)
  {
    throw luaG_runerror(L, Lt.memory_allocation_error_block_too_big);
  }

  public static void luaM_takeobj(lua_State L, long size)
  {
    take_debt(L, size, novariant(LUA_VUSERDATA));
  }

  public static void luaM_freeobj(lua_State L, long size)
  {
    free_debt(L, size, novariant(LUA_VUSERDATA));
  }

  /*
  ** Memory the host counts that is not in the Lua heap (what a freeze keeps,
  ** which a program does not see): one block, which grows from 'osize' to
  ** 'nsize' (a memory error if the host does not allow it) ...
  */
  public static void luaM_growhost(lua_State L, long osize, long nsize)
  {
    assert osize <= nsize;
    if (!luaM_permit_(L.l_G, L, osize, nsize, 0))
      luaD_throw(L, (byte) LUA_ERRMEM);
  }

  /* ... or shrinks to 'nsize', if the host allows it (to 0, given back: always) */
  public static boolean luaM_shrinkhost(global_State g, long osize, long nsize)
  {
    assert nsize <= osize;
    if (osize == 0)
      return true;
    if (nsize == 0)
    {
      g.frealloc.permit(osize, 0, 0);
      return true;
    }
    return g.frealloc.permit(osize, nsize, 0);
  }

  /*
  ** A block of 'size' bytes (as an array or buffer counts it) that comes to
  ** be counted on its own, or no longer: its memory goes from or to memory
  ** counted another way (see 'luaM_growhost').
  */
  public static void luaM_takeblock(lua_State L, long size)
  {
    if (size != 0)
      take_debt(L, size, 0);
  }

  public static void luaM_freeblock(lua_State L, long size)
  {
    if (size != 0)
      free_debt(L, size, 0);
  }

  /* a counted block that is not the state's memory resized, as one block (a memory error if it cannot grow) */
  public static void luaM_resizeblock(lua_State L, long osize, long nsize)
  {
    _alloc_span_request(L.l_G, L, osize, nsize, 1, true);
  }

  // type allocators
  public static TValue[] luaM_alloc_TValue_span(global_State g, lua_State L, TValue[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.TValue, bthrow)) return null;
    try
    {
      TValue[] nb = new TValue[0];
      if (count > 0)
      {
        nb = new TValue[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new TValue();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            copyobj(nb[ci], block[ci]);
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.TValue, bthrow);
      return null;
    }
  }

  public static long[] luaM_alloc_TValue_span(global_State g, lua_State L, long[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.TValue, bthrow)) return null;
    try
    {
      long[] nb = new long[0];
      if (count > 0)
      {
        nb = new long[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.TValue, bthrow);
      return null;
    }
  }

  public static Object[] luaM_alloc_TValue_span(Object[] block, int count)
  {
    Object[] nb = new Object[0];
    if (count > 0)
    {
      nb = new Object[count];
      if (block.length != 0)
        for (int ci = 0; ci < Math.min(count, block.length); ci++)
          nb[ci] = block[ci];
    }
    return nb;
  }

  public static byte[] luaM_alloc_TValue_span(byte[] block, int count)
  {
    byte[] nb = new byte[0];
    if (count > 0)
    {
      nb = new byte[count];
      if (block.length != 0)
        for (int ci = 0; ci < Math.min(count, block.length); ci++)
          nb[ci] = block[ci];
    }
    return nb;
  }

  public static long[] luaM_alloc_Value_span(global_State g, lua_State L, long[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Value, bthrow)) return null;
    try
    {
      long[] nb = new long[0];
      if (count > 0)
      {
        nb = new long[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Value, bthrow);
      return null;
    }
  }

  public static Object[] luaM_alloc_Value_span(Object[] block, int count)
  {
    Object[] nb = new Object[0];
    if (count > 0)
    {
      nb = new Object[count];
      if (block.length != 0)
        for (int ci = 0; ci < Math.min(count, block.length); ci++)
          nb[ci] = block[ci];
    }
    return nb;
  }

  public static BudgetGroup[] luaM_alloc_BudgetGroup_ptr_span(global_State g, lua_State L, BudgetGroup[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Ref, bthrow)) return null;
    try
    {
      BudgetGroup[] nb = new BudgetGroup[0];
      if (count > 0)
      {
        nb = new BudgetGroup[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Ref, bthrow);
      return null;
    }
  }

  public static UpVal[] luaM_alloc_UpVal_ptr_span(global_State g, lua_State L, UpVal[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Ref, bthrow)) return null;
    try
    {
      UpVal[] nb = new UpVal[0];
      if (count > 0)
      {
        nb = new UpVal[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Ref, bthrow);
      return null;
    }
  }

  public static byte[] luaM_alloc_lu_byte_span(global_State g, lua_State L, byte[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Byte, bthrow)) return null;
    try
    {
      byte[] nb = new byte[0];
      if (count > 0)
      {
        nb = new byte[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Byte, bthrow);
      return null;
    }
  }

  public static CallInfo luaM_alloc_CallInfo(lua_State L)
  {
    global_State g = L.l_G;
    if (!luaM_permit_(g, L, 0, Cost.CallInfo, 0))
      return null; /* denied; do not update 'GCdebt' */
    g.GCdebt -= Cost.CallInfo;
    return new CallInfo();
  }

  // typed grow/shrink span allocators
  public static LocVar[] luaM_alloc_LocVar_span(global_State g, lua_State L, LocVar[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.LocVar, bthrow)) return null;
    try
    {
      LocVar[] nb = new LocVar[0];
      if (count > 0)
      {
        nb = new LocVar[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new LocVar();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
          {
            nb[ci].varname = block[ci].varname;
            nb[ci].startpc = block[ci].startpc;
            nb[ci].endpc = block[ci].endpc;
          }
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.LocVar, bthrow);
      return null;
    }
  }

  public static LocVar[] luaM_safegrow_LocVar_span(lua_State L, LocVar[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Short.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.LocVar)) <= 0) ? Short.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.LocVar);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.local_variables);
    if (newsize == block.length)
      return block;
    return luaM_alloc_LocVar_span(L.l_G, L, block, newsize, true);
  }

  public static Vardesc[] luaM_alloc_Vardesc_span(global_State g, lua_State L, Vardesc[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Vardesc, bthrow)) return null;
    try
    {
      Vardesc[] nb = new Vardesc[0];
      if (count > 0)
      {
        nb = new Vardesc[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new Vardesc();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
          {
            copyobj(nb[ci].k, block[ci].k);
            nb[ci].kind = block[ci].kind;
            nb[ci].ridx = block[ci].ridx;
            nb[ci].pidx = block[ci].pidx;
            nb[ci].name = block[ci].name;
          }
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Vardesc, bthrow);
      return null;
    }
  }

  public static Vardesc[] luaM_safegrow_Vardesc_span(lua_State L, Vardesc[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Short.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.Vardesc)) <= 0) ? Short.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.Vardesc);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.variable_declarations);
    if (newsize == block.length)
      return block;
    return luaM_alloc_Vardesc_span(L.l_G, L, block, newsize, true);
  }

  public static Upvaldesc[] luaM_alloc_Upvaldesc_span(global_State g, lua_State L, Upvaldesc[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Upvaldesc, bthrow)) return null;
    try
    {
      Upvaldesc[] nb = new Upvaldesc[0];
      if (count > 0)
      {
        nb = new Upvaldesc[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new Upvaldesc();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
          {
            nb[ci].name = block[ci].name;
            nb[ci].instack = block[ci].instack;
            nb[ci].idx = block[ci].idx;
            nb[ci].kind = block[ci].kind;
          }
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Upvaldesc, bthrow);
      return null;
    }
  }

  public static Upvaldesc[] luaM_safegrow_Upvaldesc_span(lua_State L, Upvaldesc[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(MAXUPVAL, Long.divideUnsigned(MAX_SIZET, Cost.Upvaldesc)) <= 0) ? MAXUPVAL : (int) Long.divideUnsigned(MAX_SIZET, Cost.Upvaldesc);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.upvalues);
    if (newsize == block.length)
      return block;
    return luaM_alloc_Upvaldesc_span(L.l_G, L, block, newsize, true);
  }

  public static Labeldesc[] luaM_alloc_Labeldesc_span(global_State g, lua_State L, Labeldesc[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Labeldesc, bthrow)) return null;
    try
    {
      Labeldesc[] nb = new Labeldesc[0];
      if (count > 0)
      {
        nb = new Labeldesc[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new Labeldesc();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
          {
            nb[ci].name = block[ci].name;
            nb[ci].pc = block[ci].pc;
            nb[ci].line = block[ci].line;
            nb[ci].nactvar = block[ci].nactvar;
            nb[ci].close = block[ci].close;
          }
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Labeldesc, bthrow);
      return null;
    }
  }

  public static Labeldesc[] luaM_safegrow_Labeldesc_span(lua_State L, Labeldesc[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Short.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.Labeldesc)) <= 0) ? Short.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.Labeldesc);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.labels_gotos);
    if (newsize == block.length)
      return block;
    return luaM_alloc_Labeldesc_span(L.l_G, L, block, newsize, true);
  }

  public static Proto[] luaM_alloc_Proto_ptr_span(global_State g, lua_State L, Proto[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Ref, bthrow)) return null;
    try
    {
      Proto[] nb = new Proto[0];
      if (count > 0)
      {
        nb = new Proto[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Ref, bthrow);
      return null;
    }
  }

  public static Proto[] luaM_safegrow_Proto_ptr_span(lua_State L, Proto[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(MAXARG_Bx, Long.divideUnsigned(MAX_SIZET, Cost.Ref)) <= 0) ? MAXARG_Bx : (int) Long.divideUnsigned(MAX_SIZET, Cost.Ref);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.functions);
    if (newsize == block.length)
      return block;
    return luaM_alloc_Proto_ptr_span(L.l_G, L, block, newsize, true);
  }

  public static AbsLineInfo[] luaM_alloc_AbsLineInfo_span(global_State g, lua_State L, AbsLineInfo[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.AbsLineInfo, bthrow)) return null;
    try
    {
      AbsLineInfo[] nb = new AbsLineInfo[0];
      if (count > 0)
      {
        nb = new AbsLineInfo[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new AbsLineInfo();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
          {
            nb[ci].pc = block[ci].pc;
            nb[ci].line = block[ci].line;
          }
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.AbsLineInfo, bthrow);
      return null;
    }
  }

  public static AbsLineInfo[] luaM_safegrow_AbsLineInfo_span(lua_State L, AbsLineInfo[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Integer.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.AbsLineInfo)) <= 0) ? Integer.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.AbsLineInfo);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.lines);
    if (newsize == block.length)
      return block;
    return luaM_alloc_AbsLineInfo_span(L.l_G, L, block, newsize, true);
  }

  public static byte[] luaM_alloc_ls_byte_span(global_State g, lua_State L, byte[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Byte, bthrow)) return null;
    try
    {
      byte[] nb = new byte[0];
      if (count > 0)
      {
        nb = new byte[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Byte, bthrow);
      return null;
    }
  }

  public static byte[] luaM_safegrow_ls_byte_span(lua_State L, byte[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Integer.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.Byte)) <= 0) ? Integer.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.Byte);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.opcodes);
    if (newsize == block.length)
      return block;
    return luaM_alloc_ls_byte_span(L.l_G, L, block, newsize, true);
  }

  public static int[] luaM_alloc_Instruction_span(global_State g, lua_State L, int[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Instruction, bthrow)) return null;
    try
    {
      int[] nb = new int[0];
      if (count > 0)
      {
        nb = new int[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Instruction, bthrow);
      return null;
    }
  }

  public static int[] luaM_safegrow_Instruction_span(lua_State L, int[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Integer.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.Instruction)) <= 0) ? Integer.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.Instruction);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.opcodes);
    if (newsize == block.length)
      return block;
    return luaM_alloc_Instruction_span(L.l_G, L, block, newsize, true);
  }

  public static TValue[] luaM_safegrow_TValue_span(lua_State L, TValue[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(MAXARG_Ax, Long.divideUnsigned(MAX_SIZET, Cost.TValue)) <= 0) ? MAXARG_Ax : (int) Long.divideUnsigned(MAX_SIZET, Cost.TValue);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.constants);
    if (newsize == block.length)
      return block;
    return luaM_alloc_TValue_span(L.l_G, L, block, newsize, true);
  }

  public static int[] luaM_alloc_StkId_span(global_State g, lua_State L, int[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Ref, bthrow)) return null;
    try
    {
      int[] nb = new int[0];
      if (count > 0)
      {
        nb = new int[count];
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            nb[ci] = block[ci];
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Ref, bthrow);
      return null;
    }
  }

  public static int[] luaM_safegrow_StkId_span(lua_State L, int[] block, int nelems)
  {
    final int limit = (Long.compareUnsigned(Integer.MAX_VALUE, Long.divideUnsigned(MAX_SIZET, Cost.Ref)) <= 0) ? Integer.MAX_VALUE : (int) Long.divideUnsigned(MAX_SIZET, Cost.Ref);
    int newsize = luaM_growsize_(L, block.length, nelems, limit, Lt.to_be_closed_variables);
    if (newsize == block.length)
      return block;
    return luaM_alloc_StkId_span(L.l_G, L, block, newsize, true);
  }

  // typed fresh (throwing) allocators
  public static Node[] luaM_alloc_Node_span(global_State g, lua_State L, Node[] block, int count, boolean bthrow)
  {
    if (!_alloc_span_request(g, L, block.length, count, Cost.Node, bthrow)) return null;
    try
    {
      Node[] nb = new Node[0];
      if (count > 0)
      {
        nb = new Node[count];
        for (int ci = 0; ci < count; ci++)
          nb[ci] = new Node();
        if (block.length != 0)
          for (int ci = 0; ci < Math.min(count, block.length); ci++)
            copynode(nb[ci], block[ci]);
      }
      return nb;
    }
    catch (OutOfMemoryError e)
    {
      _alloc_span_undo(g, L, block.length, count, Cost.Node, bthrow);
      return null;
    }
  }

  // typed GC-object allocators (throwing, fixed-size per type)
  public static TString luaM_malloc_TString(lua_State L, long bytes)
  {
    if (bytes > MAX_JARRAY)
      luaD_throw(L, (byte) LUA_ERRMEM);
    take_debt(L, Cost.TString + bytes, novariant(LUA_VSTRING));
    return new TString();
  }

  public static Udata luaM_malloc_Udata(lua_State L)
  {
    take_debt(L, Cost.Udata, novariant(LUA_VUSERDATA));
    return new Udata();
  }

  public static Table luaM_malloc_Table(lua_State L)
  {
    take_debt(L, Cost.Table, novariant(LUA_VTABLE));
    return new Table();
  }

  public static CClosure luaM_malloc_CClosure(lua_State L)
  {
    take_debt(L, Cost.CClosure, novariant(LUA_VCCL));
    return new CClosure();
  }

  public static LClosure luaM_malloc_LClosure(lua_State L)
  {
    take_debt(L, Cost.LClosure, novariant(LUA_VLCL));
    return new LClosure();
  }

  public static CFuncInfo luaM_malloc_CFuncInfo(lua_State L)
  {
    take_debt(L, Cost.CFuncInfo, novariant(LUA_VLCF));
    return new CFuncInfo();
  }

  public static UpVal luaM_malloc_UpVal(lua_State L)
  {
    take_debt(L, Cost.UpVal, novariant(LUA_VUPVAL));
    return new UpVal();
  }

  public static BudgetGroup luaM_malloc_BudgetGroup(lua_State L)
  {
    take_debt(L, Cost.BudgetGroup, novariant(LUA_VBUDGET));
    return new BudgetGroup();
  }

  public static Proto luaM_malloc_Proto(lua_State L)
  {
    take_debt(L, Cost.Proto, novariant(LUA_VPROTO));
    return new Proto();
  }

  public static lua_State luaM_malloc_lua_State(lua_State L)
  {
    take_debt(L, Cost.Thread, novariant(LUA_TTHREAD));
    return new lua_State();
  }

  public static void luaM_free_UpVal(lua_State L, UpVal p)
  {
    if (p != null) free_debt(L, Cost.UpVal, 0);
  }

  public static void luaM_free_BudgetGroup(lua_State L, BudgetGroup p)
  {
    if (p != null) free_debt(L, Cost.BudgetGroup, 0);
  }

  public static void luaM_free_LClosure(lua_State L, LClosure p)
  {
    if (p != null) free_debt(L, Cost.LClosure, novariant(LUA_VLCL));
  }

  public static void luaM_free_CClosure(lua_State L, CClosure p)
  {
    if (p != null) free_debt(L, Cost.CClosure, novariant(LUA_VCCL));
  }

  public static void luaM_free_Udata(lua_State L, Udata p)
  {
    if (p != null) free_debt(L, Cost.Udata, novariant(LUA_VUSERDATA));
  }

  public static void luaM_free_TString(lua_State L, TString p, long bytes)
  {
    if (p != null) free_debt(L, Cost.TString + bytes, novariant(LUA_VSTRING));
  }

  public static void luaM_free_CFuncInfo(lua_State L, CFuncInfo p)
  {
    if (p != null) free_debt(L, Cost.CFuncInfo, novariant(LUA_VLCF));
  }

  public static void luaM_free_Proto(lua_State L, Proto p)
  {
    if (p != null) free_debt(L, Cost.Proto, 0);
  }

  public static void luaM_free_Table(lua_State L, Table p)
  {
    if (p != null) free_debt(L, Cost.Table, novariant(LUA_VTABLE));
  }

  public static void luaM_free_lua_State(lua_State L, lua_State p)
  {
    if (p != null) free_debt(L, Cost.Thread, LUA_TTHREAD);
  }

  public static void luaM_free_CallInfo(lua_State L, CallInfo p)
  {
    if (p != null) free_debt(L, Cost.CallInfo, 0);
  }
}
