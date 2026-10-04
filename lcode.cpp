/*
** $Id: lcode.c $
** Code generator for Lua
** See Copyright Notice in lua.h
*/


#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"

/* (note that expressions VJMP also have jumps.) */
static inline int hasjumps(const expdesc* e)
{
  return e->t != e->f;
}

static int codesJ(FuncState* fs, OpCode o, int sj, int k);

/* semantic error */
l_noret luaK_semerror(LexState* ls, byte_view fmt, const FmtArgs& args)
{
  const InternalString* msg = luaO_pushvfstring(ls->L, fmt, args);
  if (msg == nullptr)
    luaD_throw(ls->L, LUA_ERRMEM);
  ls->t.token = 0;               /* remove "near <token>" from final message */
  ls->linenumber = ls->lastline; /* back to line of last used token */
  luaX_syntaxerror(ls, msg->view());
}

/*
** If expression is a numeric constant, fills 'v' with its value
** and returns 1. Otherwise, returns 0.
*/
static int tonumeral(const expdesc* e, TValue* v)
{
  if (hasjumps(e))
    return 0; /* not a numeral */
  switch (e->k)
  {
  case VKINT:
    setivalue(v, e->ival);
    return 1;
  case VKFLT:
    setfltvalue(v, e->nval);
    return 1;
  default:
    return 0;
  }
}

static int isnumeral(const expdesc* e)
{
  TValue junk;
  return tonumeral(e, &junk);
}

/*
** Get the constant value from a constant expression
*/
static TValue* const2val(FuncState* fs, const expdesc* e)
{
  assert(e->k == VCONST);
  return &fs->ls->dyd->actvar.arr[e->info].k;
}

/*
** If expression is a constant, fills 'v' with its value
** and returns 1. Otherwise, returns 0.
*/
int luaK_exp2const(FuncState* fs, const expdesc* e, TValue* v)
{
  if (hasjumps(e))
    return 0; /* not a constant */
  switch (e->k)
  {
  case VFALSE:
    v->tt_ = LUA_VFALSE;
    return 1;
  case VTRUE:
    v->tt_ = LUA_VTRUE;
    return 1;
  case VNIL:
    v->tt_ = LUA_VNIL;
    return 1;
  case VKSTR:
  {
    setsvalue(fs->ls->L, v, e->strval);
    return 1;
  }
  case VCONST:
  {
    setobj(fs->ls->L, v, const2val(fs, e));
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
static int previousinstruction(FuncState* fs)
{
  if (fs->pc > fs->lasttarget)
    return fs->pc - 1; /* previous instruction */
  else
    return -1;
}

/*
** Create a OP_LOADNIL instruction, but try to optimize: if the previous
** instruction is also OP_LOADNIL and ranges are compatible, adjust
** range of previous instruction instead of emitting a new one. (For
** instance, 'local a; local b' will generate a single opcode.)
*/
void luaK_nil(FuncState* fs, int from, int n)
{
  int l = from + n - 1; /* last register to set nil */
  int previous = previousinstruction(fs);
  if (previous >= 0 && GET_OPCODE(fs->f->code[previous]) == OP_LOADNIL)
  {                                  /* previous is LOADNIL? */
    int pfrom = GETARG_A(fs->f->code[previous]); /* get previous range */
    int pl = pfrom + GETARG_B(fs->f->code[previous]);
    if ((pfrom <= from && from <= pl + 1) ||
        (from <= pfrom && pfrom <= l + 1))
    { /* can connect both? */
      if (pfrom < from)
        from = pfrom; /* from = min(from, pfrom) */
      if (pl > l)
        l = pl; /* l = max(l, pl) */
      fs->f->code[previous] = ((fs->f->code[previous] & MASK0(SIZE_A, POS_A)) | ((((Instruction)(from)) << POS_A) & MASK1(SIZE_A, POS_A)));
      fs->f->code[previous] = ((fs->f->code[previous] & MASK0(SIZE_B, POS_B)) | ((((Instruction)(l - from)) << POS_B) & MASK1(SIZE_B, POS_B)));
      return;
    } /* else go through */
  }
  luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0); /* else no optimization */
}

/*
** Gets the destination address of a jump instruction. Used to traverse
** a list of jumps.
*/
static int getjump(FuncState* fs, int pc)
{
  int offset = GETARG_sJ(fs->f->code[pc]);
  if (offset == NO_JUMP) /* point to itself represents end of list */
    return NO_JUMP;      /* end of list */
  else
    return (pc + 1) + offset; /* turn offset into absolute position */
}

/*
** Fix jump instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua)
*/
static void fixjump(FuncState* fs, int pc, int dest)
{
  int jmp = pc;
  int offset = dest - (pc + 1);
  assert(dest != NO_JUMP);
  if (!(-OFFSET_sJ <= offset && offset <= MAXARG_sJ - OFFSET_sJ))
    luaX_syntaxerror(fs->ls, lt::control_structure_too_long);
  assert(GET_OPCODE(fs->f->code[jmp]) == OP_JMP);
  fs->f->code[jmp] = ((fs->f->code[jmp] & MASK0(SIZE_sJ, POS_sJ)) | ((((Instruction)(((unsigned int)((offset) + OFFSET_sJ)))) << POS_sJ) & MASK1(SIZE_sJ, POS_sJ)));
}

/*
** Concatenate jump-list 'l2' into jump-list 'l1'
*/
int luaK_concat(FuncState* fs, int l1, int l2)
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
int luaK_jump(FuncState* fs)
{
  return codesJ(fs, OP_JMP, NO_JUMP, 0);
}

/*
** Code a 'return' instruction
*/
void luaK_ret(FuncState* fs, int first, int nret)
{
  OpCode op;
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
  luaY_checklimit(fs, nret + 1, MAXARG_B, lt::returns);
  luaK_codeABC(fs, op, first, nret + 1, 0);
}

/*
** Code a "conditional jump", that is, a test or comparison opcode
** followed by a jump. Return jump position.
*/
static int condjump(FuncState* fs, OpCode op, int A, int B, int C, int k)
{
  luaK_codeABCk(fs, op, A, B, C, k);
  return luaK_jump(fs);
}

/*
** returns current 'pc' and marks it as a jump target (to avoid wrong
** optimizations with consecutive instructions not in the same basic block).
*/
int luaK_getlabel(FuncState* fs)
{
  fs->lasttarget = fs->pc;
  return fs->pc;
}

/*
** Returns the position of the instruction "controlling" a given
** jump (that is, its condition), or the jump itself if it is
** unconditional.
*/
static int getjumpcontrol(FuncState* fs, int pc)
{
  if (pc >= 1 && testTMode(GET_OPCODE(fs->f->code[pc - 1])))
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
static int patchtestreg(FuncState* fs, int node, int reg)
{
  int i = getjumpcontrol(fs, node);
  if (GET_OPCODE(fs->f->code[i]) != OP_TESTSET)
    return 0; /* cannot patch other instructions */
  if (reg != NO_REG && reg != GETARG_B(fs->f->code[i]))
    fs->f->code[i] = ((fs->f->code[i] & MASK0(SIZE_A, POS_A)) | ((((Instruction)(reg)) << POS_A) & MASK1(SIZE_A, POS_A)));
  else
  {
    /* no register to put value or register already has the value;
       change instruction to simple test */
    fs->f->code[i] = CREATE_ABCk(OP_TEST, GETARG_B(fs->f->code[i]), 0, 0, GETARG_k(fs->f->code[i]));
  }
  return 1;
}

/*
** Traverse a list of tests ensuring no one produces a value
*/
static void removevalues(FuncState* fs, int list)
{
  for (; list != NO_JUMP; list = getjump(fs, list))
    patchtestreg(fs, list, NO_REG);
}

/*
** Traverse a list of tests, patching their destination address and
** registers: tests producing values jump to 'vtarget' (and put their
** values in 'reg'), other tests jump to 'dtarget'.
*/
static void patchlistaux(FuncState* fs, int list, int vtarget, int reg,
                         int dtarget)
{
  while (list != NO_JUMP)
  {
    int next = getjump(fs, list);
    if (patchtestreg(fs, list, reg))
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
void luaK_patchlist(FuncState* fs, int list, int target)
{
  assert(target <= fs->pc);
  patchlistaux(fs, list, target, NO_REG, target);
}

void luaK_patchtohere(FuncState* fs, int list)
{
  int hr = luaK_getlabel(fs); /* mark "here" as a jump target */
  luaK_patchlist(fs, list, hr);
}

/* limit for difference between lines in relative line info. */
constexpr int LIMLINEDIFF = 0x80;

/*
** Save line info for a new instruction. If difference from last line
** does not fit in a byte, of after that many instructions, save a new
** absolute line info; (in that case, the special value 'ABSLINEINFO'
** in 'lineinfo' signals the existence of this absolute information.)
** Otherwise, store the difference from last line in 'lineinfo'.
*/
static void savelineinfo(FuncState* fs, Proto* f, int line)
{
  int linedif = line - fs->previousline;
  int pc = fs->pc - 1; /* last instruction coded */
  if (abs(linedif) >= LIMLINEDIFF || fs->iwthabs++ >= MAXIWTHABS)
  {
    f->abslineinfo = luaM_safegrow_AbsLineInfo_span(fs->ls->L, f->abslineinfo, fs->nabslineinfo);
    f->abslineinfo[fs->nabslineinfo].pc = pc;
    f->abslineinfo[fs->nabslineinfo++].line = line;
    linedif = ABSLINEINFO; /* signal that there is absolute information */
    fs->iwthabs = 1;       /* restart counter */
  }
  f->lineinfo = luaM_safegrow_ls_byte_span(fs->ls->L, f->lineinfo, pc);
  f->lineinfo[pc] = ((ls_byte)(linedif));
  fs->previousline = line; /* last line saved */
}

/*
** Remove line information from the last instruction.
** If line information for that instruction is absolute, set 'iwthabs'
** above its max to force the new (replacing) instruction to have
** absolute line info, too.
*/
static void removelastlineinfo(FuncState* fs)
{
  Proto* f = fs->f;
  int pc = fs->pc - 1; /* last instruction coded */
  if (f->lineinfo[pc] != ABSLINEINFO)
  {                                      /* relative line info? */
    fs->previousline -= f->lineinfo[pc]; /* correct last line saved */
    fs->iwthabs--;                       /* undo previous increment */
  }
  else
  { /* absolute line information */
    assert(f->abslineinfo[fs->nabslineinfo - 1].pc == pc);
    fs->nabslineinfo--;           /* remove it */
    fs->iwthabs = MAXIWTHABS + 1; /* force next line info to be absolute */
  }
}

/*
** Remove the last instruction created, correcting line information
** accordingly.
*/
static void removelastinstruction(FuncState* fs)
{
  removelastlineinfo(fs);
  fs->pc--;
}

/*
** Emit instruction 'i', checking for array sizes and saving also its
** line information. Return 'i' position.
*/
int luaK_code(FuncState* fs, Instruction i)
{
  Proto* f = fs->f;
  /* put new instruction in code array */
  f->code = luaM_safegrow_Instruction_span(fs->ls->L, f->code, fs->pc);
  f->code[fs->pc++] = i;
  savelineinfo(fs, f, fs->ls->lastline);
  return fs->pc - 1; /* index of new instruction */
}

/*
** Format and emit an 'iABC' instruction. (Assertions check consistency
** of parameters versus opcode.)
*/
int luaK_codeABCk(FuncState* fs, OpCode o, int A, int B, int C, int k)
{
  assert(getOpMode(o) == iABC);
  assert(A <= MAXARG_A && B <= MAXARG_B &&
             C <= MAXARG_C && (k & ~1) == 0);
  return luaK_code(fs, CREATE_ABCk(o, A, B, C, k));
}

int luaK_codevABCk(FuncState* fs, OpCode o, int A, int B, int C, int k)
{
  assert(getOpMode(o) == ivABC);
  assert(A <= MAXARG_A && B <= MAXARG_vB &&
             C <= MAXARG_vC && (k & ~1) == 0);
  return luaK_code(fs, CREATE_vABCk(o, A, B, C, k));
}

/*
** Format and emit an 'iABx' instruction.
*/
int luaK_codeABx(FuncState* fs, OpCode o, int A, int Bc)
{
  assert(getOpMode(o) == iABx);
  assert(A <= MAXARG_A && Bc <= MAXARG_Bx);
  return luaK_code(fs, CREATE_ABx(o, A, Bc));
}

/*
** Format and emit an 'iAsBx' instruction.
*/
static int codeAsBx(FuncState* fs, OpCode o, int A, int Bc)
{
  int b = Bc + OFFSET_sBx;
  assert(getOpMode(o) == iAsBx);
  assert(A <= MAXARG_A && b <= MAXARG_Bx);
  return luaK_code(fs, CREATE_ABx(o, A, b));
}

/*
** Format and emit an 'isJ' instruction.
*/
static int codesJ(FuncState* fs, OpCode o, int sj, int k)
{
  int j = sj + OFFSET_sJ;
  assert(getOpMode(o) == isJ);
  assert(j <= MAXARG_sJ && (k & ~1) == 0);
  return luaK_code(fs, CREATE_sJ(o, j, k));
}

/*
** Emit an "extra argument" instruction (format 'iAx')
*/
static int codeextraarg(FuncState* fs, int A)
{
  assert(A <= MAXARG_Ax);
  return luaK_code(fs, CREATE_Ax(OP_EXTRAARG, A));
}

/*
** Emit a "load constant" instruction, using either 'OP_LOADK'
** (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
** instruction with "extra argument".
*/
static int luaK_codek(FuncState* fs, int reg, int k)
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
void luaK_checkstack(FuncState* fs, int n)
{
  int newstack = fs->freereg + n;
  if (newstack > fs->f->maxstacksize)
  {
    luaY_checklimit(fs, newstack, MAX_FSTACK, lt::registers);
    fs->f->maxstacksize = newstack;
  }
}

/*
** Reserve 'n' registers in register stack
*/
void luaK_reserveregs(FuncState* fs, int n)
{
  luaK_checkstack(fs, n);
  fs->freereg = fs->freereg + n;
}

/*
** Free register 'reg', if it is neither a constant index nor
** a local variable.
)
*/
static void freereg(FuncState* fs, int reg)
{
  if (reg >= luaY_nvarstack(fs))
  {
    fs->freereg--;
    assert(reg == fs->freereg);
  }
}

/*
** Free two registers in proper order
*/
static void freeregs(FuncState* fs, int r1, int r2)
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
static void freeexp(FuncState* fs, expdesc* e)
{
  if (e->k == VNONRELOC)
    freereg(fs, e->info);
}

/*
** Free registers used by expressions 'e1' and 'e2' (if any) in proper
** order.
*/
static void freeexps(FuncState* fs, expdesc* e1, expdesc* e2)
{
  int r1 = (e1->k == VNONRELOC) ? e1->info : -1;
  int r2 = (e2->k == VNONRELOC) ? e2->info : -1;
  freeregs(fs, r1, r2);
}

/*
** Add constant 'v' to prototype's list of constants (field 'k').
*/
static int addk(FuncState* fs, Proto* f, TValue* v)
{
  lua_State* L = fs->ls->L;
  int oldsize = (int)f->k.size();
  int k = fs->nk;
  f->k = luaM_safegrow_TValue_span(L, f->k, k);
  while (oldsize < (int)f->k.size())
    f->k[oldsize++].tt_ = LUA_VNIL;
  setobj(L, &f->k[k], v);
  fs->nk++;
  luaC_barrier(L, f, v);
  return k;
}

/*
** Use scanner's table to cache position of constants in constant list
** and try to reuse constants. Because some values should not be used
** as keys (nil cannot be a key, integer keys can collapse with float
** keys), the caller must provide a useful 'key' for indexing the cache.
*/
static int k2proto(FuncState* fs, TValue* key, TValue* v)
{
  TValue val;
  Proto* f = fs->f;
  int tag = fs->kcache->luaH_get(key, &val); /* query scanner table */
  if (!((tag & NOVARIANT_MASK) == LUA_TNIL))
  { /* is there an index there? */
    int k = ((int)(ivalue(&val)));
    /* collisions can happen only for float keys */
    assert((key->tt_ == LUA_VNUMFLT) || luaV_rawequalobj(&f->k[k], v));
    return k; /* reuse index */
  }
  else
  { /* constant not found; create a new entry */
    int k = addk(fs, f, v);
    /* cache it for reuse; numerical value does not need GC barrier;
       table is not a metatable, so it does not need to invalidate cache */
    setivalue(&val, k);
    fs->kcache->luaH_set(fs->ls->L, key, &val);
    return k;
  }
}

/*
** Add a string to list of constants and return its index.
*/
static int stringK(FuncState* fs, TString* s)
{
  TValue o;
  setsvalue(fs->ls->L, &o, s);
  return k2proto(fs, &o, &o); /* use string itself as key */
}

/*
** Add an integer to list of constants and return its index.
*/
static int luaK_intK(FuncState* fs, lua_Integer n)
{
  TValue o;
  setivalue(&o, n);
  return k2proto(fs, &o, &o); /* use integer itself as key */
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
static int luaK_numberK(FuncState* fs, lua_Number r)
{
  TValue o, kv;
  setfltvalue(&o, r); /* value as a TValue */
  if (r == 0)
  {                              /* handle zero as a special case */
    setpvalue(&kv, fs);          /* use FuncState as index */
    return k2proto(fs, &kv, &o); /* cannot collide */
  }
  else
  {
    const int nbm = DBL_MANT_DIG;
    const lua_Number q = ldexp(1.0, -nbm + 1);
    const lua_Number k = r * (1 + q); /* key */
    IntResult ik;
    setfltvalue(&kv, k); /* key as a TValue */
    if (!luaV_flttointeger(k, &ik, F2Ieq))
    {                                         /* not an integer value? */
      int n = k2proto(fs, &kv, &o);           /* use key */
      if (luaV_rawequalobj(&fs->f->k[n], &o)) /* correct value? */
        return n;
    }
    /* else, either key is still an integer or there was a collision;
       anyway, do not try to reuse constant; instead, create a new one */
    return addk(fs, fs->f, &o);
  }
}

/*
** Add a false to list of constants and return its index.
*/
static int boolF(FuncState* fs)
{
  TValue o;
  o.tt_ = LUA_VFALSE;
  return k2proto(fs, &o, &o); /* use boolean itself as key */
}

/*
** Add a true to list of constants and return its index.
*/
static int boolT(FuncState* fs)
{
  TValue o;
  o.tt_ = LUA_VTRUE;
  return k2proto(fs, &o, &o); /* use boolean itself as key */
}

/*
** Add nil to list of constants and return its index.
*/
static int nilK(FuncState* fs)
{
  lua_State* L = fs->ls->L;
  TValue k;
  /* cannot use nil as key; instead use table itself */
  sethvalue(L, &k, fs->kcache);
  return k2proto(fs, &k, &L->l_G->nilvalue);
}

/*
** Check whether 'i' can be stored in an 'sC' operand. Equivalent to
** (0 <= int2sC(i) && int2sC(i) <= MAXARG_C) but without risk of
** overflows in the hidden addition inside 'int2sC'.
*/
static int fitsC(lua_Integer i)
{
  return (-OFFSET_sC <= i && i <= MAXARG_C - OFFSET_sC);
}

/*
** Check whether 'i' can be stored in an 'sBx' operand.
*/
static int fitsBx(lua_Integer i)
{
  return (-OFFSET_sBx <= i && i <= MAXARG_Bx - OFFSET_sBx);
}

void luaK_int(FuncState* fs, int reg, lua_Integer i)
{
  if (fitsBx(i))
    codeAsBx(fs, OP_LOADI, reg, ((int)(i)));
  else
    luaK_codek(fs, reg, luaK_intK(fs, i));
}

static void luaK_float(FuncState* fs, int reg, lua_Number f)
{
  IntResult fi;
  if (luaV_flttointeger(f, &fi, F2Ieq) && fitsBx(fi.v))
    codeAsBx(fs, OP_LOADF, reg, ((int)(fi.v)));
  else
    luaK_codek(fs, reg, luaK_numberK(fs, f));
}

/*
** Get the value of 'var' in a register and generate an opcode to check
** whether that register is nil. 'k' is the index of the variable name
** in the list of constants. If its value cannot be encoded in Bx, a 0
** will use '?' for the name.
*/
void luaK_codecheckglobal(FuncState* fs, expdesc* var, int k, int line)
{
  luaK_exp2anyreg(fs, var);
  luaK_fixline(fs, line);
  k = (k >= MAXARG_Bx) ? 0 : k + 1;
  luaK_codeABx(fs, OP_ERRNNIL, var->info, k);
  luaK_fixline(fs, line);
  freeexp(fs, var);
}

/*
** Convert a constant in 'v' into an expression description 'e'
*/
static void const2exp(TValue* v, expdesc* e)
{
  switch ((v->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VNUMINT:
    e->k = VKINT;
    assert(v->tt_ == LUA_VNUMINT);
    e->ival = ivalue(v);
    break;
  case LUA_VNUMFLT:
    e->k = VKFLT;
    assert(v->tt_ == LUA_VNUMFLT);
    e->nval = fltvalue(v);
    break;
  case LUA_VFALSE:
    e->k = VFALSE;
    break;
  case LUA_VTRUE:
    e->k = VTRUE;
    break;
  case LUA_VNIL:
    e->k = VNIL;
    break;
  case LUA_VSHRSTR:
    e->k = VKSTR;
    assert((v->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    e->strval = tsvalue(v);
    break;
  default:
    assert(0);
  }
}

/*
** Fix an expression to return the number of results 'nresults'.
** 'e' must be a multi-ret expression (function call or vararg).
*/
void luaK_setreturns(FuncState* fs, expdesc* e, int nresults)
{
  int pc = e->info;
  luaY_checklimit(fs, nresults + 1, MAXARG_C, lt::multiple_results);
  if (e->k == VCALL) /* expression is an open function call? */
    fs->f->code[pc] = ((fs->f->code[pc] & MASK0(SIZE_C, POS_C)) | ((((Instruction)(nresults + 1)) << POS_C) & MASK1(SIZE_C, POS_C)));
  else
  {
    assert(e->k == VVARARG);
    fs->f->code[pc] = ((fs->f->code[pc] & MASK0(SIZE_C, POS_C)) | ((((Instruction)(nresults + 1)) << POS_C) & MASK1(SIZE_C, POS_C)));
    fs->f->code[pc] = ((fs->f->code[pc] & MASK0(SIZE_A, POS_A)) | ((((Instruction)(fs->freereg)) << POS_A) & MASK1(SIZE_A, POS_A)));
    luaK_reserveregs(fs, 1);
  }
}

/*
** Convert a VKSTR to a VK
*/
static int str2K(FuncState* fs, expdesc* e)
{
  assert(e->k == VKSTR);
  e->info = stringK(fs, e->strval);
  e->k = VK;
  return e->info;
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
void luaK_setoneret(FuncState* fs, expdesc* e)
{
  if (e->k == VCALL)
  { /* expression is an open function call? */
    /* already returns 1 value */
    assert(GETARG_C(fs->f->code[e->info]) == 2);
    e->k = VNONRELOC; /* result has fixed position */
    e->info = GETARG_A(fs->f->code[e->info]);
  }
  else if (e->k == VVARARG)
  {
    fs->f->code[e->info] = ((fs->f->code[e->info] & MASK0(SIZE_C, POS_C)) | ((((Instruction)(2)) << POS_C) & MASK1(SIZE_C, POS_C)));
    e->k = VRELOC; /* can relocate its simple result */
  }
}

/*
** Change a vararg parameter into a regular local variable
*/
void luaK_vapar2local(FuncState* fs, expdesc* var)
{
  needvatab(fs->f); /* function will need a vararg table */
  /* now a vararg parameter is equivalent to a regular local variable */
  var->k = VLOCAL;
}

/*
** Ensure that expression 'e' is not a variable (nor a <const>).
** (Expression still may have jump lists.)
*/
void luaK_dischargevars(FuncState* fs, expdesc* e)
{
  switch (e->k)
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
    int temp = e->var.ridx;
    e->info = temp; /* (avoid a direct assignment; values overlap) */
    e->k = VNONRELOC; /* becomes a non-relocatable value */
    break;
  }
  case VUPVAL:
  { /* move value to some (pending) register */
    e->info = luaK_codeABC(fs, OP_GETUPVAL, 0, e->info, 0);
    e->k = VRELOC;
    break;
  }
  case VINDEXUP:
  {
    e->info = luaK_codeABC(fs, OP_GETTABUP, 0, e->ind.t, e->ind.idx);
    e->k = VRELOC;
    break;
  }
  case VINDEXI:
  {
    freereg(fs, e->ind.t);
    e->info = luaK_codeABC(fs, OP_GETI, 0, e->ind.t, e->ind.idx);
    e->k = VRELOC;
    break;
  }
  case VINDEXSTR:
  {
    freereg(fs, e->ind.t);
    e->info = luaK_codeABC(fs, OP_GETFIELD, 0, e->ind.t, e->ind.idx);
    e->k = VRELOC;
    break;
  }
  case VINDEXED:
  {
    freeregs(fs, e->ind.t, e->ind.idx);
    e->info = luaK_codeABC(fs, OP_GETTABLE, 0, e->ind.t, e->ind.idx);
    e->k = VRELOC;
    break;
  }
  case VVARGIND:
  {
    freeregs(fs, e->ind.t, e->ind.idx);
    e->info = luaK_codeABC(fs, OP_GETVARG, 0, e->ind.t, e->ind.idx);
    e->k = VRELOC;
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
static void discharge2reg(FuncState* fs, expdesc* e, int reg)
{
  luaK_dischargevars(fs, e);
  switch (e->k)
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
    luaK_codek(fs, reg, e->info);
    break;
  }
  case VKFLT:
  {
    luaK_float(fs, reg, e->nval);
    break;
  }
  case VKINT:
  {
    luaK_int(fs, reg, e->ival);
    break;
  }
  case VRELOC:
  {
    int pc = e->info;
    fs->f->code[pc] = ((fs->f->code[pc] & MASK0(SIZE_A, POS_A)) | ((((Instruction)(reg)) << POS_A) & MASK1(SIZE_A, POS_A))); /* instruction will put result in 'reg' */
    break;
  }
  case VNONRELOC:
  {
    if (reg != e->info)
      luaK_codeABC(fs, OP_MOVE, reg, e->info, 0);
    break;
  }
  default:
  {
    assert(e->k == VJMP);
    return; /* nothing to do... */
  }
  }
  e->info = reg;
  e->k = VNONRELOC;
}

/*
** Ensure expression value is in a register, making 'e' a
** non-relocatable expression.
** (Expression still may have jump lists.)
*/
static void discharge2anyreg(FuncState* fs, expdesc* e)
{
  if (e->k != VNONRELOC)
  {                                        /* no fixed register yet? */
    luaK_reserveregs(fs, 1);               /* get a register */
    discharge2reg(fs, e, fs->freereg - 1); /* put value there */
  }
}

static int code_loadbool(FuncState* fs, int A, OpCode op)
{
  luaK_getlabel(fs); /* those instructions may be jump targets */
  return luaK_codeABC(fs, op, A, 0, 0);
}

/*
** check whether list has any jump that do not produce a value
** or produce an inverted value
*/
static int need_value(FuncState* fs, int list)
{
  for (; list != NO_JUMP; list = getjump(fs, list))
  {
    Instruction i = fs->f->code[getjumpcontrol(fs, list)];
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
static void exp2reg(FuncState* fs, expdesc* e, int reg)
{
  discharge2reg(fs, e, reg);
  if (e->k == VJMP)                    /* expression itself is a test? */
    e->t = luaK_concat(fs, e->t, e->info); /* put this jump in 't' list */
  if (hasjumps(e))
  {
    int final;         /* position after whole expression */
    int p_f = NO_JUMP; /* position of an eventual LOAD false */
    int p_t = NO_JUMP; /* position of an eventual LOAD true */
    if (need_value(fs, e->t) || need_value(fs, e->f))
    {
      int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
      p_f = code_loadbool(fs, reg, OP_LFALSESKIP); /* skip next inst. */
      p_t = code_loadbool(fs, reg, OP_LOADTRUE);
      /* jump around these booleans if 'e' is not a test */
      luaK_patchtohere(fs, fj);
    }
    final = luaK_getlabel(fs);
    patchlistaux(fs, e->f, final, reg, p_f);
    patchlistaux(fs, e->t, final, reg, p_t);
  }
  e->f = e->t = NO_JUMP;
  e->info = reg;
  e->k = VNONRELOC;
}

/*
** Ensures final expression result is in next available register.
*/
void luaK_exp2nextreg(FuncState* fs, expdesc* e)
{
  luaK_dischargevars(fs, e);
  freeexp(fs, e);
  luaK_reserveregs(fs, 1);
  exp2reg(fs, e, fs->freereg - 1);
}

/*
** Ensures final expression result is in some (any) register
** and return that register.
*/
int luaK_exp2anyreg(FuncState* fs, expdesc* e)
{
  luaK_dischargevars(fs, e);
  if (e->k == VNONRELOC)
  {                     /* expression already has a register? */
    if (!hasjumps(e))   /* no jumps? */
      return e->info; /* result is already in a register */
    if (e->info >= luaY_nvarstack(fs))
    {                            /* reg. is not a local? */
      exp2reg(fs, e, e->info); /* put final result in it */
      return e->info;
    }
    /* else expression has jumps and cannot change its register
       to hold the jump values, because it is a local variable.
       Go through to the default case. */
  }
  luaK_exp2nextreg(fs, e); /* default: use next available register */
  return e->info;
}

/*
** Ensures final expression result is either in a register,
** in an upvalue, or it is the vararg parameter.
*/
void luaK_exp2anyregup(FuncState* fs, expdesc* e)
{
  if ((e->k != VUPVAL && e->k != VVARGVAR) || hasjumps(e))
    luaK_exp2anyreg(fs, e);
}

/*
** Ensures final expression result is either in a register
** or it is a constant.
*/
void luaK_exp2val(FuncState* fs, expdesc* e)
{
  if (e->k == VJMP || hasjumps(e))
    luaK_exp2anyreg(fs, e);
  else
    luaK_dischargevars(fs, e);
}

/*
** Try to make 'e' a K expression with an index in the range of R/K
** indices. Return true iff succeeded.
*/
static int luaK_exp2K(FuncState* fs, expdesc* e)
{
  if (!hasjumps(e))
  {
    int info;
    switch (e->k)
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
      info = luaK_intK(fs, e->ival);
      break;
    case VKFLT:
      info = luaK_numberK(fs, e->nval);
      break;
    case VKSTR:
      info = stringK(fs, e->strval);
      break;
    case VK:
      info = e->info;
      break;
    default:
      return 0; /* not a constant */
    }
    if (info <= MAXINDEXRK)
    {            /* does constant fit in 'argC'? */
      e->k = VK; /* make expression a 'K' expression */
      e->info = info;
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
static int exp2RK(FuncState* fs, expdesc* e)
{
  if (luaK_exp2K(fs, e))
    return 1;
  else
  { /* not a constant in the right range: put it in a register */
    luaK_exp2anyreg(fs, e);
    return 0;
  }
}

static void codeABRK(FuncState* fs, OpCode o, int A, int B,
                     expdesc* ec)
{
  int k = exp2RK(fs, ec);
  luaK_codeABCk(fs, o, A, B, ec->info, k);
}

/*
** Generate code to store result of expression 'ex' into variable 'var'.
*/
void luaK_storevar(FuncState* fs, expdesc* var, expdesc* ex)
{
  switch (var->k)
  {
  case VLOCAL:
  {
    freeexp(fs, ex);
    exp2reg(fs, ex, var->var.ridx); /* compute 'ex' into proper place */
    return;
  }
  case VUPVAL:
  {
    int e = luaK_exp2anyreg(fs, ex);
    luaK_codeABC(fs, OP_SETUPVAL, e, var->info, 0);
    break;
  }
  case VINDEXUP:
  {
    codeABRK(fs, OP_SETTABUP, var->ind.t, var->ind.idx, ex);
    break;
  }
  case VINDEXI:
  {
    codeABRK(fs, OP_SETI, var->ind.t, var->ind.idx, ex);
    break;
  }
  case VINDEXSTR:
  {
    codeABRK(fs, OP_SETFIELD, var->ind.t, var->ind.idx, ex);
    break;
  }
  case VVARGIND:
  {
    needvatab(fs->f); /* function will need a vararg table */
    /* now, assignment is to a regular table */
  } /* FALLTHROUGH */
  case VINDEXED:
  {
    codeABRK(fs, OP_SETTABLE, var->ind.t, var->ind.idx, ex);
    break;
  }
  default:
    assert(0); /* invalid var kind to store */
  }
  freeexp(fs, ex);
}

/*
** Negate condition 'e' (where 'e' is a comparison).
*/
static void negatecondition(FuncState* fs, expdesc* e)
{
  int pc = getjumpcontrol(fs, e->info);
  assert(testTMode(GET_OPCODE(fs->f->code[pc])) && GET_OPCODE(fs->f->code[pc]) != OP_TESTSET &&
             GET_OPCODE(fs->f->code[pc]) != OP_TEST);
  fs->f->code[pc] = ((fs->f->code[pc] & MASK0(1, POS_k)) | ((((Instruction)((GETARG_k(fs->f->code[pc]) ^ 1))) << POS_k) & MASK1(1, POS_k)));
}

/*
** Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
** is true, code will jump if 'e' is true.) Return jump position.
** Optimize when 'e' is 'not' something, inverting the condition
** and removing the 'not'.
*/
static int jumponcond(FuncState* fs, expdesc* e, int cond)
{
  if (e->k == VRELOC)
  {
    Instruction ie = fs->f->code[e->info];
    if (GET_OPCODE(ie) == OP_NOT)
    {
      removelastinstruction(fs); /* remove previous OP_NOT */
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, 0, !cond);
    }
    /* else go through */
  }
  discharge2anyreg(fs, e);
  freeexp(fs, e);
  return condjump(fs, OP_TESTSET, NO_REG, e->info, 0, cond);
}

/*
** Emit code to go through if 'e' is true, jump otherwise.
*/
void luaK_goiftrue(FuncState* fs, expdesc* e)
{
  int pc; /* pc of new jump */
  luaK_dischargevars(fs, e);
  switch (e->k)
  {
  case VJMP:
  {                         /* condition? */
    negatecondition(fs, e); /* jump when it is false */
    pc = e->info;         /* save jump position */
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
  e->f = luaK_concat(fs, e->f, pc); /* insert new jump in false list */
  luaK_patchtohere(fs, e->t); /* true list jumps to here (to go through) */
  e->t = NO_JUMP;
}

/*
** Emit code to go through if 'e' is false, jump otherwise.
*/
static void luaK_goiffalse(FuncState* fs, expdesc* e)
{
  int pc; /* pc of new jump */
  luaK_dischargevars(fs, e);
  switch (e->k)
  {
  case VJMP:
  {
    pc = e->info; /* already jump if true */
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
  e->t = luaK_concat(fs, e->t, pc); /* insert new jump in 't' list */
  luaK_patchtohere(fs, e->f); /* false list jumps to here (to go through) */
  e->f = NO_JUMP;
}

/*
** Code 'not e', doing constant folding.
*/
static void codenot(FuncState* fs, expdesc* e)
{
  switch (e->k)
  {
  case VNIL:
  case VFALSE:
  {
    e->k = VTRUE; /* true == not nil == not false */
    break;
  }
  case VK:
  case VKFLT:
  case VKINT:
  case VKSTR:
  case VTRUE:
  {
    e->k = VFALSE; /* false == not "x" == not 0.5 == not 1 == not true */
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
    e->info = luaK_codeABC(fs, OP_NOT, 0, e->info, 0);
    e->k = VRELOC;
    break;
  }
  default:
    assert(0); /* cannot happen */
  }
  /* interchange true and false lists */
  {
    int temp = e->f;
    e->f = e->t;
    e->t = temp;
  }
  removevalues(fs, e->f); /* values are useless when negated */
  removevalues(fs, e->t);
}

/*
** Check whether expression 'e' is a short literal string
*/
static int isKstr(FuncState* fs, expdesc* e)
{
  return (e->k == VK && !hasjumps(e) && e->info <= MAXINDEXRK &&
          ((fs->f->k[e->info].tt_ & NOVARIANT_MASK) == LUA_TSTRING));
}

/*
** Check whether expression 'e' is a literal integer.
*/
static int isKint(expdesc* e)
{
  return (e->k == VKINT && !hasjumps(e));
}

/*
** Check whether expression 'e' is a literal integer in
** proper range to fit in register C
*/
static int isCint(expdesc* e)
{
  return isKint(e) && (0 <= e->ival && e->ival <= MAXARG_C);
}

/*
** Check whether expression 'e' is a literal integer in
** proper range to fit in register sC
*/
static int isSCint(expdesc* e)
{
  return isKint(e) && fitsC(e->ival);
}

/*
** Check whether expression 'e' is a literal integer or float in
** proper range to fit in a register (sB or sC).
*/
struct SCImm
{
  int im;
  int isfloat;
};

static int isSCnumber(expdesc* e, SCImm* sc)
{
  IntResult i;
  if (e->k == VKINT)
    i.v = e->ival;
  else if (e->k == VKFLT && luaV_flttointeger(e->nval, &i, F2Ieq))
    sc->isfloat = 1;
  else
    return 0; /* not a number */
  if (!hasjumps(e) && fitsC(i.v))
  {
    sc->im = int2sC(((int)(i.v)));
    return 1;
  }
  else
    return 0;
}

/*
** Emit SELF instruction or equivalent: the code will convert
** expression 'e' into 'e.key(e,'.
*/
void luaK_self(FuncState* fs, expdesc* e, expdesc* key)
{
  int ereg, base;
  luaK_exp2anyreg(fs, e);
  ereg = e->info; /* register where 'e' (the receiver) was placed */
  freeexp(fs, e);
  base = e->info = fs->freereg; /* base register for op_self */
  e->k = VNONRELOC;               /* self expression has a fixed register */
  luaK_reserveregs(fs, 2);        /* method and 'self' produced by op_self */
  assert(key->k == VKSTR);
  if (luaK_exp2K(fs, key))
  {
    /* can use 'self' opcode */
    luaK_codeABCk(fs, OP_SELF, base, ereg, key->info, 0);
  }
  else
  {                                                         /* cannot use 'self' opcode; use move+gettable */
    luaK_exp2anyreg(fs, key);                               /* put method name in a register */
    luaK_codeABC(fs, OP_MOVE, base + 1, ereg, 0);           /* copy self to base+1 */
    luaK_codeABC(fs, OP_GETTABLE, base, ereg, key->info); /* get method */
  }
  freeexp(fs, key);
}

/* auxiliary function to define indexing expressions */
static void fillidxk(expdesc* t, int idx, expkind k)
{
  t->ind.idx = idx;
  t->k = k;
}

/*
** Create expression 't[k]'. 't' must have its final result already in a
** register or upvalue. Upvalues can only be indexed by literal strings.
** Keys can be literal strings in the constant table or arbitrary
** values in registers.
*/
void luaK_indexed(FuncState* fs, expdesc* t, expdesc* k)
{
  int keystr = -1;
  if (k->k == VKSTR)
    keystr = str2K(fs, k);
  assert(!hasjumps(t) &&
             (t->k == VLOCAL || t->k == VVARGVAR ||
              t->k == VNONRELOC || t->k == VUPVAL));
  if (t->k == VUPVAL && !isKstr(fs, k)) /* upvalue indexed by non 'Kstr'? */
    luaK_exp2anyreg(fs, t);             /* put it in a register */
  if (t->k == VUPVAL)
  {
    int temp = t->info; /* upvalue index */
    t->ind.t = temp;                   /* (avoid a direct assignment; values overlap) */
    assert(isKstr(fs, k));
    fillidxk(t, k->info, VINDEXUP); /* literal short string */
  }
  else if (t->k == VVARGVAR)
  {                                          /* indexing the vararg parameter? */
    int kreg = luaK_exp2anyreg(fs, k);       /* put key in some register */
    int vreg = t->var.ridx; /* register with vararg param. */
    assert(vreg == fs->f->numparams);
    t->ind.t = vreg;           /* (avoid a direct assignment; values may overlap?) */
    fillidxk(t, kreg, VVARGIND); /* 't' represents 'vararg[k]' */
  }
  else
  {
    /* register index of the table */
    int temp = (t->k == VLOCAL) ? t->var.ridx : t->info;
    t->ind.t = temp; /* (avoid a direct assignment; values may overlap?) */
    if (isKstr(fs, k))
      fillidxk(t, k->info, VINDEXSTR); /* literal short string */
    else if (isCint(k))                  /* int. constant in proper range? */
      fillidxk(t, ((int)(k->ival)), VINDEXI);
    else
      fillidxk(t, luaK_exp2anyreg(fs, k), VINDEXED); /* register */
  }
  t->ind.keystr = keystr; /* string index in 'k' */
  t->ind.ro = 0;          /* by default, not read-only */
}

/*
** Return false if folding can raise an error.
** Bitwise operations need operands convertible to integers; division
** operations cannot have 0 as divisor.
*/
static int validop(int op, TValue* v1, TValue* v2)
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
    IntResult i;
    return (luaV_tointegerns(v1, &i, LUA_FLOORN2I) &&
            luaV_tointegerns(v2, &i, LUA_FLOORN2I));
  }
  case LUA_OPDIV:
  case LUA_OPIDIV:
  case LUA_OPMOD: /* division by 0 */
  {
    assert((v2->tt_ & NOVARIANT_MASK) == LUA_TNUMBER);
    lua_Number n2 = (v2->tt_ == LUA_VNUMINT) ? (lua_Number)ivalue(v2) : fltvalue(v2);
    return n2 != 0;
  }
  default:
    return 1; /* everything else is valid */
  }
}

/*
** Try to "constant-fold" an operation; return 1 iff successful.
** (In this case, 'e1' has the final result.)
*/
static int constfolding(FuncState* fs, int op, expdesc* e1,
                        const expdesc* e2)
{
  TValue v1, v2, res;
  if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
    return 0;                                   /* non-numeric operands or not safe to fold */
  luaO_rawarith(fs->ls->L, op, &v1, &v2, &res); /* does operation */
  if (res.tt_ == LUA_VNUMINT)
  {
    e1->k = VKINT;
    assert(res.tt_ == LUA_VNUMINT);
    e1->ival = ivalue(&res);
  }
  else
  { /* folds neither NaN nor 0.0 (to avoid problems with -0.0) */
    assert(res.tt_ == LUA_VNUMFLT);
    lua_Number n = fltvalue(&res);
    if ((!(n == n)) || n == 0)
      return 0;
    e1->k = VKFLT;
    e1->nval = n;
  }
  return 1;
}

/*
** Convert a BinOpr to an OpCode  (ORDER OPR - ORDER OP)
*/
static inline OpCode binopr2op(BinOpr opr, BinOpr baser, OpCode base)
{
  assert(baser <= opr &&
             ((baser == OPR_ADD && opr <= OPR_SHR) ||
              (baser == OPR_LT && opr <= OPR_LE)));
  return (OpCode)((((int)(opr)) - ((int)(baser))) + ((int)(base)));
}

/*
** Convert a UnOpr to an OpCode  (ORDER OPR - ORDER OP)
*/
static inline OpCode unopr2op(UnOpr opr)
{
  return (OpCode)((((int)(opr)) - ((int)(OPR_MINUS))) +
                          ((int)(OP_UNM)));
}

/*
** Convert a BinOpr to a tag method  (ORDER OPR - ORDER TM)
*/
static inline TMS binopr2TM(BinOpr opr)
{
  assert(OPR_ADD <= opr && opr <= OPR_SHR);
  return (TMS)((((int)(opr)) - ((int)(OPR_ADD))) + ((int)(TM_ADD)));
}

/*
** Emit code for unary expressions that "produce values"
** (everything but 'not').
** Expression to produce final result will be encoded in 'e'.
*/
static void codeunexpval(FuncState* fs, OpCode op, expdesc* e, int line)
{
  int r = luaK_exp2anyreg(fs, e); /* opcodes operate only on registers */
  freeexp(fs, e);
  e->info = luaK_codeABC(fs, op, 0, r, 0); /* generate opcode */
  e->k = VRELOC;                             /* all those operations are relocatable */
  luaK_fixline(fs, line);
}

/*
** Emit code for binary expressions that "produce values"
** (everything but logical operators 'and'/'or' and comparison
** operators).
** Expression to produce final result will be encoded in 'e1'.
*/
static void finishbinexpval(FuncState* fs, expdesc* e1, expdesc* e2,
                            OpCode op, int v2, int flip, int line,
                            OpCode mmop, TMS event)
{
  int v1 = luaK_exp2anyreg(fs, e1);
  int pc = luaK_codeABCk(fs, op, 0, v1, v2, 0);
  freeexps(fs, e1, e2);
  e1->info = pc;
  e1->k = VRELOC; /* all those operations are relocatable */
  luaK_fixline(fs, line);
  luaK_codeABCk(fs, mmop, v1, v2, ((int)(event)), flip); /* metamethod */
  luaK_fixline(fs, line);
}

/*
** Emit code for binary expressions that "produce values" over
** two registers.
*/
static void codebinexpval(FuncState* fs, BinOpr opr,
                          expdesc* e1, expdesc* e2, int line)
{
  OpCode op = binopr2op(opr, OPR_ADD, OP_ADD);
  int v2 = luaK_exp2anyreg(fs, e2); /* make sure 'e2' is in a register */
  /* 'e1' must be already in a register or it is a constant */
  assert((VNIL <= e1->k && e1->k <= VKSTR) ||
             e1->k == VNONRELOC || e1->k == VRELOC);
  assert(OP_ADD <= op && op <= OP_SHR);
  finishbinexpval(fs, e1, e2, op, v2, 0, line, OP_MMBIN, binopr2TM(opr));
}

/*
** Code binary operators with immediate operands.
*/
static void codebini(FuncState* fs, OpCode op,
                     expdesc* e1, expdesc* e2, int flip, int line,
                     TMS event)
{
  int v2 = int2sC(((int)(e2->ival))); /* immediate operand */
  assert(e2->k == VKINT);
  finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINI, event);
}

/*
** Code binary operators with K operand.
*/
static void codebinK(FuncState* fs, BinOpr opr,
                     expdesc* e1, expdesc* e2, int flip, int line)
{
  TMS event = binopr2TM(opr);
  int v2 = e2->info; /* K index */
  OpCode op = binopr2op(opr, OPR_ADD, OP_ADDK);
  finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINK, event);
}

/* Try to code a binary operator negating its second operand.
** For the metamethod, 2nd operand must keep its original value.
*/
static int finishbinexpneg(FuncState* fs, expdesc* e1, expdesc* e2,
                           OpCode op, int line, TMS event)
{
  if (!isKint(e2))
    return 0; /* not an integer constant */
  else
  {
    lua_Integer i2 = e2->ival;
    if (!(fitsC(i2) && fitsC(-i2)))
      return 0; /* not in the proper range */
    else
    { /* operating a small integer constant */
      int v2 = ((int)(i2));
      finishbinexpval(fs, e1, e2, op, int2sC(-v2), 0, line, OP_MMBINI, event);
      /* correct metamethod argument */
      fs->f->code[fs->pc - 1] = ((fs->f->code[fs->pc - 1] & MASK0(SIZE_B, POS_B)) | ((((Instruction)(int2sC(v2))) << POS_B) & MASK1(SIZE_B, POS_B)));
      return 1; /* successfully coded */
    }
  }
}

static void swapexps(expdesc* e1, expdesc* e2)
{
  expdesc temp = *e1;
  *e1 = *e2;
  *e2 = temp; /* swap 'e1' and 'e2' */
}

/*
** Code binary operators with no constant operand.
*/
static void codebinNoK(FuncState* fs, BinOpr opr,
                       expdesc* e1, expdesc* e2, int flip, int line)
{
  if (flip)
    swapexps(e1, e2);                   /* back to original order */
  codebinexpval(fs, opr, e1, e2, line); /* use standard operators */
}

/*
** Code arithmetic operators ('+', '-', ...). If second operand is a
** constant in the proper range, use variant opcodes with K operands.
*/
static void codearith(FuncState* fs, BinOpr opr,
                      expdesc* e1, expdesc* e2, int flip, int line)
{
  if (isnumeral(e2) && luaK_exp2K(fs, e2)) /* K operand? */
    codebinK(fs, opr, e1, e2, flip, line);
  else /* 'e2' is neither an immediate nor a K operand */
    codebinNoK(fs, opr, e1, e2, flip, line);
}

/*
** Code commutative operators ('+', '*'). If first operand is a
** numeric constant, change order of operands to try to use an
** immediate or K operator.
*/
static void codecommutative(FuncState* fs, BinOpr op,
                            expdesc* e1, expdesc* e2, int line)
{
  int flip = 0;
  if (isnumeral(e1))
  {                   /* is first operand a numeric constant? */
    swapexps(e1, e2); /* change order */
    flip = 1;
  }
  if (op == OPR_ADD && isSCint(e2)) /* immediate operand? */
    codebini(fs, OP_ADDI, e1, e2, flip, line, TM_ADD);
  else
    codearith(fs, op, e1, e2, flip, line);
}

/*
** Code bitwise operations; they are all commutative, so the function
** tries to put an integer constant as the 2nd operand (a K operand).
*/
static void codebitwise(FuncState* fs, BinOpr opr,
                        expdesc* e1, expdesc* e2, int line)
{
  int flip = 0;
  if (e1->k == VKINT)
  {
    swapexps(e1, e2); /* 'e2' will be the constant operand */
    flip = 1;
  }
  if (e2->k == VKINT && luaK_exp2K(fs, e2)) /* K operand? */
    codebinK(fs, opr, e1, e2, flip, line);
  else /* no constants */
    codebinNoK(fs, opr, e1, e2, flip, line);
}

/*
** Emit code for order comparisons. When using an immediate operand,
** 'isfloat' tells whether the original value was a float.
*/
static void codeorder(FuncState* fs, BinOpr opr, expdesc* e1, expdesc* e2)
{
  int r1, r2;
  SCImm sc = {0, 0};
  OpCode op;
  if (isSCnumber(e2, &sc))
  {
    /* use immediate operand */
    r1 = luaK_exp2anyreg(fs, e1);
    r2 = sc.im;
    op = binopr2op(opr, OPR_LT, OP_LTI);
  }
  else if (isSCnumber(e1, &sc))
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
  e1->info = condjump(fs, op, r1, r2, sc.isfloat, 1);
  e1->k = VJMP;
}

/*
** Emit code for equality comparisons ('==', '~=').
** 'e1' was already put as RK by 'luaK_infix'.
*/
static void codeeq(FuncState* fs, BinOpr opr, expdesc* e1, expdesc* e2)
{
  int r1, r2;
  SCImm sc = {0, 0}; /* not needed here, but kept for symmetry */
  OpCode op;
  if (e1->k != VNONRELOC)
  {
    assert(e1->k == VK || e1->k == VKINT || e1->k == VKFLT);
    swapexps(e1, e2);
  }
  r1 = luaK_exp2anyreg(fs, e1); /* 1st expression must be in register */
  if (isSCnumber(e2, &sc))
  {
    op = OP_EQI;
    r2 = sc.im; /* immediate operand */
  }
  else if (exp2RK(fs, e2))
  { /* 2nd expression is constant? */
    op = OP_EQK;
    r2 = e2->info; /* constant index */
  }
  else
  {
    op = OP_EQ; /* will compare two registers */
    r2 = luaK_exp2anyreg(fs, e2);
  }
  freeexps(fs, e1, e2);
  e1->info = condjump(fs, op, r1, r2, sc.isfloat, (opr == OPR_EQ));
  e1->k = VJMP;
}

/*
** Apply prefix operation 'op' to expression 'e'.
*/
void luaK_prefix(FuncState* fs, UnOpr opr, expdesc* e, int line)
{
  static const expdesc ef = {VKINT, 0, 0, nullptr, 0, {0, 0, 0, 0}, {0, 0}, NO_JUMP, NO_JUMP};
  luaK_dischargevars(fs, e);
  switch (opr)
  {
  case OPR_MINUS:
  case OPR_BNOT: /* use 'ef' as fake 2nd operand */
    if (constfolding(fs, ((int)(opr + LUA_OPUNM)), e, &ef))
      break;
    /* else */ /* FALLTHROUGH */
  case OPR_LEN:
    codeunexpval(fs, unopr2op(opr), e, line);
    break;
  case OPR_NOT:
    codenot(fs, e);
    break;
  default:
    assert(0);
  }
}

/*
** Process 1st operand 'v' of binary operation 'op' before reading
** 2nd operand.
*/
void luaK_infix(FuncState* fs, BinOpr op, expdesc* v)
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
    if (!isnumeral(v))
      luaK_exp2anyreg(fs, v);
    /* else keep numeral, which may be folded or used as an immediate
       operand */
    break;
  }
  case OPR_EQ:
  case OPR_NE:
  {
    if (!isnumeral(v))
      exp2RK(fs, v);
    /* else keep numeral, which may be an immediate operand */
    break;
  }
  case OPR_LT:
  case OPR_LE:
  case OPR_GT:
  case OPR_GE:
  {
    SCImm dummy = {0, 0};
    if (!isSCnumber(v, &dummy))
      luaK_exp2anyreg(fs, v);
    /* else keep numeral, which may be an immediate operand */
    break;
  }
  default:
    assert(0);
  }
}

/*
** Create code for '(e1 .. e2)'.
** For '(e1 .. e2.1 .. e2.2)' (which is '(e1 .. (e2.1 .. e2.2))',
** because concatenation is right associative), merge both CONCATs.
*/
static void codeconcat(FuncState* fs, expdesc* e1, expdesc* e2, int line)
{
  int ie2 = previousinstruction(fs);
  if (ie2 >= 0 && GET_OPCODE(fs->f->code[ie2]) == OP_CONCAT)
  {                         /* is 'e2' a concatenation? */
    int n = GETARG_B(fs->f->code[ie2]); /* # of elements concatenated in 'e2' */
    assert(e1->info + 1 == GETARG_A(fs->f->code[ie2]));
    freeexp(fs, e2);
    fs->f->code[ie2] = ((fs->f->code[ie2] & MASK0(SIZE_A, POS_A)) | ((((Instruction)(e1->info)) << POS_A) & MASK1(SIZE_A, POS_A))); /* correct first element ('e1') */
    fs->f->code[ie2] = ((fs->f->code[ie2] & MASK0(SIZE_B, POS_B)) | ((((Instruction)(n + 1)) << POS_B) & MASK1(SIZE_B, POS_B)));      /* will concatenate one more element */
  }
  else
  {                                                /* 'e2' is not a concatenation */
    luaK_codeABC(fs, OP_CONCAT, e1->info, 2, 0); /* new concat opcode */
    freeexp(fs, e2);
    luaK_fixline(fs, line);
  }
}

/*
** Finalize code for binary operation, after reading 2nd operand.
*/
void luaK_posfix(FuncState* fs, BinOpr opr,
                 expdesc* e1, expdesc* e2, int line)
{
  luaK_dischargevars(fs, e2);
  if (foldbinop(opr) && constfolding(fs, ((int)(opr + LUA_OPADD)), e1, e2))
    return; /* done by folding */
  switch (opr)
  {
  case OPR_AND:
  {
    assert(e1->t == NO_JUMP); /* list closed by 'luaK_infix' */
    e2->f = luaK_concat(fs, e2->f, e1->f);
    *e1 = *e2;
    break;
  }
  case OPR_OR:
  {
    assert(e1->f == NO_JUMP); /* list closed by 'luaK_infix' */
    e2->t = luaK_concat(fs, e2->t, e1->t);
    *e1 = *e2;
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
    if (finishbinexpneg(fs, e1, e2, OP_ADDI, line, TM_SUB))
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
    if (isSCint(e1))
    {
      swapexps(e1, e2);
      codebini(fs, OP_SHLI, e1, e2, 1, line, TM_SHL); /* I << r2 */
    }
    else if (finishbinexpneg(fs, e1, e2, OP_SHRI, line, TM_SHL))
    {
      /* coded as (r1 >> -I) */;
    }
    else /* regular case (two registers) */
      codebinexpval(fs, opr, e1, e2, line);
    break;
  }
  case OPR_SHR:
  {
    if (isSCint(e2))
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
    opr = ((BinOpr)((opr - OPR_GT) + OPR_LT));
  } /* FALLTHROUGH */
  case OPR_LT:
  case OPR_LE:
  {
    codeorder(fs, opr, e1, e2);
    break;
  }
  default:
    assert(0);
  }
}

/*
** Change line information associated with current position, by removing
** previous info and adding it again with new line.
*/
void luaK_fixline(FuncState* fs, int line)
{
  removelastlineinfo(fs);
  savelineinfo(fs, fs->f, line);
}

void luaK_settablesize(FuncState* fs, int pc, int ra, int asize, int hsize)
{
  int inst = pc;
  int extra = asize / (MAXARG_vC + 1); /* higher bits of array size */
  int rc = asize % (MAXARG_vC + 1);    /* lower bits of array size */
  int k = (extra > 0);                 /* true iff needs extra argument */
  hsize = (hsize != 0) ? luaO_ceillog2(hsize) + 1 : 0;
  fs->f->code[inst] = CREATE_vABCk(OP_NEWTABLE, ra, hsize, rc, k);
  fs->f->code[inst + 1] = CREATE_Ax(OP_EXTRAARG, extra);
}

/*
** Emit a SETLIST instruction.
** 'base' is register that keeps table;
** 'nelems' is #table plus those to be stored now;
** 'tostore' is number of values (in registers 'base + 1',...) to add to
** table (or LUA_MULTRET to add up to stack top).
*/
void luaK_setlist(FuncState* fs, int base, int nelems, int tostore)
{
  assert(tostore != 0);
  if (tostore == LUA_MULTRET)
    tostore = 0;
  if (nelems <= MAXARG_vC)
    luaK_codevABCk(fs, OP_SETLIST, base, tostore, nelems, 0);
  else
  {
    int extra = nelems / (MAXARG_vC + 1);
    nelems %= (MAXARG_vC + 1);
    luaK_codevABCk(fs, OP_SETLIST, base, tostore, nelems, 1);
    codeextraarg(fs, extra);
  }
  fs->freereg = base + 1; /* free registers with list values */
}

/*
** return the final target of a jump (skipping jumps to jumps)
*/
static int finaltarget(std::span<const Instruction> code, int i)
{
  int count;
  for (count = 0; count < 100; count++)
  { /* avoid infinite loops */
    Instruction pc = code[i];
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
#include "lopnames.h"
#include "literals.h"
void luaK_finish(FuncState* fs)
{
  int i;
  Proto* p = fs->f;
  if (p->flag & PF_VATAB)            /* will it use a vararg table? */
    p->flag &= ((lu_byte)(~PF_VAHID)); /* then it will not use hidden args. */
  for (i = 0; i < fs->pc; i++)
  {
    (void) luaP_isOT;
    (void) luaP_isIT;
    assert(i == 0 || luaP_isOT(p->code[i - 1]) == luaP_isIT(p->code[i]));
    switch (GET_OPCODE(p->code[i]))
    {
    case OP_RETURN0:
    case OP_RETURN1:
    {
      if (!(fs->needclose || (p->flag & PF_VAHID)))
        break; /* no extra work */
      /* else use OP_RETURN to do the extra work */
      p->code[i] = ((p->code[i] & MASK0(SIZE_OP, POS_OP)) | ((((Instruction)(OP_RETURN)) << POS_OP) & MASK1(SIZE_OP, POS_OP)));
    } /* FALLTHROUGH */
    case OP_RETURN:
    case OP_TAILCALL:
    {
      if (fs->needclose)
        p->code[i] = ((p->code[i] & MASK0(1, POS_k)) | ((((Instruction)(1)) << POS_k) & MASK1(1, POS_k)));                /* signal that it needs to close */
      if (p->flag & PF_VAHID)            /* does it use hidden arguments? */
        p->code[i] = ((p->code[i] & MASK0(SIZE_C, POS_C)) | ((((Instruction)(p->numparams + 1)) << POS_C) & MASK1(SIZE_C, POS_C))); /* signal that */
      break;
    }
    case OP_GETVARG:
    {
      if (p->flag & PF_VATAB)         /* function has a vararg table? */
        p->code[i] = ((p->code[i] & MASK0(SIZE_OP, POS_OP)) | ((((Instruction)(OP_GETTABLE)) << POS_OP) & MASK1(SIZE_OP, POS_OP))); /* must get vararg there */
      break;
    }
    case OP_VARARG:
    {
      if (p->flag & PF_VATAB) /* function has a vararg table? */
        p->code[i] = ((p->code[i] & MASK0(1, POS_k)) | ((((Instruction)(1)) << POS_k) & MASK1(1, POS_k)));     /* must get vararg there */
      break;
    }
    case OP_JMP:
    { /* to optimize jumps to jumps */
      int target = finaltarget(p->code, i);
      fixjump(fs, i, target); /* jump directly to final target */
      break;
    }
    default:
      break;
    }
  }
}
