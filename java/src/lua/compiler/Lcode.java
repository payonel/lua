package lua.compiler;

import static lua.api.LuaApi.*;
import static lua.compiler.Llex.*;
import static lua.compiler.Lparser.*;
import static lua.gc.Lgc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Ldebug.*;
import static lua.vm.Lopcodes.*;
import static lua.vm.Lvm.*;

import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.object.FmtArgs;
import lua.object.Proto;
import lua.object.TString;
import lua.object.TValue;
import lua.runtime.lua_State;

public final class Lcode
{
  private Lcode()
  {
  }

  /*
  ** Marks the end of a patch list. It is an invalid value both as an absolute
  ** address, and as a list link (would link an element to itself).
  */
  public static final int NO_JUMP = -1;

  /*
  ** grep "ORDER OPR" if you change these enums  (ORDER OP)
  */
  /* arithmetic operators */
  public static final int OPR_ADD = 0;
  public static final int OPR_SUB = 1;
  public static final int OPR_MUL = 2;
  public static final int OPR_MOD = 3;
  public static final int OPR_POW = 4;
  public static final int OPR_DIV = 5;
  public static final int OPR_IDIV = 6;
  /* bitwise operators */
  public static final int OPR_BAND = 7;
  public static final int OPR_BOR = 8;
  public static final int OPR_BXOR = 9;
  public static final int OPR_SHL = 10;
  public static final int OPR_SHR = 11;
  /* string operator */
  public static final int OPR_CONCAT = 12;
  /* comparison operators */
  public static final int OPR_EQ = 13;
  public static final int OPR_LT = 14;
  public static final int OPR_LE = 15;
  public static final int OPR_NE = 16;
  public static final int OPR_GT = 17;
  public static final int OPR_GE = 18;
  /* logical operators */
  public static final int OPR_AND = 19;
  public static final int OPR_OR = 20;
  public static final int OPR_NOBINOPR = 21;

  /* true if operation is foldable (that is, it is arithmetic or bitwise) */
  public static int foldbinop(int op)
  {
    return (op <= OPR_SHR) ? 1 : 0;
  }

  public static final int OPR_MINUS = 0;
  public static final int OPR_BNOT = 1;
  public static final int OPR_NOT = 2;
  public static final int OPR_LEN = 3;
  public static final int OPR_NOUNOPR = 4;

  /* get (pointer to) instruction of given 'expdesc' */

  public static int luaK_codeABC(FuncState fs, int o, int a, int b, int c)
  {
    return luaK_codeABCk(fs, o, a, b, c, 0);
  }

  public static void luaK_setmultret(FuncState fs, expdesc e)
  {
    luaK_setreturns(fs, e, LUA_MULTRET);
  }

  public static void luaK_jumpto(FuncState fs, int t)
  {
    luaK_patchlist(fs, luaK_jump(fs), t);
  }

  /* (note that expressions VJMP also have jumps.) */
  private static int hasjumps(expdesc e)
  {
    return (e.t != e.f) ? 1 : 0;
  }

  /* semantic error */
  public static RuntimeException luaK_semerror(LexState ls, ByteView fmt, FmtArgs args)
  {
    InternalString msg = luaO_pushvfstring(ls.L, fmt, args);
    if (msg == null)
      luaD_throw(ls.L, (byte) LUA_ERRMEM);
    ls.t.token = 0;               /* remove "near <token>" from final message */
    ls.linenumber = ls.lastline; /* back to line of last used token */
    throw luaX_syntaxerror(ls, msg.view());
  }

  /*
  ** If expression is a numeric constant, fills 'v' with its value
  ** and returns 1. Otherwise, returns 0.
  */
  private static int tonumeral(expdesc e, TValue v)
  {
    if (hasjumps(e) != 0)
      return 0; /* not a numeral */
    switch (e.k)
    {
    case VKINT:
      setivalue(v, e.ival);
      return 1;
    case VKFLT:
      setfltvalue(v, e.nval);
      return 1;
    default:
      return 0;
    }
  }

  private static int isnumeral(expdesc e)
  {
    TValue junk = new TValue();
    return tonumeral(e, junk);
  }

  /*
  ** Get the constant value from a constant expression
  */
  private static TValue const2val(FuncState fs, expdesc e)
  {
    assert e.k == VCONST;
    return fs.ls.dyd.actvar.arr[e.info].k;
  }

  /*
  ** If expression is a constant, fills 'v' with its value
  ** and returns 1. Otherwise, returns 0.
  */
  public static int luaK_exp2const(FuncState fs, expdesc e, TValue v)
  {
    if (hasjumps(e) != 0)
      return 0; /* not a constant */
    switch (e.k)
    {
    case VFALSE:
      v.tt_ = LUA_VFALSE;
      return 1;
    case VTRUE:
      v.tt_ = LUA_VTRUE;
      return 1;
    case VNIL:
      setnilvalue(v);
      return 1;
    case VKSTR:
    {
      setsvalue(fs.ls.L, v, e.strval);
      return 1;
    }
    case VCONST:
    {
      setobj(fs.ls.L, v, const2val(fs, e));
      return 1;
    }
    default:
      return tonumeral(e, v);
    }
  }

  /*
  ** Return the previous instruction of the current code. If there
  ** may be a jump target between the current instruction and the
  ** previous one, return an invalid instruction (to avoid wrong
  ** optimizations).
  */
  private static int previousinstruction(FuncState fs)
  {
    if (fs.pc > fs.lasttarget)
      return fs.pc - 1; /* previous instruction */
    else
      return -1;
  }

  /*
  ** Create a OP_LOADNIL instruction, but try to optimize: if the previous
  ** instruction is also OP_LOADNIL and ranges are compatible, adjust
  ** range of previous instruction instead of emitting a new one. (For
  ** instance, 'local a; local b' will generate a single opcode.)
  */
  public static void luaK_nil(FuncState fs, int from, int n)
  {
    int l = from + n - 1; /* last register to set nil */
    int previous = previousinstruction(fs);
    if (previous >= 0 && GET_OPCODE(fs.f.code[previous]) == OP_LOADNIL)
    {                                  /* previous is LOADNIL? */
      int pfrom = GETARG_A(fs.f.code[previous]); /* get previous range */
      int pl = pfrom + GETARG_B(fs.f.code[previous]);
      if ((pfrom <= from && from <= pl + 1) ||
          (from <= pfrom && pfrom <= l + 1))
      { /* can connect both? */
        if (pfrom < from)
          from = pfrom; /* from = min(from, pfrom) */
        if (pl > l)
          l = pl; /* l = max(l, pl) */
        fs.f.code[previous] = SETARG_A(fs.f.code[previous], from);
        fs.f.code[previous] = SETARG_B(fs.f.code[previous], l - from);
        return;
      } /* else go through */
    }
    luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0); /* else no optimization */
  }

  /*
  ** Gets the destination address of a jump instruction. Used to traverse
  ** a list of jumps.
  */
  private static int getjump(FuncState fs, int pc)
  {
    int offset = GETARG_sJ(fs.f.code[pc]);
    if (offset == NO_JUMP) /* point to itself represents end of list */
      return NO_JUMP;      /* end of list */
    else
      return (pc + 1) + offset; /* turn offset into absolute position */
  }

  /*
  ** Fix jump instruction at position 'pc' to jump to 'dest'.
  ** (Jump addresses are relative in Lua)
  */
  private static void fixjump(FuncState fs, int pc, int dest)
  {
    int jmp = pc;
    int offset = dest - (pc + 1);
    assert dest != NO_JUMP;
    if (!(-OFFSET_sJ <= offset && offset <= MAXARG_sJ - OFFSET_sJ))
      luaX_syntaxerror(fs.ls, Lt.control_structure_too_long);
    assert GET_OPCODE(fs.f.code[jmp]) == OP_JMP;
    fs.f.code[jmp] = SETARG_sJ(fs.f.code[jmp], offset);
  }

  /*
  ** Concatenate jump-list 'l2' into jump-list 'l1'
  */
  public static int luaK_concat(FuncState fs, int l1, int l2)
  {
    if (l2 == NO_JUMP)
      return l1;                /* nothing to concatenate? */
    else if (l1 == NO_JUMP) /* no original list? */
      return l2;              /* 'l1' points to 'l2' */
    else
    {
      int list = l1;
      int next;
      while ((next = getjump(fs, list)) != NO_JUMP) /* find last element */
        list = next;
      fixjump(fs, list, l2); /* last element links to 'l2' */
      return l1;
    }
  }

  /*
  ** Create a jump instruction and return its position, so its destination
  ** can be fixed later (with 'fixjump').
  */
  public static int luaK_jump(FuncState fs)
  {
    return codesJ(fs, OP_JMP, NO_JUMP, 0);
  }

  /*
  ** Code a 'return' instruction
  */
  public static void luaK_ret(FuncState fs, int first, int nret)
  {
    int op;
    switch (nret)
    {
    case 0:
      op = OP_RETURN0;
      break;
    case 1:
      op = OP_RETURN1;
      break;
    default:
      op = OP_RETURN;
      break;
    }
    luaY_checklimit(fs, nret + 1, MAXARG_B, Lt.returns);
    luaK_codeABC(fs, op, first, nret + 1, 0);
  }

  /*
  ** Code a "conditional jump", that is, a test or comparison opcode
  ** followed by a jump. Return jump position.
  */
  private static int condjump(FuncState fs, int op, int A, int B, int C, int k)
  {
    luaK_codeABCk(fs, op, A, B, C, k);
    return luaK_jump(fs);
  }

  /*
  ** returns current 'pc' and marks it as a jump target (to avoid wrong
  ** optimizations with consecutive instructions not in the same basic block).
  */
  public static int luaK_getlabel(FuncState fs)
  {
    fs.lasttarget = fs.pc;
    return fs.pc;
  }

  /*
  ** Returns the position of the instruction "controlling" a given
  ** jump (that is, its condition), or the jump itself if it is
  ** unconditional.
  */
  private static int getjumpcontrol(FuncState fs, int pc)
  {
    if (pc >= 1 && testTMode(GET_OPCODE(fs.f.code[pc - 1])) != 0)
      return pc - 1;
    else
      return pc;
  }

  /*
  ** Patch destination register for a TESTSET instruction.
  ** If instruction in position 'node' is not a TESTSET, return 0 ("fails").
  ** Otherwise, if 'reg' is not 'NO_REG', set it as the destination
  ** register. Otherwise, change instruction to a simple 'TEST' (produces
  ** no register value)
  */
  private static int patchtestreg(FuncState fs, int node, int reg)
  {
    int i = getjumpcontrol(fs, node);
    if (GET_OPCODE(fs.f.code[i]) != OP_TESTSET)
      return 0; /* cannot patch other instructions */
    if (reg != NO_REG && reg != GETARG_B(fs.f.code[i]))
      fs.f.code[i] = SETARG_A(fs.f.code[i], reg);
    else
    {
      /* no register to put value or register already has the value;
         change instruction to simple test */
      fs.f.code[i] = CREATE_ABCk(OP_TEST, GETARG_B(fs.f.code[i]), 0, 0, GETARG_k(fs.f.code[i]));
    }
    return 1;
  }

  /*
  ** Traverse a list of tests ensuring no one produces a value
  */
  private static void removevalues(FuncState fs, int list)
  {
    for (; list != NO_JUMP; list = getjump(fs, list))
      patchtestreg(fs, list, NO_REG);
  }

  /*
  ** Traverse a list of tests, patching their destination address and
  ** registers: tests producing values jump to 'vtarget' (and put their
  ** values in 'reg'), other tests jump to 'dtarget'.
  */
  private static void patchlistaux(FuncState fs, int list, int vtarget, int reg,
                                   int dtarget)
  {
    while (list != NO_JUMP)
    {
      int next = getjump(fs, list);
      if (patchtestreg(fs, list, reg) != 0)
        fixjump(fs, list, vtarget);
      else
        fixjump(fs, list, dtarget); /* jump to default target */
      list = next;
    }
  }

  /*
  ** Path all jumps in 'list' to jump to 'target'.
  ** (The assert means that we cannot fix a jump to a forward address
  ** because we only know addresses once code is generated.)
  */
  public static void luaK_patchlist(FuncState fs, int list, int target)
  {
    assert target <= fs.pc;
    patchlistaux(fs, list, target, NO_REG, target);
  }

  public static void luaK_patchtohere(FuncState fs, int list)
  {
    int hr = luaK_getlabel(fs); /* mark "here" as a jump target */
    luaK_patchlist(fs, list, hr);
  }

  /* limit for difference between lines in relative line info. */
  private static final int LIMLINEDIFF = 0x80;

  /*
  ** Save line info for a new instruction. If difference from last line
  ** does not fit in a byte, of after that many instructions, save a new
  ** absolute line info; (in that case, the special value 'ABSLINEINFO'
  ** in 'lineinfo' signals the existence of this absolute information.)
  ** Otherwise, store the difference from last line in 'lineinfo'.
  */
  private static void savelineinfo(FuncState fs, Proto f, int line)
  {
    int linedif = line - fs.previousline;
    int pc = fs.pc - 1; /* last instruction coded */
    if (Math.abs(linedif) >= LIMLINEDIFF || fs.iwthabs++ >= MAXIWTHABS)
    {
      f.abslineinfo = luaM_safegrow_AbsLineInfo_span(fs.ls.L, f.abslineinfo, fs.nabslineinfo);
      f.abslineinfo[fs.nabslineinfo].pc = pc;
      f.abslineinfo[fs.nabslineinfo++].line = line;
      linedif = ABSLINEINFO; /* signal that there is absolute information */
      fs.iwthabs = 1;       /* restart counter */
    }
    f.lineinfo = luaM_safegrow_ls_byte_span(fs.ls.L, f.lineinfo, pc);
    f.lineinfo[pc] = (byte) linedif;
    fs.previousline = line; /* last line saved */
  }

  /*
  ** Remove line information from the last instruction.
  ** If line information for that instruction is absolute, set 'iwthabs'
  ** above its max to force the new (replacing) instruction to have
  ** absolute line info, too.
  */
  private static void removelastlineinfo(FuncState fs)
  {
    Proto f = fs.f;
    int pc = fs.pc - 1; /* last instruction coded */
    if (f.lineinfo[pc] != ABSLINEINFO)
    {                                      /* relative line info? */
      fs.previousline -= f.lineinfo[pc]; /* correct last line saved */
      fs.iwthabs--;                       /* undo previous increment */
    }
    else
    { /* absolute line information */
      assert f.abslineinfo[fs.nabslineinfo - 1].pc == pc;
      fs.nabslineinfo--;           /* remove it */
      fs.iwthabs = MAXIWTHABS + 1; /* force next line info to be absolute */
    }
  }

  /*
  ** Remove the last instruction created, correcting line information
  ** accordingly.
  */
  private static void removelastinstruction(FuncState fs)
  {
    removelastlineinfo(fs);
    fs.pc--;
  }

  /*
  ** Emit instruction 'i', checking for array sizes and saving also its
  ** line information. Return 'i' position.
  */
  public static int luaK_code(FuncState fs, int i)
  {
    Proto f = fs.f;
    /* put new instruction in code array */
    f.code = luaM_safegrow_Instruction_span(fs.ls.L, f.code, fs.pc);
    f.code[fs.pc++] = i;
    savelineinfo(fs, f, fs.ls.lastline);
    return fs.pc - 1; /* index of new instruction */
  }

  /*
  ** Format and emit an 'iABC' instruction. (Assertions check consistency
  ** of parameters versus opcode.)
  */
  public static int luaK_codeABCk(FuncState fs, int o, int A, int B, int C, int k)
  {
    assert getOpMode(o) == iABC;
    assert A <= MAXARG_A && B <= MAXARG_B &&
           C <= MAXARG_C && (k & ~1) == 0;
    return luaK_code(fs, CREATE_ABCk(o, A, B, C, k));
  }

  public static int luaK_codevABCk(FuncState fs, int o, int A, int B, int C, int k)
  {
    assert getOpMode(o) == ivABC;
    assert A <= MAXARG_A && B <= MAXARG_vB &&
           C <= MAXARG_vC && (k & ~1) == 0;
    return luaK_code(fs, CREATE_vABCk(o, A, B, C, k));
  }

  /*
  ** Format and emit an 'iABx' instruction.
  */
  public static int luaK_codeABx(FuncState fs, int o, int A, int Bc)
  {
    assert getOpMode(o) == iABx;
    assert A <= MAXARG_A && Bc <= MAXARG_Bx;
    return luaK_code(fs, CREATE_ABx(o, A, Bc));
  }

  /*
  ** Format and emit an 'iAsBx' instruction.
  */
  private static int codeAsBx(FuncState fs, int o, int A, int Bc)
  {
    int b = Bc + OFFSET_sBx;
    assert getOpMode(o) == iAsBx;
    assert A <= MAXARG_A && b <= MAXARG_Bx;
    return luaK_code(fs, CREATE_ABx(o, A, b));
  }

  /*
  ** Format and emit an 'isJ' instruction.
  */
  private static int codesJ(FuncState fs, int o, int sj, int k)
  {
    int j = sj + OFFSET_sJ;
    assert getOpMode(o) == isJ;
    assert j <= MAXARG_sJ && (k & ~1) == 0;
    return luaK_code(fs, CREATE_sJ(o, j, k));
  }

  /*
  ** Emit an "extra argument" instruction (format 'iAx')
  */
  private static int codeextraarg(FuncState fs, int A)
  {
    assert A <= MAXARG_Ax;
    return luaK_code(fs, CREATE_Ax(OP_EXTRAARG, A));
  }

  /*
  ** Emit a "load constant" instruction, using either 'OP_LOADK'
  ** (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
  ** instruction with "extra argument".
  */
  private static int luaK_codek(FuncState fs, int reg, int k)
  {
    if (k <= MAXARG_Bx)
      return luaK_codeABx(fs, OP_LOADK, reg, k);
    else
    {
      int p = luaK_codeABx(fs, OP_LOADKX, reg, 0);
      codeextraarg(fs, k);
      return p;
    }
  }

  /*
  ** Check register-stack level, keeping track of its maximum size
  ** in field 'maxstacksize'
  */
  public static void luaK_checkstack(FuncState fs, int n)
  {
    int newstack = fs.freereg + n;
    if (newstack > fs.f.maxstacksize)
    {
      luaY_checklimit(fs, newstack, MAX_FSTACK, Lt.registers);
      fs.f.maxstacksize = newstack;
    }
  }

  /*
  ** Reserve 'n' registers in register stack
  */
  public static void luaK_reserveregs(FuncState fs, int n)
  {
    luaK_checkstack(fs, n);
    fs.freereg = fs.freereg + n;
  }

  /*
  ** Free register 'reg', if it is neither a constant index nor
  ** a local variable.
  )
  */
  private static void freereg(FuncState fs, int reg)
  {
    if (reg >= luaY_nvarstack(fs))
    {
      fs.freereg--;
      assert reg == fs.freereg;
    }
  }

  /*
  ** Free two registers in proper order
  */
  private static void freeregs(FuncState fs, int r1, int r2)
  {
    if (r1 > r2)
    {
      freereg(fs, r1);
      freereg(fs, r2);
    }
    else
    {
      freereg(fs, r2);
      freereg(fs, r1);
    }
  }

  /*
  ** Free register used by expression 'e' (if any)
  */
  private static void freeexp(FuncState fs, expdesc e)
  {
    if (e.k == VNONRELOC)
      freereg(fs, e.info);
  }

  /*
  ** Free registers used by expressions 'e1' and 'e2' (if any) in proper
  ** order.
  */
  private static void freeexps(FuncState fs, expdesc e1, expdesc e2)
  {
    int r1 = (e1.k == VNONRELOC) ? e1.info : -1;
    int r2 = (e2.k == VNONRELOC) ? e2.info : -1;
    freeregs(fs, r1, r2);
  }

  /*
  ** Add constant 'v' to prototype's list of constants (field 'k').
  */
  private static int addk(FuncState fs, Proto f, TValue v)
  {
    lua_State L = fs.ls.L;
    int oldsize = f.k.length;
    int k = fs.nk;
    f.k = luaM_safegrow_TValue_span(L, f.k, k);
    while (oldsize < f.k.length)
      setnilvalue(f.k[oldsize++]);
    setobj(L, f.k[k], v);
    fs.nk++;
    luaC_barrier(L, f, v);
    return k;
  }

  /*
  ** Use scanner's table to cache position of constants in constant list
  ** and try to reuse constants. Because some values should not be used
  ** as keys (nil cannot be a key, integer keys can collapse with float
  ** keys), the caller must provide a useful 'key' for indexing the cache.
  */
  private static int k2proto(FuncState fs, TValue key, TValue v)
  {
    TValue val = new TValue();
    Proto f = fs.f;
    int tag = fs.kcache.luaH_get(key, val); /* query scanner table */
    if (!tagisempty(tag))
    { /* is there an index there? */
      int k = (int) ivalue(val);
      /* collisions can happen only for float keys */
      assert ttisfloat(key) || luaV_rawequalobj(f.k[k], v) != 0;
      return k; /* reuse index */
    }
    else
    { /* constant not found; create a new entry */
      int k = addk(fs, f, v);
      /* cache it for reuse; numerical value does not need GC barrier;
         table is not a metatable, so it does not need to invalidate cache */
      setivalue(val, k);
      fs.kcache.luaH_set(fs.ls.L, key, val);
      return k;
    }
  }

  /*
  ** Add a string to list of constants and return its index.
  */
  private static int stringK(FuncState fs, TString s)
  {
    TValue o = new TValue();
    setsvalue(fs.ls.L, o, s);
    return k2proto(fs, o, o); /* use string itself as key */
  }

  /*
  ** Add an integer to list of constants and return its index.
  */
  private static int luaK_intK(FuncState fs, long n)
  {
    TValue o = new TValue();
    setivalue(o, n);
    return k2proto(fs, o, o); /* use integer itself as key */
  }

  /*
  ** Add a float to list of constants and return its index. Floats
  ** with integral values need a different key, to avoid collision
  ** with actual integers. To that end, we add to the number its smaller
  ** power-of-two fraction that is still significant in its scale.
  ** (For doubles, the fraction would be 2^-52).
  ** This method is not bulletproof: different numbers may generate the
  ** same key (e.g., very large numbers will overflow to 'inf') and for
  ** floats larger than 2^53 the result is still an integer. For those
  ** cases, just generate a new entry. At worst, this only wastes an entry
  ** with a duplicate.
  */
  private static int luaK_numberK(FuncState fs, double r)
  {
    TValue o = new TValue(), kv = new TValue();
    setfltvalue(o, r); /* value as a TValue */
    if (r == 0)
    {                              /* handle zero as a special case */
      setpvalue(kv, fs);          /* use FuncState as index */
      return k2proto(fs, kv, o); /* cannot collide */
    }
    else
    {
      final int nbm = Double.PRECISION;
      final double q = Math.scalb(1.0, -nbm + 1);
      final double k = r * (1 + q); /* key */
      IntResult ik = new IntResult();
      setfltvalue(kv, k); /* key as a TValue */
      if (luaV_flttointeger(k, ik, F2Ieq) == 0)
      {                                         /* not an integer value? */
        int n = k2proto(fs, kv, o);           /* use key */
        if (luaV_rawequalobj(fs.f.k[n], o) != 0) /* correct value? */
          return n;
      }
      /* else, either key is still an integer or there was a collision;
         anyway, do not try to reuse constant; instead, create a new one */
      return addk(fs, fs.f, o);
    }
  }

  /*
  ** Add a false to list of constants and return its index.
  */
  private static int boolF(FuncState fs)
  {
    TValue o = new TValue();
    o.tt_ = LUA_VFALSE;
    return k2proto(fs, o, o); /* use boolean itself as key */
  }

  /*
  ** Add a true to list of constants and return its index.
  */
  private static int boolT(FuncState fs)
  {
    TValue o = new TValue();
    o.tt_ = LUA_VTRUE;
    return k2proto(fs, o, o); /* use boolean itself as key */
  }

  /*
  ** Add nil to list of constants and return its index.
  */
  private static int nilK(FuncState fs)
  {
    lua_State L = fs.ls.L;
    TValue k = new TValue();
    /* cannot use nil as key; instead use table itself */
    sethvalue(L, k, fs.kcache);
    return k2proto(fs, k, L.l_G.nilvalue);
  }

  /*
  ** Check whether 'i' can be stored in an 'sC' operand. Equivalent to
  ** (0 <= int2sC(i) && int2sC(i) <= MAXARG_C) but without risk of
  ** overflows in the hidden addition inside 'int2sC'.
  */
  private static int fitsC(long i)
  {
    return (-OFFSET_sC <= i && i <= MAXARG_C - OFFSET_sC) ? 1 : 0;
  }

  /*
  ** Check whether 'i' can be stored in an 'sBx' operand.
  */
  private static int fitsBx(long i)
  {
    return (-OFFSET_sBx <= i && i <= MAXARG_Bx - OFFSET_sBx) ? 1 : 0;
  }

  public static void luaK_int(FuncState fs, int reg, long i)
  {
    if (fitsBx(i) != 0)
      codeAsBx(fs, OP_LOADI, reg, (int) i);
    else
      luaK_codek(fs, reg, luaK_intK(fs, i));
  }

  private static void luaK_float(FuncState fs, int reg, double f)
  {
    IntResult fi = new IntResult();
    if (luaV_flttointeger(f, fi, F2Ieq) != 0 && fitsBx(fi.v) != 0)
      codeAsBx(fs, OP_LOADF, reg, (int) fi.v);
    else
      luaK_codek(fs, reg, luaK_numberK(fs, f));
  }

  /*
  ** Get the value of 'var' in a register and generate an opcode to check
  ** whether that register is nil. 'k' is the index of the variable name
  ** in the list of constants. If its value cannot be encoded in Bx, a 0
  ** will use '?' for the name.
  */
  public static void luaK_codecheckglobal(FuncState fs, expdesc var, int k, int line)
  {
    luaK_exp2anyreg(fs, var);
    luaK_fixline(fs, line);
    k = (k >= MAXARG_Bx) ? 0 : k + 1;
    luaK_codeABx(fs, OP_ERRNNIL, var.info, k);
    luaK_fixline(fs, line);
    freeexp(fs, var);
  }

  /*
  ** Convert a constant in 'v' into an expression description 'e'
  */
  private static void const2exp(TValue v, expdesc e)
  {
    switch (ttypetag(v))
    {
    case LUA_VNUMINT:
      e.k = VKINT;
      assert ttisinteger(v);
      e.ival = ivalue(v);
      break;
    case LUA_VNUMFLT:
      e.k = VKFLT;
      assert ttisfloat(v);
      e.nval = fltvalue(v);
      break;
    case LUA_VFALSE:
      e.k = VFALSE;
      break;
    case LUA_VTRUE:
      e.k = VTRUE;
      break;
    case LUA_VNIL:
      e.k = VNIL;
      break;
    case LUA_VSHRSTR:
      e.k = VKSTR;
      assert ttisstring(v);
      e.strval = tsvalue(v);
      break;
    default:
      assert false;
    }
  }

  /*
  ** Fix an expression to return the number of results 'nresults'.
  ** 'e' must be a multi-ret expression (function call or vararg).
  */
  public static void luaK_setreturns(FuncState fs, expdesc e, int nresults)
  {
    int pc = e.info;
    luaY_checklimit(fs, nresults + 1, MAXARG_C, Lt.multiple_results);
    if (e.k == VCALL) /* expression is an open function call? */
      fs.f.code[pc] = SETARG_C(fs.f.code[pc], nresults + 1);
    else
    {
      assert e.k == VVARARG;
      fs.f.code[pc] = SETARG_C(fs.f.code[pc], nresults + 1);
      fs.f.code[pc] = SETARG_A(fs.f.code[pc], fs.freereg);
      luaK_reserveregs(fs, 1);
    }
  }

  /*
  ** Convert a VKSTR to a VK
  */
  private static int str2K(FuncState fs, expdesc e)
  {
    assert e.k == VKSTR;
    e.info = stringK(fs, e.strval);
    e.k = VK;
    return e.info;
  }

  /*
  ** Fix an expression to return one result.
  ** If expression is not a multi-ret expression (function call or
  ** vararg), it already returns one result, so nothing needs to be done.
  ** Function calls become VNONRELOC expressions (as its result comes
  ** fixed in the base register of the call), while vararg expressions
  ** become VRELOC (as OP_VARARG puts its results where it wants).
  ** (Calls are created returning one result, so that does not need
  ** to be fixed.)
  */
  public static void luaK_setoneret(FuncState fs, expdesc e)
  {
    if (e.k == VCALL)
    { /* expression is an open function call? */
      /* already returns 1 value */
      assert GETARG_C(fs.f.code[e.info]) == 2;
      e.k = VNONRELOC; /* result has fixed position */
      e.info = GETARG_A(fs.f.code[e.info]);
    }
    else if (e.k == VVARARG)
    {
      fs.f.code[e.info] = SETARG_C(fs.f.code[e.info], 2);
      e.k = VRELOC; /* can relocate its simple result */
    }
  }

  /*
  ** Change a vararg parameter into a regular local variable
  */
  public static void luaK_vapar2local(FuncState fs, expdesc var)
  {
    needvatab(fs.f); /* function will need a vararg table */
    /* now a vararg parameter is equivalent to a regular local variable */
    var.k = VLOCAL;
  }

  /*
  ** Ensure that expression 'e' is not a variable (nor a <const>).
  ** (Expression still may have jump lists.)
  */
  public static void luaK_dischargevars(FuncState fs, expdesc e)
  {
    switch (e.k)
    {
    case VCONST:
    {
      const2exp(const2val(fs, e), e);
      break;
    }
    case VVARGVAR:
    {
      luaK_vapar2local(fs, e); /* turn it into a local variable */
    } /* FALLTHROUGH */
    case VLOCAL:
    { /* already in a register */
      int temp = e.var.ridx;
      e.info = temp; /* (avoid a direct assignment; values overlap) */
      e.k = VNONRELOC; /* becomes a non-relocatable value */
      break;
    }
    case VUPVAL:
    { /* move value to some (pending) register */
      e.info = luaK_codeABC(fs, OP_GETUPVAL, 0, e.info, 0);
      e.k = VRELOC;
      break;
    }
    case VINDEXUP:
    {
      e.info = luaK_codeABC(fs, OP_GETTABUP, 0, e.ind.t, e.ind.idx);
      e.k = VRELOC;
      break;
    }
    case VINDEXI:
    {
      freereg(fs, e.ind.t);
      e.info = luaK_codeABC(fs, OP_GETI, 0, e.ind.t, e.ind.idx);
      e.k = VRELOC;
      break;
    }
    case VINDEXSTR:
    {
      freereg(fs, e.ind.t);
      e.info = luaK_codeABC(fs, OP_GETFIELD, 0, e.ind.t, e.ind.idx);
      e.k = VRELOC;
      break;
    }
    case VINDEXED:
    {
      freeregs(fs, e.ind.t, e.ind.idx);
      e.info = luaK_codeABC(fs, OP_GETTABLE, 0, e.ind.t, e.ind.idx);
      e.k = VRELOC;
      break;
    }
    case VVARGIND:
    {
      freeregs(fs, e.ind.t, e.ind.idx);
      e.info = luaK_codeABC(fs, OP_GETVARG, 0, e.ind.t, e.ind.idx);
      e.k = VRELOC;
      break;
    }
    case VVARARG:
    case VCALL:
    {
      luaK_setoneret(fs, e);
      break;
    }
    default:
      break; /* there is one value available (somewhere) */
    }
  }

  /*
  ** Ensure expression value is in register 'reg', making 'e' a
  ** non-relocatable expression.
  ** (Expression still may have jump lists.)
  */
  private static void discharge2reg(FuncState fs, expdesc e, int reg)
  {
    luaK_dischargevars(fs, e);
    switch (e.k)
    {
    case VNIL:
    {
      luaK_nil(fs, reg, 1);
      break;
    }
    case VFALSE:
    {
      luaK_codeABC(fs, OP_LOADFALSE, reg, 0, 0);
      break;
    }
    case VTRUE:
    {
      luaK_codeABC(fs, OP_LOADTRUE, reg, 0, 0);
      break;
    }
    case VKSTR:
    {
      str2K(fs, e);
    } /* FALLTHROUGH */
    case VK:
    {
      luaK_codek(fs, reg, e.info);
      break;
    }
    case VKFLT:
    {
      luaK_float(fs, reg, e.nval);
      break;
    }
    case VKINT:
    {
      luaK_int(fs, reg, e.ival);
      break;
    }
    case VRELOC:
    {
      int pc = e.info;
      fs.f.code[pc] = SETARG_A(fs.f.code[pc], reg); /* instruction will put result in 'reg' */
      break;
    }
    case VNONRELOC:
    {
      if (reg != e.info)
        luaK_codeABC(fs, OP_MOVE, reg, e.info, 0);
      break;
    }
    default:
    {
      assert e.k == VJMP;
      return; /* nothing to do... */
    }
    }
    e.info = reg;
    e.k = VNONRELOC;
  }

  /*
  ** Ensure expression value is in a register, making 'e' a
  ** non-relocatable expression.
  ** (Expression still may have jump lists.)
  */
  private static void discharge2anyreg(FuncState fs, expdesc e)
  {
    if (e.k != VNONRELOC)
    {                                        /* no fixed register yet? */
      luaK_reserveregs(fs, 1);               /* get a register */
      discharge2reg(fs, e, fs.freereg - 1); /* put value there */
    }
  }

  private static int code_loadbool(FuncState fs, int A, int op)
  {
    luaK_getlabel(fs); /* those instructions may be jump targets */
    return luaK_codeABC(fs, op, A, 0, 0);
  }

  /*
  ** check whether list has any jump that do not produce a value
  ** or produce an inverted value
  */
  private static int need_value(FuncState fs, int list)
  {
    for (; list != NO_JUMP; list = getjump(fs, list))
    {
      int i = fs.f.code[getjumpcontrol(fs, list)];
      if (GET_OPCODE(i) != OP_TESTSET)
        return 1;
    }
    return 0; /* not found */
  }

  /*
  ** Ensures final expression result (which includes results from its
  ** jump lists) is in register 'reg'.
  ** If expression has jumps, need to patch these jumps either to
  ** its final position or to "load" instructions (for those tests
  ** that do not produce values).
  */
  private static void exp2reg(FuncState fs, expdesc e, int reg)
  {
    discharge2reg(fs, e, reg);
    if (e.k == VJMP)                    /* expression itself is a test? */
      e.t = luaK_concat(fs, e.t, e.info); /* put this jump in 't' list */
    if (hasjumps(e) != 0)
    {
      int final_kw;         /* position after whole expression */
      int p_f = NO_JUMP; /* position of an eventual LOAD false */
      int p_t = NO_JUMP; /* position of an eventual LOAD true */
      if (need_value(fs, e.t) != 0 || need_value(fs, e.f) != 0)
      {
        int fj = (e.k == VJMP) ? NO_JUMP : luaK_jump(fs);
        p_f = code_loadbool(fs, reg, OP_LFALSESKIP); /* skip next inst. */
        p_t = code_loadbool(fs, reg, OP_LOADTRUE);
        /* jump around these booleans if 'e' is not a test */
        luaK_patchtohere(fs, fj);
      }
      final_kw = luaK_getlabel(fs);
      patchlistaux(fs, e.f, final_kw, reg, p_f);
      patchlistaux(fs, e.t, final_kw, reg, p_t);
    }
    e.f = e.t = NO_JUMP;
    e.info = reg;
    e.k = VNONRELOC;
  }

  /*
  ** Ensures final expression result is in next available register.
  */
  public static void luaK_exp2nextreg(FuncState fs, expdesc e)
  {
    luaK_dischargevars(fs, e);
    freeexp(fs, e);
    luaK_reserveregs(fs, 1);
    exp2reg(fs, e, fs.freereg - 1);
  }

  /*
  ** Ensures final expression result is in some (any) register
  ** and return that register.
  */
  public static int luaK_exp2anyreg(FuncState fs, expdesc e)
  {
    luaK_dischargevars(fs, e);
    if (e.k == VNONRELOC)
    {                     /* expression already has a register? */
      if (hasjumps(e) == 0)   /* no jumps? */
        return e.info; /* result is already in a register */
      if (e.info >= luaY_nvarstack(fs))
      {                            /* reg. is not a local? */
        exp2reg(fs, e, e.info); /* put final result in it */
        return e.info;
      }
      /* else expression has jumps and cannot change its register
         to hold the jump values, because it is a local variable.
         Go through to the default case. */
    }
    luaK_exp2nextreg(fs, e); /* default: use next available register */
    return e.info;
  }

  /*
  ** Ensures final expression result is either in a register,
  ** in an upvalue, or it is the vararg parameter.
  */
  public static void luaK_exp2anyregup(FuncState fs, expdesc e)
  {
    if ((e.k != VUPVAL && e.k != VVARGVAR) || hasjumps(e) != 0)
      luaK_exp2anyreg(fs, e);
  }

  /*
  ** Ensures final expression result is either in a register
  ** or it is a constant.
  */
  public static void luaK_exp2val(FuncState fs, expdesc e)
  {
    if (e.k == VJMP || hasjumps(e) != 0)
      luaK_exp2anyreg(fs, e);
    else
      luaK_dischargevars(fs, e);
  }

  /*
  ** Try to make 'e' a K expression with an index in the range of R/K
  ** indices. Return true iff succeeded.
  */
  private static int luaK_exp2K(FuncState fs, expdesc e)
  {
    if (hasjumps(e) == 0)
    {
      int info;
      switch (e.k)
      { /* move constants to 'k' */
      case VTRUE:
        info = boolT(fs);
        break;
      case VFALSE:
        info = boolF(fs);
        break;
      case VNIL:
        info = nilK(fs);
        break;
      case VKINT:
        info = luaK_intK(fs, e.ival);
        break;
      case VKFLT:
        info = luaK_numberK(fs, e.nval);
        break;
      case VKSTR:
        info = stringK(fs, e.strval);
        break;
      case VK:
        info = e.info;
        break;
      default:
        return 0; /* not a constant */
      }
      if (info <= MAXINDEXRK)
      {            /* does constant fit in 'argC'? */
        e.k = VK; /* make expression a 'K' expression */
        e.info = info;
        return 1;
      }
    }
    /* else, expression doesn't fit; leave it unchanged */
    return 0;
  }

  /*
  ** Ensures final expression result is in a valid R/K index
  ** (that is, it is either in a register or in 'k' with an index
  ** in the range of R/K indices).
  ** Returns 1 iff expression is K.
  */
  private static int exp2RK(FuncState fs, expdesc e)
  {
    if (luaK_exp2K(fs, e) != 0)
      return 1;
    else
    { /* not a constant in the right range: put it in a register */
      luaK_exp2anyreg(fs, e);
      return 0;
    }
  }

  private static void codeABRK(FuncState fs, int o, int A, int B,
                               expdesc ec)
  {
    int k = exp2RK(fs, ec);
    luaK_codeABCk(fs, o, A, B, ec.info, k);
  }

  /*
  ** Generate code to store result of expression 'ex' into variable 'var'.
  */
  public static void luaK_storevar(FuncState fs, expdesc var, expdesc ex)
  {
    switch (var.k)
    {
    case VLOCAL:
    {
      freeexp(fs, ex);
      exp2reg(fs, ex, var.var.ridx); /* compute 'ex' into proper place */
      return;
    }
    case VUPVAL:
    {
      int e = luaK_exp2anyreg(fs, ex);
      luaK_codeABC(fs, OP_SETUPVAL, e, var.info, 0);
      break;
    }
    case VINDEXUP:
    {
      codeABRK(fs, OP_SETTABUP, var.ind.t, var.ind.idx, ex);
      break;
    }
    case VINDEXI:
    {
      codeABRK(fs, OP_SETI, var.ind.t, var.ind.idx, ex);
      break;
    }
    case VINDEXSTR:
    {
      codeABRK(fs, OP_SETFIELD, var.ind.t, var.ind.idx, ex);
      break;
    }
    case VVARGIND:
    {
      needvatab(fs.f); /* function will need a vararg table */
      /* now, assignment is to a regular table */
    } /* FALLTHROUGH */
    case VINDEXED:
    {
      codeABRK(fs, OP_SETTABLE, var.ind.t, var.ind.idx, ex);
      break;
    }
    default:
      assert false; /* invalid var kind to store */
    }
    freeexp(fs, ex);
  }

  /*
  ** Negate condition 'e' (where 'e' is a comparison).
  */
  private static void negatecondition(FuncState fs, expdesc e)
  {
    int pc = getjumpcontrol(fs, e.info);
    assert testTMode(GET_OPCODE(fs.f.code[pc])) != 0 && GET_OPCODE(fs.f.code[pc]) != OP_TESTSET &&
           GET_OPCODE(fs.f.code[pc]) != OP_TEST;
    fs.f.code[pc] = SETARG_k(fs.f.code[pc], GETARG_k(fs.f.code[pc]) ^ 1);
  }

  /*
  ** Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
  ** is true, code will jump if 'e' is true.) Return jump position.
  ** Optimize when 'e' is 'not' something, inverting the condition
  ** and removing the 'not'.
  */
  private static int jumponcond(FuncState fs, expdesc e, int cond)
  {
    if (e.k == VRELOC)
    {
      int ie = fs.f.code[e.info];
      if (GET_OPCODE(ie) == OP_NOT)
      {
        removelastinstruction(fs); /* remove previous OP_NOT */
        return condjump(fs, OP_TEST, GETARG_B(ie), 0, 0, (cond == 0) ? 1 : 0);
      }
      /* else go through */
    }
    discharge2anyreg(fs, e);
    freeexp(fs, e);
    return condjump(fs, OP_TESTSET, NO_REG, e.info, 0, cond);
  }

  /*
  ** Emit code to go through if 'e' is true, jump otherwise.
  */
  public static void luaK_goiftrue(FuncState fs, expdesc e)
  {
    int pc; /* pc of new jump */
    luaK_dischargevars(fs, e);
    switch (e.k)
    {
    case VJMP:
    {                         /* condition? */
      negatecondition(fs, e); /* jump when it is false */
      pc = e.info;         /* save jump position */
      break;
    }
    case VK:
    case VKFLT:
    case VKINT:
    case VKSTR:
    case VTRUE:
    {
      pc = NO_JUMP; /* always true; do nothing */
      break;
    }
    default:
    {
      pc = jumponcond(fs, e, 0); /* jump when false */
      break;
    }
    }
    e.f = luaK_concat(fs, e.f, pc); /* insert new jump in false list */
    luaK_patchtohere(fs, e.t); /* true list jumps to here (to go through) */
    e.t = NO_JUMP;
  }

  /*
  ** Emit code to go through if 'e' is false, jump otherwise.
  */
  private static void luaK_goiffalse(FuncState fs, expdesc e)
  {
    int pc; /* pc of new jump */
    luaK_dischargevars(fs, e);
    switch (e.k)
    {
    case VJMP:
    {
      pc = e.info; /* already jump if true */
      break;
    }
    case VNIL:
    case VFALSE:
    {
      pc = NO_JUMP; /* always false; do nothing */
      break;
    }
    default:
    {
      pc = jumponcond(fs, e, 1); /* jump if true */
      break;
    }
    }
    e.t = luaK_concat(fs, e.t, pc); /* insert new jump in 't' list */
    luaK_patchtohere(fs, e.f); /* false list jumps to here (to go through) */
    e.f = NO_JUMP;
  }

  /*
  ** Code 'not e', doing constant folding.
  */
  private static void codenot(FuncState fs, expdesc e)
  {
    switch (e.k)
    {
    case VNIL:
    case VFALSE:
    {
      e.k = VTRUE; /* true == not nil == not false */
      break;
    }
    case VK:
    case VKFLT:
    case VKINT:
    case VKSTR:
    case VTRUE:
    {
      e.k = VFALSE; /* false == not "x" == not 0.5 == not 1 == not true */
      break;
    }
    case VJMP:
    {
      negatecondition(fs, e);
      break;
    }
    case VRELOC:
    case VNONRELOC:
    {
      discharge2anyreg(fs, e);
      freeexp(fs, e);
      e.info = luaK_codeABC(fs, OP_NOT, 0, e.info, 0);
      e.k = VRELOC;
      break;
    }
    default:
      assert false; /* cannot happen */
    }
    /* interchange true and false lists */
    {
      int temp = e.f;
      e.f = e.t;
      e.t = temp;
    }
    removevalues(fs, e.f); /* values are useless when negated */
    removevalues(fs, e.t);
  }

  /*
  ** Check whether expression 'e' is a short literal string
  */
  private static int isKstr(FuncState fs, expdesc e)
  {
    return (e.k == VK && hasjumps(e) == 0 && e.info <= MAXINDEXRK &&
            ttisstring(fs.f.k[e.info])) ? 1 : 0;
  }

  /*
  ** Check whether expression 'e' is a literal integer.
  */
  private static int isKint(expdesc e)
  {
    return (e.k == VKINT && hasjumps(e) == 0) ? 1 : 0;
  }

  /*
  ** Check whether expression 'e' is a literal integer in
  ** proper range to fit in register C
  */
  private static int isCint(expdesc e)
  {
    return (isKint(e) != 0 && (0 <= e.ival && e.ival <= MAXARG_C)) ? 1 : 0;
  }

  /*
  ** Check whether expression 'e' is a literal integer in
  ** proper range to fit in register sC
  */
  private static int isSCint(expdesc e)
  {
    return (isKint(e) != 0 && fitsC(e.ival) != 0) ? 1 : 0;
  }

  /*
  ** Check whether expression 'e' is a literal integer or float in
  ** proper range to fit in a register (sB or sC).
  */
  private static final class SCImm
  {
    int im;
    int isfloat;

    SCImm(int im, int isfloat)
    {
      this.im = im;
      this.isfloat = isfloat;
    }
  }

  private static int isSCnumber(expdesc e, SCImm sc)
  {
    IntResult i = new IntResult();
    if (e.k == VKINT)
      i.v = e.ival;
    else if (e.k == VKFLT && luaV_flttointeger(e.nval, i, F2Ieq) != 0)
      sc.isfloat = 1;
    else
      return 0; /* not a number */
    if (hasjumps(e) == 0 && fitsC(i.v) != 0)
    {
      sc.im = int2sC((int) i.v);
      return 1;
    }
    else
      return 0;
  }

  /*
  ** Emit SELF instruction or equivalent: the code will convert
  ** expression 'e' into 'e.key(e,'.
  */
  public static void luaK_self(FuncState fs, expdesc e, expdesc key)
  {
    int ereg, base;
    luaK_exp2anyreg(fs, e);
    ereg = e.info; /* register where 'e' (the receiver) was placed */
    freeexp(fs, e);
    base = e.info = fs.freereg; /* base register for op_self */
    e.k = VNONRELOC;               /* self expression has a fixed register */
    luaK_reserveregs(fs, 2);        /* method and 'self' produced by op_self */
    assert key.k == VKSTR;
    if (luaK_exp2K(fs, key) != 0)
    {
      /* can use 'self' opcode */
      luaK_codeABCk(fs, OP_SELF, base, ereg, key.info, 0);
    }
    else
    {                                                         /* cannot use 'self' opcode; use move+gettable */
      luaK_exp2anyreg(fs, key);                               /* put method name in a register */
      luaK_codeABC(fs, OP_MOVE, base + 1, ereg, 0);           /* copy self to base+1 */
      luaK_codeABC(fs, OP_GETTABLE, base, ereg, key.info); /* get method */
    }
    freeexp(fs, key);
  }

  /* auxiliary function to define indexing expressions */
  private static void fillidxk(expdesc t, int idx, int k)
  {
    t.ind.idx = idx;
    t.k = k;
  }

  /*
  ** Create expression 't[k]'. 't' must have its final result already in a
  ** register or upvalue. Upvalues can only be indexed by literal strings.
  ** Keys can be literal strings in the constant table or arbitrary
  ** values in registers.
  */
  public static void luaK_indexed(FuncState fs, expdesc t, expdesc k)
  {
    int keystr = -1;
    if (k.k == VKSTR)
      keystr = str2K(fs, k);
    assert hasjumps(t) == 0 &&
           (t.k == VLOCAL || t.k == VVARGVAR ||
            t.k == VNONRELOC || t.k == VUPVAL);
    if (t.k == VUPVAL && isKstr(fs, k) == 0) /* upvalue indexed by non 'Kstr'? */
      luaK_exp2anyreg(fs, t);             /* put it in a register */
    if (t.k == VUPVAL)
    {
      int temp = t.info; /* upvalue index */
      t.ind.t = temp;                   /* (avoid a direct assignment; values overlap) */
      assert isKstr(fs, k) != 0;
      fillidxk(t, k.info, VINDEXUP); /* literal short string */
    }
    else if (t.k == VVARGVAR)
    {                                          /* indexing the vararg parameter? */
      int kreg = luaK_exp2anyreg(fs, k);       /* put key in some register */
      int vreg = t.var.ridx; /* register with vararg param. */
      assert vreg == fs.f.numparams;
      t.ind.t = vreg;           /* (avoid a direct assignment; values may overlap?) */
      fillidxk(t, kreg, VVARGIND); /* 't' represents 'vararg[k]' */
    }
    else
    {
      /* register index of the table */
      int temp = (t.k == VLOCAL) ? t.var.ridx : t.info;
      t.ind.t = temp; /* (avoid a direct assignment; values may overlap?) */
      if (isKstr(fs, k) != 0)
        fillidxk(t, k.info, VINDEXSTR); /* literal short string */
      else if (isCint(k) != 0)                  /* int. constant in proper range? */
        fillidxk(t, (int) k.ival, VINDEXI);
      else
        fillidxk(t, luaK_exp2anyreg(fs, k), VINDEXED); /* register */
    }
    t.ind.keystr = keystr; /* string index in 'k' */
    t.ind.ro = 0;          /* by default, not read-only */
  }

  /*
  ** Return false if folding can raise an error.
  ** Bitwise operations need operands convertible to integers; division
  ** operations cannot have 0 as divisor.
  */
  private static int validop(int op, TValue v1, TValue v2)
  {
    switch (op)
    {
    case LUA_OPBAND:
    case LUA_OPBOR:
    case LUA_OPBXOR:
    case LUA_OPSHL:
    case LUA_OPSHR:
    case LUA_OPBNOT:
    { /* conversion errors */
      IntResult i = new IntResult();
      return (luaV_tointegerns(v1, i, LUA_FLOORN2I) != 0 &&
              luaV_tointegerns(v2, i, LUA_FLOORN2I) != 0) ? 1 : 0;
    }
    case LUA_OPDIV:
    case LUA_OPIDIV:
    case LUA_OPMOD: /* division by 0 */
    {
      assert ttisnumber(v2);
      double n2 = ttisinteger(v2) ? (double) ivalue(v2) : fltvalue(v2);
      return (n2 != 0) ? 1 : 0;
    }
    default:
      return 1; /* everything else is valid */
    }
  }

  /*
  ** Try to "constant-fold" an operation; return 1 iff successful.
  ** (In this case, 'e1' has the final result.)
  */
  private static int constfolding(FuncState fs, int op, expdesc e1,
                                  expdesc e2)
  {
    TValue v1 = new TValue(), v2 = new TValue(), res = new TValue();
    if (tonumeral(e1, v1) == 0 || tonumeral(e2, v2) == 0 || validop(op, v1, v2) == 0)
      return 0;                                   /* non-numeric operands or not safe to fold */
    luaO_rawarith(fs.ls.L, op, v1, v2, res); /* does operation */
    if (ttisinteger(res))
    {
      e1.k = VKINT;
      assert ttisinteger(res);
      e1.ival = ivalue(res);
    }
    else
    { /* folds neither NaN nor 0.0 (to avoid problems with -0.0) */
      assert ttisfloat(res);
      double n = fltvalue(res);
      if ((!(n == n)) || n == 0)
        return 0;
      e1.k = VKFLT;
      e1.nval = n;
    }
    return 1;
  }

  /*
  ** Convert a BinOpr to an OpCode  (ORDER OPR - ORDER OP)
  */
  private static int binopr2op(int opr, int baser, int base)
  {
    assert baser <= opr &&
           ((baser == OPR_ADD && opr <= OPR_SHR) ||
            (baser == OPR_LT && opr <= OPR_LE));
    return ((opr - baser) + base);
  }

  /*
  ** Convert a UnOpr to an OpCode  (ORDER OPR - ORDER OP)
  */
  private static int unopr2op(int opr)
  {
    return ((opr - OPR_MINUS) +
            OP_UNM);
  }

  /*
  ** Convert a BinOpr to a tag method  (ORDER OPR - ORDER TM)
  */
  private static int binopr2TM(int opr)
  {
    assert OPR_ADD <= opr && opr <= OPR_SHR;
    return ((opr - OPR_ADD) + TM_ADD);
  }

  /*
  ** Emit code for unary expressions that "produce values"
  ** (everything but 'not').
  ** Expression to produce final result will be encoded in 'e'.
  */
  private static void codeunexpval(FuncState fs, int op, expdesc e, int line)
  {
    int r = luaK_exp2anyreg(fs, e); /* opcodes operate only on registers */
    freeexp(fs, e);
    e.info = luaK_codeABC(fs, op, 0, r, 0); /* generate opcode */
    e.k = VRELOC;                             /* all those operations are relocatable */
    luaK_fixline(fs, line);
  }

  /*
  ** Emit code for binary expressions that "produce values"
  ** (everything but logical operators 'and'/'or' and comparison
  ** operators).
  ** Expression to produce final result will be encoded in 'e1'.
  */
  private static void finishbinexpval(FuncState fs, expdesc e1, expdesc e2,
                                      int op, int v2, int flip, int line,
                                      int mmop, int event)
  {
    int v1 = luaK_exp2anyreg(fs, e1);
    int pc = luaK_codeABCk(fs, op, 0, v1, v2, 0);
    freeexps(fs, e1, e2);
    e1.info = pc;
    e1.k = VRELOC; /* all those operations are relocatable */
    luaK_fixline(fs, line);
    luaK_codeABCk(fs, mmop, v1, v2, event, flip); /* metamethod */
    luaK_fixline(fs, line);
  }

  /*
  ** Emit code for binary expressions that "produce values" over
  ** two registers.
  */
  private static void codebinexpval(FuncState fs, int opr,
                                    expdesc e1, expdesc e2, int line)
  {
    int op = binopr2op(opr, OPR_ADD, OP_ADD);
    int v2 = luaK_exp2anyreg(fs, e2); /* make sure 'e2' is in a register */
    /* 'e1' must be already in a register or it is a constant */
    assert (VNIL <= e1.k && e1.k <= VKSTR) ||
           e1.k == VNONRELOC || e1.k == VRELOC;
    assert OP_ADD <= op && op <= OP_SHR;
    finishbinexpval(fs, e1, e2, op, v2, 0, line, OP_MMBIN, binopr2TM(opr));
  }

  /*
  ** Code binary operators with immediate operands.
  */
  private static void codebini(FuncState fs, int op,
                               expdesc e1, expdesc e2, int flip, int line,
                               int event)
  {
    int v2 = int2sC((int) e2.ival); /* immediate operand */
    assert e2.k == VKINT;
    finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINI, event);
  }

  /*
  ** Code binary operators with K operand.
  */
  private static void codebinK(FuncState fs, int opr,
                               expdesc e1, expdesc e2, int flip, int line)
  {
    int event = binopr2TM(opr);
    int v2 = e2.info; /* K index */
    int op = binopr2op(opr, OPR_ADD, OP_ADDK);
    finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINK, event);
  }

  /* Try to code a binary operator negating its second operand.
  ** For the metamethod, 2nd operand must keep its original value.
  */
  private static int finishbinexpneg(FuncState fs, expdesc e1, expdesc e2,
                                     int op, int line, int event)
  {
    if (isKint(e2) == 0)
      return 0; /* not an integer constant */
    else
    {
      long i2 = e2.ival;
      if (!(fitsC(i2) != 0 && fitsC(-i2) != 0))
        return 0; /* not in the proper range */
      else
      { /* operating a small integer constant */
        int v2 = (int) i2;
        finishbinexpval(fs, e1, e2, op, int2sC(-v2), 0, line, OP_MMBINI, event);
        /* correct metamethod argument */
        fs.f.code[fs.pc - 1] = SETARG_B(fs.f.code[fs.pc - 1], int2sC(v2));
        return 1; /* successfully coded */
      }
    }
  }

  private static void swapexps(expdesc e1, expdesc e2)
  {
    expdesc temp = new expdesc(e1);
    e1.copyfrom(e2);
    e2.copyfrom(temp); /* swap 'e1' and 'e2' */
  }

  /*
  ** Code binary operators with no constant operand.
  */
  private static void codebinNoK(FuncState fs, int opr,
                                 expdesc e1, expdesc e2, int flip, int line)
  {
    if (flip != 0)
      swapexps(e1, e2);                   /* back to original order */
    codebinexpval(fs, opr, e1, e2, line); /* use standard operators */
  }

  /*
  ** Code arithmetic operators ('+', '-', ...). If second operand is a
  ** constant in the proper range, use variant opcodes with K operands.
  */
  private static void codearith(FuncState fs, int opr,
                                expdesc e1, expdesc e2, int flip, int line)
  {
    if (isnumeral(e2) != 0 && luaK_exp2K(fs, e2) != 0) /* K operand? */
      codebinK(fs, opr, e1, e2, flip, line);
    else /* 'e2' is neither an immediate nor a K operand */
      codebinNoK(fs, opr, e1, e2, flip, line);
  }

  /*
  ** Code commutative operators ('+', '*'). If first operand is a
  ** numeric constant, change order of operands to try to use an
  ** immediate or K operator.
  */
  private static void codecommutative(FuncState fs, int op,
                                      expdesc e1, expdesc e2, int line)
  {
    int flip = 0;
    if (isnumeral(e1) != 0)
    {                   /* is first operand a numeric constant? */
      swapexps(e1, e2); /* change order */
      flip = 1;
    }
    if (op == OPR_ADD && isSCint(e2) != 0) /* immediate operand? */
      codebini(fs, OP_ADDI, e1, e2, flip, line, TM_ADD);
    else
      codearith(fs, op, e1, e2, flip, line);
  }

  /*
  ** Code bitwise operations; they are all commutative, so the function
  ** tries to put an integer constant as the 2nd operand (a K operand).
  */
  private static void codebitwise(FuncState fs, int opr,
                                  expdesc e1, expdesc e2, int line)
  {
    int flip = 0;
    if (e1.k == VKINT)
    {
      swapexps(e1, e2); /* 'e2' will be the constant operand */
      flip = 1;
    }
    if (e2.k == VKINT && luaK_exp2K(fs, e2) != 0) /* K operand? */
      codebinK(fs, opr, e1, e2, flip, line);
    else /* no constants */
      codebinNoK(fs, opr, e1, e2, flip, line);
  }

  /*
  ** Emit code for order comparisons. When using an immediate operand,
  ** 'isfloat' tells whether the original value was a float.
  */
  private static void codeorder(FuncState fs, int opr, expdesc e1, expdesc e2)
  {
    int r1, r2;
    SCImm sc = new SCImm(0, 0);
    int op;
    if (isSCnumber(e2, sc) != 0)
    {
      /* use immediate operand */
      r1 = luaK_exp2anyreg(fs, e1);
      r2 = sc.im;
      op = binopr2op(opr, OPR_LT, OP_LTI);
    }
    else if (isSCnumber(e1, sc) != 0)
    {
      /* transform (A < B) to (B > A) and (A <= B) to (B >= A) */
      r1 = luaK_exp2anyreg(fs, e2);
      r2 = sc.im;
      op = binopr2op(opr, OPR_LT, OP_GTI);
    }
    else
    { /* regular case, compare two registers */
      r1 = luaK_exp2anyreg(fs, e1);
      r2 = luaK_exp2anyreg(fs, e2);
      op = binopr2op(opr, OPR_LT, OP_LT);
    }
    freeexps(fs, e1, e2);
    e1.info = condjump(fs, op, r1, r2, sc.isfloat, 1);
    e1.k = VJMP;
  }

  /*
  ** Emit code for equality comparisons ('==', '~=').
  ** 'e1' was already put as RK by 'luaK_infix'.
  */
  private static void codeeq(FuncState fs, int opr, expdesc e1, expdesc e2)
  {
    int r1, r2;
    SCImm sc = new SCImm(0, 0); /* not needed here, but kept for symmetry */
    int op;
    if (e1.k != VNONRELOC)
    {
      assert e1.k == VK || e1.k == VKINT || e1.k == VKFLT;
      swapexps(e1, e2);
    }
    r1 = luaK_exp2anyreg(fs, e1); /* 1st expression must be in register */
    if (isSCnumber(e2, sc) != 0)
    {
      op = OP_EQI;
      r2 = sc.im; /* immediate operand */
    }
    else if (exp2RK(fs, e2) != 0)
    { /* 2nd expression is constant? */
      op = OP_EQK;
      r2 = e2.info; /* constant index */
    }
    else
    {
      op = OP_EQ; /* will compare two registers */
      r2 = luaK_exp2anyreg(fs, e2);
    }
    freeexps(fs, e1, e2);
    e1.info = condjump(fs, op, r1, r2, sc.isfloat, (opr == OPR_EQ) ? 1 : 0);
    e1.k = VJMP;
  }

  private static final expdesc ef = new expdesc();

  static
  {
    ef.k = VKINT;
    ef.ival = 0;
    ef.nval = 0;
    ef.strval = null;
    ef.info = 0;
    ef.ind.idx = 0;
    ef.ind.t = 0;
    ef.ind.ro = 0;
    ef.ind.keystr = 0;
    ef.var.ridx = 0;
    ef.var.vidx = 0;
    ef.t = NO_JUMP;
    ef.f = NO_JUMP;
  }

  /*
  ** Apply prefix operation 'op' to expression 'e'.
  */
  public static void luaK_prefix(FuncState fs, int opr, expdesc e, int line)
  {
    luaK_dischargevars(fs, e);
    switch (opr)
    {
    case OPR_MINUS:
    case OPR_BNOT: /* use 'ef' as fake 2nd operand */
      if (constfolding(fs, opr + LUA_OPUNM, e, ef) != 0)
        break;
      /* else */ /* FALLTHROUGH */
    case OPR_LEN:
      codeunexpval(fs, unopr2op(opr), e, line);
      break;
    case OPR_NOT:
      codenot(fs, e);
      break;
    default:
      assert false;
    }
  }

  /*
  ** Process 1st operand 'v' of binary operation 'op' before reading
  ** 2nd operand.
  */
  public static void luaK_infix(FuncState fs, int op, expdesc v)
  {
    luaK_dischargevars(fs, v);
    switch (op)
    {
    case OPR_AND:
    {
      luaK_goiftrue(fs, v); /* go ahead only if 'v' is true */
      break;
    }
    case OPR_OR:
    {
      luaK_goiffalse(fs, v); /* go ahead only if 'v' is false */
      break;
    }
    case OPR_CONCAT:
    {
      luaK_exp2nextreg(fs, v); /* operand must be on the stack */
      break;
    }
    case OPR_ADD:
    case OPR_SUB:
    case OPR_MUL:
    case OPR_DIV:
    case OPR_IDIV:
    case OPR_MOD:
    case OPR_POW:
    case OPR_BAND:
    case OPR_BOR:
    case OPR_BXOR:
    case OPR_SHL:
    case OPR_SHR:
    {
      if (isnumeral(v) == 0)
        luaK_exp2anyreg(fs, v);
      /* else keep numeral, which may be folded or used as an immediate
         operand */
      break;
    }
    case OPR_EQ:
    case OPR_NE:
    {
      if (isnumeral(v) == 0)
        exp2RK(fs, v);
      /* else keep numeral, which may be an immediate operand */
      break;
    }
    case OPR_LT:
    case OPR_LE:
    case OPR_GT:
    case OPR_GE:
    {
      SCImm dummy = new SCImm(0, 0);
      if (isSCnumber(v, dummy) == 0)
        luaK_exp2anyreg(fs, v);
      /* else keep numeral, which may be an immediate operand */
      break;
    }
    default:
      assert false;
    }
  }

  /*
  ** Create code for '(e1 .. e2)'.
  ** For '(e1 .. e2.1 .. e2.2)' (which is '(e1 .. (e2.1 .. e2.2))',
  ** because concatenation is right associative), merge both CONCATs.
  */
  private static void codeconcat(FuncState fs, expdesc e1, expdesc e2, int line)
  {
    int ie2 = previousinstruction(fs);
    if (ie2 >= 0 && GET_OPCODE(fs.f.code[ie2]) == OP_CONCAT)
    {                         /* is 'e2' a concatenation? */
      int n = GETARG_B(fs.f.code[ie2]); /* # of elements concatenated in 'e2' */
      assert e1.info + 1 == GETARG_A(fs.f.code[ie2]);
      freeexp(fs, e2);
      fs.f.code[ie2] = SETARG_A(fs.f.code[ie2], e1.info); /* correct first element ('e1') */
      fs.f.code[ie2] = SETARG_B(fs.f.code[ie2], n + 1);      /* will concatenate one more element */
    }
    else
    {                                                /* 'e2' is not a concatenation */
      luaK_codeABC(fs, OP_CONCAT, e1.info, 2, 0); /* new concat opcode */
      freeexp(fs, e2);
      luaK_fixline(fs, line);
    }
  }

  /*
  ** Finalize code for binary operation, after reading 2nd operand.
  */
  public static void luaK_posfix(FuncState fs, int opr,
                                 expdesc e1, expdesc e2, int line)
  {
    luaK_dischargevars(fs, e2);
    if (foldbinop(opr) != 0 && constfolding(fs, opr + LUA_OPADD, e1, e2) != 0)
      return; /* done by folding */
    switch (opr)
    {
    case OPR_AND:
    {
      assert e1.t == NO_JUMP; /* list closed by 'luaK_infix' */
      e2.f = luaK_concat(fs, e2.f, e1.f);
      e1.copyfrom(e2);
      break;
    }
    case OPR_OR:
    {
      assert e1.f == NO_JUMP; /* list closed by 'luaK_infix' */
      e2.t = luaK_concat(fs, e2.t, e1.t);
      e1.copyfrom(e2);
      break;
    }
    case OPR_CONCAT:
    { /* e1 .. e2 */
      luaK_exp2nextreg(fs, e2);
      codeconcat(fs, e1, e2, line);
      break;
    }
    case OPR_ADD:
    case OPR_MUL:
    {
      codecommutative(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SUB:
    {
      if (finishbinexpneg(fs, e1, e2, OP_ADDI, line, TM_SUB) != 0)
        break; /* coded as (r1 + -I) */
      /* ELSE */
    } /* FALLTHROUGH */
    case OPR_DIV:
    case OPR_IDIV:
    case OPR_MOD:
    case OPR_POW:
    {
      codearith(fs, opr, e1, e2, 0, line);
      break;
    }
    case OPR_BAND:
    case OPR_BOR:
    case OPR_BXOR:
    {
      codebitwise(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SHL:
    {
      if (isSCint(e1) != 0)
      {
        swapexps(e1, e2);
        codebini(fs, OP_SHLI, e1, e2, 1, line, TM_SHL); /* I << r2 */
      }
      else if (finishbinexpneg(fs, e1, e2, OP_SHRI, line, TM_SHL) != 0)
      {
        /* coded as (r1 >> -I) */;
      }
      else /* regular case (two registers) */
        codebinexpval(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SHR:
    {
      if (isSCint(e2) != 0)
        codebini(fs, OP_SHRI, e1, e2, 0, line, TM_SHR); /* r1 >> I */
      else                                              /* regular case (two registers) */
        codebinexpval(fs, opr, e1, e2, line);
      break;
    }
    case OPR_EQ:
    case OPR_NE:
    {
      codeeq(fs, opr, e1, e2);
      break;
    }
    case OPR_GT:
    case OPR_GE:
    {
      /* '(a > b)' <=> '(b < a)';  '(a >= b)' <=> '(b <= a)' */
      swapexps(e1, e2);
      opr = (opr - OPR_GT) + OPR_LT;
    } /* FALLTHROUGH */
    case OPR_LT:
    case OPR_LE:
    {
      codeorder(fs, opr, e1, e2);
      break;
    }
    default:
      assert false;
    }
  }

  /*
  ** Change line information associated with current position, by removing
  ** previous info and adding it again with new line.
  */
  public static void luaK_fixline(FuncState fs, int line)
  {
    removelastlineinfo(fs);
    savelineinfo(fs, fs.f, line);
  }

  public static void luaK_settablesize(FuncState fs, int pc, int ra, int asize, int hsize)
  {
    int inst = pc;
    int extra = asize / (MAXARG_vC + 1); /* higher bits of array size */
    int rc = asize % (MAXARG_vC + 1);    /* lower bits of array size */
    int k = (extra > 0) ? 1 : 0;                 /* true iff needs extra argument */
    hsize = (hsize != 0) ? luaO_ceillog2(hsize) + 1 : 0;
    fs.f.code[inst] = CREATE_vABCk(OP_NEWTABLE, ra, hsize, rc, k);
    fs.f.code[inst + 1] = CREATE_Ax(OP_EXTRAARG, extra);
  }

  /*
  ** Emit a SETLIST instruction.
  ** 'base' is register that keeps table;
  ** 'nelems' is #table plus those to be stored now;
  ** 'tostore' is number of values (in registers 'base + 1',...) to add to
  ** table (or LUA_MULTRET to add up to stack top).
  */
  public static void luaK_setlist(FuncState fs, int base, int nelems, int tostore)
  {
    assert tostore != 0;
    if (tostore == LUA_MULTRET)
      tostore = 0;
    if (nelems <= MAXARG_vC)
      luaK_codevABCk(fs, OP_SETLIST, base, tostore, nelems, 0);
    else
    {
      int extra = nelems / (MAXARG_vC + 1);
      nelems %= MAXARG_vC + 1;
      luaK_codevABCk(fs, OP_SETLIST, base, tostore, nelems, 1);
      codeextraarg(fs, extra);
    }
    fs.freereg = base + 1; /* free registers with list values */
  }

  /*
  ** return the final target of a jump (skipping jumps to jumps)
  */
  private static int finaltarget(int[] code, int i)
  {
    int count;
    for (count = 0; count < 100; count++)
    { /* avoid infinite loops */
      int pc = code[i];
      if (GET_OPCODE(pc) != OP_JMP)
        break;
      else
        i += GETARG_sJ(pc) + 1;
    }
    return i;
  }

  /*
  ** Do a final pass over the code of a function, doing small peephole
  ** optimizations and adjustments.
  */
  public static void luaK_finish(FuncState fs)
  {
    int i;
    Proto p = fs.f;
    if ((p.flag & PF_VATAB) != 0)            /* will it use a vararg table? */
      p.flag &= (byte) (~PF_VAHID); /* then it will not use hidden args. */
    for (i = 0; i < fs.pc; i++)
    {
      assert i == 0 || luaP_isOT(p.code[i - 1]) == luaP_isIT(p.code[i]);
      switch (GET_OPCODE(p.code[i]))
      {
      case OP_RETURN0:
      case OP_RETURN1:
      {
        if (!(fs.needclose != 0 || (p.flag & PF_VAHID) != 0))
          break; /* no extra work */
        /* else use OP_RETURN to do the extra work */
        p.code[i] = SET_OPCODE(p.code[i], OP_RETURN);
      } /* FALLTHROUGH */
      case OP_RETURN:
      case OP_TAILCALL:
      {
        if (fs.needclose != 0)
          p.code[i] = SETARG_k(p.code[i], 1);                /* signal that it needs to close */
        if ((p.flag & PF_VAHID) != 0)            /* does it use hidden arguments? */
          p.code[i] = SETARG_C(p.code[i], p.numparams + 1); /* signal that */
        break;
      }
      case OP_GETVARG:
      {
        if ((p.flag & PF_VATAB) != 0)         /* function has a vararg table? */
          p.code[i] = SET_OPCODE(p.code[i], OP_GETTABLE); /* must get vararg there */
        break;
      }
      case OP_VARARG:
      {
        if ((p.flag & PF_VATAB) != 0) /* function has a vararg table? */
          p.code[i] = SETARG_k(p.code[i], 1);     /* must get vararg there */
        break;
      }
      case OP_JMP:
      { /* to optimize jumps to jumps */
        int target = finaltarget(p.code, i);
        fixjump(fs, i, target); /* jump directly to final target */
        break;
      }
      default:
        break;
      }
    }
  }
}
