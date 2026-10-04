/*
** $Id: lcode.h $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include "llex.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"

namespace lua::compiler
{

/*
** Marks the end of a patch list. It is an invalid value both as an absolute
** address, and as a list link (would link an element to itself).
*/
constexpr int NO_JUMP = (-1);

/*
** grep "ORDER OPR" if you change these enums  (ORDER OP)
*/
typedef enum BinOpr
{
  /* arithmetic operators */
  OPR_ADD,
  OPR_SUB,
  OPR_MUL,
  OPR_MOD,
  OPR_POW,
  OPR_DIV,
  OPR_IDIV,
  /* bitwise operators */
  OPR_BAND,
  OPR_BOR,
  OPR_BXOR,
  OPR_SHL,
  OPR_SHR,
  /* string operator */
  OPR_CONCAT,
  /* comparison operators */
  OPR_EQ,
  OPR_LT,
  OPR_LE,
  OPR_NE,
  OPR_GT,
  OPR_GE,
  /* logical operators */
  OPR_AND,
  OPR_OR,
  OPR_NOBINOPR
} BinOpr;

/* true if operation is foldable (that is, it is arithmetic or bitwise) */
constexpr int foldbinop(BinOpr op)
{
  return op <= OPR_SHR;
}

typedef enum UnOpr
{
  OPR_MINUS,
  OPR_BNOT,
  OPR_NOT,
  OPR_LEN,
  OPR_NOUNOPR
} UnOpr;

/* get (pointer to) instruction of given 'expdesc' */

int luaK_code(FuncState* fs, Instruction i);
int luaK_codeABx(FuncState* fs, OpCode o, int A, int Bx);
int luaK_codeABCk(FuncState* fs, OpCode o, int A, int B, int C,
                            int k);
int luaK_codevABCk(FuncState* fs, OpCode o, int A, int B, int C,
                             int k);
int luaK_exp2const(FuncState* fs, const expdesc* e, TValue* v);
void luaK_fixline(FuncState* fs, int line);
void luaK_nil(FuncState* fs, int from, int n);
void luaK_codecheckglobal(FuncState* fs, expdesc* var, int k,
                                    int line);
void luaK_reserveregs(FuncState* fs, int n);
void luaK_checkstack(FuncState* fs, int n);
void luaK_int(FuncState* fs, int reg, lua_Integer n);
void luaK_vapar2local(FuncState* fs, expdesc* var);
void luaK_dischargevars(FuncState* fs, expdesc* e);
int luaK_exp2anyreg(FuncState* fs, expdesc* e);
void luaK_exp2anyregup(FuncState* fs, expdesc* e);
void luaK_exp2nextreg(FuncState* fs, expdesc* e);
void luaK_exp2val(FuncState* fs, expdesc* e);
void luaK_self(FuncState* fs, expdesc* e, expdesc* key);
void luaK_indexed(FuncState* fs, expdesc* t, expdesc* k);
void luaK_goiftrue(FuncState* fs, expdesc* e);
void luaK_storevar(FuncState* fs, expdesc* var, expdesc* e);
void luaK_setreturns(FuncState* fs, expdesc* e, int nresults);
void luaK_setoneret(FuncState* fs, expdesc* e);
int luaK_jump(FuncState* fs);
void luaK_ret(FuncState* fs, int first, int nret);
void luaK_patchlist(FuncState* fs, int list, int target);
void luaK_patchtohere(FuncState* fs, int list);
int luaK_concat(FuncState* fs, int l1, int l2);
int luaK_getlabel(FuncState* fs);
void luaK_prefix(FuncState* fs, UnOpr op, expdesc* v, int line);
void luaK_infix(FuncState* fs, BinOpr op, expdesc* v);
void luaK_posfix(FuncState* fs, BinOpr op, expdesc* v1,
                           expdesc* v2, int line);
void luaK_settablesize(FuncState* fs, int pc,
                                 int ra, int asize, int hsize);
void luaK_setlist(FuncState* fs, int base, int nelems, int tostore);
void luaK_finish(FuncState* fs);
[[noreturn]] void luaK_semerror(LexState* ls, byte_view fmt, const FmtArgs& args);

static inline int luaK_codeABC(FuncState* fs, OpCode o, int a, int b, int c)
{
  return luaK_codeABCk(fs, o, a, b, c, 0);
}

static inline void luaK_setmultret(FuncState* fs, expdesc* e)
{
  luaK_setreturns(fs, e, LUA_MULTRET);
}

static inline void luaK_jumpto(FuncState* fs, int t)
{
  luaK_patchlist(fs, luaK_jump(fs), t);
}

}
