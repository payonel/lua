/*
** $Id: lopcodes.h $
** Opcodes for Lua virtual machine
** See Copyright Notice in lua.h
*/

#pragma once

#include "llimits.h"
#include "lobject.h"

namespace lua::vm
{

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

/* basic instruction formats */
enum OpMode
{
  iABC,
  ivABC,
  iABx,
  iAsBx,
  iAx,
  isJ
};

/*
** size and position of opcode arguments.
*/
constexpr int SIZE_C = 8;
constexpr int SIZE_vC = 10;
constexpr int SIZE_B = 8;
constexpr int SIZE_vB = 6;
constexpr int SIZE_Bx = SIZE_C + SIZE_B + 1;
constexpr int SIZE_A = 8;
constexpr int SIZE_Ax = SIZE_Bx + SIZE_A;
constexpr int SIZE_sJ = SIZE_Bx + SIZE_A;

constexpr int SIZE_OP = 7;

constexpr int POS_OP = 0;

constexpr int POS_A = POS_OP + SIZE_OP;
constexpr int POS_k = POS_A + SIZE_A;
constexpr int POS_B = POS_k + 1;
constexpr int POS_vB = POS_k + 1;
constexpr int POS_C = POS_B + SIZE_B;
constexpr int POS_vC = POS_vB + SIZE_vB;

constexpr int POS_Bx = POS_k;

constexpr int POS_Ax = POS_A;

constexpr int POS_sJ = POS_A;

/*
** limits for opcode arguments.
** we use (signed) 'int' to manipulate most arguments,
** so they must fit in ints.
*/

static_assert(SIZE_Ax < (int)(sizeof(int) * CHAR_BIT) - 1 &&
                  SIZE_sJ < (int)(sizeof(int) * CHAR_BIT) - 1,
              "int too narrow for instruction arguments");

constexpr int MAXARG_Bx = (1 << SIZE_Bx) - 1;

constexpr int OFFSET_sBx = MAXARG_Bx >> 1; /* 'sBx' is signed */

constexpr int MAXARG_Ax = (1 << SIZE_Ax) - 1;

constexpr int MAXARG_sJ = (1 << SIZE_sJ) - 1;

constexpr int OFFSET_sJ = MAXARG_sJ >> 1;

constexpr int MAXARG_A = (1 << SIZE_A) - 1;
constexpr int MAXARG_B = (1 << SIZE_B) - 1;
constexpr int MAXARG_vB = (1 << SIZE_vB) - 1;
constexpr int MAXARG_C = (1 << SIZE_C) - 1;
constexpr int MAXARG_vC = (1 << SIZE_vC) - 1;
constexpr int OFFSET_sC = MAXARG_C >> 1;

constexpr int int2sC(int i)
{
  return i + OFFSET_sC;
}
constexpr int sC2int(int i)
{
  return i - OFFSET_sC;
}

/* creates a mask with 'n' 1 bits at position 'p' */
constexpr Instruction MASK1(int n, int p)
{
  return (~((~(Instruction)0) << n)) << p;
}

/* creates a mask with 'n' 0 bits at position 'p' */
constexpr Instruction MASK0(int n, int p)
{
  return ~MASK1(n, p);
}

/*
** the following macros help to manipulate instructions
*/

/*
** Maximum size for the stack of a Lua function. It must fit in 8 bits.
** The highest valid register is one less than this value.
*/
constexpr int MAX_FSTACK = MAXARG_A;

/*
** Invalid register (one more than last valid register).
*/
constexpr int NO_REG = MAX_FSTACK;

/*
** R[x] - register
** K[x] - constant (in constant table)
** RK(x) == if k(i) then K[x] else R[x]
*/

/*
** Grep "ORDER OP" if you change this enum.
** See "Notes" below for more information about some instructions.
*/

typedef enum
{
  /*----------------------------------------------------------------------
    name    args  description
  ------------------------------------------------------------------------*/
  OP_MOVE,       /*  A B  R[A] := R[B]          */
  OP_LOADI,      /*  A sBx  R[A] := sBx          */
  OP_LOADF,      /*  A sBx  R[A] := (lua_Number)sBx        */
  OP_LOADK,      /*  A Bx  R[A] := K[Bx]          */
  OP_LOADKX,     /*  A  R[A] := K[extra arg]        */
  OP_LOADFALSE,  /*  A  R[A] := false          */
  OP_LFALSESKIP, /*A  R[A] := false; pc++        */
  OP_LOADTRUE,   /*  A  R[A] := true          */
  OP_LOADNIL,    /*  A B  R[A], R[A+1], ..., R[A+B] := nil    */
  OP_GETUPVAL,   /*  A B  R[A] := UpValue[B]        */
  OP_SETUPVAL,   /*  A B  UpValue[B] := R[A]        */

  OP_GETTABUP, /*  A B C  R[A] := UpValue[B][K[C]:shortstring]    */
  OP_GETTABLE, /*  A B C  R[A] := R[B][R[C]]        */
  OP_GETI,     /*  A B C  R[A] := R[B][C]          */
  OP_GETFIELD, /*  A B C  R[A] := R[B][K[C]:shortstring]      */

  OP_SETTABUP, /*  A B C  UpValue[A][K[B]:shortstring] := RK(C)    */
  OP_SETTABLE, /*  A B C  R[A][R[B]] := RK(C)        */
  OP_SETI,     /*  A B C  R[A][B] := RK(C)        */
  OP_SETFIELD, /*  A B C  R[A][K[B]:shortstring] := RK(C)      */

  OP_NEWTABLE, /*  A vB vC k  R[A] := {}        */

  OP_SELF, /*  A B C  R[A+1] := R[B]; R[A] := R[B][K[C]:shortstring]  */

  OP_ADDI, /*  A B sC  R[A] := R[B] + sC        */

  OP_ADDK,  /*  A B C  R[A] := R[B] + K[C]:number      */
  OP_SUBK,  /*  A B C  R[A] := R[B] - K[C]:number      */
  OP_MULK,  /*  A B C  R[A] := R[B] * K[C]:number      */
  OP_MODK,  /*  A B C  R[A] := R[B] % K[C]:number      */
  OP_POWK,  /*  A B C  R[A] := R[B] ^ K[C]:number      */
  OP_DIVK,  /*  A B C  R[A] := R[B] / K[C]:number      */
  OP_IDIVK, /*  A B C  R[A] := R[B] // K[C]:number      */

  OP_BANDK, /*  A B C  R[A] := R[B] & K[C]:integer      */
  OP_BORK,  /*  A B C  R[A] := R[B] | K[C]:integer      */
  OP_BXORK, /*  A B C  R[A] := R[B] ~ K[C]:integer      */

  OP_SHLI, /*  A B sC  R[A] := sC << R[B]        */
  OP_SHRI, /*  A B sC  R[A] := R[B] >> sC        */

  OP_ADD,  /*  A B C  R[A] := R[B] + R[C]        */
  OP_SUB,  /*  A B C  R[A] := R[B] - R[C]        */
  OP_MUL,  /*  A B C  R[A] := R[B] * R[C]        */
  OP_MOD,  /*  A B C  R[A] := R[B] % R[C]        */
  OP_POW,  /*  A B C  R[A] := R[B] ^ R[C]        */
  OP_DIV,  /*  A B C  R[A] := R[B] / R[C]        */
  OP_IDIV, /*  A B C  R[A] := R[B] // R[C]        */

  OP_BAND, /*  A B C  R[A] := R[B] & R[C]        */
  OP_BOR,  /*  A B C  R[A] := R[B] | R[C]        */
  OP_BXOR, /*  A B C  R[A] := R[B] ~ R[C]        */
  OP_SHL,  /*  A B C  R[A] := R[B] << R[C]        */
  OP_SHR,  /*  A B C  R[A] := R[B] >> R[C]        */

  OP_MMBIN,  /*  A B C  call C metamethod over R[A] and R[B]    */
  OP_MMBINI, /*  A sB C k  call C metamethod over R[A] and sB  */
  OP_MMBINK, /*  A B C k    call C metamethod over R[A] and K[B]  */

  OP_UNM,  /*  A B  R[A] := -R[B]          */
  OP_BNOT, /*  A B  R[A] := ~R[B]          */
  OP_NOT,  /*  A B  R[A] := not R[B]        */
  OP_LEN,  /*  A B  R[A] := #R[B] (length operator)      */

  OP_CONCAT, /*  A B  R[A] := R[A].. ... ..R[A + B - 1]    */

  OP_CLOSE, /*  A  close all upvalues >= R[A]      */
  OP_TBC,   /*  A  mark variable A "to be closed"      */
  OP_JMP,   /*  sJ  pc += sJ          */
  OP_EQ,    /*  A B k  if ((R[A] == R[B]) ~= k) then pc++    */
  OP_LT,    /*  A B k  if ((R[A] <  R[B]) ~= k) then pc++    */
  OP_LE,    /*  A B k  if ((R[A] <= R[B]) ~= k) then pc++    */

  OP_EQK, /*  A B k  if ((R[A] == K[B]) ~= k) then pc++    */
  OP_EQI, /*  A sB k  if ((R[A] == sB) ~= k) then pc++    */
  OP_LTI, /*  A sB k  if ((R[A] < sB) ~= k) then pc++      */
  OP_LEI, /*  A sB k  if ((R[A] <= sB) ~= k) then pc++    */
  OP_GTI, /*  A sB k  if ((R[A] > sB) ~= k) then pc++      */
  OP_GEI, /*  A sB k  if ((R[A] >= sB) ~= k) then pc++    */

  OP_TEST,    /*  A k  if (not R[A] == k) then pc++      */
  OP_TESTSET, /*  A B k  if (not R[B] == k) then pc++ else R[A] := R[B]  */

  OP_CALL,     /*  A B C  R[A], ... ,R[A+C-2] := R[A](R[A+1], ... ,R[A+B-1]) */
  OP_TAILCALL, /*  A B C k  return R[A](R[A+1], ... ,R[A+B-1])    */

  OP_RETURN,  /*  A B C k  return R[A], ... ,R[A+B-2]      */
  OP_RETURN0, /*    return            */
  OP_RETURN1, /*  A  return R[A]          */

  OP_FORLOOP, /*  A Bx  update counters; if loop continues then pc-=Bx; */
  OP_FORPREP, /*  A Bx  <check values and prepare counters>;
                           if not to run then pc+=Bx+1;      */

  OP_TFORPREP, /*  A Bx  create upvalue for R[A + 3]; pc+=Bx    */
  OP_TFORCALL, /*  A C  R[A+4], ... ,R[A+3+C] := R[A](R[A+1], R[A+2]);  */
  OP_TFORLOOP, /*  A Bx  if R[A+2] ~= nil then { R[A]=R[A+2]; pc -= Bx }  */

  OP_SETLIST, /*  A vB vC k  R[A][vC+i] := R[A+i], 1 <= i <= vB  */

  OP_CLOSURE, /*  A Bx  R[A] := closure(KPROTO[Bx])      */

  OP_VARARG, /*  A B C k  R[A], ..., R[A+C-2] = varargs        */

  OP_GETVARG, /* A B C  R[A] := R[B][R[C]], R[B] is vararg parameter    */

  OP_ERRNNIL, /*  A Bx  raise error if R[A] ~= nil (K[Bx - 1] is global name)*/

  OP_VARARGPREP, /*   (adjust varargs)        */

  OP_EXTRAARG /*  Ax  extra (larger) argument for previous opcode  */
} OpCode;

constexpr int NUM_OPCODES = (int) (OP_EXTRAARG) + 1;

constexpr OpCode GET_OPCODE(Instruction i)
{
  return (OpCode)((i >> POS_OP) & MASK1(SIZE_OP, 0));
}

extern const lu_byte luaP_opmodes[NUM_OPCODES];

static l_forceinline enum OpMode getOpMode(int m)
{
  return (enum OpMode)(luaP_opmodes[m] & 7);
}

static l_forceinline int checkopm(Instruction i, enum OpMode m)
{
  return getOpMode(GET_OPCODE(i)) == m;
}

constexpr int getarg(Instruction i, int pos, int size)
{
  return (i >> pos) & MASK1(size, 0);
}

constexpr int GETARG_A(Instruction i)
{
  return getarg(i, POS_A, SIZE_A);
}

static l_forceinline int GETARG_B(Instruction i)
{
  assert(checkopm(i, iABC));
  return getarg(i, POS_B, SIZE_B);
}
static l_forceinline int GETARG_vB(Instruction i)
{
  assert(checkopm(i, ivABC));
  return getarg(i, POS_vB, SIZE_vB);
}
static l_forceinline int GETARG_sB(Instruction i)
{
  return sC2int(GETARG_B(i));
}

static l_forceinline int GETARG_C(Instruction i)
{
  assert(checkopm(i, iABC));
  return getarg(i, POS_C, SIZE_C);
}
static l_forceinline int GETARG_vC(Instruction i)
{
  assert(checkopm(i, ivABC));
  return getarg(i, POS_vC, SIZE_vC);
}
static l_forceinline int GETARG_sC(Instruction i)
{
  return sC2int(GETARG_C(i));
}

constexpr int TESTARG_k(Instruction i)
{
  return i & (1u << POS_k);
}
constexpr int GETARG_k(Instruction i)
{
  return getarg(i, POS_k, 1);
}

static l_forceinline int GETARG_Bx(Instruction i)
{
  assert(checkopm(i, iABx));
  return getarg(i, POS_Bx, SIZE_Bx);
}

static l_forceinline int GETARG_Ax(Instruction i)
{
  assert(checkopm(i, iAx));
  return getarg(i, POS_Ax, SIZE_Ax);
}

static l_forceinline int GETARG_sBx(Instruction i)
{
  assert(checkopm(i, iAsBx));
  return getarg(i, POS_Bx, SIZE_Bx) - OFFSET_sBx;
}

static l_forceinline int GETARG_sJ(Instruction i)
{
  assert(checkopm(i, isJ));
  return getarg(i, POS_sJ, SIZE_sJ) - OFFSET_sJ;
}

constexpr Instruction SET_OPCODE(Instruction i, OpCode o)
{
  return (i & MASK0(SIZE_OP, POS_OP)) | (((Instruction)o << POS_OP) & MASK1(SIZE_OP, POS_OP));
}

constexpr Instruction setarg(Instruction i, int v, int pos, int size)
{
  return (i & MASK0(size, pos)) | (((Instruction)v << pos) & MASK1(size, pos));
}

constexpr Instruction SETARG_A(Instruction i, int v)
{
  return setarg(i, v, POS_A, SIZE_A);
}

constexpr Instruction SETARG_B(Instruction i, int v)
{
  return setarg(i, v, POS_B, SIZE_B);
}

constexpr Instruction SETARG_C(Instruction i, int v)
{
  return setarg(i, v, POS_C, SIZE_C);
}

constexpr Instruction SETARG_k(Instruction i, int v)
{
  return setarg(i, v, POS_k, 1);
}

constexpr Instruction SETARG_Bx(Instruction i, int v)
{
  return setarg(i, v, POS_Bx, SIZE_Bx);
}

constexpr Instruction SETARG_sJ(Instruction i, int j)
{
  return setarg(i, j + OFFSET_sJ, POS_sJ, SIZE_sJ);
}

constexpr Instruction CREATE_ABCk(int o, int a, int b, int c, int k)
{
  return ((Instruction)o << POS_OP) | ((Instruction)a << POS_A) | ((Instruction)b << POS_B) | ((Instruction)c << POS_C) | ((Instruction)k << POS_k);
}

constexpr Instruction CREATE_vABCk(int o, int a, int b, int c, int k)
{
  return ((Instruction)o << POS_OP) | ((Instruction)a << POS_A) | ((Instruction)b << POS_vB) | ((Instruction)c << POS_vC) | ((Instruction)k << POS_k);
}

constexpr Instruction CREATE_ABx(int o, int a, unsigned int bc)
{
  return ((Instruction)o << POS_OP) | ((Instruction)a << POS_A) | ((Instruction)bc << POS_Bx);
}

constexpr Instruction CREATE_Ax(int o, unsigned int a)
{
  return ((Instruction)o << POS_OP) | ((Instruction)a << POS_Ax);
}

constexpr Instruction CREATE_sJ(int o, unsigned int j, int k)
{
  return ((Instruction)o << POS_OP) | ((Instruction)j << POS_sJ) | ((Instruction)k << POS_k);
}

/*===========================================================================
  Notes:

  (*) Opcode OP_LFALSESKIP is used to convert a condition to a boolean
  value, in a code equivalent to (not cond ? false : true).  (It
  produces false and skips the next instruction producing true.)

  (*) Opcodes OP_MMBIN and variants follow each arithmetic and
  bitwise opcode. If the operation succeeds, it skips this next
  opcode. Otherwise, this opcode calls the corresponding metamethod.

  (*) Opcode OP_TESTSET is used in short-circuit expressions that need
  both to jump and to produce a value, such as (a = b or c).

  (*) In OP_CALL, if (B == 0) then B = top - A. If (C == 0), then
  'top' is set to last_result+1, so next open instruction (OP_CALL,
  OP_RETURN*, OP_SETLIST) may use 'top'.

  (*) In OP_VARARG, if (C == 0) then use actual number of varargs and
  set top (like in OP_CALL with C == 0). 'k' means function has a
  vararg table, which is in R[B].

  (*) In OP_RETURN, if (B == 0) then return up to 'top'.

  (*) In OP_LOADKX and OP_NEWTABLE, the next instruction is always
  OP_EXTRAARG.

  (*) In OP_SETLIST, if (B == 0) then real B = 'top'; if k, then
  real C = EXTRAARG _ C (the bits of EXTRAARG concatenated with the
  bits of C).

  (*) In OP_NEWTABLE, vB is log2 of the hash size (which is always a
  power of 2) plus 1, or zero for size zero. If not k, the array size
  is vC. Otherwise, the array size is EXTRAARG _ vC.

  (*) In OP_ERRNNIL, (Bx == 0) means index of global name doesn't
  fit in Bx. (So, that name is not available for the error message.)

  (*) For comparisons, k specifies what condition the test should accept
  (true or false).

  (*) In OP_MMBINI/OP_MMBINK, k means the arguments were flipped
  (the constant is the first operand).

  (*) All comparison and test instructions assume that the instruction
  being skipped (pc++) is a jump.

  (*) In instructions OP_RETURN/OP_TAILCALL, 'k' specifies that the
  function builds upvalues, which may need to be closed. C > 0 means
  the function has hidden vararg arguments, so that its 'func' must be
  corrected before returning; in this case, (C - 1) is its number of
  fixed parameters.

  (*) In comparisons with an immediate operand, C signals whether the
  original operand was a float. (It must be corrected in case of
  metamethods.)

===========================================================================*/


static l_forceinline int testAMode(int m)
{
  return luaP_opmodes[m] & (1 << 3);
}
static l_forceinline int testTMode(int m)
{
  return luaP_opmodes[m] & (1 << 4);
}
static l_forceinline int testITMode(int m)
{
  return luaP_opmodes[m] & (1 << 5);
}
static l_forceinline int testOTMode(int m)
{
  return luaP_opmodes[m] & (1 << 6);
}
static l_forceinline int testMMMode(int m)
{
  return luaP_opmodes[m] & (1 << 7);
}

int luaP_isOT(Instruction i);
int luaP_isIT(Instruction i);

}
