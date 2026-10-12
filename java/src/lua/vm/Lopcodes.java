package lua.vm;

import lua.bytes.ByteView;
import lua.bytes.Lt;

public final class Lopcodes
{
  private Lopcodes()
  {
  }

  /*===========================================================================
    We assume that instructions are unsigned 32-bit integers.
    All instructions have an opcode in the first 7 bits.
    Instructions can have the following formats:

          3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0
          1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
  iABC          C(8)     |      B(8)     |k|     A(8)      |   Op(7)     |
  ivABC         vC(10)     |     vB(6)   |k|     A(8)      |   Op(7)     |
  iABx                Bx(17)               |     A(8)      |   Op(7)     |
  iAsBx              sBx (signed)(17)      |     A(8)      |   Op(7)     |
  iAx                           Ax(25)                     |   Op(7)     |
  isJ                           sJ (signed)(25)            |   Op(7)     |

    ('v' stands for "variant", 's' for "signed", 'x' for "extended".)
    A signed argument is represented in excess K: The represented value is
    the written unsigned value minus K, where K is half (rounded down) the
    maximum value for the corresponding unsigned argument.
  ===========================================================================*/

  /* enum OpMode */
  public static final int iABC = 0;
  public static final int ivABC = 1;
  public static final int iABx = 2;
  public static final int iAsBx = 3;
  public static final int iAx = 4;
  public static final int isJ = 5;

  /*
  ** size and position of opcode arguments.
  */
  public static final int SIZE_C = 8;
  public static final int SIZE_vC = 10;
  public static final int SIZE_B = 8;
  public static final int SIZE_vB = 6;
  public static final int SIZE_Bx = SIZE_C + SIZE_B + 1;
  public static final int SIZE_A = 8;
  public static final int SIZE_Ax = SIZE_Bx + SIZE_A;
  public static final int SIZE_sJ = SIZE_Bx + SIZE_A;

  public static final int SIZE_OP = 7;

  public static final int POS_OP = 0;

  public static final int POS_A = POS_OP + SIZE_OP;
  public static final int POS_k = POS_A + SIZE_A;
  public static final int POS_B = POS_k + 1;
  public static final int POS_vB = POS_k + 1;
  public static final int POS_C = POS_B + SIZE_B;
  public static final int POS_vC = POS_vB + SIZE_vB;

  public static final int POS_Bx = POS_k;

  public static final int POS_Ax = POS_A;

  public static final int POS_sJ = POS_A;

  /*
  ** limits for opcode arguments.
  ** we use (signed) 'int' to manipulate most arguments,
  ** so they must fit in ints.
  */
  public static final int MAXARG_Bx = (1 << SIZE_Bx) - 1;

  public static final int OFFSET_sBx = MAXARG_Bx >> 1; /* 'sBx' is signed */

  public static final int MAXARG_Ax = (1 << SIZE_Ax) - 1;

  public static final int MAXARG_sJ = (1 << SIZE_sJ) - 1;

  public static final int OFFSET_sJ = MAXARG_sJ >> 1;

  public static final int MAXARG_A = (1 << SIZE_A) - 1;
  public static final int MAXARG_B = (1 << SIZE_B) - 1;
  public static final int MAXARG_vB = (1 << SIZE_vB) - 1;
  public static final int MAXARG_C = (1 << SIZE_C) - 1;
  public static final int MAXARG_vC = (1 << SIZE_vC) - 1;
  public static final int OFFSET_sC = MAXARG_C >> 1;

  public static int int2sC(int i)
  {
    return i + OFFSET_sC;
  }

  public static int sC2int(int i)
  {
    return i - OFFSET_sC;
  }

  /* creates a mask with 'n' 1 bits at position 'p' */
  public static int MASK1(int n, int p)
  {
    return (~((~0) << n)) << p;
  }

  /* creates a mask with 'n' 0 bits at position 'p' */
  public static int MASK0(int n, int p)
  {
    return ~MASK1(n, p);
  }

  /*
  ** Maximum size for the stack of a Lua function. It must fit in 8 bits.
  ** The highest valid register is one less than this value.
  */
  public static final int MAX_FSTACK = MAXARG_A;

  /*
  ** Invalid register (one more than last valid register).
  */
  public static final int NO_REG = MAX_FSTACK;

  /*
  ** Grep "ORDER OP" if you change these enums. Opcodes marked with a (*)
  ** has extra descriptions in the notes after the enumeration.
  */

  /* enum OpCode */
  /*----------------------------------------------------------------------
    name    args  description
  ------------------------------------------------------------------------*/
  public static final int OP_MOVE = 0; /*  A B  R[A] := R[B]          */
  public static final int OP_LOADI = 1; /*  A sBx  R[A] := sBx          */
  public static final int OP_LOADF = 2; /*  A sBx  R[A] := (lua_Number)sBx        */
  public static final int OP_LOADK = 3; /*  A Bx  R[A] := K[Bx]          */
  public static final int OP_LOADKX = 4; /*  A  R[A] := K[extra arg]        */
  public static final int OP_LOADFALSE = 5; /*  A  R[A] := false          */
  public static final int OP_LFALSESKIP = 6; /*A  R[A] := false; pc++        */
  public static final int OP_LOADTRUE = 7; /*  A  R[A] := true          */
  public static final int OP_LOADNIL = 8; /*  A B  R[A], R[A+1], ..., R[A+B] := nil    */
  public static final int OP_GETUPVAL = 9; /*  A B  R[A] := UpValue[B]        */
  public static final int OP_SETUPVAL = 10; /*  A B  UpValue[B] := R[A]        */
  public static final int OP_GETTABUP = 11; /*  A B C  R[A] := UpValue[B][K[C]:shortstring]    */
  public static final int OP_GETTABLE = 12; /*  A B C  R[A] := R[B][R[C]]        */
  public static final int OP_GETI = 13; /*  A B C  R[A] := R[B][C]          */
  public static final int OP_GETFIELD = 14; /*  A B C  R[A] := R[B][K[C]:shortstring]      */
  public static final int OP_SETTABUP = 15; /*  A B C  UpValue[A][K[B]:shortstring] := RK(C)    */
  public static final int OP_SETTABLE = 16; /*  A B C  R[A][R[B]] := RK(C)        */
  public static final int OP_SETI = 17; /*  A B C  R[A][B] := RK(C)        */
  public static final int OP_SETFIELD = 18; /*  A B C  R[A][K[B]:shortstring] := RK(C)      */
  public static final int OP_NEWTABLE = 19; /*  A vB vC k  R[A] := {}        */
  public static final int OP_SELF = 20; /*  A B C  R[A+1] := R[B]; R[A] := R[B][K[C]:shortstring]  */
  public static final int OP_ADDI = 21; /*  A B sC  R[A] := R[B] + sC        */
  public static final int OP_ADDK = 22; /*  A B C  R[A] := R[B] + K[C]:number      */
  public static final int OP_SUBK = 23; /*  A B C  R[A] := R[B] - K[C]:number      */
  public static final int OP_MULK = 24; /*  A B C  R[A] := R[B] * K[C]:number      */
  public static final int OP_MODK = 25; /*  A B C  R[A] := R[B] % K[C]:number      */
  public static final int OP_POWK = 26; /*  A B C  R[A] := R[B] ^ K[C]:number      */
  public static final int OP_DIVK = 27; /*  A B C  R[A] := R[B] / K[C]:number      */
  public static final int OP_IDIVK = 28; /*  A B C  R[A] := R[B] // K[C]:number      */
  public static final int OP_BANDK = 29; /*  A B C  R[A] := R[B] & K[C]:integer      */
  public static final int OP_BORK = 30; /*  A B C  R[A] := R[B] | K[C]:integer      */
  public static final int OP_BXORK = 31; /*  A B C  R[A] := R[B] ~ K[C]:integer      */
  public static final int OP_SHLI = 32; /*  A B sC  R[A] := sC << R[B]        */
  public static final int OP_SHRI = 33; /*  A B sC  R[A] := R[B] >> sC        */
  public static final int OP_ADD = 34; /*  A B C  R[A] := R[B] + R[C]        */
  public static final int OP_SUB = 35; /*  A B C  R[A] := R[B] - R[C]        */
  public static final int OP_MUL = 36; /*  A B C  R[A] := R[B] * R[C]        */
  public static final int OP_MOD = 37; /*  A B C  R[A] := R[B] % R[C]        */
  public static final int OP_POW = 38; /*  A B C  R[A] := R[B] ^ R[C]        */
  public static final int OP_DIV = 39; /*  A B C  R[A] := R[B] / R[C]        */
  public static final int OP_IDIV = 40; /*  A B C  R[A] := R[B] // R[C]        */
  public static final int OP_BAND = 41; /*  A B C  R[A] := R[B] & R[C]        */
  public static final int OP_BOR = 42; /*  A B C  R[A] := R[B] | R[C]        */
  public static final int OP_BXOR = 43; /*  A B C  R[A] := R[B] ~ R[C]        */
  public static final int OP_SHL = 44; /*  A B C  R[A] := R[B] << R[C]        */
  public static final int OP_SHR = 45; /*  A B C  R[A] := R[B] >> R[C]        */
  public static final int OP_MMBIN = 46; /*  A B C  call C metamethod over R[A] and R[B]    */
  public static final int OP_MMBINI = 47; /*  A sB C k  call C metamethod over R[A] and sB  */
  public static final int OP_MMBINK = 48; /*  A B C k    call C metamethod over R[A] and K[B]  */
  public static final int OP_UNM = 49; /*  A B  R[A] := -R[B]          */
  public static final int OP_BNOT = 50; /*  A B  R[A] := ~R[B]          */
  public static final int OP_NOT = 51; /*  A B  R[A] := not R[B]        */
  public static final int OP_LEN = 52; /*  A B  R[A] := #R[B] (length operator)      */
  public static final int OP_CONCAT = 53; /*  A B  R[A] := R[A].. ... ..R[A + B - 1]    */
  public static final int OP_CLOSE = 54; /*  A  close all upvalues >= R[A]      */
  public static final int OP_TBC = 55; /*  A  mark variable A "to be closed"      */
  public static final int OP_JMP = 56; /*  sJ  pc += sJ          */
  public static final int OP_EQ = 57; /*  A B k  if ((R[A] == R[B]) ~= k) then pc++    */
  public static final int OP_LT = 58; /*  A B k  if ((R[A] <  R[B]) ~= k) then pc++    */
  public static final int OP_LE = 59; /*  A B k  if ((R[A] <= R[B]) ~= k) then pc++    */
  public static final int OP_EQK = 60; /*  A B k  if ((R[A] == K[B]) ~= k) then pc++    */
  public static final int OP_EQI = 61; /*  A sB k  if ((R[A] == sB) ~= k) then pc++    */
  public static final int OP_LTI = 62; /*  A sB k  if ((R[A] < sB) ~= k) then pc++      */
  public static final int OP_LEI = 63; /*  A sB k  if ((R[A] <= sB) ~= k) then pc++    */
  public static final int OP_GTI = 64; /*  A sB k  if ((R[A] > sB) ~= k) then pc++      */
  public static final int OP_GEI = 65; /*  A sB k  if ((R[A] >= sB) ~= k) then pc++    */
  public static final int OP_TEST = 66; /*  A k  if (not R[A] == k) then pc++      */
  public static final int OP_TESTSET = 67; /*  A B k  if (not R[B] == k) then pc++ else R[A] := R[B]  */
  public static final int OP_CALL = 68; /*  A B C  R[A], ... ,R[A+C-2] := R[A](R[A+1], ... ,R[A+B-1]) */
  public static final int OP_TAILCALL = 69; /*  A B C k  return R[A](R[A+1], ... ,R[A+B-1])    */
  public static final int OP_RETURN = 70; /*  A B C k  return R[A], ... ,R[A+B-2]      */
  public static final int OP_RETURN0 = 71; /*    return            */
  public static final int OP_RETURN1 = 72; /*  A  return R[A]          */
  public static final int OP_FORLOOP = 73; /*  A Bx  update counters; if loop continues then pc-=Bx; */
  public static final int OP_FORPREP = 74; /*  A Bx  <check values and prepare counters>;
                           if not to run then pc+=Bx+1;      */
  public static final int OP_TFORPREP = 75; /*  A Bx  create upvalue for R[A + 3]; pc+=Bx    */
  public static final int OP_TFORCALL = 76; /*  A C  R[A+4], ... ,R[A+3+C] := R[A](R[A+1], R[A+2]);  */
  public static final int OP_TFORLOOP = 77; /*  A Bx  if R[A+2] ~= nil then { R[A]=R[A+2]; pc -= Bx }  */
  public static final int OP_SETLIST = 78; /*  A vB vC k  R[A][vC+i] := R[A+i], 1 <= i <= vB  */
  public static final int OP_CLOSURE = 79; /*  A Bx  R[A] := closure(KPROTO[Bx])      */
  public static final int OP_VARARG = 80; /*  A B C k  R[A], ..., R[A+C-2] = varargs        */
  public static final int OP_GETVARG = 81; /* A B C  R[A] := R[B][R[C]], R[B] is vararg parameter    */
  public static final int OP_ERRNNIL = 82; /*  A Bx  raise error if R[A] ~= nil (K[Bx - 1] is global name)*/
  public static final int OP_VARARGPREP = 83; /*   (adjust varargs)        */
  public static final int OP_EXTRAARG = 84; /*  Ax  extra (larger) argument for previous opcode  */

  public static final int NUM_OPCODES = OP_EXTRAARG + 1;

  public static int GET_OPCODE(int i)
  {
    return (i >>> POS_OP) & MASK1(SIZE_OP, 0);
  }

  public static int getOpMode(int m)
  {
    return luaP_opmodes[m] & 7;
  }

  public static boolean checkopm(int i, int m)
  {
    return getOpMode(GET_OPCODE(i)) == m;
  }

  public static int getarg(int i, int pos, int size)
  {
    return (i >>> pos) & MASK1(size, 0);
  }

  public static int GETARG_A(int i)
  {
    return getarg(i, POS_A, SIZE_A);
  }

  public static int GETARG_B(int i)
  {
    assert checkopm(i, iABC);
    return getarg(i, POS_B, SIZE_B);
  }

  public static int GETARG_vB(int i)
  {
    assert checkopm(i, ivABC);
    return getarg(i, POS_vB, SIZE_vB);
  }

  public static int GETARG_sB(int i)
  {
    return sC2int(GETARG_B(i));
  }

  public static int GETARG_C(int i)
  {
    assert checkopm(i, iABC);
    return getarg(i, POS_C, SIZE_C);
  }

  public static int GETARG_vC(int i)
  {
    assert checkopm(i, ivABC);
    return getarg(i, POS_vC, SIZE_vC);
  }

  public static int GETARG_sC(int i)
  {
    return sC2int(GETARG_C(i));
  }

  public static int TESTARG_k(int i)
  {
    return i & (1 << POS_k);
  }

  public static int GETARG_k(int i)
  {
    return getarg(i, POS_k, 1);
  }

  public static int GETARG_Bx(int i)
  {
    assert checkopm(i, iABx);
    return getarg(i, POS_Bx, SIZE_Bx);
  }

  public static int GETARG_Ax(int i)
  {
    assert checkopm(i, iAx);
    return getarg(i, POS_Ax, SIZE_Ax);
  }

  public static int GETARG_sBx(int i)
  {
    assert checkopm(i, iAsBx);
    return getarg(i, POS_Bx, SIZE_Bx) - OFFSET_sBx;
  }

  public static int GETARG_sJ(int i)
  {
    assert checkopm(i, isJ);
    return getarg(i, POS_sJ, SIZE_sJ) - OFFSET_sJ;
  }

  public static int SET_OPCODE(int i, int o)
  {
    return (i & MASK0(SIZE_OP, POS_OP)) | ((o << POS_OP) & MASK1(SIZE_OP, POS_OP));
  }

  public static int setarg(int i, int v, int pos, int size)
  {
    return (i & MASK0(size, pos)) | ((v << pos) & MASK1(size, pos));
  }

  public static int SETARG_A(int i, int v)
  {
    return setarg(i, v, POS_A, SIZE_A);
  }

  public static int SETARG_B(int i, int v)
  {
    return setarg(i, v, POS_B, SIZE_B);
  }

  public static int SETARG_C(int i, int v)
  {
    return setarg(i, v, POS_C, SIZE_C);
  }

  public static int SETARG_k(int i, int v)
  {
    return setarg(i, v, POS_k, 1);
  }

  public static int SETARG_Bx(int i, int v)
  {
    return setarg(i, v, POS_Bx, SIZE_Bx);
  }

  public static int SETARG_sJ(int i, int j)
  {
    return setarg(i, j + OFFSET_sJ, POS_sJ, SIZE_sJ);
  }

  public static int CREATE_ABCk(int o, int a, int b, int c, int k)
  {
    return (o << POS_OP) | (a << POS_A) | (b << POS_B) | (c << POS_C) | (k << POS_k);
  }

  public static int CREATE_vABCk(int o, int a, int b, int c, int k)
  {
    return (o << POS_OP) | (a << POS_A) | (b << POS_vB) | (c << POS_vC) | (k << POS_k);
  }

  public static int CREATE_ABx(int o, int a, int bc)
  {
    return (o << POS_OP) | (a << POS_A) | (bc << POS_Bx);
  }

  public static int CREATE_Ax(int o, int a)
  {
    return (o << POS_OP) | (a << POS_Ax);
  }

  public static int CREATE_sJ(int o, int j, int k)
  {
    return (o << POS_OP) | (j << POS_sJ) | (k << POS_k);
  }

  /*
  ** masks for instruction properties. The format is:
  ** bits 0-2: op mode
  ** bit 3: instruction set register A
  ** bit 4: operator is a test (next instruction must be a jump)
  ** bit 5: instruction uses 'L->top' set by previous instruction (when B == 0)
  ** bit 6: instruction sets 'L->top' for next instruction (when C == 0)
  ** bit 7: instruction is an MM instruction (call a metamethod)
  */

  public static int testAMode(int m)
  {
    return luaP_opmodes[m] & (1 << 3);
  }

  public static int testTMode(int m)
  {
    return luaP_opmodes[m] & (1 << 4);
  }

  public static int testITMode(int m)
  {
    return luaP_opmodes[m] & (1 << 5);
  }

  public static int testOTMode(int m)
  {
    return luaP_opmodes[m] & (1 << 6);
  }

  public static int testMMMode(int m)
  {
    return luaP_opmodes[m] & (1 << 7);
  }

  private static int opmode(int mm, int ot, int it, int t, int a, int m)
  {
    return (mm << 7) | (ot << 6) | (it << 5) | (t << 4) | (a << 3) | m;
  }

  /* ORDER OP */
  public static final int[] luaP_opmodes = {
      /*       MM OT IT T  A  mode       opcode  */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_MOVE */
      opmode(0, 0, 0, 0, 1, iAsBx), /* OP_LOADI */
      opmode(0, 0, 0, 0, 1, iAsBx), /* OP_LOADF */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_LOADK */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_LOADKX */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_LOADFALSE */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_LFALSESKIP */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_LOADTRUE */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_LOADNIL */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETUPVAL */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_SETUPVAL */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETTABUP */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETTABLE */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETI */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETFIELD */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_SETTABUP */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_SETTABLE */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_SETI */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_SETFIELD */
      opmode(0, 0, 0, 0, 1, ivABC), /* OP_NEWTABLE */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SELF */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_ADDI */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_ADDK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SUBK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_MULK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_MODK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_POWK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_DIVK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_IDIVK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BANDK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BORK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BXORK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SHLI */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SHRI */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_ADD */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SUB */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_MUL */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_MOD */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_POW */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_DIV */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_IDIV */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BAND */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BOR */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BXOR */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SHL */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_SHR */
      opmode(1, 0, 0, 0, 0, iABC), /* OP_MMBIN */
      opmode(1, 0, 0, 0, 0, iABC), /* OP_MMBINI */
      opmode(1, 0, 0, 0, 0, iABC), /* OP_MMBINK */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_UNM */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_BNOT */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_NOT */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_LEN */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_CONCAT */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_CLOSE */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_TBC */
      opmode(0, 0, 0, 0, 0, isJ), /* OP_JMP */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_EQ */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_LT */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_LE */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_EQK */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_EQI */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_LTI */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_LEI */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_GTI */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_GEI */
      opmode(0, 0, 0, 1, 0, iABC), /* OP_TEST */
      opmode(0, 0, 0, 1, 1, iABC), /* OP_TESTSET */
      opmode(0, 1, 1, 0, 1, iABC), /* OP_CALL */
      opmode(0, 1, 1, 0, 1, iABC), /* OP_TAILCALL */
      opmode(0, 0, 1, 0, 0, iABC), /* OP_RETURN */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_RETURN0 */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_RETURN1 */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_FORLOOP */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_FORPREP */
      opmode(0, 0, 0, 0, 0, iABx), /* OP_TFORPREP */
      opmode(0, 0, 0, 0, 0, iABC), /* OP_TFORCALL */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_TFORLOOP */
      opmode(0, 0, 1, 0, 0, ivABC), /* OP_SETLIST */
      opmode(0, 0, 0, 0, 1, iABx), /* OP_CLOSURE */
      opmode(0, 1, 0, 0, 1, iABC), /* OP_VARARG */
      opmode(0, 0, 0, 0, 1, iABC), /* OP_GETVARG */
      opmode(0, 0, 0, 0, 0, iABx), /* OP_ERRNNIL */
      opmode(0, 0, 1, 0, 0, iABC), /* OP_VARARGPREP */
      opmode(0, 0, 0, 0, 0, iAx), /* OP_EXTRAARG */
  };

  public static int luaP_isOT(int i)
  {
    int op = GET_OPCODE(i);
    switch (op)
    {
    case OP_TAILCALL:
      return 1;
    default:
      return (testOTMode(op) != 0 && GETARG_C(i) == 0) ? 1 : 0;
    }
  }

  public static int luaP_isIT(int i)
  {
    int op = GET_OPCODE(i);
    switch (op)
    {
    case OP_SETLIST:
      return (testITMode(GET_OPCODE(i)) != 0 && GETARG_vB(i) == 0) ? 1 : 0;
    default:
      return (testITMode(GET_OPCODE(i)) != 0 && GETARG_B(i) == 0) ? 1 : 0;
    }
  }

  /* ORDER OP */
  public static final ByteView[] opnames = {
      Lt.MOVE,
      Lt.LOADI,
      Lt.LOADF,
      Lt.LOADK,
      Lt.LOADKX,
      Lt.LOADFALSE,
      Lt.LFALSESKIP,
      Lt.LOADTRUE,
      Lt.LOADNIL,
      Lt.GETUPVAL,
      Lt.SETUPVAL,
      Lt.GETTABUP,
      Lt.GETTABLE,
      Lt.GETI,
      Lt.GETFIELD,
      Lt.SETTABUP,
      Lt.SETTABLE,
      Lt.SETI,
      Lt.SETFIELD,
      Lt.NEWTABLE,
      Lt.SELF,
      Lt.ADDI,
      Lt.ADDK,
      Lt.SUBK,
      Lt.MULK,
      Lt.MODK,
      Lt.POWK,
      Lt.DIVK,
      Lt.IDIVK,
      Lt.BANDK,
      Lt.BORK,
      Lt.BXORK,
      Lt.SHLI,
      Lt.SHRI,
      Lt.ADD,
      Lt.SUB,
      Lt.MUL,
      Lt.MOD,
      Lt.POW,
      Lt.DIV,
      Lt.IDIV,
      Lt.BAND,
      Lt.BOR,
      Lt.BXOR,
      Lt.SHL,
      Lt.SHR,
      Lt.MMBIN,
      Lt.MMBINI,
      Lt.MMBINK,
      Lt.UNM,
      Lt.BNOT,
      Lt.NOT_kw,
      Lt.LEN,
      Lt.CONCAT,
      Lt.CLOSE_kw,
      Lt.TBC,
      Lt.JMP,
      Lt.EQ,
      Lt.LT,
      Lt.LE,
      Lt.EQK,
      Lt.EQI,
      Lt.LTI,
      Lt.LEI,
      Lt.GTI,
      Lt.GEI,
      Lt.TEST,
      Lt.TESTSET,
      Lt.CALL,
      Lt.TAILCALL,
      Lt.RETURN_kw,
      Lt.RETURN0,
      Lt.RETURN1,
      Lt.FORLOOP,
      Lt.FORPREP,
      Lt.TFORPREP,
      Lt.TFORCALL,
      Lt.TFORLOOP,
      Lt.SETLIST,
      Lt.CLOSURE,
      Lt.VARARG,
      Lt.GETVARG,
      Lt.ERRNNIL,
      Lt.VARARGPREP,
      Lt.EXTRAARG,
  };
}
