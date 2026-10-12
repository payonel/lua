/*
** $Id: lparser.c $
** Lua Parser
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"

#include "lhack.h"
#include "literals.h"
#include "lload.h"

namespace lua::compiler
{

/*
** A load that a freeze can stop (see lload.h): a function with calls a
** freeze can pass through runs as a state machine, its state and its
** locals in its frame ('l'). Each such call is made at a state of its own,
** the one its function goes to again when it is made again after the
** thaw (the call made again, which goes on where it stopped); a freeze
** passing records the frame. A function's frame may hold pointers into
** its callers' frames (a chain of blocks or functions, the table of a
** constructor): made again, it links them again.
*/

/* maximum number of variable declarations per function (must be
   smaller than 250, due to the bytecode format) */
constexpr int MAXVARS = 200;

constexpr int hasmultret(expkind k)
{
  return k == VCALL || k == VVARARG;
}

/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
static inline int eqstr(const TString* a, const TString* b)
{
  return a == b;
}

/*
** nodes for block list (list of active blocks)
*/
struct BlockCnt
{
  struct BlockCnt* previous; /* chain */
  int firstlabel;            /* index of first label in this block */
  int firstgoto;             /* index of first pending goto in this block */
  short nactvar;             /* number of active declarations at block entry */
  lu_byte upval;             /* true if some variable in the block is an upvalue */
  lu_byte isloop;            /* 1 if 'block' is a loop; 2 if it has pending breaks */
  lu_byte insidetbc;         /* true if inside the scope of a to-be-closed var. */
};

/*
** prototypes for recursive non-terminal functions
*/
static void statement(LexState* ls);
static void expr(LexState* ls, expdesc* v);

[[noreturn]] static void error_expected(LexState* ls, int token)
{
  luaX_syntaxerror(ls, luaO_pushfstring(ls->L, lt::s_expected, FmtArgs().str(luaX_token2str(ls, token)))->view());
}

[[noreturn]] static void errorlimit(FuncState* fs, int limit, byte_view what)
{
  lua_State* L = fs->ls->L;
  const InternalString* msg;
  int line = fs->f->linedefined;
  byte_view where = (line == 0)
                          ? lt::main_function
                          : luaO_pushfstring(L, lt::function_at_line_d, FmtArgs().num(line))->view();
  msg = luaO_pushfstring(L, lt::too_many_s_limit_is_d_in_s, FmtArgs().str(what).num(limit).str(where));
  luaX_syntaxerror(fs->ls, msg->view());
}

void luaY_checklimit(FuncState* fs, int v, int l, byte_view what)
{
  if (l_unlikely(v > l))
    errorlimit(fs, l, what);
}

/*
** Test whether next token is 'c'; if so, skip it.
*/
/*
** (The calls below with no frame of their own, see lload.h, do nothing
** before the call that can stop but read what a stop leaves as it was:
** made again, they go the same way.)
*/
static int testnext(LexState* ls, int c)
{
  if (ls->t.token == c)
  {
    luaX_next(ls);
    return 1;
  }
  else
    return 0;
}

/*
** Check that next token is 'c'.
*/
static void check(LexState* ls, int c)
{
  if (ls->t.token != c)
    error_expected(ls, c);
}

/*
** Check that next token is 'c' and skip it.
*/
static void checknext(LexState* ls, int c)
{
  check(ls, c);
  luaX_next(ls);
}

static inline void check_condition(LexState* ls, int c, byte_view msg)
{
  if (!c)
    luaX_syntaxerror(ls, msg);
}

/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
static void check_match(LexState* ls, int what, int who, int where)
{
  if (l_unlikely(!testnext(ls, what)))
  {
    if (where == ls->linenumber) /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else
    {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
                                            lt::s_expected_to_close_s_at_line_d,
                                            FmtArgs()
                                                .str(luaX_token2str(ls, what))
                                                .str(luaX_token2str(ls, who))
                                                .num(where))->view());
    }
  }
}

static TString* str_checkname(LexState* ls)
{
  TString* ts;
  check(ls, TK_NAME);
  ts = ls->t.ts;
  luaX_next(ls);
  return ts;
}

static void init_exp(expdesc* e, expkind k, int i)
{
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->info = i;
}

static void codestring(expdesc* e, TString* s)
{
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->strval = s;
}

static void codename(LexState* ls, expdesc* e)
{
  codestring(e, str_checkname(ls));
}

/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
static short registerlocalvar(LexState* ls, FuncState* fs,
                              TString* varname)
{
  Proto* f = fs->f;
  int oldsize = (int)f->locvars.size();
  f->locvars = luaM_safegrow_LocVar_span(ls->L, f->locvars, fs->ndebugvars);
  while (oldsize < (int)f->locvars.size())
    f->locvars[oldsize++].varname = nullptr;
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}

/*
** Create a new variable with the given 'name' and given 'kind'.
** Return its index in the function.
*/
static int new_varkind(LexState* ls, TString* name, lu_byte kind)
{
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Dyndata* dyd = ls->dyd;
  Vardesc* var;
  {
    auto s = luaM_safegrow_Vardesc_span(L, std::span<Vardesc>(dyd->actvar.arr, dyd->actvar.size), dyd->actvar.n + 1);
    dyd->actvar.arr = s.data();
    dyd->actvar.size = (int)s.size();
  }
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->kind = kind; /* default */
  var->name = name;
  return dyd->actvar.n - 1 - fs->firstlocal;
}

/*
** Create a new local variable with the given 'name' and regular kind.
*/
static int new_localvar(LexState* ls, TString* name)
{
  return new_varkind(ls, name, VDKREG);
}

/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
static Vardesc* getlocalvardesc(FuncState* fs, int vidx)
{
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}

/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
static int reglevel(FuncState* fs, int nvar)
{
  while (nvar-- > 0)
  {
    Vardesc* vd = getlocalvardesc(fs, nvar); /* get previous variable */
    if (varinreg(vd))                        /* is in a register? */
      return vd->ridx + 1;
  }
  return 0; /* no variables in registers */
}

/*
** Return the number of variables in the register stack for the given
** function.
*/
int luaY_nvarstack(FuncState* fs)
{
  return reglevel(fs, fs->nactvar);
}

/*
** Get the debug-information entry for current variable 'vidx'.
*/
static LocVar* localdebuginfo(FuncState* fs, int vidx)
{
  Vardesc* vd = getlocalvardesc(fs, vidx);
  if (!varinreg(vd))
    return nullptr; /* no debug info. for constants */
  else
  {
    int idx = vd->pidx;
    assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}

/*
** Create an expression representing variable 'vidx'
*/
static void init_var(FuncState* fs, expdesc* e, int vidx)
{
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->var.vidx = (short)vidx;
  e->var.ridx = getlocalvardesc(fs, vidx)->ridx;
}

/*
** Raises an error if variable described by 'e' is read only; moreover,
** if 'e' is t[exp] where t is the vararg parameter, change it to index
** a real table. (Virtual vararg tables cannot be changed.)
*/
static void check_readonly(LexState* ls, expdesc* e)
{
  FuncState* fs = ls->fs;
  TString* varname = nullptr; /* to be set if variable is const */
  switch (e->k)
  {
  case VCONST:
  {
    varname = ls->dyd->actvar.arr[e->info].name;
    break;
  }
  case VLOCAL:
  case VVARGVAR:
  {
    Vardesc* vardesc = getlocalvardesc(fs, e->var.vidx);
    if (vardesc->kind != VDKREG) /* not a regular variable? */
      varname = vardesc->name;
    break;
  }
  case VUPVAL:
  {
    Upvaldesc* up = &fs->f->upvalues[e->info];
    if (up->kind != VDKREG)
      varname = up->name;
    break;
  }
  case VVARGIND:
  {
    needvatab(fs->f); /* function will need a vararg table */
    e->k = VINDEXED;
  } /* FALLTHROUGH */
  case VINDEXUP:
  case VINDEXSTR:
  case VINDEXED:
  {                  /* global variable */
    if (e->ind.ro) /* read-only? */
    {
      TValue* kv = &fs->f->k[e->ind.keystr];
      assert(ttisstring(kv));
      varname = tsvalue(kv);
    }
    break;
  }
  default:
    assert(e->k == VINDEXI); /* this one doesn't need any check */
    return;                      /* integer index cannot be read-only */
  }
  if (varname)
    luaK_semerror(ls, lt::attempt_to_assign_to_const_variable_s,
                  FmtArgs().str(varname->data.view()));
}

/*
** Start the scope for the last 'nvars' created variables.
*/
static void adjustlocalvars(LexState* ls, int nvars)
{
  FuncState* fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++)
  {
    int vidx = fs->nactvar++;
    Vardesc* var = getlocalvardesc(fs, vidx);
    var->ridx = reglevel++;
    var->pidx = registerlocalvar(ls, fs, var->name);
    luaY_checklimit(fs, reglevel, MAXVARS, lt::local_variables);
  }
}

/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
static void removevars(FuncState* fs, int tolevel)
{
  fs->ls->dyd->actvar.n -= fs->nactvar - tolevel;
  while (fs->nactvar > tolevel)
  {
    LocVar* var = localdebuginfo(fs, --fs->nactvar);
    if (var) /* does it have debug information? */
      var->endpc = fs->pc;
  }
}

/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
static int searchupvalue(FuncState* fs, TString* name)
{
  int i;
  Upvaldesc* up = fs->f->upvalues.data();
  for (i = 0; i < fs->nups; i++)
  {
    if (eqstr(up[i].name, name))
      return i;
  }
  return -1; /* not found */
}

static Upvaldesc* allocupvalue(FuncState* fs)
{
  Proto* f = fs->f;
  int oldsize = (int)f->upvalues.size();
  luaY_checklimit(fs, fs->nups + 1, MAXUPVAL, lt::upvalues);
  f->upvalues = luaM_safegrow_Upvaldesc_span(fs->ls->L, f->upvalues, fs->nups);
  while (oldsize < (int)f->upvalues.size())
    f->upvalues[oldsize++].name = nullptr;
  return &f->upvalues[fs->nups++];
}

static int newupvalue(FuncState* fs, TString* name, expdesc* v)
{
  Upvaldesc* up = allocupvalue(fs);
  FuncState* prev = fs->prev;
  if (v->k == VLOCAL)
  {
    up->instack = 1;
    up->idx = v->var.ridx;
    up->kind = getlocalvardesc(prev, v->var.vidx)->kind;
    assert(eqstr(name, getlocalvardesc(prev, v->var.vidx)->name));
  }
  else
  {
    up->instack = 0;
    up->idx = v->info;
    up->kind = prev->f->upvalues[v->info].kind;
    assert(eqstr(name, prev->f->upvalues[v->info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}

static int searchvar(FuncState* fs, TString* n, expdesc* var)
{
  int i;
  for (i = (int)fs->nactvar - 1; i >= 0; i--)
  {
    Vardesc* vd = getlocalvardesc(fs, i);
    if (varglobal(vd))
    { /* global declaration? */
      if (vd->name == nullptr)
      {                                     /* collective declaration? */
        if (var->info < 0)                /* no previous collective declaration? */
          var->info = fs->firstlocal + i; /* this is the first one */
      }
      else
      { /* global name */
        if (eqstr(n, vd->name))
        { /* found? */
          init_exp(var, VGLOBAL, fs->firstlocal + i);
          return VGLOBAL;
        }
        else if (var->info == -1) /* active preambular declaration? */
          var->info = -2;         /* invalidate preambular declaration */
      }
    }
    else if (eqstr(n, vd->name))
    {                            /* found? */
      if (vd->kind == RDKCTC) /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else
      { /* local variable */
        init_var(fs, var, i);
        if (vd->kind == RDKVAVAR) /* vararg parameter? */
          var->k = VVARGVAR;
      }
      return var->k;
    }
  }
  return -1; /* not found */
}

/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
static void markupval(FuncState* fs, int level)
{
  BlockCnt* bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
  fs->needclose = 1;
}

/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed(FuncState* fs)
{
  BlockCnt* bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}

/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
static void singlevaraux(FuncState* fs, TString* n, expdesc* var, int base)
{
  int v = searchvar(fs, n, var); /* look up variables at current level */
  if (v >= 0)
  { /* found? */
    if (!base)
    {
      if (var->k == VVARGVAR)      /* vararg parameter? */
        luaK_vapar2local(fs, var); /* change it to a regular local */
      if (var->k == VLOCAL)
        markupval(fs, var->var.vidx); /* will be used as an upvalue */
    }
    /* else nothing else to be done */
  }
  else
  {                                 /* not found at current level; try upvalues */
    int idx = searchupvalue(fs, n); /* try existing upvalues */
    if (idx < 0)
    {                                           /* not found? */
      if (fs->prev != nullptr)                     /* more levels? */
        singlevaraux(fs->prev, n, var, 0);      /* try upper levels */
      if (var->k == VLOCAL || var->k == VUPVAL) /* local or upvalue? */
        idx = newupvalue(fs, n, var);           /* will be a new upvalue */
      else                                      /* it is a global or a constant */
        return;                                 /* don't need to do anything at this level */
    }
    init_exp(var, VUPVAL, idx); /* new or old upvalue */
  }
}

static void buildglobal(LexState* ls, TString* varname, expdesc* var)
{
  FuncState* fs = ls->fs;
  expdesc key;
  init_exp(var, VGLOBAL, -1);         /* global by default */
  singlevaraux(fs, ls->envn, var, 1); /* get environment variable */
  if (var->k == VGLOBAL)
    luaK_semerror(ls, lt::s_is_global_when_accessing_variable_s,
                  FmtArgs().str(lt::_ENV).str(varname->data.view()));
  luaK_exp2anyregup(fs, var);  /* _ENV could be a constant */
  codestring(&key, varname);   /* key is variable name */
  luaK_indexed(fs, var, &key); /* 'var' represents _ENV[varname] */
}

/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
static void buildvar(LexState* ls, TString* varname, expdesc* var)
{
  FuncState* fs = ls->fs;
  init_exp(var, VGLOBAL, -1); /* global by default */
  singlevaraux(fs, varname, var, 1);
  if (var->k == VGLOBAL)
  { /* global name? */
    int info = var->info;
    /* global by default in the scope of a global declaration? */
    if (info == -2)
      luaK_semerror(ls, lt::variable_s_not_declared,
                    FmtArgs().str(varname->data.view()));
    buildglobal(ls, varname, var);
    if (info != -1 && ls->dyd->actvar.arr[info].kind == GDKCONST)
      var->ind.ro = 1; /* mark variable as read-only */
    else                 /* anyway must be a global */
      assert(info == -1 || ls->dyd->actvar.arr[info].kind == GDKREG);
  }
}

static void singlevar(LexState* ls, expdesc* var)
{
  buildvar(ls, str_checkname(ls), var);
}

/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
static void adjust_assign(LexState* ls, int nvars, int nexps, expdesc* e)
{
  FuncState* fs = ls->fs;
  int needed = nvars - nexps; /* extra values needed */
  luaK_checkstack(fs, needed);
  if (hasmultret(e->k))
  {                         /* last expression has multiple returns? */
    int extra = needed + 1; /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    luaK_setreturns(fs, e, extra); /* last exp. provides the difference */
  }
  else
  {
    if (e->k != VVOID)                   /* at least one expression? */
      luaK_exp2nextreg(fs, e);           /* close last expression */
    if (needed > 0)                      /* missing values? */
      luaK_nil(fs, fs->freereg, needed); /* complete with nils */
  }
  if (needed > 0)
    luaK_reserveregs(fs, needed);                  /* registers for extra values */
  else                                             /* adding 'needed' is actually a subtraction */
    fs->freereg = fs->freereg + needed; /* remove extra values */
}

static inline void enterlevel(LexState* ls)
{
  luaE_incCstack(ls->L);
}

static inline void leavelevel(LexState* ls)
{
  ls->L->nCcalls--;
}

/*
** Generates an error that a goto jumps into the scope of some
** variable declaration.
*/
[[noreturn]] static void jumpscopeerror(LexState* ls, Labeldesc* gt)
{
  TString* tsname = getlocalvardesc(ls->fs, gt->nactvar)->name;
  byte_view varname = (tsname != nullptr) ? tsname->data.view() : lt::star;
  luaK_semerror(ls, lt::goto_s_at_line_d_jumps_into_the_scope_of_s, FmtArgs().str(gt->name->data.view()).num(gt->line).str(varname)); /* raise the error */
}

/*
** Closes the goto at index 'g' to given 'label' and removes it
** from the list of pending gotos.
** If it jumps into the scope of some variable, raises an error.
** The goto needs a CLOSE if it jumps out of a block with upvalues,
** or out of the scope of some variable and the block has upvalues
** (signaled by parameter 'bup').
*/
static void closegoto(LexState* ls, int g, Labeldesc* label, int bup)
{
  int i;
  FuncState* fs = ls->fs;
  Labellist* gl = &ls->dyd->gt; /* list of gotos */
  Labeldesc* gt = &gl->arr[g];  /* goto to be resolved */
  assert(eqstr(gt->name, label->name));
  if (l_unlikely(gt->nactvar < label->nactvar)) /* enter some scope? */
    jumpscopeerror(ls, gt);
  if (gt->close ||
      (label->nactvar < gt->nactvar && bup))
  { /* needs close? */
    int stklevel = reglevel(fs, label->nactvar);
    /* move jump to CLOSE position */
    fs->f->code[gt->pc + 1] = fs->f->code[gt->pc];
    /* put CLOSE instruction at original position */
    fs->f->code[gt->pc] = CREATE_ABCk(OP_CLOSE, stklevel, 0, 0, 0);
    gt->pc++; /* must point to jump instruction */
  }
  luaK_patchlist(ls->fs, gt->pc, label->pc); /* goto jumps to label */
  for (i = g; i < gl->n - 1; i++)            /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}

/*
** Search for an active label with the given name, starting at
** index 'ilb' (so that it can search for all labels in current block
** or all labels in current function).
*/
static Labeldesc* findlabel(LexState* ls, TString* name, int ilb)
{
  Dyndata* dyd = ls->dyd;
  for (; ilb < dyd->label.n; ilb++)
  {
    Labeldesc* lb = &dyd->label.arr[ilb];
    if (eqstr(lb->name, name)) /* correct label? */
      return lb;
  }
  return nullptr; /* label not found */
}

/*
** Adds a new label/goto in the corresponding list.
*/
static int newlabelentry(LexState* ls, Labellist* l, TString* name,
                         int line, int pc)
{
  int n = l->n;
  {
    auto s = luaM_safegrow_Labeldesc_span(ls->L, std::span<Labeldesc>(l->arr, l->size), n);
    l->arr = s.data();
    l->size = (int)s.size();
  }
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}

/*
** Create an entry for the goto and the code for it. As it is not known
** at this point whether the goto may need a CLOSE, the code has a jump
** followed by an CLOSE. (As the CLOSE comes after the jump, it is a
** dead instruction; it works as a placeholder.) When the goto is closed
** against a label, if it needs a CLOSE, the two instructions swap
** positions, so that the CLOSE comes before the jump.
*/
static int newgotoentry(LexState* ls, TString* name, int line)
{
  FuncState* fs = ls->fs;
  int pc = luaK_jump(fs);              /* create jump */
  luaK_codeABC(fs, OP_CLOSE, 0, 1, 0); /* spaceholder, marked as dead */
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}

/*
** Create a new label with the given 'name' at the given 'line'.
** 'last' tells whether label is the last non-op statement in its
** block. Solves all pending gotos to this new label and adds
** a close instruction if necessary.
** Returns true iff it added a close instruction.
*/
static void createlabel(LexState* ls, TString* name, int line, int last)
{
  FuncState* fs = ls->fs;
  Labellist* ll = &ls->dyd->label;
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last)
  { /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
}

/*
** Traverse the pending gotos of the finishing block checking whether
** each match some label of that block. Those that do not match are
** "exported" to the outer block, to be solved there. In particular,
** its 'nactvar' is updated with the level of the inner block,
** as the variables of the inner block are now out of scope.
*/
static void solvegotos(FuncState* fs, BlockCnt* bl)
{
  LexState* ls = fs->ls;
  Labellist* gl = &ls->dyd->gt;
  int outlevel = reglevel(fs, bl->nactvar); /* level outside the block */
  int igt = bl->firstgoto;                  /* first goto in the finishing block */
  while (igt < gl->n)
  { /* for each pending goto */
    Labeldesc* gt = &gl->arr[igt];
    /* search for a matching label in the current block */
    Labeldesc* lb = findlabel(ls, gt->name, bl->firstlabel);
    if (lb != nullptr)                      /* found a match? */
      closegoto(ls, igt, lb, bl->upval); /* close and remove goto */
    else
    { /* adjust 'goto' for outer block */
      /* block has variables to be closed and goto escapes the scope of
         some variable? */
      if (bl->upval && reglevel(fs, gt->nactvar) > outlevel)
        gt->close = 1;           /* jump may need a close */
      gt->nactvar = bl->nactvar; /* correct level for outer block */
      igt++;                     /* go to next goto */
    }
  }
  ls->dyd->label.n = bl->firstlabel; /* remove local labels */
}

static void enterblock(FuncState* fs, BlockCnt* bl, lu_byte isloop)
{
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  /* inherit 'insidetbc' from enclosing block */
  bl->insidetbc = (fs->bl != nullptr && fs->bl->insidetbc);
  bl->previous = fs->bl; /* link block in function's block list */
  fs->bl = bl;
  assert(fs->freereg == luaY_nvarstack(fs));
}

/*
** generates an error for an undefined 'goto'.
*/
[[noreturn]] static void undefgoto(LexState* ls, Labeldesc* gt)
{
  /* breaks are checked when created, cannot be undefined */
  assert(!eqstr(gt->name, ls->brkn));
  luaK_semerror(ls, lt::no_visible_label_s_for_goto_at_line_d,
                FmtArgs().str(gt->name->data.view()).num(gt->line));
}

static void leaveblock(FuncState* fs)
{
  BlockCnt* bl = fs->bl;
  LexState* ls = fs->ls;
  int stklevel = reglevel(fs, bl->nactvar); /* level outside block */
  if (bl->previous && bl->upval)                /* need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->freereg = stklevel;                 /* free registers */
  removevars(fs, bl->nactvar);            /* remove block locals */
  assert(bl->nactvar == fs->nactvar); /* back to level on entry */
  if (bl->isloop == 2)                    /* has to fix pending breaks? */
    createlabel(ls, ls->brkn, 0, 0);
  solvegotos(fs, bl);
  if (bl->previous == nullptr)
  {                                                   /* was it the last block? */
    if (bl->firstgoto < ls->dyd->gt.n)                /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]); /* error */
  }
  fs->bl = bl->previous; /* current block now is previous one */
}

/*
** adds a new prototype into list of prototypes
*/
static Proto* addprototype(LexState* ls)
{
  Proto* clp;
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Proto* f = fs->f; /* prototype of current function */
  if (fs->np >= (int)f->p.size())
  {
    int oldsize = (int)f->p.size();
    f->p = luaM_safegrow_Proto_ptr_span(L, f->p, fs->np);
    while (oldsize < (int)f->p.size())
      f->p[oldsize++] = nullptr;
  }
  f->p[fs->np++] = clp = Proto::luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}

/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
static void codeclosure(LexState* ls, expdesc* v)
{
  FuncState* fs = ls->fs->prev;
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v); /* fix it at the last register */
}

static void open_func(LexState* ls, FuncState* fs, BlockCnt* bl)
{
  lua_State* L = ls->L;
  Proto* f = fs->f;
  fs->prev = ls->fs; /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = nullptr;
  f->source = ls->source;
  luaC_objbarrier(L, f, f->source);
  f->maxstacksize = 2;                           /* registers 0/1 are always valid */
  fs->kcache = Table::luaH_new(L);                      /* create table for function */
  luaC_givebackhash(L->l_G, fs->kcache);                /* (the parser's own) */
  assert(novariant(fs->kcache->tt) >= LUA_TSTRING);
  luaD_anchorobj(L, ls->h, fs->kcache); /* anchor it */
  enterblock(fs, bl, 0);
}

static void close_func(LexState* ls)
{
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Proto* f = fs->f;
  TValue temp;
  luaK_ret(fs, luaY_nvarstack(fs), 0); /* final return */
  leaveblock(fs);
  assert(fs->bl == nullptr);
  luaK_finish(fs);
  f->code = luaM_alloc_Instruction_span(L->l_G, L, f->code, fs->pc, true);
  f->lineinfo = luaM_alloc_ls_byte_span(L->l_G, L, f->lineinfo, fs->pc, true);
  f->abslineinfo = luaM_alloc_AbsLineInfo_span(L->l_G, L, f->abslineinfo, fs->nabslineinfo, true);
  f->k = luaM_alloc_TValue_span(L->l_G, L, f->k, fs->nk, true);
  f->p = luaM_alloc_Proto_ptr_span(L->l_G, L, f->p, fs->np, true);
  f->locvars = luaM_alloc_LocVar_span(L->l_G, L, f->locvars, fs->ndebugvars, true);
  f->upvalues = luaM_alloc_Upvaldesc_span(L->l_G, L, f->upvalues, fs->nups, true);
  /* remove kcache table from scanner table ("weigh" its anchor) */
  sethvalue(L, &temp, fs->kcache); /* key to be set to nil */
  ls->h->luaH_set(L, &temp, &L->l_G->nilvalue);
  ls->fs = fs->prev;
  luaC_checkGC(L);
}

/*
** {======================================================================
** GRAMMAR RULES
** =======================================================================
*/

/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow(LexState* ls, int withuntil)
{
  switch (ls->t.token)
  {
  case TK_ELSE:
  case TK_ELSEIF:
  case TK_END:
  case TK_EOS:
    return 1;
  case TK_UNTIL:
    return withuntil;
  default:
    return 0;
  }
}

static void statlist(LexState* ls)
{
  /* statlist -> { stat [';'] } */
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_STATLIST);
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0: /* the loop */
        if (block_follow(ls, 1))
          return;
        l.site = (ls->t.token == TK_RETURN) ? 1 : 2;
        continue;
      case 1:
        statement(ls);
        return; /* 'return' must be last statement */
      case 2:
        statement(ls);
        l.site = 0;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_STATLIST, l);
    throw;
  }
}

static void fieldsel(LexState* ls, expdesc* v)
{
  /* fieldsel -> ['.' | ':'] NAME */
  LoadFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_FIELDSEL);
  try
  {
    switch (l.site)
    {
    case 0:
      luaK_exp2anyregup(fs, v);
      l.site = 1;
      [[fallthrough]];
    case 1:
      luaX_next(ls); /* skip the dot or colon */
      l.site = 2;
      [[fallthrough]];
    case 2:
    {
      expdesc key;
      codename(ls, &key);
      luaK_indexed(fs, v, &key);
      break;
    }
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FIELDSEL, l);
    throw;
  }
}

static void yindex(LexState* ls, expdesc* v)
{
  /* index -> '[' expr ']' */
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_YINDEX);
  try
  {
    switch (l.site)
    {
    case 0:
      luaX_next(ls); /* skip the '[' */
      l.site = 1;
      [[fallthrough]];
    case 1:
      expr(ls, v);
      luaK_exp2val(ls->fs, v);
      l.site = 2;
      [[fallthrough]];
    case 2:
      checknext(ls, ']');
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_YINDEX, l);
    throw;
  }
}

/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

struct ConsControl
{
  expdesc v;      /* last list item read */
  expdesc* t;     /* table descriptor */
  int nh;         /* total number of 'record' elements */
  int na;         /* number of array elements already stored */
  int tostore;    /* number of array elements pending to be stored */
  int maxtostore; /* maximum number of pending elements */
};

/*
** Maximum number of elements in a constructor, to control the following:
** * counter overflows;
** * overflows in 'extra' for OP_NEWTABLE and OP_SETLIST;
** * overflows when adding multiple returns in OP_SETLIST.
*/
constexpr int MAX_CNST = ((INT_MAX / 2) / (MAXARG_vC + 1) > MAXARG_Ax)
                             ? MAXARG_Ax * (MAXARG_vC + 1)
                             : INT_MAX / 2;

/* the frame of 'recfield' (see lload.h) */
struct RecfieldFrame : LoadFrame
{
  int reg = 0;
  expdesc tab{}, key{}, val{};
  LoadFrame* copy() const override { return new (std::nothrow) RecfieldFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void recfield(LexState* ls, ConsControl* cc)
{
  /* recfield -> (NAME | '['exp']') = exp */
  RecfieldFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const RecfieldFrame&>(luaZ_restore(ls->z->rz, LF_RECFIELD));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.reg = ls->fs->freereg;
        l.site = (ls->t.token == TK_NAME) ? 1 : 2; /* (else '[') */
        continue;
      case 1:
        codename(ls, &l.key);
        l.site = 3;
        continue;
      case 2:
        yindex(ls, &l.key);
        l.site = 3;
        [[fallthrough]];
      case 3:
        cc->nh++;
        l.site = 4;
        [[fallthrough]];
      case 4:
        checknext(ls, '=');
        l.tab = *cc->t;
        luaK_indexed(fs, &l.tab, &l.key);
        l.site = 5;
        [[fallthrough]];
      case 5:
        expr(ls, &l.val);
        luaK_storevar(fs, &l.tab, &l.val);
        fs->freereg = l.reg; /* free registers */
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_RECFIELD, l);
    throw;
  }
}

static void closelistfield(FuncState* fs, ConsControl* cc)
{
  assert(cc->tostore > 0);
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore >= cc->maxtostore)
  {
    luaK_setlist(fs, cc->t->info, cc->na, cc->tostore); /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0; /* no more items pending */
  }
}

static void lastlistfield(FuncState* fs, ConsControl* cc)
{
  if (cc->tostore == 0)
    return;
  if (hasmultret(cc->v.k))
  {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->info, cc->na, LUA_MULTRET);
    cc->na--; /* do not count last expression (unknown number of elements) */
  }
  else
  {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}

static void listfield(LexState* ls, ConsControl* cc)
{
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}

static void field(LexState* ls, ConsControl* cc)
{
  /* field -> listfield | recfield */
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_FIELD);
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        switch (ls->t.token)
        {
        case TK_NAME: /* may be 'listfield' or 'recfield' */
          l.site = 1;
          break;
        case '[':
          l.site = 3;
          break;
        default:
          l.site = 2;
          break;
        }
        continue;
      case 1:
        l.site = (luaX_lookahead(ls) != '=') ? 2 : 3; /* expression? */
        continue;
      case 2:
        listfield(ls, cc);
        return;
      case 3:
        recfield(ls, cc);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FIELD, l);
    throw;
  }
}

/*
** Compute a limit for how many registers a constructor can use before
** emitting a 'SETLIST' instruction, based on how many registers are
** available.
*/
static int maxtostore(FuncState* fs)
{
  int numfreeregs = MAX_FSTACK - fs->freereg;
  if (numfreeregs >= 160)     /* "lots" of registers? */
    return numfreeregs / 5;   /* use up to 1/5 of them */
  else if (numfreeregs >= 80) /* still "enough" registers? */
    return 10;                /* one 'SETLIST' instruction for each 10 values */
  else                        /* save registers for potential more nesting */
    return 1;
}

/* the frame of 'constructor' (see lload.h) */
struct ConstructorFrame : LoadFrame
{
  int line = 0;
  int pc = 0;
  ConsControl cc{};
  LoadFrame* copy() const override { return new (std::nothrow) ConstructorFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void constructor(LexState* ls, expdesc* t)
{
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  ConstructorFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const ConstructorFrame&>(luaZ_restore(ls->z->rz, LF_CONSTRUCTOR));
    l.cc.t = t; /* (again its table) */
  }
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.line = ls->linenumber;
        l.pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
        luaK_code(fs, 0); /* space for extra arg. */
        l.cc.na = l.cc.nh = l.cc.tostore = 0;
        l.cc.t = t;
        init_exp(t, VNONRELOC, fs->freereg); /* table will be at stack top */
        luaK_reserveregs(fs, 1);
        init_exp(&l.cc.v, VVOID, 0); /* no value (yet) */
        l.site = 1;
        [[fallthrough]];
      case 1:
        checknext(ls, '{' /*}*/);
        l.cc.maxtostore = maxtostore(fs);
        l.site = 2;
        [[fallthrough]];
      case 2: /* the loop */
        if (ls->t.token == /*{*/ '}')
        {
          l.site = 6;
          continue;
        }
        if (l.cc.v.k != VVOID)       /* is there a previous list item? */
          closelistfield(fs, &l.cc); /* close it */
        l.site = 3;
        [[fallthrough]];
      case 3:
        field(ls, &l.cc);
        luaY_checklimit(fs, l.cc.tostore + l.cc.na + l.cc.nh, MAX_CNST, lt::items_in_a_constructor);
        l.site = 4;
        [[fallthrough]];
      case 4:
        if (testnext(ls, ','))
        {
          l.site = 2;
          continue;
        }
        l.site = 5;
        [[fallthrough]];
      case 5:
        if (testnext(ls, ';'))
        {
          l.site = 2;
          continue;
        }
        l.site = 6;
        [[fallthrough]];
      case 6:
        check_match(ls, /*{*/ '}', '{' /*}*/, l.line);
        lastlistfield(fs, &l.cc);
        luaK_settablesize(fs, l.pc, t->info, l.cc.na, l.cc.nh);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_CONSTRUCTOR, l);
    throw;
  }
}

/* }====================================================================== */

static void setvararg(FuncState* fs)
{
  fs->f->flag |= PF_VAHID; /* by default, use hidden vararg arguments */
  luaK_codeABC(fs, OP_VARARGPREP, 0, 0, 0);
}

/* the frame of 'parlist' (see lload.h) */
struct ParlistFrame : LoadFrame
{
  int nparams = 0;
  int varargk = 0;
  LoadFrame* copy() const override { return new (std::nothrow) ParlistFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void parlist(LexState* ls)
{
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  ParlistFrame l;
  FuncState* fs = ls->fs;
  Proto* f = fs->f;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ParlistFrame&>(luaZ_restore(ls->z->rz, LF_PARLIST));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.nparams = 0;
        l.varargk = 0;
        if (ls->t.token == ')') /* is 'parlist' empty? */
          break;
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        switch (ls->t.token)
        {
        case TK_NAME:
          l.site = 2;
          break;
        case TK_DOTS:
          l.varargk = 1;
          l.site = 3;
          break;
        default:
          luaX_syntaxerror(ls, lt::name_or_expected);
        }
        continue;
      case 2:
      {
        TString* name = str_checkname(ls);
        new_localvar(ls, name);
        l.nparams++;
        l.site = 5;
        continue;
      }
      case 3:
        luaX_next(ls); /* skip '...' */
        if (ls->t.token != TK_NAME)
        {
          byte_view name = lt::vararg_table;
          new_localvar(ls, luaX_newstring(ls, name, 0, name.size()));
          break;
        }
        l.site = 4;
        [[fallthrough]];
      case 4:
      {
        TString* name = str_checkname(ls);
        new_varkind(ls, name, RDKVAVAR);
        break;
      }
      case 5:
        if (testnext(ls, ','))
        {
          l.site = 1;
          continue;
        }
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the list is read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_PARLIST, l);
    throw;
  }
  adjustlocalvars(ls, l.nparams);
  f->numparams = fs->nactvar;
  if (l.varargk)
  {
    setvararg(fs);          /* declared vararg */
    adjustlocalvars(ls, 1); /* vararg parameter */
  }
  /* reserve registers for parameters (plus vararg parameter, if present) */
  luaK_reserveregs(fs, fs->nactvar);
}

/* the frame of 'body' (see lload.h) */
struct BodyFrame : LoadFrame
{
  FuncState new_fs{};
  BlockCnt bl{};
  LoadFrame* copy() const override { return new (std::nothrow) BodyFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void body(LexState* ls, expdesc* e, int ismethod, int line)
{
  /* body ->  '(' parlist ')' block END */
  BodyFrame l;
  if (l_unlikely(ls->z->resuming))
  { /* (its function and block again in the chains) */
    l = static_cast<const BodyFrame&>(luaZ_restore(ls->z->rz, LF_BODY));
    l.new_fs.prev = ls->fs;
    l.new_fs.ls = ls;
    ls->fs = &l.new_fs;
    l.bl.previous = nullptr;
    l.new_fs.bl = &l.bl;
  }
  try
  {
    switch (l.site)
    {
    case 0:
      l.new_fs.f = addprototype(ls);
      l.new_fs.f->linedefined = line;
      open_func(ls, &l.new_fs, &l.bl);
      l.site = 1;
      [[fallthrough]];
    case 1:
      checknext(ls, '(');
      if (ismethod)
      {
        byte_view self = lt::self;
        new_localvar(ls, luaX_newstring(ls, self, 0, self.size())); /* create 'self' parameter */
        adjustlocalvars(ls, 1);
      }
      l.site = 2;
      [[fallthrough]];
    case 2:
      parlist(ls);
      l.site = 3;
      [[fallthrough]];
    case 3:
      checknext(ls, ')');
      l.site = 4;
      [[fallthrough]];
    case 4:
      statlist(ls);
      l.new_fs.f->lastlinedefined = ls->linenumber;
      l.site = 5;
      [[fallthrough]];
    case 5:
      check_match(ls, TK_END, TK_FUNCTION, line);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_BODY, l);
    throw;
  }
  codeclosure(ls, e);
  close_func(ls);
}

/* the frame of 'explist' (see lload.h) */
struct ExplistFrame : LoadFrame
{
  int n = 0;
  LoadFrame* copy() const override { return new (std::nothrow) ExplistFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static int explist(LexState* ls, expdesc* v)
{
  /* explist -> expr { ',' expr } */
  ExplistFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ExplistFrame&>(luaZ_restore(ls->z->rz, LF_EXPLIST));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = 1; /* at least one expression */
        l.site = 1;
        [[fallthrough]];
      case 1:
        expr(ls, v);
        l.site = 2;
        [[fallthrough]];
      case 2: /* the loop */
        if (!testnext(ls, ','))
          return l.n;
        luaK_exp2nextreg(ls->fs, v);
        l.site = 3;
        [[fallthrough]];
      case 3:
        expr(ls, v);
        l.n++;
        l.site = 2;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_EXPLIST, l);
    throw;
  }
}

/* the frame of 'funcargs' (see lload.h) */
struct FuncargsFrame : LoadFrame
{
  expdesc args{};
  int line = 0;
  LoadFrame* copy() const override { return new (std::nothrow) FuncargsFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void funcargs(LexState* ls, expdesc* f)
{
  FuncargsFrame l;
  FuncState* fs = ls->fs;
  int base, nparams;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const FuncargsFrame&>(luaZ_restore(ls->z->rz, LF_FUNCARGS));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.line = ls->linenumber;
        switch (ls->t.token)
        {
        case '(': /* funcargs -> '(' [ explist ] ')' */
          l.site = 1;
          break;
        case '{' /*}*/: /* funcargs -> constructor */
          l.site = 4;
          break;
        case TK_STRING: /* funcargs -> STRING */
          codestring(&l.args, ls->t.ts);
          l.site = 5;
          break;
        default:
          luaX_syntaxerror(ls, lt::function_arguments_expected);
        }
        continue;
      case 1:
        luaX_next(ls);
        if (ls->t.token == ')') /* arg list is empty? */
        {
          l.args.k = VVOID;
          l.site = 3;
          continue;
        }
        l.site = 2;
        [[fallthrough]];
      case 2:
        explist(ls, &l.args);
        if (hasmultret(l.args.k))
          luaK_setmultret(fs, &l.args);
        l.site = 3;
        [[fallthrough]];
      case 3:
        check_match(ls, ')', '(', l.line);
        break;
      case 4:
        constructor(ls, &l.args);
        break;
      case 5:
        luaX_next(ls); /* must use 'seminfo' before 'next' */
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the arguments are read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FUNCARGS, l);
    throw;
  }
  assert(f->k == VNONRELOC);
  base = f->info; /* base register for call */
  if (hasmultret(l.args.k))
    nparams = LUA_MULTRET; /* open call */
  else
  {
    if (l.args.k != VVOID)
      luaK_exp2nextreg(fs, &l.args); /* close last argument */
    nparams = fs->freereg - (base + 1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
  luaK_fixline(fs, l.line);
  /* call removes function and arguments and leaves one result (unless
     changed later) */
  fs->freereg = base + 1;
}

/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

/* the frame of a function with a line to keep (see lload.h) */
struct LineFrame : LoadFrame
{
  int line = 0;
  LoadFrame* copy() const override { return new (std::nothrow) LineFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void primaryexp(LexState* ls, expdesc* v)
{
  /* primaryexp -> NAME | '(' expr ')' */
  LineFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const LineFrame&>(luaZ_restore(ls->z->rz, LF_PRIMARYEXP));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        switch (ls->t.token)
        {
        case '(':
          l.line = ls->linenumber;
          l.site = 1;
          break;
        case TK_NAME:
          l.site = 4;
          break;
        default:
          luaX_syntaxerror(ls, lt::unexpected_symbol);
        }
        continue;
      case 1:
        luaX_next(ls);
        l.site = 2;
        [[fallthrough]];
      case 2:
        expr(ls, v);
        l.site = 3;
        [[fallthrough]];
      case 3:
        check_match(ls, ')', '(', l.line);
        luaK_dischargevars(ls->fs, v);
        return;
      case 4:
        singlevar(ls, v);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_PRIMARYEXP, l);
    throw;
  }
}

/* the frame of a function with a key to keep (see lload.h) */
struct KeyFrame : LoadFrame
{
  expdesc key{};
  LoadFrame* copy() const override { return new (std::nothrow) KeyFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void suffixedexp(LexState* ls, expdesc* v)
{
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  KeyFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const KeyFrame&>(luaZ_restore(ls->z->rz, LF_SUFFIXEDEXP));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        primaryexp(ls, v);
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        switch (ls->t.token)
        {
        case '.': /* fieldsel */
          l.site = 2;
          break;
        case '[': /* '[' exp ']' */
          luaK_exp2anyregup(fs, v);
          l.site = 3;
          break;
        case ':': /* ':' NAME funcargs */
          l.site = 4;
          break;
        case '(':
        case TK_STRING:
        case '{' /*}*/: /* funcargs */
          luaK_exp2nextreg(fs, v);
          l.site = 6;
          break;
        default:
          return;
        }
        continue;
      case 2:
        fieldsel(ls, v);
        l.site = 1;
        continue;
      case 3:
        yindex(ls, &l.key);
        luaK_indexed(fs, v, &l.key);
        l.site = 1;
        continue;
      case 4:
        luaX_next(ls);
        l.site = 5;
        [[fallthrough]];
      case 5:
        codename(ls, &l.key);
        luaK_self(fs, v, &l.key);
        l.site = 6;
        [[fallthrough]];
      case 6:
        funcargs(ls, v);
        l.site = 1;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_SUFFIXEDEXP, l);
    throw;
  }
}

static void simpleexp(LexState* ls, expdesc* v)
{
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  LineFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const LineFrame&>(luaZ_restore(ls->z->rz, LF_SIMPLEEXP));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        switch (ls->t.token)
        {
        case TK_FLT:
        {
          init_exp(v, VKFLT, 0);
          v->nval = ls->t.r;
          break;
        }
        case TK_INT:
        {
          init_exp(v, VKINT, 0);
          v->ival = ls->t.i;
          break;
        }
        case TK_STRING:
        {
          codestring(v, ls->t.ts);
          break;
        }
        case TK_NIL:
        {
          init_exp(v, VNIL, 0);
          break;
        }
        case TK_TRUE:
        {
          init_exp(v, VTRUE, 0);
          break;
        }
        case TK_FALSE:
        {
          init_exp(v, VFALSE, 0);
          break;
        }
        case TK_DOTS:
        { /* vararg */
          FuncState* fs = ls->fs;
          check_condition(ls, isvararg(fs->f), lt::cannot_use_outside_a_vararg_function);
          init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
          break;
        }
        case '{' /*}*/: /* constructor */
          l.site = 2;
          continue;
        case TK_FUNCTION:
          l.site = 3;
          continue;
        default:
          l.site = 5;
          continue;
        }
        l.site = 1;
        [[fallthrough]];
      case 1:
        luaX_next(ls);
        return;
      case 2:
        constructor(ls, v);
        return;
      case 3:
        luaX_next(ls);
        l.line = ls->linenumber;
        l.site = 4;
        [[fallthrough]];
      case 4:
        body(ls, v, 0, l.line);
        return;
      case 5:
        suffixedexp(ls, v);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_SIMPLEEXP, l);
    throw;
  }
}

static UnOpr getunopr(int op)
{
  switch (op)
  {
  case TK_NOT:
    return OPR_NOT;
  case '-':
    return OPR_MINUS;
  case '~':
    return OPR_BNOT;
  case '#':
    return OPR_LEN;
  default:
    return OPR_NOUNOPR;
  }
}

static BinOpr getbinopr(int op)
{
  switch (op)
  {
  case '+':
    return OPR_ADD;
  case '-':
    return OPR_SUB;
  case '*':
    return OPR_MUL;
  case '%':
    return OPR_MOD;
  case '^':
    return OPR_POW;
  case '/':
    return OPR_DIV;
  case TK_IDIV:
    return OPR_IDIV;
  case '&':
    return OPR_BAND;
  case '|':
    return OPR_BOR;
  case '~':
    return OPR_BXOR;
  case TK_SHL:
    return OPR_SHL;
  case TK_SHR:
    return OPR_SHR;
  case TK_CONCAT:
    return OPR_CONCAT;
  case TK_NE:
    return OPR_NE;
  case TK_EQ:
    return OPR_EQ;
  case '<':
    return OPR_LT;
  case TK_LE:
    return OPR_LE;
  case '>':
    return OPR_GT;
  case TK_GE:
    return OPR_GE;
  case TK_AND:
    return OPR_AND;
  case TK_OR:
    return OPR_OR;
  default:
    return OPR_NOBINOPR;
  }
}

/*
** Priority table for binary operators.
*/
static const struct
{
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {
    /* ORDER OPR */
    {10, 10},
    {10, 10}, /* '+' '-' */
    {11, 11},
    {11, 11}, /* '*' '%' */
    {14, 13}, /* '^' (right associative) */
    {11, 11},
    {11, 11}, /* '/' '//' */
    {6, 6},
    {4, 4},
    {5, 5}, /* '&' '|' '~' */
    {7, 7},
    {7, 7}, /* '<<' '>>' */
    {9, 8}, /* '..' (right associative) */
    {3, 3},
    {3, 3},
    {3, 3}, /* ==, <, <= */
    {3, 3},
    {3, 3},
    {3, 3}, /* ~=, >, >= */
    {2, 2},
    {1, 1} /* and, or */
};

constexpr int UNARY_PRIORITY = 12; /* priority for unary operators */

/* the frame of 'subexpr' (see lload.h) */
struct SubexprFrame : LoadFrame
{
  BinOpr op = OPR_NOBINOPR;
  UnOpr uop = OPR_NOUNOPR;
  int line = 0;
  expdesc v2{};
  LoadFrame* copy() const override { return new (std::nothrow) SubexprFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static BinOpr subexpr(LexState* ls, expdesc* v, int limit)
{
  SubexprFrame l;
  enterlevel(ls);
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const SubexprFrame&>(luaZ_restore(ls->z->rz, LF_SUBEXPR));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.uop = getunopr(ls->t.token);
        if (l.uop == OPR_NOUNOPR)
        {
          l.site = 3;
          continue;
        }
        /* prefix (unary) operator */
        l.line = ls->linenumber;
        l.site = 1;
        [[fallthrough]];
      case 1:
        luaX_next(ls); /* skip operator */
        l.site = 2;
        [[fallthrough]];
      case 2:
        subexpr(ls, v, UNARY_PRIORITY);
        luaK_prefix(ls->fs, l.uop, v, l.line);
        l.site = 4;
        continue;
      case 3:
        simpleexp(ls, v);
        l.site = 4;
        [[fallthrough]];
      case 4:
        /* expand while operators have priorities higher than 'limit' */
        l.op = getbinopr(ls->t.token);
        l.site = 5;
        [[fallthrough]];
      case 5: /* the loop */
        if (l.op == OPR_NOBINOPR || priority[l.op].left <= limit)
        {
          leavelevel(ls);
          return l.op; /* return first untreated operator */
        }
        l.line = ls->linenumber;
        l.site = 6;
        [[fallthrough]];
      case 6:
        luaX_next(ls); /* skip operator */
        luaK_infix(ls->fs, l.op, v);
        l.site = 7;
        [[fallthrough]];
      case 7:
      {
        /* read sub-expression with higher priority */
        BinOpr nextop = subexpr(ls, &l.v2, priority[l.op].right);
        luaK_posfix(ls->fs, l.op, v, &l.v2, l.line);
        l.op = nextop;
        l.site = 5;
        continue;
      }
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_SUBEXPR, l);
    throw;
  }
}

static void expr(LexState* ls, expdesc* v)
{
  subexpr(ls, v, 0); /* (no frame of its own: see 'testnext') */
}

/* }==================================================================== */

/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

/* the frame of a function with a block (see lload.h) */
struct BlockFrame : LoadFrame
{
  BlockCnt bl{};
  LoadFrame* copy() const override { return new (std::nothrow) BlockFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void block(LexState* ls)
{
  /* block -> statlist */
  BlockFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const BlockFrame&>(luaZ_restore(ls->z->rz, LF_BLOCK));
    l.bl.previous = fs->bl; /* (again in the chain) */
    fs->bl = &l.bl;
  }
  try
  {
    switch (l.site)
    {
    case 0:
      enterblock(fs, &l.bl, 0);
      l.site = 1;
      [[fallthrough]];
    case 1:
      statlist(ls);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_BLOCK, l);
    throw;
  }
  leaveblock(fs);
}

/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign
{
  struct LHS_assign* prev;
  expdesc v; /* variable (global, local, upvalue, or indexed) */
};

/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict(LexState* ls, struct LHS_assign* lh, expdesc* v)
{
  FuncState* fs = ls->fs;
  int extra = fs->freereg; /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev)
  { /* check all previous assignments */
    if (vkisindexed(lh->v.k))
    { /* assignment to table field? */
      if (lh->v.k == VINDEXUP)
      { /* is table an upvalue? */
        if (v->k == VUPVAL && lh->v.ind.t == v->info)
        {
          conflict = 1; /* table is the upvalue being assigned now */
          lh->v.k = VINDEXSTR;
          lh->v.ind.t = extra; /* assignment will use safe copy */
        }
      }
      else
      { /* table is a register */
        if (v->k == VLOCAL && lh->v.ind.t == v->var.ridx)
        {
          conflict = 1;          /* table is the local being assigned now */
          lh->v.ind.t = extra; /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.ind.idx == v->var.ridx)
        {
          conflict = 1;
          lh->v.ind.idx = extra; /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict)
  {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      luaK_codeABC(fs, OP_MOVE, extra, v->var.ridx, 0);
    else
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->info, 0);
    luaK_reserveregs(fs, 1);
  }
}

/* Create code to store the "top" register in 'var' */
static void storevartop(FuncState* fs, expdesc* var)
{
  expdesc e;
  init_exp(&e, VNONRELOC, fs->freereg - 1);
  luaK_storevar(fs, var, &e); /* will also free the top register */
}

/* the frame of 'restassign' (see lload.h) */
struct RestassignFrame : LoadFrame
{
  expdesc e{};
  struct LHS_assign nv{};
  LoadFrame* copy() const override { return new (std::nothrow) RestassignFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/*
** Parse and compile a multiple assignment. The first "variable"
** (a 'suffixedexp') was already read by the caller.
**
** assignment -> suffixedexp restassign
** restassign -> ',' suffixedexp restassign | '=' explist
*/
static void restassign(LexState* ls, struct LHS_assign* lh, int nvars)
{
  RestassignFrame l;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const RestassignFrame&>(luaZ_restore(ls->z->rz, LF_RESTASSIGN));
    l.nv.prev = lh; /* (again in the chain) */
  }
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        check_condition(ls, vkisvar(lh->v.k), lt::syntax_error);
        check_readonly(ls, &lh->v);
        l.site = 1;
        [[fallthrough]];
      case 1:
        if (!testnext(ls, ','))
        { /* restassign -> '=' explist */
          l.site = 4;
          continue;
        }
        /* restassign -> ',' suffixedexp restassign */
        l.nv.prev = lh;
        l.site = 2;
        [[fallthrough]];
      case 2:
        suffixedexp(ls, &l.nv.v);
        if (!vkisindexed(l.nv.v.k))
          check_conflict(ls, lh, &l.nv.v);
        l.site = 3;
        [[fallthrough]];
      case 3:
        enterlevel(ls); /* control recursion depth */
        restassign(ls, &l.nv, nvars + 1);
        leavelevel(ls);
        break;
      case 4:
        checknext(ls, '=');
        l.site = 5;
        [[fallthrough]];
      case 5:
      {
        int nexps = explist(ls, &l.e);
        if (nexps != nvars)
          adjust_assign(ls, nvars, nexps, &l.e);
        else
        {
          luaK_setoneret(ls->fs, &l.e); /* close last expression */
          luaK_storevar(ls->fs, &lh->v, &l.e);
          return; /* avoid default */
        }
        break;
      }
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the assignment is read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_RESTASSIGN, l);
    throw;
  }
  storevartop(ls->fs, &lh->v); /* default assignment */
}

/* the frame of a function with an expression to keep (see lload.h) */
struct ExpFrame : LoadFrame
{
  expdesc v{};
  LoadFrame* copy() const override { return new (std::nothrow) ExpFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static int cond(LexState* ls)
{
  /* cond -> exp */
  ExpFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ExpFrame&>(luaZ_restore(ls->z->rz, LF_COND));
  try
  {
    switch (l.site)
    {
    case 0:
      expr(ls, &l.v); /* read condition */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_COND, l);
    throw;
  }
  if (l.v.k == VNIL)
    l.v.k = VFALSE; /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &l.v);
  return l.v.f;
}

static void gotostat(LexState* ls, int line)
{
  TString* name = str_checkname(ls); /* label's name */
  newgotoentry(ls, name, line);
}

/*
** Break statement. Semantically equivalent to "goto break".
*/
static void breakstat(LexState* ls, int line)
{
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_BREAKSTAT);
  try
  {
    switch (l.site)
    {
    case 0:
    {
      BlockCnt* bl; /* to look for an enclosing loop */
      for (bl = ls->fs->bl; bl != nullptr; bl = bl->previous)
      {
        if (bl->isloop) /* found one? */
          break;
      }
      if (bl == nullptr)
        luaX_syntaxerror(ls, lt::break_outside_loop);
      bl->isloop = 2; /* signal that block has pending breaks */
      l.site = 1;
      [[fallthrough]];
    }
    case 1:
      luaX_next(ls); /* skip break */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_BREAKSTAT, l);
    throw;
  }
  newgotoentry(ls, ls->brkn, line);
}

/*
** Check whether there is already a label with the given 'name' at
** current function.
*/
static void checkrepeated(LexState* ls, TString* name)
{
  Labeldesc* lb = findlabel(ls, name, ls->fs->firstlabel);
  if (l_unlikely(lb != nullptr)) /* already defined? */
    luaK_semerror(ls, lt::label_s_already_defined_on_line_d,
                  FmtArgs().str(name->data.view()).num(lb->line)); /* error */
}

static void labelstat(LexState* ls, TString* name, int line)
{
  /* label -> '::' NAME '::' */
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_LABELSTAT);
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        checknext(ls, TK_DBCOLON); /* skip double colon */
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        if (ls->t.token != ';' && ls->t.token != TK_DBCOLON)
          break;
        l.site = 2;
        [[fallthrough]];
      case 2:
        statement(ls); /* skip other no-op statements */
        l.site = 1;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the no-op statements are skipped) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_LABELSTAT, l);
    throw;
  }
  checkrepeated(ls, name); /* check for repeated labels */
  createlabel(ls, name, line, block_follow(ls, 0));
}

/* the frame of 'whilestat' (see lload.h) */
struct WhileFrame : LoadFrame
{
  int whileinit = 0;
  int condexit = 0;
  BlockCnt bl{};
  LoadFrame* copy() const override { return new (std::nothrow) WhileFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void whilestat(LexState* ls, int line)
{
  /* whilestat -> WHILE cond DO block END */
  WhileFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const WhileFrame&>(luaZ_restore(ls->z->rz, LF_WHILESTAT));
    if (l.site >= 2)
    { /* (its block again in the chain) */
      l.bl.previous = fs->bl;
      fs->bl = &l.bl;
    }
  }
  try
  {
    switch (l.site)
    {
    case 0:
      luaX_next(ls); /* skip WHILE */
      l.whileinit = luaK_getlabel(fs);
      l.site = 1;
      [[fallthrough]];
    case 1:
      l.condexit = cond(ls);
      enterblock(fs, &l.bl, 1);
      l.site = 2;
      [[fallthrough]];
    case 2:
      checknext(ls, TK_DO);
      l.site = 3;
      [[fallthrough]];
    case 3:
      block(ls);
      luaK_jumpto(fs, l.whileinit);
      l.site = 4;
      [[fallthrough]];
    case 4:
      check_match(ls, TK_END, TK_WHILE, line);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_WHILESTAT, l);
    throw;
  }
  leaveblock(fs);
  luaK_patchtohere(fs, l.condexit); /* false conditions finish the loop */
}

/* the frame of 'repeatstat' (see lload.h) */
struct RepeatFrame : LoadFrame
{
  int condexit = 0;
  int repeat_init = 0;
  BlockCnt bl1{}, bl2{};
  LoadFrame* copy() const override { return new (std::nothrow) RepeatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void repeatstat(LexState* ls, int line)
{
  /* repeatstat -> REPEAT block UNTIL cond */
  RepeatFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const RepeatFrame&>(luaZ_restore(ls->z->rz, LF_REPEATSTAT));
    l.bl1.previous = fs->bl; /* (its blocks again in the chain) */
    l.bl2.previous = &l.bl1;
    fs->bl = &l.bl2;
  }
  try
  {
    switch (l.site)
    {
    case 0:
      l.repeat_init = luaK_getlabel(fs);
      enterblock(fs, &l.bl1, 1); /* loop block */
      enterblock(fs, &l.bl2, 0); /* scope block */
      l.site = 1;
      [[fallthrough]];
    case 1:
      luaX_next(ls); /* skip REPEAT */
      l.site = 2;
      [[fallthrough]];
    case 2:
      statlist(ls);
      l.site = 3;
      [[fallthrough]];
    case 3:
      check_match(ls, TK_UNTIL, TK_REPEAT, line);
      l.site = 4;
      [[fallthrough]];
    case 4:
      l.condexit = cond(ls); /* read condition (inside scope block) */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_REPEATSTAT, l);
    throw;
  }
  if (l.bl2.upval)
  {                                   /* upvalues? */
    int exit = luaK_jump(fs);         /* normal exit must jump over fix */
    luaK_patchtohere(fs, l.condexit); /* repetition must close upvalues */
    luaK_codeABC(fs, OP_CLOSE, reglevel(fs, l.bl2.nactvar), 0, 0);
    l.condexit = luaK_jump(fs); /* repeat after closing upvalues */
    luaK_patchtohere(fs, exit); /* normal exit comes to here */
  }
  luaK_patchlist(fs, l.condexit, l.repeat_init); /* close the loop */
  leaveblock(fs);                                /* finish scope */
  leaveblock(fs);                                /* finish loop */
}

/*
** Read an expression and generate code to put its results in next
** stack slot.
**
*/
static void exp1(LexState* ls)
{
  ExpFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ExpFrame&>(luaZ_restore(ls->z->rz, LF_EXP1));
  try
  {
    switch (l.site)
    {
    case 0:
      expr(ls, &l.v);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_EXP1, l);
    throw;
  }
  luaK_exp2nextreg(ls->fs, &l.v);
  assert(l.v.k == VNONRELOC);
}

/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump(FuncState* fs, int pc, int dest, int back)
{
  std::span<Instruction> code = fs->f->code;
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, lt::control_structure_too_long);
  code[pc] = SETARG_Bx(code[pc], offset);
}

/* the frame of 'forbody' (see lload.h) */
struct ForbodyFrame : LoadFrame
{
  int prep = 0;
  BlockCnt bl{};
  LoadFrame* copy() const override { return new (std::nothrow) ForbodyFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/*
** Generate code for a 'for' loop.
*/
static void forbody(LexState* ls, int base, int line, int nvars, int isgen)
{
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  ForbodyFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const ForbodyFrame&>(luaZ_restore(ls->z->rz, LF_FORBODY));
    if (l.site == 1)
    { /* (its block again in the chain) */
      l.bl.previous = fs->bl;
      fs->bl = &l.bl;
    }
  }
  try
  {
    switch (l.site)
    {
    case 0:
      checknext(ls, TK_DO);
      l.prep = luaK_codeABx(fs, forprep[isgen], base, 0);
      fs->freereg--;            /* both 'forprep' remove one register from the stack */
      enterblock(fs, &l.bl, 0); /* scope for declared variables */
      adjustlocalvars(ls, nvars);
      luaK_reserveregs(fs, nvars);
      l.site = 1;
      [[fallthrough]];
    case 1:
      block(ls);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FORBODY, l);
    throw;
  }
  leaveblock(fs); /* end of scope for declared variables */
  fixforjump(fs, l.prep, luaK_getlabel(fs), 0);
  if (isgen)
  { /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  int endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, l.prep + 1, 1);
  luaK_fixline(fs, line);
}

/* the frame of 'fornum' (see lload.h) */
struct FornumFrame : LoadFrame
{
  int base = 0;
  LoadFrame* copy() const override { return new (std::nothrow) FornumFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void fornum(LexState* ls, TString* varname, int line)
{
  /* fornum -> NAME = exp,exp[,exp] forbody */
  FornumFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const FornumFrame&>(luaZ_restore(ls->z->rz, LF_FORNUM));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.base = fs->freereg;
        new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
        new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
        new_varkind(ls, varname, RDKCONST);
        l.site = 1;
        [[fallthrough]];
      case 1:
        checknext(ls, '=');
        l.site = 2;
        [[fallthrough]];
      case 2:
        exp1(ls); /* initial value */
        l.site = 3;
        [[fallthrough]];
      case 3:
        checknext(ls, ',');
        l.site = 4;
        [[fallthrough]];
      case 4:
        exp1(ls); /* limit */
        l.site = 5;
        [[fallthrough]];
      case 5:
        if (!testnext(ls, ','))
        { /* default step = 1 */
          luaK_int(fs, fs->freereg, 1);
          luaK_reserveregs(fs, 1);
          l.site = 7;
          continue;
        }
        l.site = 6;
        [[fallthrough]];
      case 6:
        exp1(ls); /* optional step */
        l.site = 7;
        [[fallthrough]];
      case 7:
        adjustlocalvars(ls, 2); /* start scope for internal variables */
        l.site = 8;
        [[fallthrough]];
      case 8:
        forbody(ls, l.base, line, 1, 0);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FORNUM, l);
    throw;
  }
}

/* the frame of 'forlist' (see lload.h) */
struct ForlistFrame : LoadFrame
{
  int nvars = 0; /* function, state, closing, control */
  int line = 0;
  int base = 0;
  expdesc e{};
  LoadFrame* copy() const override { return new (std::nothrow) ForlistFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void forlist(LexState* ls, TString* indexname)
{
  /* forlist -> NAME {,NAME} IN explist forbody */
  ForlistFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ForlistFrame&>(luaZ_restore(ls->z->rz, LF_FORLIST));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.nvars = 4;
        l.base = fs->freereg;
        /* create internal variables */
        new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* iterator function */
        new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* state */
        new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* closing var. (after swap) */
        new_varkind(ls, indexname, RDKCONST);
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop: other declared variables */
        if (!testnext(ls, ','))
        {
          l.site = 3;
          continue;
        }
        l.site = 2;
        [[fallthrough]];
      case 2:
      {
        TString* name = str_checkname(ls);
        new_localvar(ls, name);
        l.nvars++;
        l.site = 1;
        continue;
      }
      case 3:
        checknext(ls, TK_IN);
        l.line = ls->linenumber;
        l.site = 4;
        [[fallthrough]];
      case 4:
      {
        int nexps = explist(ls, &l.e);
        adjust_assign(ls, 4, nexps, &l.e);
        adjustlocalvars(ls, 3); /* start scope for internal variables */
        marktobeclosed(fs);     /* last internal var. must be closed */
        luaK_checkstack(fs, 2); /* extra space to call iterator */
        l.site = 5;
        [[fallthrough]];
      }
      case 5:
        forbody(ls, l.base, l.line, l.nvars - 3, 1);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FORLIST, l);
    throw;
  }
}

/* the frame of 'forstat' (see lload.h) */
struct ForstatFrame : LoadFrame
{
  TString* varname = nullptr;
  BlockCnt bl{};
  LoadFrame* copy() const override { return new (std::nothrow) ForstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void forstat(LexState* ls, int line)
{
  /* forstat -> FOR (fornum | forlist) END */
  ForstatFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const ForstatFrame&>(luaZ_restore(ls->z->rz, LF_FORSTAT));
    l.bl.previous = fs->bl; /* (its block again in the chain) */
    fs->bl = &l.bl;
  }
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        enterblock(fs, &l.bl, 1); /* scope for loop and control variables */
        l.site = 1;
        [[fallthrough]];
      case 1:
        luaX_next(ls); /* skip 'for' */
        l.site = 2;
        [[fallthrough]];
      case 2:
        l.varname = str_checkname(ls); /* first variable name */
        switch (ls->t.token)
        {
        case '=':
          l.site = 3;
          break;
        case ',':
        case TK_IN:
          l.site = 4;
          break;
        default:
          luaX_syntaxerror(ls, lt::or_in_expected);
        }
        continue;
      case 3:
        fornum(ls, l.varname, line);
        l.site = 5;
        continue;
      case 4:
        forlist(ls, l.varname);
        l.site = 5;
        [[fallthrough]];
      case 5:
        check_match(ls, TK_END, TK_FOR, line);
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the loop is read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FORSTAT, l);
    throw;
  }
  leaveblock(fs); /* loop scope ('break' jumps to this point) */
}

/* the frame of 'test_then_block' (see lload.h) */
struct TestThenFrame : LoadFrame
{
  int condtrue = 0;
  LoadFrame* copy() const override { return new (std::nothrow) TestThenFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static int test_then_block(LexState* ls, int escapelist)
{
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  TestThenFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const TestThenFrame&>(luaZ_restore(ls->z->rz, LF_TESTTHEN));
  try
  {
    switch (l.site)
    {
    case 0:
      luaX_next(ls); /* skip IF or ELSEIF */
      l.site = 1;
      [[fallthrough]];
    case 1:
      l.condtrue = cond(ls); /* read condition */
      l.site = 2;
      [[fallthrough]];
    case 2:
      checknext(ls, TK_THEN);
      l.site = 3;
      [[fallthrough]];
    case 3:
      block(ls); /* 'then' part */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_TESTTHEN, l);
    throw;
  }
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)                 /* followed by 'else'/'elseif'? */
    escapelist = luaK_concat(fs, escapelist, luaK_jump(fs)); /* must jump over it */
  luaK_patchtohere(fs, l.condtrue);
  return escapelist;
}

/* the frame of 'ifstat' (see lload.h) */
struct IfFrame : LoadFrame
{
  int escapelist = 0; /* exit list for finished parts */
  LoadFrame* copy() const override { return new (std::nothrow) IfFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void ifstat(LexState* ls, int line)
{
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  IfFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const IfFrame&>(luaZ_restore(ls->z->rz, LF_IFSTAT));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.escapelist = NO_JUMP;
        l.site = 1;
        [[fallthrough]];
      case 1:
        l.escapelist = test_then_block(ls, l.escapelist); /* IF cond THEN block, or ELSEIF cond THEN block */
        l.site = 2;
        [[fallthrough]];
      case 2: /* the loop */
        if (ls->t.token == TK_ELSEIF)
        {
          l.site = 1;
          continue;
        }
        l.site = 3;
        [[fallthrough]];
      case 3:
        if (!testnext(ls, TK_ELSE))
        {
          l.site = 5;
          continue;
        }
        l.site = 4;
        [[fallthrough]];
      case 4:
        block(ls); /* 'else' part */
        l.site = 5;
        [[fallthrough]];
      case 5:
        check_match(ls, TK_END, TK_IF, line);
        luaK_patchtohere(fs, l.escapelist); /* patch escape list to 'if' end */
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_IFSTAT, l);
    throw;
  }
}

/* the frame of 'localfunc' (see lload.h) */
struct LocalfuncFrame : LoadFrame
{
  int fvar = 0; /* function's variable index */
  int line = 0;
  expdesc b{};
  LoadFrame* copy() const override { return new (std::nothrow) LocalfuncFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void localfunc(LexState* ls)
{
  LocalfuncFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const LocalfuncFrame&>(luaZ_restore(ls->z->rz, LF_LOCALFUNC));
  try
  {
    switch (l.site)
    {
    case 0:
    {
      l.fvar = fs->nactvar;
      TString* name = str_checkname(ls);
      new_localvar(ls, name); /* new local variable */
      adjustlocalvars(ls, 1); /* enter its scope */
      l.line = ls->linenumber;
      l.site = 1;
      [[fallthrough]];
    }
    case 1:
      body(ls, &l.b, 0, l.line); /* function created in next register */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_LOCALFUNC, l);
    throw;
  }
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, l.fvar)->startpc = fs->pc;
}

/* the frame of 'getvarattribute' (see lload.h) */
struct VarAttribFrame : LoadFrame
{
  TString* ts = nullptr;
  LoadFrame* copy() const override { return new (std::nothrow) VarAttribFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static lu_byte getvarattribute(LexState* ls, lu_byte df)
{
  /* attrib -> ['<' NAME '>'] */
  VarAttribFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const VarAttribFrame&>(luaZ_restore(ls->z->rz, LF_VARATTRIB));
  try
  {
    switch (l.site)
    {
    case 0:
      if (!testnext(ls, '<'))
        return df; /* return default value */
      l.site = 1;
      [[fallthrough]];
    case 1:
      l.ts = str_checkname(ls);
      l.site = 2;
      [[fallthrough]];
    case 2:
      checknext(ls, '>');
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_VARATTRIB, l);
    throw;
  }
  byte_view attr = l.ts->data.view();
  if (attr == lt::const_kw)
    return RDKCONST; /* read-only variable */
  else if (attr == lt::close_kw)
    return RDKTOCLOSE; /* to-be-closed variable */
  else
    luaK_semerror(ls, lt::unknown_attribute_s, FmtArgs().str(l.ts->data.view()));
  return df; /* return default value */
}

static void checktoclose(FuncState* fs, int level)
{
  if (level != -1)
  { /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}

/* the frame of 'localstat' (see lload.h) */
struct LocalstatFrame : LoadFrame
{
  int toclose = 0; /* index of to-be-closed variable (if any) */
  int vidx = 0;    /* index of last variable */
  int nvars = 0;
  int nexps = 0;
  lu_byte defkind = 0;
  TString* vname = nullptr;
  expdesc e{};
  LoadFrame* copy() const override { return new (std::nothrow) LocalstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void localstat(LexState* ls)
{
  /* stat -> LOCAL NAME attrib { ',' NAME attrib } ['=' explist] */
  LocalstatFrame l;
  FuncState* fs = ls->fs;
  Vardesc* var; /* last variable */
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const LocalstatFrame&>(luaZ_restore(ls->z->rz, LF_LOCALSTAT));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.toclose = -1;
        l.nvars = 0;
        l.site = 1;
        [[fallthrough]];
      case 1:
        /* get prefixed attribute (if any); default is regular local variable */
        l.defkind = getvarattribute(ls, VDKREG);
        l.site = 2;
        [[fallthrough]];
      case 2:                          /* the loop: for each variable */
        l.vname = str_checkname(ls); /* get its name */
        l.site = 3;
        [[fallthrough]];
      case 3:
      {
        lu_byte kind = getvarattribute(ls, l.defkind); /* postfixed attribute */
        l.vidx = new_varkind(ls, l.vname, kind);       /* predeclare it */
        if (kind == RDKTOCLOSE)
        {                      /* to-be-closed? */
          if (l.toclose != -1) /* one already present? */
            luaK_semerror(ls, lt::multiple_to_be_closed_variables_in_local_list, FmtArgs());
          l.toclose = fs->nactvar + l.nvars;
        }
        l.nvars++;
        l.site = 4;
        [[fallthrough]];
      }
      case 4:
        if (testnext(ls, ','))
        {
          l.site = 2;
          continue;
        }
        l.site = 5;
        [[fallthrough]];
      case 5:
        if (!testnext(ls, '='))
        {
          l.e.k = VVOID;
          l.nexps = 0;
          break;
        }
        l.site = 6; /* initialization */
        [[fallthrough]];
      case 6:
        l.nexps = explist(ls, &l.e);
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the declaration is read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_LOCALSTAT, l);
    throw;
  }
  var = getlocalvardesc(fs, l.vidx); /* retrieve last variable */
  if (l.nvars == l.nexps &&          /* no adjustments? */
      var->kind == RDKCONST &&       /* last variable is const? */
      luaK_exp2const(fs, &l.e, &var->k))
  {                                   /* compile-time constant? */
    var->kind = RDKCTC;               /* variable is a compile-time constant */
    adjustlocalvars(ls, l.nvars - 1); /* exclude last variable */
    fs->nactvar++;                    /* but count it */
  }
  else
  {
    adjust_assign(ls, l.nvars, l.nexps, &l.e);
    adjustlocalvars(ls, l.nvars);
  }
  checktoclose(fs, l.toclose);
}

static lu_byte getglobalattribute(LexState* ls, lu_byte df)
{
  lu_byte kind = getvarattribute(ls, df);
  switch (kind)
  {
  case RDKTOCLOSE:
    luaK_semerror(ls, lt::global_variables_cannot_be_to_be_closed, FmtArgs());
    return kind; /* to avoid warnings */
  case RDKCONST:
    return GDKCONST; /* adjust kind for global variable */
  default:
    return kind;
  }
}

static void checkglobal(LexState* ls, TString* varname, int line)
{
  FuncState* fs = ls->fs;
  expdesc var;
  int k;
  buildglobal(ls, varname, &var); /* create global variable in 'var' */
  k = var.ind.keystr;           /* index of global name in 'k' */
  luaK_codecheckglobal(fs, &var, k, line);
}

/* the frame of 'initglobal' (see lload.h) */
struct InitglobalFrame : LoadFrame
{
  TString* varname = nullptr;
  expdesc e{};
  expdesc var{};
  LoadFrame* copy() const override { return new (std::nothrow) InitglobalFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/*
** Recursively traverse list of globals to be initalized. When
** going, generate table description for the global. In the end,
** after all indices have been generated, read list of initializing
** expressions. When returning, generate the assignment of the value on
** the stack to the corresponding table description. 'n' is the variable
** being handled, range [0, nvars - 1].
*/
static void initglobal(LexState* ls, int nvars, int firstidx, int n,
                       int line)
{
  InitglobalFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const InitglobalFrame&>(luaZ_restore(ls->z->rz, LF_INITGLOBAL));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        if (n != nvars)
        { /* handle variable 'n' */
          l.varname = getlocalvardesc(fs, firstidx + n)->name;
          buildglobal(ls, l.varname, &l.var); /* create global variable in 'var' */
          l.site = 2;
          continue;
        }
        /* traversed all variables */
        l.site = 1;
        [[fallthrough]];
      case 1:
      {
        int nexps = explist(ls, &l.e); /* read list of expressions */
        adjust_assign(ls, nvars, nexps, &l.e);
        return;
      }
      case 2:
        enterlevel(ls); /* control recursion depth */
        initglobal(ls, nvars, firstidx, n + 1, line);
        leavelevel(ls);
        checkglobal(ls, l.varname, line);
        storevartop(fs, &l.var);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_INITGLOBAL, l);
    throw;
  }
}

/* the frame of 'globalnames' (see lload.h) */
struct GlobalnamesFrame : LoadFrame
{
  int nvars = 0;
  int lastidx = 0; /* index of last registered variable */
  int line = 0;
  TString* vname = nullptr;
  LoadFrame* copy() const override { return new (std::nothrow) GlobalnamesFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void globalnames(LexState* ls, lu_byte defkind)
{
  GlobalnamesFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const GlobalnamesFrame&>(luaZ_restore(ls->z->rz, LF_GLOBALNAMES));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.nvars = 0;
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop: for each name */
        l.vname = str_checkname(ls);
        l.site = 2;
        [[fallthrough]];
      case 2:
      {
        lu_byte kind = getglobalattribute(ls, defkind);
        l.lastidx = new_varkind(ls, l.vname, kind);
        l.nvars++;
        l.site = 3;
        [[fallthrough]];
      }
      case 3:
        if (testnext(ls, ','))
        {
          l.site = 1;
          continue;
        }
        l.site = 4;
        [[fallthrough]];
      case 4:
        if (!testnext(ls, '='))
          break;
        /* initialization */
        l.line = ls->linenumber;
        l.site = 5;
        [[fallthrough]];
      case 5:
        initglobal(ls, l.nvars, l.lastidx - l.nvars + 1, 0, l.line);
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the names are read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_GLOBALNAMES, l);
    throw;
  }
  fs->nactvar = (short)(fs->nactvar + l.nvars); /* activate declaration */
}

/* the frame of 'globalstat' (see lload.h) */
struct GlobalstatFrame : LoadFrame
{
  lu_byte defkind = 0;
  LoadFrame* copy() const override { return new (std::nothrow) GlobalstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void globalstat(LexState* ls)
{
  /* globalstat -> (GLOBAL) attrib '*'
     globalstat -> (GLOBAL) attrib NAME attrib {',' NAME attrib} */
  GlobalstatFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const GlobalstatFrame&>(luaZ_restore(ls->z->rz, LF_GLOBALSTAT));
  try
  {
    switch (l.site)
    {
    case 0:
      /* get prefixed attribute (if any); default is regular global variable */
      l.defkind = getglobalattribute(ls, GDKREG);
      l.site = 1;
      [[fallthrough]];
    case 1:
      if (testnext(ls, '*'))
      {
        new_varkind(ls, nullptr, l.defkind);
        fs->nactvar++; /* activate declaration */
        break;
      }
      l.site = 2;
      [[fallthrough]];
    case 2:
      globalnames(ls, l.defkind);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_GLOBALSTAT, l);
    throw;
  }
}

/* the frame of 'globalfunc' (see lload.h) */
struct GlobalfuncFrame : LoadFrame
{
  int bline = 0;
  TString* fname = nullptr;
  expdesc var{}, b{};
  LoadFrame* copy() const override { return new (std::nothrow) GlobalfuncFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void globalfunc(LexState* ls, int line)
{
  /* globalfunc -> (GLOBAL FUNCTION) NAME body */
  GlobalfuncFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const GlobalfuncFrame&>(luaZ_restore(ls->z->rz, LF_GLOBALFUNC));
  try
  {
    switch (l.site)
    {
    case 0:
      l.fname = str_checkname(ls);
      new_varkind(ls, l.fname, GDKREG); /* declare global variable */
      fs->nactvar++;                    /* enter its scope */
      buildglobal(ls, l.fname, &l.var);
      l.bline = ls->linenumber;
      l.site = 1;
      [[fallthrough]];
    case 1:
      body(ls, &l.b, 0, l.bline); /* compile and return closure in 'b' */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_GLOBALFUNC, l);
    throw;
  }
  checkglobal(ls, l.fname, line);
  luaK_storevar(fs, &l.var, &l.b);
  luaK_fixline(fs, line); /* definition "happens" in the first line */
}

static void globalstatfunc(LexState* ls, int line)
{
  /* stat -> GLOBAL globalfunc | GLOBAL globalstat */
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_GLOBALSTATFUNC);
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        luaX_next(ls); /* skip 'global' */
        l.site = 1;
        [[fallthrough]];
      case 1:
        l.site = testnext(ls, TK_FUNCTION) ? 2 : 3;
        continue;
      case 2:
        globalfunc(ls, line);
        return;
      case 3:
        globalstat(ls);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_GLOBALSTATFUNC, l);
    throw;
  }
}

/* the frame of 'funcname' (see lload.h) */
struct FuncnameFrame : LoadFrame
{
  int ismethod = 0;
  LoadFrame* copy() const override { return new (std::nothrow) FuncnameFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static int funcname(LexState* ls, expdesc* v)
{
  /* funcname -> NAME {fieldsel} [':' NAME] */
  FuncnameFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const FuncnameFrame&>(luaZ_restore(ls->z->rz, LF_FUNCNAME));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.ismethod = 0;
        singlevar(ls, v);
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        if (ls->t.token == '.')
          l.site = 2;
        else if (ls->t.token == ':')
        {
          l.ismethod = 1;
          l.site = 3;
        }
        else
          return l.ismethod;
        continue;
      case 2:
        fieldsel(ls, v);
        l.site = 1;
        continue;
      case 3:
        fieldsel(ls, v);
        return l.ismethod;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FUNCNAME, l);
    throw;
  }
}

/* the frame of 'funcstat' (see lload.h) */
struct FuncstatFrame : LoadFrame
{
  int ismethod = 0;
  expdesc v{}, b{};
  LoadFrame* copy() const override { return new (std::nothrow) FuncstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void funcstat(LexState* ls, int line)
{
  /* funcstat -> FUNCTION funcname body */
  FuncstatFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const FuncstatFrame&>(luaZ_restore(ls->z->rz, LF_FUNCSTAT));
  try
  {
    switch (l.site)
    {
    case 0:
      luaX_next(ls); /* skip FUNCTION */
      l.site = 1;
      [[fallthrough]];
    case 1:
      l.ismethod = funcname(ls, &l.v);
      check_readonly(ls, &l.v);
      l.site = 2;
      [[fallthrough]];
    case 2:
      body(ls, &l.b, l.ismethod, line);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_FUNCSTAT, l);
    throw;
  }
  luaK_storevar(ls->fs, &l.v, &l.b);
  luaK_fixline(ls->fs, line); /* definition "happens" in the first line */
}

/* the frame of 'exprstat' (see lload.h) */
struct ExprstatFrame : LoadFrame
{
  struct LHS_assign v{};
  LoadFrame* copy() const override { return new (std::nothrow) ExprstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void exprstat(LexState* ls)
{
  /* stat -> func | assignment */
  ExprstatFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const ExprstatFrame&>(luaZ_restore(ls->z->rz, LF_EXPRSTAT));
  try
  {
    switch (l.site)
    {
    case 0:
      suffixedexp(ls, &l.v.v);
      if (ls->t.token != '=' && ls->t.token != ',')
      { /* stat -> func */
        std::span<Instruction> code = fs->f->code;
        check_condition(ls, l.v.v.k == VCALL, lt::syntax_error);
        int pc = l.v.v.info;
        code[pc] = SETARG_C(code[pc], 1); /* call statement uses no results */
        break;
      }
      /* stat -> assignment */
      l.v.prev = nullptr;
      l.site = 1;
      [[fallthrough]];
    case 1:
      restassign(ls, &l.v, 1);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_EXPRSTAT, l);
    throw;
  }
}

/* the frame of 'retstat' (see lload.h) */
struct RetstatFrame : LoadFrame
{
  int nret = 0;  /* number of values being returned */
  int first = 0; /* first slot to be returned */
  expdesc e{};
  LoadFrame* copy() const override { return new (std::nothrow) RetstatFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void retstat(LexState* ls)
{
  /* stat -> RETURN [explist] [';'] */
  RetstatFrame l;
  FuncState* fs = ls->fs;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const RetstatFrame&>(luaZ_restore(ls->z->rz, LF_RETSTAT));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.first = luaY_nvarstack(fs);
        if (block_follow(ls, 1) || ls->t.token == ';')
        {
          l.nret = 0; /* return no values */
          l.site = 2;
          continue;
        }
        l.site = 1;
        [[fallthrough]];
      case 1:
        l.nret = explist(ls, &l.e); /* optional return values */
        if (hasmultret(l.e.k))
        {
          luaK_setmultret(fs, &l.e);
          if (l.e.k == VCALL && l.nret == 1 && !fs->bl->insidetbc)
          { /* tail call? */
            fs->f->code[l.e.info] = SET_OPCODE(fs->f->code[l.e.info], OP_TAILCALL);
            assert(GETARG_A(fs->f->code[l.e.info]) == luaY_nvarstack(fs));
          }
          l.nret = LUA_MULTRET; /* return all values */
        }
        else
        {
          if (l.nret == 1)                       /* only one single value? */
            l.first = luaK_exp2anyreg(fs, &l.e); /* can use original slot */
          else
          { /* values must go to the top of the stack */
            luaK_exp2nextreg(fs, &l.e);
            assert(l.nret == fs->freereg - l.first);
          }
        }
        l.site = 2;
        [[fallthrough]];
      case 2:
        luaK_ret(fs, l.first, l.nret);
        l.site = 3;
        [[fallthrough]];
      case 3:
        testnext(ls, ';'); /* skip optional semicolon */
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_RETSTAT, l);
    throw;
  }
}

/* the frame of 'statement' (see lload.h) */
struct StatementFrame : LoadFrame
{
  int line = 0; /* may be needed for error messages */
  TString* name = nullptr;
  LoadFrame* copy() const override { return new (std::nothrow) StatementFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

static void statement(LexState* ls)
{
  StatementFrame l;
  enterlevel(ls);
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const StatementFrame&>(luaZ_restore(ls->z->rz, LF_STATEMENT));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.line = ls->linenumber;
        switch (ls->t.token)
        {
        case ';': /* stat -> ';' (empty statement) */
          l.site = 1;
          break;
        case TK_IF: /* stat -> ifstat */
          l.site = 2;
          break;
        case TK_WHILE: /* stat -> whilestat */
          l.site = 3;
          break;
        case TK_DO: /* stat -> DO block END */
          l.site = 4;
          break;
        case TK_FOR: /* stat -> forstat */
          l.site = 7;
          break;
        case TK_REPEAT: /* stat -> repeatstat */
          l.site = 8;
          break;
        case TK_FUNCTION: /* stat -> funcstat */
          l.site = 9;
          break;
        case TK_LOCAL: /* stat -> localstat */
          l.site = 10;
          break;
        case TK_GLOBAL: /* stat -> globalstatfunc */
          l.site = 14;
          break;
        case TK_DBCOLON: /* stat -> label */
          l.site = 15;
          break;
        case TK_RETURN: /* stat -> retstat */
          l.site = 18;
          break;
        case TK_BREAK: /* stat -> breakstat */
          l.site = 20;
          break;
        case TK_GOTO: /* stat -> 'goto' NAME */
          l.site = 21;
          break;
        default: /* stat -> func | assignment */
          l.site = 23;
          break;
        }
        continue;
      case 1:
        luaX_next(ls); /* skip ';' */
        break;
      case 2:
        ifstat(ls, l.line);
        break;
      case 3:
        whilestat(ls, l.line);
        break;
      case 4:
        luaX_next(ls); /* skip DO */
        l.site = 5;
        [[fallthrough]];
      case 5:
        block(ls);
        l.site = 6;
        [[fallthrough]];
      case 6:
        check_match(ls, TK_END, TK_DO, l.line);
        break;
      case 7:
        forstat(ls, l.line);
        break;
      case 8:
        repeatstat(ls, l.line);
        break;
      case 9:
        funcstat(ls, l.line);
        break;
      case 10:
        luaX_next(ls); /* skip LOCAL */
        l.site = 11;
        [[fallthrough]];
      case 11:
        l.site = testnext(ls, TK_FUNCTION) ? 12 : 13; /* local function? */
        continue;
      case 12:
        localfunc(ls);
        break;
      case 13:
        localstat(ls);
        break;
      case 14:
        globalstatfunc(ls, l.line);
        break;
      case 15:
        luaX_next(ls); /* skip double colon */
        l.site = 16;
        [[fallthrough]];
      case 16:
        l.name = str_checkname(ls);
        l.site = 17;
        [[fallthrough]];
      case 17:
        labelstat(ls, l.name, l.line);
        break;
      case 18:
        luaX_next(ls); /* skip RETURN */
        l.site = 19;
        [[fallthrough]];
      case 19:
        retstat(ls);
        break;
      case 20:
        breakstat(ls, l.line);
        break;
      case 21:
        luaX_next(ls); /* skip 'goto' */
        l.site = 22;
        [[fallthrough]];
      case 22:
        gotostat(ls, l.line);
        break;
      case 23:
        exprstat(ls);
        break;
      default:
        luaZ_badstate(ls->z->rz);
      }
      break; /* (the statement is read) */
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_STATEMENT, l);
    throw;
  }
  assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  ls->fs->freereg = luaY_nvarstack(ls->fs); /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */

/* }====================================================================== */

/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc(LexState* ls, FuncState* fs)
{
  BlockFrame l;
  if (l_unlikely(ls->z->resuming))
  { /* (its function and block again in the chains) */
    l = static_cast<const BlockFrame&>(luaZ_restore(ls->z->rz, LF_MAINFUNC));
    fs->prev = nullptr;
    fs->ls = ls;
    ls->fs = fs;
    l.bl.previous = nullptr;
    fs->bl = &l.bl;
  }
  try
  {
    switch (l.site)
    {
    case 0:
    {
      open_func(ls, fs, &l.bl);
      setvararg(fs);                      /* main function is always vararg */
      Upvaldesc* env = allocupvalue(fs); /* ...set environment upvalue */
      env->instack = 1;
      env->idx = 0;
      env->kind = VDKREG;
      env->name = ls->envn;
      luaC_objbarrier(ls->L, fs->f, env->name);
      l.site = 1;
      [[fallthrough]];
    }
    case 1:
      luaX_next(ls); /* read first token */
      l.site = 2;
      [[fallthrough]];
    case 2:
      statlist(ls); /* parse main body */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_MAINFUNC, l);
    throw;
  }
  check(ls, TK_EOS);
  close_func(ls);
}

/* the frame of 'luaY_parser' (see lload.h) */
struct ParserFrame : LoadFrame
{
  LexState lexstate{};
  FuncState funcstate{};
  LClosure* cl = nullptr;
  LoadFrame* copy() const override { return new (std::nothrow) ParserFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

LClosure* luaY_parser(lua_State* L, ZIO* z, Table* anchor, MBuffer* buff, Dyndata* dyd, byte_view name, int firstchar)
{
  ParserFrame l;
  if (l_unlikely(z->resuming))
  { /* (the scanner again with this load's stream, buffer and lists) */
    l = static_cast<const ParserFrame&>(luaZ_restore(z->rz, LF_PARSER));
    l.lexstate.L = L;
    l.lexstate.z = z;
    l.lexstate.buff = buff;
    l.lexstate.dyd = dyd;
    l.lexstate.h = anchor;
    l.lexstate.fs = nullptr;
  }
  try
  {
    switch (l.site)
    {
    case 0:
      l.lexstate.h = anchor;                    /* table for scanner */
      l.cl = LClosure::luaF_newLclosure(L, 1); /* create main closure */
      luaZ_madefunction(z->rz, l.cl);
      assert(novariant(l.cl->tt) >= LUA_TSTRING);
      luaD_anchorobj(L, anchor, l.cl); /* anchor it in scanner table */
      l.funcstate.f = l.cl->p = Proto::luaF_newproto(L);
      luaC_objbarrier(L, l.cl, l.cl->p);
      l.funcstate.f->source = TString::luaS_createstr(L, name); /* create and anchor TString */
      luaC_objbarrier(L, l.funcstate.f, l.funcstate.f->source);
      l.lexstate.buff = buff;
      l.lexstate.dyd = dyd;
      dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
      luaX_setinput(L, &l.lexstate, z, l.funcstate.f->source, firstchar);
      l.site = 1;
      [[fallthrough]];
    case 1:
      mainfunc(&l.lexstate, &l.funcstate);
      break;
    default:
      luaZ_badstate(z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(z->rz, LF_PARSER, l);
    throw;
  }
  assert(!l.funcstate.prev && l.funcstate.nups == 1 && !l.lexstate.fs);
  /* all scopes should be correctly finished */
  assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  return l.cl;
}

}
